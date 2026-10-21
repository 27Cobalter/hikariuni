// 光芒エフェクトの本体（光源の検出と描画）。Windows や SDK には依存しない
#pragma once

#include <array>
#include <cstdint>
#include <mdspan>
#include <span>
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
  double soft     = 0.5;
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

inline constexpr auto kParams = std::to_array<ParamInfo>({
    {L"thr",      L"明るさ",                     0,   255, 1,    &Params::thr,      true },
    {L"contrast", L"コントラスト（周囲との差）", 5,   200, 1,    &Params::contrast, true },
    {L"size",     L"最大サイズ",                 1,   50,  1,    &Params::size,     true },
    {L"max",      L"最大個数",                   1,   300, 1,    &Params::max,      false},
    {L"gap",      L"最小間隔",                   0,   200, 1,    &Params::gap,      false},
    {L"rays",     L"光芒の本数",                 2,   16,  1,    &Params::rays,     false},
    {L"angle",    L"角度",                       0,   180, 1,    &Params::angle,    false},
    {L"len",      L"長さ",                       0,   600, 1,    &Params::len,      false},
    {L"width",    L"太さ",                       0.5, 20,  0.5,  &Params::width,    false},
    {L"soft",     L"柔らかさ",                   0,   1,   0.05, &Params::soft,     false},
    {L"power",    L"強さ",                       0,   2,   0.05, &Params::power,    false},
    {L"disp",     L"虹色（分散）",               0,   1,   0.05, &Params::disp,     false},
    {L"glow",     L"ハロー",                     0,   200, 1,    &Params::glow,     false},
});

// 光源の形（元画像座標）。(x0, y0) を左上に cell 四方のマスを w * h 個並べ、マスごとに明るさの重みを持つ。
// weight が空なら点光源
struct Shape {
  double x0 = 0, y0 = 0, cell = 0;
  int w = 0, h = 0;
  std::vector<float> weight; // w * h
};

// 形を持つときに 1 辺に並べるマスの数の上限（大きな光源は数画素ずつまとめる）
inline constexpr int kShapeCells = 64;

// 光源。座標は元画像の px。e は強さ（Detect / MakeLight ではエネルギー = コントラストの合計、
// Arrange 後は検出した最も強い光源を 1 とした値）。色は最大チャンネルが 1 になるよう正規化。
// shape があれば、光芒はその形を光芒と直角の向きに投影した幅を持つ
struct Light {
  double x, y, e;
  float r, g, b;
  Shape shape = {};
};

// 手動の編集（元画像座標）。added は追加した光源、removed は除外した位置
struct Point {
  double x, y;
};
struct Edits {
  std::vector<Light> added;
  std::vector<Point> removed;
};

// 0〜255 基準の RGB 画像。view()[y, x, c] で読み書きする
struct RgbImage {
  int w = 0, h = 0;
  std::vector<float> px; // w * h * 3

  auto view(this auto& self) {
    return std::mdspan(self.px.data(), self.h, self.w, 3);
  }
};

// ホストの画素データ。全プレーンをインターリーブで持つ
struct PixelView {
  const void* data;
  int64_t rowBytes;
  int planes; // 1 画素あたりのプレーン数（透明度を含む）
  int colors; // 色プレーン数（RGB は 3、グレースケールは 1）
  int depth;  // 8 または 16（16bit は Photoshop と同じ 0〜32768）
  int w, h;
  bool alphaWritable = true; // 透明度プレーンを書き換えてよいか（ホストが透明度を保護しているときは false）
};

// 光を加算するバッファ。描画先座標の (x0, y0) から w * h の範囲。値は 0〜255 基準の RGB。
// view()[y - y0, x - x0, c] で読み書きする
struct LightBuffer {
  int x0, y0, w, h;
  std::vector<float> rgb; // w * h * 3

  auto view(this auto& self) {
    return std::mdspan(self.rgb.data(), self.h, self.w, 3);
  }
};

// src を dw * dh に面積平均で縮小する
[[nodiscard]] RgbImage Resample(const PixelView& src, int dw, int dh);
[[nodiscard]] RgbImage Resample(const RgbImage& src, int dw, int dh);
// Resample(src, dw, dh) の行 [y0, y1) だけを作る（結果の h は y1 - y0）
[[nodiscard]] RgbImage ResampleRows(const PixelView& src, int dw, int dh, int y0, int y1);

// プレビュー用: 元画像を w * h に縮小した 8bit の画素（0x00RRGGBB）
[[nodiscard]] std::vector<uint32_t> PreviewBase(const PixelView& image, int w, int h);

// プレビュー用: base（PreviewBase で倍率 k に縮小したもの、w * h）に光芒を加算して out に書く。
// p.raysOnly なら黒に加算する。unit は元画像の短辺 / 1000
void ComposePreview(std::span<const uint32_t> base, int w, int h, double k, double unit,
                    const std::vector<Light>& lights, const Params& p, std::span<uint32_t> out);

// img（元画像を縮小したもの。scale = img.w / 元画像の幅）から光源を探し、強い順に返す
[[nodiscard]] std::vector<Light> Detect(const RgbImage& img, const Params& p, double scale);

// 強い順に、最小間隔を空けて最大個数まで選ぶ。unit は元画像の短辺 / 1000
[[nodiscard]] std::vector<Light> Pick(const std::vector<Light>& lights, const Params& p, double unit);

// 手動の光源の強さの下限と上限（Arrange 後の値。上限では光芒の長さとハローが検出した最も強い光源の 2 倍）
inline constexpr double kMinManual = 0.05;
inline constexpr double kMaxManual = 4;

// 描く光源を決める。Pick したうえで、除外した位置の近く（5 単位以内）の光源を外し、手動の光源を足す。
// 強さは detected のうち最も強いもの（detected が空なら手動の光源のうち最も強いもの）を 1 にそろえる
// （手動の光源は kMinManual〜kMaxManual）
[[nodiscard]] std::vector<Light> Arrange(const std::vector<Light>& detected, const Params& p, double unit,
                                         const Edits& edits);

// (x, y)（元画像座標）に手動で置く光源。検出用画像のその付近で最も明るい画素から、輝度で領域をたどって
// エネルギー（すぐ外側との輝度の差の合計）と形を求める。形が取れたら位置は形の重心にする。
// 色はその画素から取る（暗ければ白）
[[nodiscard]] Light MakeLight(const RgbImage& img, const Params& p, double scale, double x, double y);

// lights のハローと光芒を buf に加算する。k は元画像 px → 描画先 px、unit は描画先での短辺 / 1000
void Render(const std::vector<Light>& lights, const Params& p, double k, double unit, LightBuffer& buf);

// in に光芒を加算して out に書き出す（同じ並び・ビット深度）。透明度プレーンはそのまま。
// p.raysOnly なら光芒だけにし、透明度を書き換えられれば光の強さを不透明度にして透過、できなければ黒背景にする
void Apply(const PixelView& in, void* out, int64_t outRowBytes, const std::vector<Light>& lights, const Params& p);

} // namespace hikariuni
