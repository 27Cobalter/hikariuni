// Hikariuni: 光源を見つけて光芒を描く Photoshop 互換フィルター（.8bf）。ホストとのやり取りだけをここに書く
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <windows.h>

#include <PIDefines.h>
#include <PIFilter.h>

#include "dialog.h"
#include "effect.h"

using namespace hikariuni;

static bool Use32(FilterRecord* fr) {
  return fr->bigDocumentData && fr->bigDocumentData->PluginUsing32BitCoordinates;
}

static VRect FilterRect(FilterRecord* fr) {
  if (Use32(fr)) return fr->bigDocumentData->filterRect32;
  const Rect& r = fr->filterRect;
  return {r.top, r.left, r.bottom, r.right};
}

static void SetRects(FilterRecord* fr, const VRect& v) {
  if (Use32(fr)) {
    fr->bigDocumentData->inRect32 = fr->bigDocumentData->outRect32 = v;
    return;
  }
  fr->inRect = fr->outRect = Rect{int16(v.top), int16(v.left), int16(v.bottom), int16(v.right)};
}

static HWND Owner(FilterRecord* fr) {
  auto pd = static_cast<PlatformData*>(fr->platformData);
  return pd ? reinterpret_cast<HWND>(pd->hwnd) : nullptr;
}

static void DoStart(FilterRecord* fr, int16* result) {
  if (fr->depth != 8 && fr->depth != 16) {
    *result = filterBadMode;
    return;
  }
  const bool gray = fr->imageMode == plugInModeGrayScale || fr->imageMode == plugInModeGray16;
  const VRect r   = FilterRect(fr);
  if (r.right <= r.left || r.bottom <= r.top) return; // 処理する範囲がない

  // ponytail: 画像全体を一度に要求。巨大画像でホストのメモリが足りないならタイル分割する
  SetRects(fr, r);
  fr->inLoPlane = fr->outLoPlane = 0;
  fr->inHiPlane = fr->outHiPlane = int16(fr->planes - 1);
  if ((*result = fr->advanceState()) != noErr) return;

  // 透明度が保護されている（ホストが元の透明度を残す）ときは、透明度を書き換えない
  const bool alphaWritable = fr->filterCase != filterCaseProtectedTransparencyNoSelection &&
                             fr->filterCase != filterCaseProtectedTransparencyWithSelection;
  const PixelView in{fr->inData, fr->inRowBytes,   fr->planes,       gray ? 1 : 3,
                     fr->depth,  r.right - r.left, r.bottom - r.top, alphaWritable};
  // 光源の検出は長辺 2000px に縮小して行う
  const double ds    = std::min(1.0, 2000.0 / std::max(in.w, in.h));
  const RgbImage det = Resample(in, std::max(1, int(std::lround(in.w * ds))), std::max(1, int(std::lround(in.h * ds))));

  Params p = LoadParams();
  Edits edits;
  if (!ShowDialog(Owner(fr), in, det, p, edits)) {
    *result = userCanceledErr;
    SetRects(fr, VRect{});
    return;
  }
  SaveParams(p);

  const auto lights = Arrange(Detect(det, p, double(det.w) / in.w), p, std::min(in.w, in.h) / 1000.0, edits);
  Apply(in, fr->outData, fr->outRowBytes, lights, p);
  SetRects(fr, VRect{}); // 空の矩形 = もう要求しない
}

DLLExport MACPASCAL void PluginMain(const int16 selector, FilterRecordPtr fr, intptr_t* /*data*/, int16* result) {
  *result = noErr;
  if (selector == filterSelectorAbout) { // About のときだけ fr は AboutRecord なので触らない
    MessageBoxW(nullptr, L"Hikariuni\n光源を見つけて光芒を描くフィルター", L"Hikariuni", MB_OK);
    return;
  }
  if (fr->bigDocumentData) fr->bigDocumentData->PluginUsing32BitCoordinates = true;
  if (selector == filterSelectorStart) DoStart(fr, result);
  else if (selector == filterSelectorContinue) SetRects(fr, VRect{});
}
