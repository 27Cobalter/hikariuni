// effect.cpp のテスト（GoogleTest）
#include <algorithm>
#include <functional>
#include <gtest/gtest.h>
#include <ranges>
#include <vector>

#include "../hikariuni/effect.h"

using namespace hikariuni;

namespace {

// 画素を planes 個ずつインターリーブで並べた v を、ホストの画素データとして見る（8bit / 16bit）
template<typename T>
PixelView Pixels(const std::vector<T>& v, int w, int h, int planes, int colors) {
  return {.data     = v.data(),
          .rowBytes = int64_t(w) * planes * int64_t(sizeof(T)),
          .planes   = planes,
          .colors   = colors,
          .depth    = sizeof(T) == 1 ? 8 : 16,
          .w        = w,
          .h        = h};
}

// 101x101 のバッファの中央に光源 1 つを描く（shape を渡せば形のある光源）
LightBuffer RenderCenter(const Params& p, const Shape& shape = {}) {
  LightBuffer buf{.x0 = 0, .y0 = 0, .w = 101, .h = 101, .rgb = std::vector<float>(101 * 101 * 3)};
  Render(
      {
          Light{50.5, 50.5, 1, 1, 1, 1, shape}
  },
      p, 1.0, 1.0, buf);
  return buf;
}

double SumRed(const LightBuffer& b) {
  return std::ranges::fold_left(b.rgb | std::views::stride(3), 0.0, std::plus{});
}

} // namespace

// 灰色の画像に、白い 12x12 の四角（大きすぎ）・8x1 の線（細長い）・点を置くと、点だけが光源になる
TEST(Detect, KeepsOnlyPointLights) {
  RgbImage img{40, 40, std::vector<float>(40 * 40 * 3, 80.f)};
  const auto px = img.view();
  auto white    = [&](int x, int y) {
    for (int c = 0; c < 3; c++)
      px[y, x, c] = 255;
  };
  for (int y = 2; y < 14; y++)
    for (int x = 2; x < 14; x++)
      white(x, y);
  for (int x = 20; x < 28; x++)
    white(x, 35);
  white(30, 30);

  Params p;
  p.thr = 200, p.contrast = 50, p.size = 100; // 最大サイズ 100 → 40px の画像で直径 4px
  const auto lights = Detect(img, p, 1.0);
  ASSERT_EQ(lights.size(), 1u);
  EXPECT_DOUBLE_EQ(lights[0].x, 30.5);
  EXPECT_DOUBLE_EQ(lights[0].y, 30.5);
}

// 検出した光源は形（元画像座標、重みは輝度）を持つ
TEST(Detect, KeepsShape) {
  RgbImage img{40, 40, std::vector<float>(40 * 40 * 3, 80.f)};
  const auto px = img.view();
  for (int y = 10; y < 13; y++)
    for (int x = 20; x < 23; x++)
      for (int c = 0; c < 3; c++)
        px[y, x, c] = 255;
  Params p;
  p.thr = 200, p.contrast = 50, p.size = 100;
  const auto lights = Detect(img, p, 0.5); // 検出用画像は元画像の半分
  ASSERT_EQ(lights.size(), 1u);
  const Shape& sh = lights[0].shape;
  EXPECT_EQ(sh.w, 3);
  EXPECT_EQ(sh.h, 3);
  EXPECT_DOUBLE_EQ(sh.x0, 40);
  EXPECT_DOUBLE_EQ(sh.y0, 20);
  EXPECT_DOUBLE_EQ(sh.cell, 2);
  EXPECT_FLOAT_EQ(std::ranges::fold_left(sh.weight, 0.f, std::plus{}), 9 * 255.f);
}

// 大きな光源は、縁をクリックしても中心をクリックしても形が全体になる。
// kShapeCells 四方に収まるようまとめ、位置は形の重心にする
TEST(MakeLight, FillsAndCompactsLargeShape) {
  RgbImage img{200, 200, std::vector<float>(200 * 200 * 3, 10.f)};
  const auto px = img.view();
  for (int y = 25; y < 175; y++)
    for (int x = 25; x < 175; x++)
      for (int c = 0; c < 3; c++)
        px[y, x, c] = 255;
  const Light light = MakeLight(img, Params{}, 1.0, 25, 100);
  const Shape& sh   = light.shape;
  EXPECT_EQ(sh.w, 50); // 150px を 3px ずつ
  EXPECT_EQ(sh.h, 50);
  EXPECT_DOUBLE_EQ(sh.cell, 3);
  EXPECT_FLOAT_EQ(std::ranges::fold_left(sh.weight, 0.f, std::plus{}), 150 * 150 * 255.f);
  EXPECT_DOUBLE_EQ(light.x, 100);
  EXPECT_DOUBLE_EQ(light.y, 100);
  const Light center = MakeLight(img, Params{}, 1.0, 100, 100);
  EXPECT_EQ(center.shape.w, 50);
  EXPECT_DOUBLE_EQ(center.e, light.e);
}

// 手動の光源の強さは面積に比例する（大きな光源の中まで数える）
TEST(MakeLight, EnergyGrowsWithArea) {
  auto square = [](int side) {
    RgbImage img{200, 200, std::vector<float>(200 * 200 * 3, 10.f)};
    const auto px = img.view();
    for (int y = 100 - side / 2; y < 100 + side / 2; y++)
      for (int x = 100 - side / 2; x < 100 + side / 2; x++)
        for (int c = 0; c < 3; c++)
          px[y, x, c] = 250;
    return MakeLight(img, Params{}, 1.0, 100, 100).e;
  };
  EXPECT_DOUBLE_EQ(square(40), 40 * 40 * 240.0);
  EXPECT_DOUBLE_EQ(square(80), 4 * square(40));
}

// 除外した位置の近くの光源を外し、手動の光源を足す。強さは検出した最も強い光源を 1 にそろえ、
// 外しても、ほかの光源の強さは変わらない。手動の光源は kMinManual〜kMaxManual
TEST(Arrange, AppliesEdits) {
  const std::vector<Light> detected = {
      {0,   0, 4, 1, 1, 1},
      {100, 0, 2, 1, 1, 1}
  };
  Params p;
  p.gap = 0;
  const Edits edits{
      .added   = {{50, 50, 1, 1, 0, 0}, {70, 70, 0, 1, 1, 1}, {90, 90, 100, 1, 1, 1}},
      .removed = {{2, 0}}, // 光源 (0, 0) から 2px（5 単位以内）
  };
  const auto lights = Arrange(detected, p, 1.0, edits);
  ASSERT_EQ(lights.size(), 4u);
  EXPECT_DOUBLE_EQ(lights[0].x, 100);
  EXPECT_DOUBLE_EQ(lights[0].e, 0.5);
  EXPECT_DOUBLE_EQ(lights[1].x, 50);
  EXPECT_DOUBLE_EQ(lights[1].e, 0.25);
  EXPECT_DOUBLE_EQ(lights[2].e, kMinManual);
  EXPECT_DOUBLE_EQ(lights[3].e, kMaxManual);

  // 検出が 0 個なら、手動の光源のうち最も強いものが 1
  const auto manual = Arrange({}, p, 1.0, edits);
  ASSERT_EQ(manual.size(), 3u);
  EXPECT_DOUBLE_EQ(manual[0].e, kMinManual); // 1 / 100 は下限より小さい
  EXPECT_DOUBLE_EQ(manual[2].e, 1);
}

// 手動の光源の色は付近の最も明るい画素から取り、暗い場所なら白
TEST(MakeLight, TakesColorFromBrightestNearbyPixel) {
  RgbImage img{10, 10, std::vector<float>(10 * 10 * 3, 10.f)};
  const auto px = img.view();
  px[5, 6, 0] = 200, px[5, 6, 1] = 100, px[5, 6, 2] = 50;
  const Light colored = MakeLight(img, Params{}, 1.0, 5, 5);
  EXPECT_FLOAT_EQ(colored.r, 1);
  EXPECT_FLOAT_EQ(colored.g, 0.5f);
  const Light dark = MakeLight(img, Params{}, 1.0, 1, 1);
  EXPECT_FLOAT_EQ(dark.g, 1);
  EXPECT_FLOAT_EQ(dark.b, 1);
}

// 手動の光源の強さは、検出された光源の上なら検出とほぼ同じ値。明るさ未満の場所なら 0
TEST(MakeLight, MeasuresEnergyLikeDetect) {
  RgbImage img{40, 40, std::vector<float>(40 * 40 * 3, 80.f)};
  const auto px = img.view();
  for (int y = 10; y < 12; y++)
    for (int x = 10; x < 12; x++)
      for (int c = 0; c < 3; c++)
        px[y, x, c] = 255;
  px[30, 30, 0] = px[30, 30, 1] = px[30, 30, 2] = 230; // 検出の閾値より暗い点

  Params p;
  p.thr = 200, p.contrast = 160, p.size = 100;
  const auto lights = Detect(img, p, 1.0);
  ASSERT_EQ(lights.size(), 1u);
  EXPECT_NEAR(MakeLight(img, p, 1.0, 11, 11).e, lights[0].e, lights[0].e * 0.05);
  const double faint = MakeLight(img, p, 1.0, 30, 30).e; // 暗い点は弱い光源になる
  EXPECT_GT(faint, 0);
  EXPECT_LT(faint, lights[0].e);
  EXPECT_DOUBLE_EQ(MakeLight(img, p, 1.0, 20, 20).e, 0);
}

// 強い順に、最小間隔より近いものは捨て、最大個数で打ち切る
TEST(Pick, RespectsGapAndMax) {
  const std::vector<Light> lights = {
      {0,  0, 2, 1, 1, 1},
      {10, 0, 1, 1, 1, 1}
  };
  Params p;
  p.gap = 20;
  EXPECT_EQ(Pick(lights, p, 1.0).size(), 1u);
  p.gap = 5;
  EXPECT_EQ(Pick(lights, p, 1.0).size(), 2u);
  p.max = 1;
  EXPECT_EQ(Pick(lights, p, 1.0).size(), 1u);
}

// 角度 0・4 本なら水平と垂直に伸び、斜めと長さの外には描かない
TEST(Render, DrawsRaysOnlyAlongAngles) {
  Params p;
  p.rays = 4, p.angle = 0, p.len = 20, p.width = 3, p.glow = 0, p.disp = 0;
  const LightBuffer buf = RenderCenter(p);
  const auto red        = [v = buf.view()](int x, int y) { return v[y, x, 0]; };
  EXPECT_GT(red(60, 50), 0);
  EXPECT_GT(red(40, 50), 0);
  EXPECT_GT(red(50, 60), 0);
  EXPECT_EQ(red(60, 60), 0);
  EXPECT_EQ(red(75, 50), 0);
}

// 柔らかさ 1 は中心の明るさはそのままで幅 2 倍の山形になり、光の合計はほぼ同じ
TEST(Render, SoftRaysSpreadWithSimilarTotal) {
  Params p;
  p.rays = 4, p.angle = 0, p.len = 40, p.width = 10, p.glow = 0, p.disp = 0;
  p.soft                 = 0;
  const LightBuffer hard = RenderCenter(p);
  p.soft                 = 1;
  const LightBuffer soft = RenderCenter(p);
  EXPECT_NEAR(SumRed(soft) / SumRed(hard), 1, 0.15);
  const auto h = hard.view(), s = soft.view();
  EXPECT_GT((s[55, 65, 0]), 0); // くっきり（この位置では中心から 3.6px まで）では届かない位置まで広がる
  EXPECT_EQ((h[55, 65, 0]), 0);
  EXPECT_FLOAT_EQ((s[50, 65, 0]), (h[50, 65, 0])); // 中心線の明るさは同じ
}

// 縦に長い光源（1x21）: 横の光芒は光源の高さぶんの帯になり、帯の中は点光源の光芒の中心と同じ明るさ。
// 縦の光芒は光源の太さ（1px）のままで、点光源と同じ幅
TEST(Render, WideLightDrawsWideRays) {
  Params p;
  p.rays = 4, p.angle = 0, p.len = 40, p.width = 3, p.soft = 0, p.glow = 0, p.disp = 0;
  const Shape bar{.x0 = 50, .y0 = 40, .cell = 1, .w = 1, .h = 21, .weight = std::vector<float>(21, 255.f)};
  const LightBuffer point = RenderCenter(p), wide = RenderCenter(p, bar);
  const auto pv = point.view(), wv = wide.view();
  EXPECT_EQ((pv[58, 70, 0]), 0);
  EXPECT_GT((wv[58, 70, 0]), 0);
  EXPECT_GT((wv[42, 70, 0]), 0);
  EXPECT_EQ((wv[63, 70, 0]), 0);                                         // 光源の外まではほとんど広がらない
  EXPECT_NEAR((wv[50, 70, 0]), (pv[50, 70, 0]), (pv[50, 70, 0]) * 0.02); // 帯の中は点光源の中心と同じ明るさ
  EXPECT_NEAR((wv[56, 70, 0]), (wv[50, 70, 0]), (wv[50, 70, 0]) * 0.02); // 帯の中に明暗はない
  EXPECT_EQ((wv[80, 54, 0]), 0);                                         // 縦の光芒は細いまま
  EXPECT_GT((wv[80, 50, 0]), 0);
}

// 1 マスだけの形は、点光源とほぼ同じに描く（形の側は山形が 1 マスの幅でぼけるぶん、中心が 5% ほど暗い）
TEST(Render, SinglePixelShapeMatchesPoint) {
  Params p;
  p.rays = 4, p.angle = 0, p.len = 40, p.width = 6, p.soft = 0.5, p.glow = 0, p.disp = 0;
  const Shape dot{.x0 = 50, .y0 = 50, .cell = 1, .w = 1, .h = 1, .weight = {255.f}};
  const LightBuffer point = RenderCenter(p), shaped = RenderCenter(p, dot);
  EXPECT_NEAR(SumRed(shaped) / SumRed(point), 1, 0.1); // 点光源の山形は縁の分だけ半径が 0.5px 広い
  EXPECT_NEAR((shaped.view()[50, 70, 0]), (point.view()[50, 70, 0]), (point.view()[50, 70, 0]) * 0.06);
}

// 色は加算して上限で止め、透明度はそのまま
template<typename T>
void ExpectApplyClamps(T base, T maxValue) {
  const int w = 20, h = 20;
  std::vector<T> in(w * h * 4), out(w * h * 4);
  for (int i = 0; i < w * h; i++)
    in[i * 4] = in[i * 4 + 1] = in[i * 4 + 2] = base, in[i * 4 + 3] = 77;
  Params p;
  p.glow = 200, p.len = 200;
  Apply(Pixels(in, w, h, 4, 3), out.data(), int64_t(w) * 4 * sizeof(T),
        {
            Light{10.5, 10.5, 1, 1, 1, 1}
  },
        p);
  const int center = (10 * w + 10) * 4;
  EXPECT_EQ(out[center], maxValue);
  EXPECT_EQ(out[center + 3], 77);
  EXPECT_EQ(out[0], base);
}

TEST(Apply, Clamps8Bit) {
  ExpectApplyClamps<uint8_t>(250, 255);
}

TEST(Apply, Clamps16Bit) {
  ExpectApplyClamps<uint16_t>(32000, 32768);
}

// 光芒のみ: 透明度がなければ黒背景、あれば透過。透過の結果を黒の上に通常合成すると黒背景版と同じになる
TEST(Apply, RaysOnlyTransparentMatchesBlackWhenComposited) {
  const int w = 20, h = 20;
  Params p;
  p.raysOnly = true, p.glow = 200, p.len = 200;
  const std::vector<Light> light{
      {10.5, 10.5, 1, 1, 0.5f, 0.25f}
  };

  std::vector<uint8_t> rgb(w * h * 3, 200), black(w * h * 3);
  Apply(Pixels(rgb, w, h, 3, 3), black.data(), w * 3, light, p);
  std::vector<uint8_t> rgba(w * h * 4, 200), clear(w * h * 4);
  Apply(Pixels(rgba, w, h, 4, 3), clear.data(), w * 4, light, p);

  EXPECT_EQ(black[0], 0); // 光のない画素は黒
  EXPECT_EQ(clear[3], 0); // 光のない画素は透明
  EXPECT_GT(clear[(10 * w + 10) * 4 + 3], 0);
  for (int i = 0; i < w * h; i++)
    for (int c = 0; c < 3; c++)
      EXPECT_NEAR(clear[i * 4 + c] * clear[i * 4 + 3] / 255.0, black[i * 3 + c], 1.5) << "pixel " << i;
}

// 透明度が保護されているとき（alphaWritable = false）は、透明度プレーンがあっても黒背景で書き、透明度は変えない
TEST(Apply, RaysOnlyKeepsProtectedAlpha) {
  const int w = 20, h = 20;
  Params p;
  p.raysOnly = true, p.glow = 200, p.len = 200;
  std::vector<uint8_t> in(w * h * 4, 77), out(w * h * 4);
  PixelView view     = Pixels(in, w, h, 4, 3);
  view.alphaWritable = false;
  Apply(view, out.data(), w * 4,
        {
            Light{10.5, 10.5, 1, 1, 1, 1}
  },
        p);
  EXPECT_EQ(out[0], 0);
  EXPECT_EQ(out[3], 77);
  EXPECT_GT(out[(10 * w + 10) * 4], 0);
  EXPECT_EQ(out[(10 * w + 10) * 4 + 3], 77);
}

// グレースケール（色プレーン 1 つ + 透明度）: 光の RGB の平均を加算する。光芒のみなら透過
TEST(Apply, GrayAddsAverageLight) {
  const int w = 20, h = 20;
  Params p;
  p.glow = 200, p.len = 200;
  const std::vector<Light> light{
      {10.5, 10.5, 1, 1, 1, 1}
  };
  std::vector<uint8_t> in(w * h * 2), out(w * h * 2);
  for (int i = 0; i < w * h; i++)
    in[i * 2] = 100, in[i * 2 + 1] = 200;
  const PixelView view = Pixels(in, w, h, 2, 1);
  const int center     = (10 * w + 10) * 2;
  Apply(view, out.data(), w * 2, light, p);
  EXPECT_EQ(out[0], 100);
  EXPECT_GT(out[center], 100);
  EXPECT_EQ(out[center + 1], 200);
  p.raysOnly = true;
  Apply(view, out.data(), w * 2, light, p);
  EXPECT_EQ(out[1], 0);          // 光のない画素は透明
  EXPECT_GT(out[center + 1], 0); // 光源の中心は不透明度あり
}

// 行を分けて縮小しても、まとめて縮小したのと同じになる（帯ごとに並列で作るため）
TEST(Resample, RowsMatchWholeImage) {
  std::vector<uint8_t> px(37 * 23 * 3);
  for (size_t i = 0; i < px.size(); i++)
    px[i] = uint8_t(i * 7);
  const PixelView view = Pixels(px, 37, 23, 3, 3);
  const RgbImage whole = Resample(view, 15, 9);
  const RgbImage part  = ResampleRows(view, 15, 9, 4, 7);
  ASSERT_EQ(part.h, 3);
  for (int y = 4; y < 7; y++)
    for (int x = 0; x < 15; x++)
      for (int c = 0; c < 3; c++)
        EXPECT_FLOAT_EQ((part.view()[y - 4, x, c]), (whole.view()[y, x, c]));
}

// プレビューの合成は、等倍なら Apply（8bit RGB）と同じ結果になる。光芒のみなら黒の上に描く
TEST(ComposePreview, MatchesApplyAtFullSize) {
  const int w = 40, h = 30;
  std::vector<uint8_t> rgb(w * h * 3);
  for (size_t i = 0; i < rgb.size(); i++)
    rgb[i] = uint8_t(i % 200);
  const PixelView view = Pixels(rgb, w, h, 3, 3);
  const std::vector<Light> light{
      {20.5, 15.5, 1, 1, 0.5f, 0.25f}
  };
  Params p;
  p.glow = 200, p.len = 400;
  for (const bool raysOnly : {false, true}) {
    p.raysOnly = raysOnly;
    std::vector<uint8_t> applied(rgb.size());
    Apply(view, applied.data(), w * 3, light, p);
    const auto base = PreviewBase(view, w, h);
    std::vector<uint32_t> preview(base.size());
    ComposePreview(base, w, h, 1.0, std::min(w, h) / 1000.0, light, p, preview);
    for (int i = 0; i < w * h; i++) {
      EXPECT_NEAR(int(preview[i] >> 16 & 255), applied[i * 3], 1) << "pixel " << i;
      EXPECT_NEAR(int(preview[i] & 255), applied[i * 3 + 2], 1) << "pixel " << i;
    }
  }
}

// 縮小済みの RgbImage からも面積平均で縮小できる
TEST(Resample, AveragesRgbImage) {
  const RgbImage src{
      .w = 2, .h = 1, .px = {0, 0, 0, 100, 200, 50}
  };
  const RgbImage img = Resample(src, 1, 1); // view() は img の画素を指すので、img を残しておく
  const auto px      = img.view();
  EXPECT_FLOAT_EQ((px[0, 0, 0]), 50);
  EXPECT_FLOAT_EQ((px[0, 0, 1]), 100);
  EXPECT_FLOAT_EQ((px[0, 0, 2]), 25);
}

// 16bit（0〜32768）は 0〜255 に換算して縮小する
TEST(Resample, Converts16BitTo8BitScale) {
  const std::vector<uint16_t> px(4, 32768);
  const RgbImage img = Resample(Pixels(px, 2, 2, 1, 1), 1, 1);
  ASSERT_EQ(img.w, 1);
  ASSERT_EQ(img.h, 1);
  EXPECT_FLOAT_EQ(img.px[0], 255);
  EXPECT_FLOAT_EQ(img.px[2], 255);
}
