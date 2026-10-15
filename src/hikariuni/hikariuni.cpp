// Hikariuni: 光源に光芒を描く Photoshop 互換フィルター（.8bf）。
// 現段階は Affinity に読み込まれるかの確認用で、色を反転するだけ。
#include <cstdint>
#include <windows.h>

#include <PIDefines.h>
#include <PIFilter.h>

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

// 全プレーンをインターリーブで受け取り、色プレーンだけ反転（透明度はそのまま）
template<typename T>
static void Invert(FilterRecord* fr, int32 w, int32 h, int colors, T max) {
  const int planes = fr->planes;
  for (int32 y = 0; y < h; y++) {
    auto in  = reinterpret_cast<const T*>(static_cast<const char*>(fr->inData) + intptr_t(y) * fr->inRowBytes);
    auto out = reinterpret_cast<T*>(static_cast<char*>(fr->outData) + intptr_t(y) * fr->outRowBytes);
    for (int32 i = 0; i < w * planes; i++)
      out[i] = i % planes < colors ? T(max - in[i]) : in[i];
  }
}

static void DoStart(FilterRecord* fr, int16* result) {
  if (fr->depth != 8 && fr->depth != 16) {
    *result = filterBadMode;
    return;
  }
  const bool gray = fr->imageMode == plugInModeGrayScale || fr->imageMode == plugInModeGray16;
  const VRect r   = FilterRect(fr);

  // ponytail: 画像全体を一度に要求。巨大画像でホストのメモリが足りないならタイル分割する
  SetRects(fr, r);
  fr->inLoPlane = fr->outLoPlane = 0;
  fr->inHiPlane = fr->outHiPlane = int16(fr->planes - 1);
  if ((*result = fr->advanceState()) != noErr) return;

  const int32 w = r.right - r.left, h = r.bottom - r.top;
  if (fr->depth == 8) Invert<uint8_t>(fr, w, h, gray ? 1 : 3, 255);
  else Invert<uint16_t>(fr, w, h, gray ? 1 : 3, 32768); // Photoshop の 16bit は 0〜32768

  SetRects(fr, VRect{}); // 空の矩形 = もう要求しない
}

DLLExport MACPASCAL void PluginMain(const int16 selector, FilterRecordPtr fr, intptr_t* /*data*/, int16* result) {
  *result = noErr;
  if (selector == filterSelectorAbout) { // About のときだけ fr は AboutRecord なので触らない
    MessageBoxW(nullptr, L"Hikariuni\n光源に光芒を描くフィルター", L"Hikariuni", MB_OK);
    return;
  }
  if (fr->bigDocumentData) fr->bigDocumentData->PluginUsing32BitCoordinates = true;
  if (selector == filterSelectorStart) DoStart(fr, result);
  else if (selector == filterSelectorContinue) SetRects(fr, VRect{});
}
