#include "dialog.h"

#include <algorithm>
#include <cmath>
#include <commctrl.h>
#include <cwchar>
#include <iterator>
#include <string>
#include <utility>

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

bool ReadIni(const wchar_t* section, const wchar_t* key, double& v) {
  wchar_t text[32];
  if (IniPath().empty() ||
      !GetPrivateProfileStringW(section, key, L"", text, DWORD(std::size(text)), IniPath().c_str()))
    return false;
  wchar_t* end;
  const double d = wcstod(text, &end);
  if (end == text) return false;
  v = d;
  return true;
}

void WriteIni(const wchar_t* section, const wchar_t* key, double v) {
  if (IniPath().empty()) return;
  wchar_t text[32];
  swprintf(text, std::size(text), L"%g", v);
  WritePrivateProfileStringW(section, key, text, IniPath().c_str());
}

// ---- パラメーター ----

// 範囲内に収めて刻みにそろえる
double Snap(const ParamInfo& info, double v) {
  return info.min + std::lround((std::clamp(v, info.min, info.max) - info.min) / info.step) * info.step;
}

int SliderPos(const ParamInfo& info, double v) {
  return int(std::lround((v - info.min) / info.step));
}

struct State {
  const PixelView* image;
  const RgbImage* detectImage;
  Params p;
  SIZE minSize{};             // ダイアログの最小サイズ（テンプレートの大きさ）
  RgbImage preview;           // プレビューの大きさに縮小した元画像
  std::vector<Light> lights;  // 検出結果（元画像座標）
  std::vector<uint32_t> bgra; // 光芒を合成したプレビュー（bw × bh）
  int bw = 0, bh = 0;
  bool refreshQueued = false, redetectQueued = false;
};

constexpr UINT_PTR kRefreshTimer = 1;

// ---- プレビュー ----

// プレビュー枠に収まる大きさ（拡大はしない）に元画像を縮小し直す
void ResizePreview(HWND dlg, State& s) {
  RECT rc;
  GetClientRect(GetDlgItem(dlg, IDC_PREVIEW), &rc);
  const double k = std::min({1.0, double(rc.right) / s.image->w, double(rc.bottom) / s.image->h});
  const int w = std::max(1, int(std::lround(s.image->w * k))), h = std::max(1, int(std::lround(s.image->h * k)));
  if (w != s.preview.w || h != s.preview.h) s.preview = Resample(*s.image, w, h);
}

void Recompose(State& s) {
  const int w = s.preview.w, h = s.preview.h;
  const double k    = double(w) / s.image->w;
  const double unit = std::min(s.image->w, s.image->h) / 1000.0;
  LightBuffer buf{0, 0, w, h, std::vector<float>(size_t(w) * h * 3)};
  Render(Pick(s.lights, s.p, unit), s.p, k, unit * k, buf);
  s.bgra.resize(size_t(w) * h);
  for (size_t i = 0; i < s.bgra.size(); i++) {
    auto ch = [&](int c) {
      const float base = s.p.raysOnly ? 0.f : s.preview.px[i * 3 + c]; // 光芒のみなら透明な部分を黒で見せる
      return uint32_t(std::min(255.f, base + buf.rgb[i * 3 + c] + 0.5f));
    };
    s.bgra[i] = ch(0) << 16 | ch(1) << 8 | ch(2);
  }
  s.bw = w, s.bh = h;
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
  wchar_t* end;
  const double v = wcstod(text, &end);
  SetValue(dlg, s, i, end == text ? s.p.*kParams[i].field : v);
}

void SetAll(HWND dlg, const Params& p) {
  for (int i = 0; i < kParamCount; i++) {
    const double v = p.*kParams[i].field;
    SendDlgItemMessageW(dlg, kSliderId + i, TBM_SETPOS, TRUE, SliderPos(kParams[i], v));
    SetValueText(dlg, i, v);
  }
  CheckDlgButton(dlg, IDC_RAYS_ONLY, p.raysOnly ? BST_CHECKED : BST_UNCHECKED);
}

// kParams の数だけ「名前・数値入力・スライダー」を作る。位置は Layout で決める
void CreateRows(HWND dlg) {
  const auto font = reinterpret_cast<WPARAM>(reinterpret_cast<HFONT>(SendMessageW(dlg, WM_GETFONT, 0, 0)));
  auto create     = [&](DWORD exStyle, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    HWND c = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, dlg,
                             reinterpret_cast<HMENU>(INT_PTR(id)), Instance(), nullptr);
    SendMessageW(c, WM_SETFONT, font, FALSE);
    return c;
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

// 右にパラメーター（幅固定）、下にボタン、残りをプレビューにする
void Layout(HWND dlg) {
  RECT c;
  GetClientRect(dlg, &c);
  const SIZE m = Du(dlg, 7, 7), panel = Du(dlg, 200, 22), button = Du(dlg, 56, 14), reset = Du(dlg, 70, 14);
  const SIZE edit = Du(dlg, 40, 11), label = Du(dlg, 0, 9), slider = Du(dlg, 6, 11);
  const int x = c.right - m.cx - panel.cx, bottom = c.bottom - m.cy - button.cy;
  auto move = [&](int id, int l, int t, int w, int h) { MoveWindow(GetDlgItem(dlg, id), l, t, w, h, TRUE); };
  move(IDC_PREVIEW, m.cx, m.cy, x - 2 * m.cx, bottom - 2 * m.cy);
  for (int i = 0; i < kParamCount; i++) {
    const int y = m.cy + i * panel.cy;
    move(kLabelId + i, x, y + (edit.cy - label.cy) / 2, panel.cx - edit.cx - m.cx, label.cy);
    move(kEditId + i, x + panel.cx - edit.cx, y, edit.cx, edit.cy);
    move(kSliderId + i, x - slider.cx / 2, y + edit.cy, panel.cx + slider.cx, slider.cy);
  }
  move(IDC_RESET, m.cx, bottom, reset.cx, reset.cy);
  move(IDC_RAYS_ONLY, m.cx * 2 + reset.cx, bottom, Du(dlg, 120, 0).cx, reset.cy);
  move(IDOK, c.right - m.cx - button.cx * 2 - m.cx / 2, bottom, button.cx, button.cy);
  move(IDCANCEL, c.right - m.cx - button.cx, bottom, button.cx, button.cy);
}

// 前回のウィンドウサイズに戻し、画面の中央に置く
void RestoreSize(HWND dlg, const State& s) {
  double w, h;
  if (!ReadIni(L"window", L"width", w) || !ReadIni(L"window", L"height", h)) return;
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(MonitorFromWindow(dlg, MONITOR_DEFAULTTONEAREST), &mi);
  const RECT& a = mi.rcWork;
  const int cw  = std::clamp(int(w), int(s.minSize.cx), int(a.right - a.left));
  const int ch  = std::clamp(int(h), int(s.minSize.cy), int(a.bottom - a.top));
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
    RestoreSize(dlg, *s);
    Layout(dlg);
    ResizePreview(dlg, *s);
    Refresh(dlg, *s, true);
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
      s->p = Params{};
      SetAll(dlg, s->p);
      RequestRefresh(dlg, *s, true);
      return TRUE;
    case IDC_RAYS_ONLY:
      s->p.raysOnly = IsDlgButtonChecked(dlg, IDC_RAYS_ONLY) == BST_CHECKED;
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

bool ShowDialog(HWND owner, const PixelView& image, const RgbImage& detectImage, Params& p) {
  const INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES};
  InitCommonControlsEx(&icc);
  State s{&image, &detectImage, p};
  if (DialogBoxParamW(Instance(), MAKEINTRESOURCEW(IDD_MAIN), owner, DialogProc, reinterpret_cast<LPARAM>(&s)) != IDOK)
    return false;
  p = s.p;
  return true;
}

} // namespace hikariuni
