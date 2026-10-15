// パラメーター調整ダイアログと、前回の設定の保存
#pragma once

#include <windows.h>

#include "effect.h"

namespace hikariuni {

// 前回 OK したときのパラメーター（%APPDATA%\Hikariuni\hikariuni.ini）。なければ既定値
Params LoadParams();
void SaveParams(const Params& p);

// プレビュー付きのダイアログを出す。OK なら p と edits（光源の手動の追加・除外）を更新して true
bool ShowDialog(HWND owner, const PixelView& image, const RgbImage& detectImage, Params& p, Edits& edits);

} // namespace hikariuni
