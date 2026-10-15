#include "dialog.h"

#include <algorithm>
#include <cmath>
#include <commctrl.h>
#include <cwchar>
#include <iterator>
#include <string>
#include <utility>
#include <windowsx.h>

#include "resource.h"

#pragma comment(lib, "comctl32.lib")
// 今の見た目のコモンコントロール（comctl32 v6）を使う
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' "   \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' " \
                        "language='*'\"")

extern "C" IMAGE_DOS_HEADER __ImageBase; // この DLL の HINSTANCE

namespace hikariuni {
namespace {

constexpr int kParamCount = int(std::size(kParams));
constexpr int kLabelId = 2000, kEditId = 2100, kSliderId = 2200;
constexpr UINT_PTR kRefreshTimer = 1;
constexpr int kPickRadius        = 12; // 右クリックで除外するとき、光源をつかめる画面上の距離（px）

HINSTANCE Instance() {
  return reinterpret_cast<HINSTANCE>(&__ImageBase);
}

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

// 数値として読めて有限なら true（nan や inf は受け付けない）
bool ParseNumber(const wchar_t* text, double& v) {
  wchar_t* end;
  const double d = wcstod(text, &end);
  if (end == text || !std::isfinite(d)) return false;
  v = d;
  return true;
}

bool ReadIni(const wchar_t* section, const wchar_t* key, double& v) {
  wchar_t text[32];
  return !IniPath().empty() &&
         GetPrivateProfileStringW(section, key, L"", text, DWORD(std::size(text)), IniPath().c_str()) &&
         ParseNumber(text, v);
}

void WriteIni(const wchar_t* section, const wchar_t* key, double v) {
  if (IniPath().empty()) return;
  wchar_t text[32];
  swprintf(text, std::size(text), L"%g", v);
  WritePrivateProfileStringW(section, key, text, IniPath().c_str());
}

// ---- パラメーター ----

int SliderPos(const ParamInfo& info, double v) {
  return int(std::lround((v - info.min) / info.step));
}

// 範囲内に収めて刻みにそろえる
double Snap(const ParamInfo& info, double v) {
  return info.min + SliderPos(info, std::clamp(v, info.min, info.max)) * info.step;
}

// プレビューに表示している範囲。元画像の (ox, oy) が左上で、倍率 k
struct View {
  double k = 1;
  int ox = 0, oy = 0;
};

struct State {
  const PixelView* image;
  const RgbImage* detectImage;
  Params p;
  Edits edits;
  SIZE minSize{};              // ダイアログの最小サイズ（テンプレートの大きさ）
  bool zoom = false;           // 等倍表示
  double cx = 0, cy = 0;       // 等倍表示の中心（元画像座標）
  View view;                   // preview の範囲
  RgbImage preview;            // プレビューに出す範囲の元画像
  std::vector<Light> lights;   // 検出結果（元画像座標）
  std::vector<Light> arranged; // 描いている光源（手動の追加・除外を反映したもの）
  std::vector<uint32_t> bgra;  // 光芒を合成したプレビュー（bw × bh、範囲は shown）
  int bw = 0, bh = 0;
  View shown;
  bool refreshQueued = false, redetectQueued = false;
  bool dragging = false, moved = false;
  POINT dragFrom{};
};

// ---- プレビュー ----

// プレビューに出す範囲の元画像を用意する。全体表示なら枠に収まるよう縮小し（拡大はしない）、等倍表示なら中心付近を切り出す
void ResizePreview(HWND dlg, State& s) {
  RECT rc;
  GetClientRect(GetDlgItem(dlg, IDC_PREVIEW), &rc);
  const PixelView& img = *s.image;
  if (s.zoom) {
    const int w = std::clamp(int(rc.right), 1, img.w), h = std::clamp(int(rc.bottom), 1, img.h);
    s.view = {1, std::clamp(int(std::lround(s.cx - w / 2.0)), 0, img.w - w),
              std::clamp(int(std::lround(s.cy - h / 2.0)), 0, img.h - h)};
    s.cx = s.view.ox + w / 2.0, s.cy = s.view.oy + h / 2.0; // 端で止まった位置に合わせる
    PixelView crop = img;
    crop.data      = static_cast<const char*>(img.data) + s.view.oy * img.rowBytes +
                     int64_t(s.view.ox) * img.planes * (img.depth == 8 ? 1 : 2);
    crop.w = w, crop.h = h;
    s.preview = Resample(crop, w, h);
    return;
  }
  const double k = std::min({1.0, double(rc.right) / img.w, double(rc.bottom) / img.h});
  const int w = std::max(1, int(std::lround(img.w * k))), h = std::max(1, int(std::lround(img.h * k)));
  s.view = {double(w) / img.w, 0, 0};
  // 縮小は、すでにある検出用画像（長辺 2000px）からのほうが速い
  s.preview = w <= s.detectImage->w ? Resample(*s.detectImage, w, h) : Resample(img, w, h);
}

void Recompose(State& s) {
  const int w = s.preview.w, h = s.preview.h;
  const double unit = std::min(s.image->w, s.image->h) / 1000.0;
  s.arranged        = Arrange(s.lights, s.p, unit, s.edits);
  LightBuffer buf{int(std::lround(s.view.ox * s.view.k)), int(std::lround(s.view.oy * s.view.k)), w, h,
                  std::vector<float>(size_t(w) * h * 3)};
  Render(s.arranged, s.p, s.view.k, unit * s.view.k, buf);
  s.bgra.resize(size_t(w) * h);
  for (size_t i = 0; i < s.bgra.size(); i++) {
    auto ch = [&](int c) {
      const float base = s.p.raysOnly ? 0.f : s.preview.px[i * 3 + c]; // 光芒のみなら透明な部分を黒で見せる
      return uint32_t(std::min(255.f, base + buf.rgb[i * 3 + c] + 0.5f));
    };
    s.bgra[i] = ch(0) << 16 | ch(1) << 8 | ch(2);
  }
  s.bw = w, s.bh = h, s.shown = s.view;
}

void Refresh(HWND dlg, State& s, bool redetect) {
  if (redetect) s.lights = Detect(*s.detectImage, s.p, double(s.detectImage->w) / s.image->w);
  Recompose(s);
  InvalidateRect(GetDlgItem(dlg, IDC_PREVIEW), nullptr, FALSE);
}

// 重い再計算はタイマーで後回しにする。WM_TIMER は描画や入力より後に来るので、
// スライダーのつまみの描画を待たせず、連続した変更も 1 回の計算にまとまる
void RequestRefresh(HWND dlg, State& s, bool redetect) {
  s.redetectQueued |= redetect;
  if (!s.refreshQueued) s.refreshQueued = SetTimer(dlg, kRefreshTimer, 15, nullptr) != 0;
}

// プレビュー画像の位置（ダイアログのクライアント座標）
RECT ShownRect(HWND dlg, const State& s) {
  RECT rc;
  GetWindowRect(GetDlgItem(dlg, IDC_PREVIEW), &rc);
  MapWindowPoints(nullptr, dlg, reinterpret_cast<POINT*>(&rc), 2);
  const int x = rc.left + (rc.right - rc.left - s.bw) / 2, y = rc.top + (rc.bottom - rc.top - s.bh) / 2;
  return {x, y, x + s.bw, y + s.bh};
}

// ダイアログのクライアント座標を元画像座標にする。プレビュー画像の外なら false
bool ToImage(HWND dlg, const State& s, POINT pt, Point& out) {
  const RECT r = ShownRect(dlg, s);
  if (!PtInRect(&r, pt)) return false;
  out = {s.shown.ox + (pt.x - r.left + 0.5) / s.shown.k, s.shown.oy + (pt.y - r.top + 0.5) / s.shown.k};
  return true;
}

void DrawPreview(const DRAWITEMSTRUCT& di, const State& s) {
  const int w = s.bw, h = s.bh;
  const int x = di.rcItem.left + (di.rcItem.right - di.rcItem.left - w) / 2;
  const int y = di.rcItem.top + (di.rcItem.bottom - di.rcItem.top - h) / 2;
  BITMAPINFO bi{};
  bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
  if (w > 0 && h > 0) SetDIBitsToDevice(di.hDC, x, y, w, h, 0, 0, 0, h, s.bgra.data(), &bi, DIB_RGB_COLORS);
  // 画像の外側だけを塗る（全体を塗ってから画像を描くと、一瞬灰色が見えてちらつく）
  const int saved = SaveDC(di.hDC);
  ExcludeClipRect(di.hDC, x, y, x + w, y + h);
  FillRect(di.hDC, &di.rcItem, static_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH)));
  RestoreDC(di.hDC, saved);
}

// ---- 光源の手動編集 ----

// 左クリック: その位置に光源を足す
void AddLight(HWND dlg, State& s, POINT pt) {
  Point q;
  if (!ToImage(dlg, s, pt, q)) return;
  s.edits.added.push_back(MakeLight(*s.detectImage, double(s.detectImage->w) / s.image->w, q.x, q.y));
  RequestRefresh(dlg, s, false);
}

// 右クリック: 近くの光源を外す（手動で足したものは消し、自動のものは除外する）
void RemoveLight(HWND dlg, State& s, POINT pt) {
  Point q;
  if (!ToImage(dlg, s, pt, q)) return;
  const double radius = kPickRadius / s.shown.k;
  auto nearest        = [&](const std::vector<Light>& v) {
    return std::min_element(v.begin(), v.end(), [&](const Light& a, const Light& b) {
      return std::hypot(a.x - q.x, a.y - q.y) < std::hypot(b.x - q.x, b.y - q.y);
    });
  };
  auto within = [&](const Light& l) { return std::hypot(l.x - q.x, l.y - q.y) < radius; };
  auto& added = s.edits.added;
  if (auto it = nearest(added); it != added.end() && within(*it)) added.erase(it);
  else if (auto jt = nearest(s.arranged); jt != s.arranged.end() && within(*jt))
    s.edits.removed.push_back({jt->x, jt->y});
  else return;
  RequestRefresh(dlg, s, false);
}

// ---- コントロール ----

void SetValueText(HWND dlg, int i, double v) {
  wchar_t text[32];
  swprintf(text, std::size(text), kParams[i].step < 1 ? L"%.2f" : L"%.0f", v);
  SetDlgItemTextW(dlg, kEditId + i, text);
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
  wchar_t text[32];
  GetDlgItemTextW(dlg, kEditId + i, text, int(std::size(text)));
  double v = s.p.*kParams[i].field;
  ParseNumber(text, v);
  SetValue(dlg, s, i, v);
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

// 右にパラメーター（幅固定）、下にボタン、残りをプレビューにする。
// 自分で作った行には、ダイアログの今のフォントを付け直す（DPI が変わるとダイアログのフォントが変わるため）
void Layout(HWND dlg) {
  RECT c;
  GetClientRect(dlg, &c);
  const SIZE m = Du(dlg, 7, 7), panel = Du(dlg, 200, 22), button = Du(dlg, 56, 14), reset = Du(dlg, 70, 14);
  const SIZE edit = Du(dlg, 40, 11), label = Du(dlg, 0, 9), slider = Du(dlg, 6, 11);
  const SIZE raysOnly = Du(dlg, 110, 0), zoom = Du(dlg, 60, 0);
  const int x = c.right - m.cx - panel.cx, bottom = c.bottom - m.cy - button.cy;
  const auto font = WPARAM(SendMessageW(dlg, WM_GETFONT, 0, 0));
  auto move       = [&](int id, int l, int t, int w, int h) {
    HWND ctrl = GetDlgItem(dlg, id);
    if (WPARAM(SendMessageW(ctrl, WM_GETFONT, 0, 0)) != font) SendMessageW(ctrl, WM_SETFONT, font, FALSE);
    MoveWindow(ctrl, l, t, w, h, TRUE);
  };
  const int hintTop = bottom - m.cy - label.cy;
  move(IDC_PREVIEW, m.cx, m.cy, x - 2 * m.cx, hintTop - m.cy / 2 - m.cy);
  move(IDC_HINT, m.cx, hintTop, x - 2 * m.cx, label.cy);
  for (int i = 0; i < kParamCount; i++) {
    const int y = m.cy + i * panel.cy;
    move(kLabelId + i, x, y + (edit.cy - label.cy) / 2, panel.cx - edit.cx - m.cx, label.cy);
    move(kEditId + i, x + panel.cx - edit.cx, y, edit.cx, edit.cy);
    move(kSliderId + i, x - slider.cx / 2, y + edit.cy, panel.cx + slider.cx, slider.cy);
  }
  move(IDC_RESET, m.cx, bottom, reset.cx, reset.cy);
  move(IDC_RAYS_ONLY, m.cx * 2 + reset.cx, bottom, raysOnly.cx, reset.cy);
  move(IDC_ZOOM, m.cx * 3 + reset.cx + raysOnly.cx, bottom, zoom.cx, reset.cy);
  move(IDOK, c.right - m.cx - button.cx * 2 - m.cx / 2, bottom, button.cx, button.cy);
  move(IDCANCEL, c.right - m.cx - button.cx, bottom, button.cx, button.cy);
}

// 前回のウィンドウサイズに戻し、画面の中央に置く。最小サイズが画面より大きければ最小サイズのまま
void RestoreSize(HWND dlg, const State& s) {
  double w, h;
  if (!ReadIni(L"window", L"width", w) || !ReadIni(L"window", L"height", h)) return;
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(MonitorFromWindow(dlg, MONITOR_DEFAULTTONEAREST), &mi);
  const RECT& a = mi.rcWork;
  auto fit      = [](double v, LONG lo, LONG hi) { return int(std::clamp(v, double(lo), double(std::max(lo, hi)))); };
  const int cw = fit(w, s.minSize.cx, a.right - a.left), ch = fit(h, s.minSize.cy, a.bottom - a.top);
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

POINT MousePos(LPARAM lp) {
  return {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
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
    s->cx = s->image->w / 2.0, s->cy = s->image->h / 2.0;
    CreateRows(dlg);
    SetAll(dlg, s->p);
    RestoreSize(dlg, *s);
    Layout(dlg);
    ResizePreview(dlg, *s);
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
    ResizePreview(dlg, *s);
    RequestRefresh(dlg, *s, false);
    return TRUE;
  case WM_TIMER:
    if (wp != kRefreshTimer) break;
    KillTimer(dlg, kRefreshTimer);
    s->refreshQueued = false;
    Refresh(dlg, *s, std::exchange(s->redetectQueued, false));
    return TRUE;
  case WM_HSCROLL: {
    const int i = GetDlgCtrlID(reinterpret_cast<HWND>(lp)) - kSliderId;
    if (i < 0 || i >= kParamCount) break;
    const LRESULT pos = SendMessageW(reinterpret_cast<HWND>(lp), TBM_GETPOS, 0, 0);
    SetValue(dlg, *s, i, kParams[i].min + pos * kParams[i].step, true);
    return TRUE;
  }
  case WM_DRAWITEM:
    if (wp != IDC_PREVIEW) break;
    DrawPreview(*reinterpret_cast<const DRAWITEMSTRUCT*>(lp), *s);
    return TRUE;
  case WM_SETCURSOR: { // プレビュー画像の上では十字カーソル（クリックで光源を置けることを示す）
    if (!s) break;
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(dlg, &pt);
    const RECT r = ShownRect(dlg, *s);
    if (!PtInRect(&r, pt)) break;
    SetCursor(LoadCursor(nullptr, s->zoom ? IDC_SIZEALL : IDC_CROSS));
    SetWindowLongPtrW(dlg, DWLP_MSGRESULT, TRUE);
    return TRUE;
  }
  case WM_LBUTTONDOWN: {
    Point q;
    if (!s || !ToImage(dlg, *s, MousePos(lp), q)) break;
    s->dragging = true, s->moved = false, s->dragFrom = MousePos(lp);
    SetCapture(dlg);
    return TRUE;
  }
  case WM_MOUSEMOVE: { // 等倍表示中のドラッグで表示位置を動かす
    if (!s || !s->dragging) break;
    const POINT pt = MousePos(lp);
    if (!s->moved && std::abs(pt.x - s->dragFrom.x) + std::abs(pt.y - s->dragFrom.y) < 4) return TRUE;
    s->moved = true;
    if (!s->zoom) return TRUE;
    s->cx -= (pt.x - s->dragFrom.x) / s->shown.k, s->cy -= (pt.y - s->dragFrom.y) / s->shown.k;
    s->dragFrom = pt;
    ResizePreview(dlg, *s);
    RequestRefresh(dlg, *s, false);
    return TRUE;
  }
  case WM_LBUTTONUP:
    if (!s || !s->dragging) break;
    s->dragging = false;
    ReleaseCapture();
    if (!s->moved) AddLight(dlg, *s, MousePos(lp)); // 動かさずに離したらクリック
    return TRUE;
  case WM_CAPTURECHANGED:
    if (s) s->dragging = false;
    break;
  case WM_RBUTTONUP:
    if (!s) break;
    RemoveLight(dlg, *s, MousePos(lp));
    return TRUE;
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
    case IDC_ZOOM:
      s->zoom = IsDlgButtonChecked(dlg, IDC_ZOOM) == BST_CHECKED;
      ResizePreview(dlg, *s);
      RequestRefresh(dlg, *s, false);
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
  for (const ParamInfo& info : kParams) {
    double v;
    if (ReadIni(L"params", info.key, v)) p.*info.field = Snap(info, v);
  }
  double raysOnly;
  if (ReadIni(L"params", L"raysOnly", raysOnly)) p.raysOnly = raysOnly != 0;
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
  State s{&image, &detectImage, p, edits};
  if (DialogBoxParamW(Instance(), MAKEINTRESOURCEW(IDD_MAIN), owner, DialogProc, reinterpret_cast<LPARAM>(&s)) != IDOK)
    return false;
  p     = s.p;
  edits = s.edits;
  return true;
}

} // namespace hikariuni
