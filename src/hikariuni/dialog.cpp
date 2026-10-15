#include "dialog.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <commctrl.h>
#include <cwchar>
#include <dwmapi.h>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <uxtheme.h>
#include <vssym32.h>
#include <windowsx.h>

#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
// 今の見た目のコモンコントロール（comctl32 v6）を使う
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' "   \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' " \
                        "language='*'\"")

extern "C" IMAGE_DOS_HEADER __ImageBase; // この DLL の HINSTANCE

namespace hikariuni {
namespace {

constexpr int kParamCount = int(kParams.size());
constexpr int kLabelId = 2000, kEditId = 2100, kSliderId = 2200;
constexpr UINT_PTR kRefreshTimer = 1;
constexpr int kPickRadius        = 12;   // Alt+クリックで除外するとき、光源をつかめる画面上の距離（px）
constexpr double kMaxZoom        = 32;   // 拡大率の上限（3200%）
constexpr double kWheelZoom      = 1.25; // Ctrl+ホイール 1 目盛りの拡大率
constexpr int kWheelScroll       = 80;   // ホイール 1 目盛り・スクロールバーの矢印で動く画面上の距離（px）
constexpr double kMaxCachePixels = 24e6; // 合成キャッシュの画素数の上限

HINSTANCE Instance() {
  return reinterpret_cast<HINSTANCE>(&__ImageBase);
}

// 画面の配色。ダークは Affinity の既定（ダーク）に近い色
struct Palette {
  COLORREF background, text, editBackground, canvas;
};
constexpr Palette kDark{RGB(50, 50, 52), RGB(228, 228, 228), RGB(34, 34, 36), RGB(38, 38, 38)};
Palette LightPalette() {
  return {GetSysColor(COLOR_BTNFACE), GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_WINDOW), RGB(190, 190, 190)};
}

struct GdiDeleter {
  void operator()(HGDIOBJ o) const {
    DeleteObject(o);
  }
};
using Brush = std::unique_ptr<std::remove_pointer_t<HBRUSH>, GdiDeleter>;

// ---- 設定ファイル（%APPDATA%\Hikariuni\hikariuni.ini） ----

const std::wstring& IniPath() {
  static const std::wstring path = [] {
    wchar_t appData[MAX_PATH];
    if (!GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH)) return std::wstring();
    const std::wstring dir = std::wstring(appData) + L"\\Hikariuni";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\hikariuni.ini";
  }();
  return path;
}

// 数値として読めて有限なら値を返す（nan や inf は受け付けない）
std::optional<double> ParseNumber(const wchar_t* text) {
  wchar_t* end;
  const double d = wcstod(text, &end);
  if (end == text || !std::isfinite(d)) return std::nullopt;
  return d;
}

std::optional<double> ReadIni(const wchar_t* section, const wchar_t* key) {
  std::array<wchar_t, 32> text{};
  if (IniPath().empty() ||
      !GetPrivateProfileStringW(section, key, L"", text.data(), DWORD(text.size()), IniPath().c_str()))
    return std::nullopt;
  return ParseNumber(text.data());
}

void WriteIni(const wchar_t* section, const wchar_t* key, double v) {
  if (IniPath().empty()) return;
  WritePrivateProfileStringW(section, key, std::format(L"{}", v).c_str(), IniPath().c_str());
}

// ---- パラメーター ----

int SliderPos(const ParamInfo& info, double v) {
  return int(std::lround((v - info.min) / info.step));
}

// 範囲内に収めて刻みにそろえる
double Snap(const ParamInfo& info, double v) {
  return info.min + SliderPos(info, std::clamp(v, info.min, info.max)) * info.step;
}

struct State {
  const PixelView* image;
  const RgbImage* detectImage;
  Params p;
  Edits edits;
  SIZE minSize{}; // ダイアログの最小サイズ（テンプレートの大きさ）
  // 表示: z = 画面 px / 元画像 px。(ox, oy) = 表示枠の左上に来る元画像の座標
  double z = 1, ox = 0, oy = 0;
  bool fitted = true; // 全体表示中（ウィンドウの大きさを変えても全体表示を保つ）
  // 合成キャッシュ: 元画像全体を cw × ch に縮小し（100% を超える拡大はしない）、光芒を合成したもの。
  // 表示の移動・拡大縮小はキャッシュを描き直すだけなので速い
  int cw = 0, ch = 0;
  std::vector<uint32_t> base;  // 縮小した元画像
  std::vector<uint32_t> bgra;  // 光芒を合成したもの
  std::vector<Light> lights;   // 検出結果（元画像座標）
  std::vector<Light> arranged; // 描いている光源（手動の追加・除外を反映したもの）
  bool refreshQueued = false, redetectQueued = false;
  // 配色（ダーク / ライト）。パラメーターとは別に保存し、「初期値に戻す」では変えない
  bool dark       = true;
  Palette palette = kDark;
  Brush background, editBackground, canvas;
  enum class Drag { None, Click, Pan } drag = Drag::None;
  bool moved                                = false;
  POINT dragFrom{};
};

// ---- 再計算 ----

// 合成キャッシュの大きさ。表示の倍率に合わせて縮小し、画素数の上限で抑える
// ponytail: 元画像全体をキャッシュする。上限を超える巨大画像は 100%
// でも少しぼける（見える範囲だけのタイルにすれば解決）
SIZE CacheSize(const State& s) {
  const PixelView& img = *s.image;
  const double k       = std::min({1.0, s.z, std::sqrt(kMaxCachePixels / (double(img.w) * img.h))});
  return {std::max(1, int(std::lround(img.w * k))), std::max(1, int(std::lround(img.h * k)))};
}

void Refresh(HWND dlg, State& s, bool redetect) {
  const PixelView& img = *s.image;
  if (redetect) s.lights = Detect(*s.detectImage, s.p, double(s.detectImage->w) / img.w);
  if (const SIZE size = CacheSize(s); size.cx != s.cw || size.cy != s.ch) {
    s.cw = size.cx, s.ch = size.cy;
    s.base = PreviewBase(img, s.cw, s.ch);
  }
  const double unit = std::min(img.w, img.h) / 1000.0;
  s.arranged        = Arrange(s.lights, s.p, unit, s.edits);
  s.bgra.resize(s.base.size());
  ComposePreview(s.base, s.cw, s.ch, double(s.cw) / img.w, unit, s.arranged, s.p, s.bgra);
  InvalidateRect(GetDlgItem(dlg, IDC_PREVIEW), nullptr, FALSE);
}

// 重い再計算はタイマーで後回しにする。WM_TIMER は描画や入力より後に来るので、
// スライダーのつまみの描画を待たせず、連続した変更も 1 回の計算にまとまる
void RequestRefresh(HWND dlg, State& s, bool redetect) {
  s.redetectQueued |= redetect;
  if (!s.refreshQueued) s.refreshQueued = SetTimer(dlg, kRefreshTimer, 15, nullptr) != 0;
}

// ---- 表示（拡大率と位置） ----

// 表示枠の大きさ（px）
SIZE Viewport(HWND dlg) {
  RECT r;
  GetClientRect(GetDlgItem(dlg, IDC_PREVIEW), &r);
  return {std::max(1L, r.right), std::max(1L, r.bottom)};
}

// 表示枠のダイアログ上の位置
RECT ViewportRect(HWND dlg) {
  RECT r;
  GetWindowRect(GetDlgItem(dlg, IDC_PREVIEW), &r);
  MapWindowPoints(nullptr, dlg, reinterpret_cast<POINT*>(&r), 2);
  return r;
}

double FitZoom(HWND dlg, const State& s) {
  const SIZE v = Viewport(dlg);
  return std::min(double(v.cx) / s.image->w, double(v.cy) / s.image->h);
}

// 位置を画像の範囲に収める。画像が表示枠より小さい向きは中央に置く
void ClampView(HWND dlg, State& s) {
  const SIZE v = Viewport(dlg);
  auto clamp   = [&](double& o, int size, int view) {
    const double span = view / s.z; // 表示枠に入る元画像の長さ
    o                 = size <= span ? (size - span) / 2 : std::clamp(o, 0.0, size - span);
  };
  clamp(s.ox, s.image->w, v.cx);
  clamp(s.oy, s.image->h, v.cy);
}

void UpdateScrollBars(HWND dlg, const State& s) {
  const SIZE v = Viewport(dlg);
  auto set     = [&](int id, int size, double offset, int view) {
    const SCROLLINFO si{.cbSize = sizeof(SCROLLINFO),
                        .fMask  = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL,
                        .nMin   = 0,
                        .nMax   = std::max(0, int(std::lround(size * s.z)) - 1),
                        .nPage  = UINT(view),
                        .nPos   = int(std::lround(offset * s.z))};
    SetScrollInfo(GetDlgItem(dlg, id), SB_CTL, &si, TRUE);
  };
  set(IDC_HSCROLL, s.image->w, s.ox, v.cx);
  set(IDC_VSCROLL, s.image->h, s.oy, v.cy);
}

// 拡大率・位置を変えたあとに呼ぶ。位置を収め、スクロールバーと拡大率の表示をそろえて描き直す。
// キャッシュの大きさが変わるときだけ再計算する（それまでは今のキャッシュを拡大縮小して見せる）
void UpdateView(HWND dlg, State& s) {
  ClampView(dlg, s);
  UpdateScrollBars(dlg, s);
  SetDlgItemTextW(dlg, IDC_ZOOM_TEXT, std::format(L"{:.0f}%", s.z * 100).c_str());
  if (const SIZE size = CacheSize(s); size.cx != s.cw || size.cy != s.ch) RequestRefresh(dlg, s, false);
  InvalidateRect(GetDlgItem(dlg, IDC_PREVIEW), nullptr, FALSE);
}

void Fit(HWND dlg, State& s) {
  s.z      = FitZoom(dlg, s);
  s.fitted = true;
  UpdateView(dlg, s);
}

// at（表示枠内の座標）の下にある画像の点を動かさずに拡大縮小する
void ZoomAt(HWND dlg, State& s, double factor, POINT at) {
  const double ix = s.ox + at.x / s.z, iy = s.oy + at.y / s.z;
  s.z  = std::clamp(s.z * factor, FitZoom(dlg, s) / 2, kMaxZoom);
  s.ox = ix - at.x / s.z, s.oy = iy - at.y / s.z;
  s.fitted = false;
  UpdateView(dlg, s);
}

// 画面上で (dx, dy) px 動かす
void ScrollBy(HWND dlg, State& s, double dx, double dy) {
  s.ox += dx / s.z, s.oy += dy / s.z;
  UpdateView(dlg, s);
}

void OnScrollBar(HWND dlg, State& s, HWND bar, int code, bool horizontal) {
  SCROLLINFO si{.cbSize = sizeof(SCROLLINFO), .fMask = SIF_ALL};
  GetScrollInfo(bar, SB_CTL, &si);
  int pos = si.nPos;
  switch (code) {
  case SB_LINEUP:
    pos -= kWheelScroll;
    break;
  case SB_LINEDOWN:
    pos += kWheelScroll;
    break;
  case SB_PAGEUP:
    pos -= int(si.nPage);
    break;
  case SB_PAGEDOWN:
    pos += int(si.nPage);
    break;
  case SB_THUMBTRACK:
  case SB_THUMBPOSITION:
    pos = si.nTrackPos;
    break;
  case SB_TOP:
    pos = si.nMin;
    break;
  case SB_BOTTOM:
    pos = si.nMax;
    break;
  default:
    return;
  }
  (horizontal ? s.ox : s.oy) = pos / s.z;
  UpdateView(dlg, s);
}

// キャッシュのうち表示枠に入る部分を、拡大率に合わせて描く。画像の外側だけを背景色で塗る
// （全体を塗ってから画像を描くと、一瞬灰色が見えてちらつく）
void DrawPreview(const DRAWITEMSTRUCT& di, const State& s) {
  RECT shown{};
  if (!s.bgra.empty()) {
    const double kx = double(s.cw) / s.image->w, ky = double(s.ch) / s.image->h; // 元画像 px → キャッシュ px
    const double vw = (di.rcItem.right - di.rcItem.left) / s.z, vh = (di.rcItem.bottom - di.rcItem.top) / s.z;
    const int sx0 = std::clamp(int(std::floor(s.ox * kx)), 0, s.cw);
    const int sx1 = std::clamp(int(std::ceil((s.ox + vw) * kx)), 0, s.cw);
    const int sy0 = std::clamp(int(std::floor(s.oy * ky)), 0, s.ch);
    const int sy1 = std::clamp(int(std::ceil((s.oy + vh) * ky)), 0, s.ch);
    if (sx0 < sx1 && sy0 < sy1) {
      auto toScreen = [&](int c, double k, double o) { return LONG(std::lround((c / k - o) * s.z)); };
      shown = {toScreen(sx0, kx, s.ox), toScreen(sy0, ky, s.oy), toScreen(sx1, kx, s.ox), toScreen(sy1, ky, s.oy)};
      BITMAPINFO bi{};
      bi.bmiHeader = {sizeof(BITMAPINFOHEADER), s.cw, -s.ch, 1, 32, BI_RGB};
      SetStretchBltMode(di.hDC, COLORONCOLOR); // 拡大はドットのまま（Affinity と同じ）
      // 上から下に並んだ DIB でも、StretchDIBits の元の矩形の Y は下端から数える
      StretchDIBits(di.hDC, shown.left, shown.top, shown.right - shown.left, shown.bottom - shown.top, sx0, s.ch - sy1,
                    sx1 - sx0, sy1 - sy0, s.bgra.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    }
  }
  const int saved = SaveDC(di.hDC);
  ExcludeClipRect(di.hDC, shown.left, shown.top, shown.right, shown.bottom);
  FillRect(di.hDC, &di.rcItem, s.canvas.get());
  RestoreDC(di.hDC, saved);
}

// ---- マウス ----

POINT MousePos(LPARAM lp) {
  return {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
}

bool OverViewport(HWND dlg, POINT pt) {
  const RECT r = ViewportRect(dlg);
  return PtInRect(&r, pt);
}

// ダイアログのクライアント座標 → 表示枠内の座標
POINT InViewport(HWND dlg, POINT pt) {
  const RECT r = ViewportRect(dlg);
  return {pt.x - r.left, pt.y - r.top};
}

// ダイアログのクライアント座標を元画像座標にする。画像の外なら nullopt
std::optional<Point> ToImage(HWND dlg, const State& s, POINT pt) {
  if (!OverViewport(dlg, pt)) return std::nullopt;
  const POINT v = InViewport(dlg, pt);
  const Point q{s.ox + (v.x + 0.5) / s.z, s.oy + (v.y + 0.5) / s.z};
  if (q.x < 0 || q.y < 0 || q.x >= s.image->w || q.y >= s.image->h) return std::nullopt;
  return q;
}

// クリック: その位置に光源を足す
void AddLight(HWND dlg, State& s, POINT pt) {
  const auto q = ToImage(dlg, s, pt);
  if (!q) return;
  s.edits.added.push_back(MakeLight(*s.detectImage, double(s.detectImage->w) / s.image->w, q->x, q->y));
  RequestRefresh(dlg, s, false);
}

// Alt+クリック: 近くの光源を外す（手動で足したものは消し、自動のものは除外する）
void RemoveLight(HWND dlg, State& s, POINT pt) {
  const auto q = ToImage(dlg, s, pt);
  if (!q) return;
  const auto dist    = [&](const Light& l) { return std::hypot(l.x - q->x, l.y - q->y); };
  const auto nearest = [&](const std::vector<Light>& v) { return std::ranges::min_element(v, {}, dist); };
  const auto within  = [&](const Light& l) { return dist(l) < kPickRadius / s.z; };
  auto& added        = s.edits.added;
  if (auto it = nearest(added); it != added.end() && within(*it)) added.erase(it);
  else if (auto jt = nearest(s.arranged); jt != s.arranged.end() && within(*jt))
    s.edits.removed.push_back({jt->x, jt->y});
  else return;
  RequestRefresh(dlg, s, false);
}

// ---- 配色 ----

// ダーク / ライトを切り替える。タイトルバーは DWM、ボタン・スクロールバー・入力欄は Windows のダーク用テーマ
// （DarkMode_Explorer / DarkMode_CFD。エクスプローラーなどが使っているもの）、それ以外は WM_CTLCOLOR* で色を付ける
void ApplyTheme(HWND dlg, State& s) {
  s.palette            = s.dark ? kDark : LightPalette();
  s.background         = Brush(CreateSolidBrush(s.palette.background));
  s.editBackground     = Brush(CreateSolidBrush(s.palette.editBackground));
  s.canvas             = Brush(CreateSolidBrush(s.palette.canvas));
  const BOOL darkTitle = s.dark;
  DwmSetWindowAttribute(dlg, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkTitle, sizeof(darkTitle));
  EnumChildWindows(
      dlg,
      [](HWND child, LPARAM dark) -> BOOL {
        std::array<wchar_t, 32> cls{};
        GetClassNameW(child, cls.data(), int(cls.size()));
        const bool edit = _wcsicmp(cls.data(), WC_EDITW) == 0;
        SetWindowTheme(child, !dark ? nullptr : edit ? L"DarkMode_CFD" : L"DarkMode_Explorer", nullptr);
        return TRUE;
      },
      s.dark);
  SetDlgItemTextW(dlg, IDC_THEME, s.dark ? L"テーマ: ダーク" : L"テーマ: ライト");
  RedrawWindow(dlg, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

// テーマ付きのチェックボックスは文字色を変えられない（ダークでも黒で描かれる）ので、ダークのときは自分で描く
bool DrawCheckBox(const NMCUSTOMDRAW& cd, const State& s) {
  const HWND button  = cd.hdr.hwndFrom;
  const HTHEME theme = OpenThemeData(button, L"Button");
  if (!theme) return false;
  const bool checked = Button_GetCheck(button) == BST_CHECKED;
  const bool hot = cd.uItemState & CDIS_HOT, pressed = cd.uItemState & CDIS_SELECTED;
  const int state = checked ? (pressed ? CBS_CHECKEDPRESSED
                               : hot   ? CBS_CHECKEDHOT
                                       : CBS_CHECKEDNORMAL)
                            : (pressed ? CBS_UNCHECKEDPRESSED
                               : hot   ? CBS_UNCHECKEDHOT
                                       : CBS_UNCHECKEDNORMAL);
  SIZE box{};
  GetThemePartSize(theme, cd.hdc, BP_CHECKBOX, state, nullptr, TS_DRAW, &box);
  const RECT& rc = cd.rc;
  FillRect(cd.hdc, &rc, s.background.get());
  const LONG boxTop = rc.top + (rc.bottom - rc.top - box.cy) / 2;
  const RECT boxRect{rc.left, boxTop, rc.left + box.cx, boxTop + box.cy};
  DrawThemeBackground(theme, cd.hdc, BP_CHECKBOX, state, &boxRect, nullptr);
  CloseThemeData(theme);

  std::array<wchar_t, 64> label{};
  GetWindowTextW(button, label.data(), int(label.size()));
  RECT text = rc;
  text.left = boxRect.right + MulDiv(4, GetDpiForWindow(button), 96);
  SetTextColor(cd.hdc, s.palette.text);
  SetBkMode(cd.hdc, TRANSPARENT);
  const HGDIOBJ oldFont = SelectObject(cd.hdc, GetWindowFont(button));
  DrawTextW(cd.hdc, label.data(), -1, &text, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
  // キーボードで操作しているときはフォーカスの枠も描く
  if ((cd.uItemState & CDIS_FOCUS) && !(SendMessageW(button, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
    RECT focus = text;
    DrawTextW(cd.hdc, label.data(), -1, &focus, DT_SINGLELINE | DT_CALCRECT);
    const LONG h = focus.bottom - focus.top;
    focus.top = text.top + (text.bottom - text.top - h) / 2, focus.bottom = focus.top + h;
    InflateRect(&focus, 1, 0);
    DrawFocusRect(cd.hdc, &focus);
  }
  SelectObject(cd.hdc, oldFont);
  return true;
}

// ---- コントロール ----

void SetValueText(HWND dlg, int i, double v) {
  const std::wstring text = kParams[i].step < 1 ? std::format(L"{:.2f}", v) : std::format(L"{:.0f}", v);
  SetDlgItemTextW(dlg, kEditId + i, text.c_str());
}

// パラメーター i を v にして、数値・スライダー・プレビューをそろえる。
// fromSlider ならスライダーは動かさない（ドラッグ中に位置を設定し直すと、つまみが二重に描かれる）
void SetValue(HWND dlg, State& s, int i, double v, bool fromSlider = false) {
  const ParamInfo& info = kParams[i];
  v                     = Snap(info, v);
  SetValueText(dlg, i, v);
  if (!fromSlider) SendDlgItemMessageW(dlg, kSliderId + i, TBM_SETPOS, TRUE, SliderPos(info, v));
  if (s.p.*info.field == v) return;
  s.p.*info.field = v;
  RequestRefresh(dlg, s, info.detect);
}

// 数値入力欄の値を確定する。数値でなければ元の値に戻す
void CommitEdit(HWND dlg, State& s, int i) {
  std::array<wchar_t, 32> text{};
  GetDlgItemTextW(dlg, kEditId + i, text.data(), int(text.size()));
  SetValue(dlg, s, i, ParseNumber(text.data()).value_or(s.p.*kParams[i].field));
}

void SetAll(HWND dlg, const Params& p) {
  for (int i = 0; i < kParamCount; i++) {
    const double v = p.*kParams[i].field;
    SendDlgItemMessageW(dlg, kSliderId + i, TBM_SETPOS, TRUE, SliderPos(kParams[i], v));
    SetValueText(dlg, i, v);
  }
  CheckDlgButton(dlg, IDC_RAYS_ONLY, p.raysOnly ? BST_CHECKED : BST_UNCHECKED);
}

// kParams の数だけ「名前・数値入力・スライダー」を作る。位置とフォントは Layout で決める
void CreateRows(HWND dlg) {
  auto create = [&](DWORD exStyle, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    return CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, dlg,
                           reinterpret_cast<HMENU>(INT_PTR(id)), Instance(), nullptr);
  };
  for (int i = 0; i < kParamCount; i++) {
    create(0, WC_STATICW, kParams[i].label, 0, kLabelId + i);
    create(WS_EX_CLIENTEDGE, WC_EDITW, L"", ES_RIGHT | ES_AUTOHSCROLL | WS_TABSTOP, kEditId + i);
    HWND slider = create(0, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, kSliderId + i);
    SendMessageW(slider, TBM_SETRANGEMIN, FALSE, 0);
    SendMessageW(slider, TBM_SETRANGEMAX, FALSE, SliderPos(kParams[i], kParams[i].max));
  }
}

// ダイアログ単位 → px
SIZE Du(HWND dlg, int x, int y) {
  RECT r{0, 0, x, y};
  MapDialogRect(dlg, &r);
  return {r.right, r.bottom};
}

// 右にパラメーター（幅固定）、下にボタン、残りをプレビュー（右と下にスクロールバー）にする。
// 自分で作った行には、ダイアログの今のフォントを付け直す（DPI が変わるとダイアログのフォントが変わるため）
void Layout(HWND dlg) {
  RECT c;
  GetClientRect(dlg, &c);
  const SIZE m = Du(dlg, 7, 7), panel = Du(dlg, 200, 22), button = Du(dlg, 56, 14), reset = Du(dlg, 70, 14);
  const SIZE edit = Du(dlg, 40, 11), label = Du(dlg, 0, 9), slider = Du(dlg, 6, 11);
  const SIZE raysOnly = Du(dlg, 110, 0), fit = Du(dlg, 50, 0), zoomText = Du(dlg, 40, 0), theme = Du(dlg, 70, 0);
  const UINT dpi = GetDpiForWindow(dlg);
  const int sbw = GetSystemMetricsForDpi(SM_CXVSCROLL, dpi), sbh = GetSystemMetricsForDpi(SM_CYHSCROLL, dpi);
  const int x = c.right - m.cx - panel.cx, bottom = c.bottom - m.cy - button.cy;
  const auto font = WPARAM(SendMessageW(dlg, WM_GETFONT, 0, 0));
  auto move       = [&](int id, int l, int t, int w, int h) {
    HWND ctrl = GetDlgItem(dlg, id);
    if (WPARAM(SendMessageW(ctrl, WM_GETFONT, 0, 0)) != font) SendMessageW(ctrl, WM_SETFONT, font, FALSE);
    MoveWindow(ctrl, l, t, w, h, TRUE);
  };
  const int left = m.cx, right = x - m.cx, hintTop = bottom - m.cy - label.cy, viewBottom = hintTop - m.cy / 2;
  move(IDC_PREVIEW, left, m.cy, right - left - sbw, viewBottom - m.cy - sbh);
  move(IDC_VSCROLL, right - sbw, m.cy, sbw, viewBottom - m.cy - sbh);
  move(IDC_HSCROLL, left, viewBottom - sbh, right - left - sbw, sbh);
  move(IDC_HINT, left, hintTop, right - left, label.cy);
  for (int i = 0; i < kParamCount; i++) {
    const int y = m.cy + i * panel.cy;
    move(kLabelId + i, x, y + (edit.cy - label.cy) / 2, panel.cx - edit.cx - m.cx, label.cy);
    move(kEditId + i, x + panel.cx - edit.cx, y, edit.cx, edit.cy);
    move(kSliderId + i, x - slider.cx / 2, y + edit.cy, panel.cx + slider.cx, slider.cy);
  }
  int bx    = m.cx;
  auto next = [&](int id, int w, int h, int t) {
    move(id, bx, t, w, h);
    bx += w + m.cx;
  };
  next(IDC_RESET, reset.cx, reset.cy, bottom);
  next(IDC_RAYS_ONLY, raysOnly.cx, reset.cy, bottom);
  next(IDC_FIT, fit.cx, reset.cy, bottom);
  next(IDC_ZOOM_TEXT, zoomText.cx, label.cy, bottom + (reset.cy - label.cy) / 2);
  next(IDC_THEME, theme.cx, reset.cy, bottom);
  move(IDOK, c.right - m.cx - button.cx * 2 - m.cx / 2, bottom, button.cx, button.cy);
  move(IDCANCEL, c.right - m.cx - button.cx, bottom, button.cx, button.cy);
}

// 前回のウィンドウサイズに戻し、画面の中央に置く。最小サイズが画面より大きければ最小サイズのまま
void RestoreSize(HWND dlg, const State& s) {
  const auto w = ReadIni(L"window", L"width"), h = ReadIni(L"window", L"height");
  if (!w || !h) return;
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(MonitorFromWindow(dlg, MONITOR_DEFAULTTONEAREST), &mi);
  const RECT& a = mi.rcWork;
  auto fit      = [](double v, LONG lo, LONG hi) { return int(std::clamp(v, double(lo), double(std::max(lo, hi)))); };
  const int cw = fit(*w, s.minSize.cx, a.right - a.left), ch = fit(*h, s.minSize.cy, a.bottom - a.top);
  SetWindowPos(dlg, nullptr, a.left + (a.right - a.left - cw) / 2, a.top + (a.bottom - a.top - ch) / 2, cw, ch,
               SWP_NOZORDER);
}

void SaveSize(HWND dlg) {
  WINDOWPLACEMENT wp{sizeof(wp)};
  GetWindowPlacement(dlg, &wp);
  WriteIni(L"window", L"width", wp.rcNormalPosition.right - wp.rcNormalPosition.left);
  WriteIni(L"window", L"height", wp.rcNormalPosition.bottom - wp.rcNormalPosition.top);
}

int EditIndex(HWND control) {
  const int i = GetDlgCtrlID(control) - kEditId;
  return i >= 0 && i < kParamCount ? i : -1;
}

INT_PTR CALLBACK DialogProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  auto s = reinterpret_cast<State*>(GetWindowLongPtrW(dlg, DWLP_USER));
  switch (msg) {
  case WM_INITDIALOG: {
    s = reinterpret_cast<State*>(lp);
    SetWindowLongPtrW(dlg, DWLP_USER, lp);
    RECT r;
    GetWindowRect(dlg, &r);
    s->minSize = {r.right - r.left, r.bottom - r.top};
    CreateRows(dlg);
    SetAll(dlg, s->p);
    s->dark = ReadIni(L"ui", L"dark").value_or(1) != 0; // 既定はダーク（Affinity の既定に合わせる）
    ApplyTheme(dlg, *s);
    RestoreSize(dlg, *s);
    Layout(dlg);
    Fit(dlg, *s);
    RequestRefresh(dlg, *s, true);
    return TRUE;
  }
  case WM_GETMINMAXINFO:
    if (!s) break;
    reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {s->minSize.cx, s->minSize.cy};
    return TRUE;
  case WM_SIZE:
    if (!s || wp == SIZE_MINIMIZED) break;
    Layout(dlg);
    if (s->fitted) Fit(dlg, *s);
    else UpdateView(dlg, *s);
    return TRUE;
  case WM_TIMER:
    if (wp != kRefreshTimer) break;
    KillTimer(dlg, kRefreshTimer);
    s->refreshQueued = false;
    Refresh(dlg, *s, std::exchange(s->redetectQueued, false));
    return TRUE;
  case WM_HSCROLL: {
    const auto from = reinterpret_cast<HWND>(lp);
    if (s && from == GetDlgItem(dlg, IDC_HSCROLL)) {
      OnScrollBar(dlg, *s, from, LOWORD(wp), true);
      return TRUE;
    }
    const int i = GetDlgCtrlID(from) - kSliderId;
    if (i < 0 || i >= kParamCount) break;
    const LRESULT pos = SendMessageW(from, TBM_GETPOS, 0, 0);
    SetValue(dlg, *s, i, kParams[i].min + pos * kParams[i].step, true);
    return TRUE;
  }
  case WM_VSCROLL:
    if (!s || reinterpret_cast<HWND>(lp) != GetDlgItem(dlg, IDC_VSCROLL)) break;
    OnScrollBar(dlg, *s, reinterpret_cast<HWND>(lp), LOWORD(wp), false);
    return TRUE;
  case WM_CTLCOLORDLG:
  case WM_CTLCOLORSTATIC: // 文字、スライダーの背景、チェックボックス
  case WM_CTLCOLORBTN:
  case WM_CTLCOLOREDIT: { // ダイアログでは WM_CTLCOLOR* だけはブラシをそのまま返す
    if (!s || !s->background) break;
    const auto dc   = reinterpret_cast<HDC>(wp);
    const bool edit = msg == WM_CTLCOLOREDIT;
    SetTextColor(dc, s->palette.text);
    SetBkColor(dc, edit ? s->palette.editBackground : s->palette.background);
    return reinterpret_cast<INT_PTR>((edit ? s->editBackground : s->background).get());
  }
  case WM_NOTIFY: {
    const auto& hdr = *reinterpret_cast<const NMHDR*>(lp);
    if (!s || !s->dark || hdr.idFrom != IDC_RAYS_ONLY || hdr.code != NM_CUSTOMDRAW) break;
    const auto& cd = *reinterpret_cast<const NMCUSTOMDRAW*>(lp);
    if (cd.dwDrawStage != CDDS_PREPAINT || !DrawCheckBox(cd, *s)) break;
    SetWindowLongPtrW(dlg, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
    return TRUE;
  }
  case WM_DRAWITEM:
    if (wp != IDC_PREVIEW) break;
    DrawPreview(*reinterpret_cast<const DRAWITEMSTRUCT*>(lp), *s);
    return TRUE;
  case WM_SETCURSOR: { // 画像の上では十字カーソル（クリックで光源を置けることを示す）
    if (!s) break;
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(dlg, &pt);
    if (!ToImage(dlg, *s, pt)) break;
    SetCursor(LoadCursor(nullptr, IDC_CROSS));
    SetWindowLongPtrW(dlg, DWLP_MSGRESULT, TRUE);
    return TRUE;
  }
  case WM_MOUSEWHEEL: { // Ctrl: 拡大縮小、Shift: 横に移動、なし: 縦に移動（Affinity と同じ）
    if (!s) break;
    POINT pt = MousePos(lp);
    ScreenToClient(dlg, &pt);
    if (!OverViewport(dlg, pt)) break;
    const double notches = GET_WHEEL_DELTA_WPARAM(wp) / double(WHEEL_DELTA);
    const auto keys      = GET_KEYSTATE_WPARAM(wp);
    if (keys & MK_CONTROL) ZoomAt(dlg, *s, std::pow(kWheelZoom, notches), InViewport(dlg, pt));
    else if (keys & MK_SHIFT) ScrollBy(dlg, *s, -notches * kWheelScroll, 0);
    else ScrollBy(dlg, *s, 0, -notches * kWheelScroll);
    return TRUE;
  }
  case WM_LBUTTONDOWN:
  case WM_MBUTTONDOWN: // 左: クリック（離したときに光源を追加・除外）、中: ドラッグで移動
    if (!s || !OverViewport(dlg, MousePos(lp))) break;
    s->drag  = msg == WM_MBUTTONDOWN ? State::Drag::Pan : State::Drag::Click;
    s->moved = false, s->dragFrom = MousePos(lp);
    if (s->drag == State::Drag::Pan) SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
    SetCapture(dlg);
    return TRUE;
  case WM_MOUSEMOVE: {
    if (!s || s->drag == State::Drag::None) break;
    const POINT pt = MousePos(lp);
    if (s->drag == State::Drag::Pan) {
      ScrollBy(dlg, *s, s->dragFrom.x - pt.x, s->dragFrom.y - pt.y);
      s->dragFrom = pt;
    } else if (std::abs(pt.x - s->dragFrom.x) + std::abs(pt.y - s->dragFrom.y) >= 4) {
      s->moved = true; // 動かしたらクリックとみなさない
    }
    return TRUE;
  }
  case WM_LBUTTONUP:
  case WM_MBUTTONUP: {
    if (!s || s->drag == State::Drag::None) break;
    const bool click = s->drag == State::Drag::Click && msg == WM_LBUTTONUP && !s->moved;
    s->drag          = State::Drag::None;
    ReleaseCapture();
    if (click) (GetKeyState(VK_MENU) < 0 ? RemoveLight : AddLight)(dlg, *s, MousePos(lp));
    return TRUE;
  }
  case WM_CAPTURECHANGED:
    if (s) s->drag = State::Drag::None;
    break;
  case WM_COMMAND: {
    const int id = LOWORD(wp);
    if (const int i = EditIndex(reinterpret_cast<HWND>(lp)); i >= 0) {
      if (HIWORD(wp) == EN_KILLFOCUS) CommitEdit(dlg, *s, i);
      break;
    }
    switch (id) {
    case IDOK:
      // 数値入力中の Enter はダイアログを閉じずに値を確定する
      if (const int i = EditIndex(GetFocus()); i >= 0) {
        CommitEdit(dlg, *s, i);
        SendMessageW(GetFocus(), EM_SETSEL, 0, -1);
        return TRUE;
      }
      [[fallthrough]];
    case IDCANCEL:
      SaveSize(dlg);
      EndDialog(dlg, id);
      return TRUE;
    case IDC_RESET:
      s->p     = Params{};
      s->edits = {};
      SetAll(dlg, s->p);
      RequestRefresh(dlg, *s, true);
      return TRUE;
    case IDC_RAYS_ONLY:
      s->p.raysOnly = IsDlgButtonChecked(dlg, IDC_RAYS_ONLY) == BST_CHECKED;
      RequestRefresh(dlg, *s, false);
      return TRUE;
    case IDC_FIT:
      Fit(dlg, *s);
      return TRUE;
    case IDC_THEME: // すぐ保存する（キャンセルしても残す）
      s->dark = !s->dark;
      WriteIni(L"ui", L"dark", s->dark ? 1 : 0);
      ApplyTheme(dlg, *s);
      return TRUE;
    }
    break;
  }
  }
  return FALSE;
}

} // namespace

Params LoadParams() {
  Params p;
  for (const ParamInfo& info : kParams)
    if (const auto v = ReadIni(L"params", info.key)) p.*info.field = Snap(info, *v);
  if (const auto raysOnly = ReadIni(L"params", L"raysOnly")) p.raysOnly = *raysOnly != 0;
  return p;
}

void SaveParams(const Params& p) {
  for (const ParamInfo& info : kParams)
    WriteIni(L"params", info.key, p.*info.field);
  WriteIni(L"params", L"raysOnly", p.raysOnly ? 1 : 0);
}

bool ShowDialog(HWND owner, const PixelView& image, const RgbImage& detectImage, Params& p, Edits& edits) {
  const INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES};
  InitCommonControlsEx(&icc);
  State s{.image = &image, .detectImage = &detectImage, .p = p, .edits = edits};
  if (DialogBoxParamW(Instance(), MAKEINTRESOURCEW(IDD_MAIN), owner, DialogProc, reinterpret_cast<LPARAM>(&s)) != IDOK)
    return false;
  p     = s.p;
  edits = s.edits;
  return true;
}

} // namespace hikariuni
