// 光芒エフェクトの本体（光源の検出と描画）。Windows や SDK には依存しない
#pragma once

#include <cstdint>
#include <vector>

namespace hikariuni {

// ダイアログで調整するパラメーター。既定値は docs/spec.md のパラメーター表
struct Params {
  double thr      = 160;
  double contrast = 60;
  double size     = 8;
  double max      = 60;
  double gap      = 60;
  double rays     = 4;
  double angle    = 45;
  double len      = 200;
  double width    = 3;
  double power    = 1;
  double disp     = 0.3;
  double glow     = 30;
  bool raysOnly   = false; // true なら元画像を使わず光芒だけを書き出す（透明度があれば透過、なければ黒背景）
};

struct ParamInfo {
  const wchar_t* key;   // 設定ファイルのキー
  const wchar_t* label; // ダイアログの表示名
  double min, max, step;
  double Params::* field;
  bool detect; // 変えたら光源の検出をやり直す
};

inline constexpr ParamInfo kParams[] = {
    {L"thr",      L"明るさ",                     0,   255, 1,    &Params::thr,      true },
    {L"contrast", L"コントラスト（周囲との差）", 5,   200, 1,    &Params::contrast, true },
    {L"size",     L"最大サイズ",                 1,   50,  1,    &Params::size,     true },
    {L"max",      L"最大個数",                   1,   300, 1,    &Params::max,      false},
    {L"gap",      L"最小間隔",                   0,   200, 1,    &Params::gap,      false},
    {L"rays",     L"光芒の本数",                 2,   16,  1,    &Params::rays,     false},
    {L"angle",    L"角度",                       0,   180, 1,    &Params::angle,    false},
    {L"len",      L"長さ",                       0,   600, 1,    &Params::len,      false},
    {L"width",    L"太さ",                       0.5, 20,  0.5,  &Params::width,    false},
    {L"power",    L"強さ",                       0,   2,   0.05, &Params::power,    false},
    {L"disp",     L"虹色（分散）",               0,   1,   0.05, &Params::disp,     false},
    {L"glow",     L"ハロー",                     0,   200, 1,    &Params::glow,     false},
};

// 光源。座標は元画像の px、色は最大チャンネルが 1 になるよう正規化
struct Light {
  double x, y, e;
  float r, g, b;
};

// 0〜255 基準の RGB 画像（w * h * 3）
struct RgbImage {
  int w = 0, h = 0;
  std::vector<float> px;
};

// ホストの画素データ。全プレーンをインターリーブで持つ
struct PixelView {
  const void* data;
  int64_t rowBytes;
  int planes; // 1 画素あたりのプレーン数（透明度を含む）
  int colors; // 色プレーン数（RGB は 3、グレースケールは 1）
  int depth;  // 8 または 16（16bit は Photoshop と同じ 0〜32768）
  int w, h;
};

// 光を加算するバッファ。描画先座標の (x0, y0) から w * h の範囲。値は 0〜255 基準の RGB
struct LightBuffer {
  int x0, y0, w, h;
  std::vector<float> rgb;
};

// src を dw * dh に面積平均で縮小する
RgbImage Resample(const PixelView& src, int dw, int dh);

// img（元画像を縮小したもの。scale = img.w / 元画像の幅）から光源を探し、エネルギーの降順で返す
std::vector<Light> Detect(const RgbImage& img, const Params& p, double scale);

// エネルギー順に、最小間隔を空けて最大個数まで選ぶ。unit は元画像の短辺 / 1000
std::vector<Light> Pick(const std::vector<Light>& lights, const Params& p, double unit);

// picked のハローと光芒を buf に加算する。k は元画像 px → 描画先 px、unit は描画先での短辺 / 1000
void Render(const std::vector<Light>& picked, const Params& p, double k, double unit, LightBuffer& buf);

// in に光芒を加算して out に書き出す（同じ並び・ビット深度）。透明度プレーンはそのまま。
// p.raysOnly なら光芒だけにし、透明度プレーンがあれば光の強さを不透明度にして透過、なければ黒背景にする
void Apply(const PixelView& in, void* out, int64_t outRowBytes, const std::vector<Light>& picked, const Params& p);

} // namespace hikariuni
