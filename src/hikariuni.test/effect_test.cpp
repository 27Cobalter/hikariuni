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

// 101x101 のバッファの中央に光源 1 つを描く
LightBuffer RenderCenter(const Params& p) {
  LightBuffer buf{.x0 = 0, .y0 = 0, .w = 101, .h = 101, .rgb = std::vector<float>(101 * 101 * 3)};
  Render(
      {
          Light{50.5, 50.5, 1, 1, 1, 1}
  },
      p, 1.0, 1.0, buf);
  return buf;
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
  EXPECT_DOUBLE_EQ(lights[0].e, 1); // 最も強い光源の強さは 1
}

// 除外した位置の近くの光源を外し、手動の光源を足す。外しても、ほかの光源の強さは変わらない
TEST(Arrange, AppliesEdits) {
  const std::vector<Light> detected = {
      {0,   0, 1,   1, 1, 1},
      {100, 0, 0.5, 1, 1, 1}
  };
  Params p;
  p.gap = 0;
  const Edits edits{
      .added   = {{50, 50, 1, 1, 0, 0}},
      .removed = {{2, 0}}, // 光源 (0, 0) から 2px（5 単位以内）
  };
  const auto lights = Arrange(detected, p, 1.0, edits);
  ASSERT_EQ(lights.size(), 2u);
  EXPECT_DOUBLE_EQ(lights[0].x, 100);
  EXPECT_DOUBLE_EQ(lights[0].e, 0.5);
  EXPECT_DOUBLE_EQ(lights[1].x, 50);
}

// 手動の光源の色は付近の最も明るい画素から取り、暗い場所なら白
TEST(MakeLight, TakesColorFromBrightestNearbyPixel) {
  RgbImage img{10, 10, std::vector<float>(10 * 10 * 3, 10.f)};
  const auto px = img.view();
  px[5, 6, 0] = 200, px[5, 6, 1] = 100, px[5, 6, 2] = 50;
  const Light colored = MakeLight(img, 1.0, 5, 5);
  EXPECT_FLOAT_EQ(colored.r, 1);
  EXPECT_FLOAT_EQ(colored.g, 0.5f);
  EXPECT_DOUBLE_EQ(colored.e, 1);
  const Light dark = MakeLight(img, 1.0, 1, 1);
  EXPECT_FLOAT_EQ(dark.g, 1);
  EXPECT_FLOAT_EQ(dark.b, 1);
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
  const auto sumRed      = [](const LightBuffer& b) {
    return std::ranges::fold_left(b.rgb | std::views::stride(3), 0.0, std::plus{});
  };
  EXPECT_NEAR(sumRed(soft) / sumRed(hard), 1, 0.15);
  const auto h = hard.view(), s = soft.view();
  EXPECT_GT((s[55, 65, 0]), 0); // くっきり（この位置では中心から 3.6px まで）では届かない位置まで広がる
  EXPECT_EQ((h[55, 65, 0]), 0);
  EXPECT_FLOAT_EQ((s[50, 65, 0]), (h[50, 65, 0])); // 中心線の明るさは同じ
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
