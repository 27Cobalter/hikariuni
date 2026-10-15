// effect.cpp のテスト（GoogleTest）
#include <gtest/gtest.h>
#include <vector>

#include "../hikariuni/effect.h"

using namespace hikariuni;

// 灰色の画像に、白い 12x12 の四角（大きすぎ）・8x1 の線（細長い）・点を置くと、点だけが光源になる
TEST(Detect, KeepsOnlyPointLights) {
  RgbImage img{40, 40, std::vector<float>(40 * 40 * 3, 80.f)};
  auto white = [&](int x, int y) {
    for (int c = 0; c < 3; c++)
      img.px[(y * 40 + x) * 3 + c] = 255;
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
  LightBuffer buf{0, 0, 101, 101, std::vector<float>(101 * 101 * 3)};
  const std::vector<Light> center(1, Light{50.5, 50.5, 1, 1, 1, 1});
  Render(center, p, 1.0, 1.0, buf);
  auto at = [&](int x, int y) { return buf.rgb[(y * 101 + x) * 3]; };
  EXPECT_GT(at(60, 50), 0);
  EXPECT_GT(at(40, 50), 0);
  EXPECT_GT(at(50, 60), 0);
  EXPECT_EQ(at(60, 60), 0);
  EXPECT_EQ(at(75, 50), 0);
}

// 色は加算して上限で止め、透明度はそのまま
template<typename T>
void ExpectApplyClamps(int depth, T base, T maxValue) {
  const int w = 20, h = 20;
  std::vector<T> in(w * h * 4), out(w * h * 4);
  for (int i = 0; i < w * h; i++)
    in[i * 4] = in[i * 4 + 1] = in[i * 4 + 2] = base, in[i * 4 + 3] = 77;
  const PixelView view{in.data(), int64_t(w * 4 * sizeof(T)), 4, 3, depth, w, h};
  Params p;
  p.glow = 200, p.len = 200;
  const std::vector<Light> light(1, Light{10.5, 10.5, 1, 1, 1, 1});
  Apply(view, out.data(), int64_t(w * 4 * sizeof(T)), light, p);
  const int center = (10 * w + 10) * 4;
  EXPECT_EQ(out[center], maxValue);
  EXPECT_EQ(out[center + 3], 77);
  EXPECT_EQ(out[0], base);
}

TEST(Apply, Clamps8Bit) {
  ExpectApplyClamps<uint8_t>(8, 250, 255);
}

TEST(Apply, Clamps16Bit) {
  ExpectApplyClamps<uint16_t>(16, 32000, 32768);
}

// 光芒のみ: 透明度がなければ黒背景、あれば透過。透過の結果を黒の上に通常合成すると黒背景版と同じになる
TEST(Apply, RaysOnlyTransparentMatchesBlackWhenComposited) {
  const int w = 20, h = 20;
  Params p;
  p.raysOnly = true, p.glow = 200, p.len = 200;
  const std::vector<Light> light(1, Light{10.5, 10.5, 1, 1, 0.5f, 0.25f});

  std::vector<uint8_t> rgb(w * h * 3, 200), black(w * h * 3);
  Apply({rgb.data(), w * 3, 3, 3, 8, w, h}, black.data(), w * 3, light, p);
  std::vector<uint8_t> rgba(w * h * 4, 200), clear(w * h * 4);
  Apply({rgba.data(), w * 4, 4, 3, 8, w, h}, clear.data(), w * 4, light, p);

  EXPECT_EQ(black[0], 0); // 光のない画素は黒
  EXPECT_EQ(clear[3], 0); // 光のない画素は透明
  EXPECT_GT(clear[(10 * w + 10) * 4 + 3], 0);
  for (int i = 0; i < w * h; i++)
    for (int c = 0; c < 3; c++)
      EXPECT_NEAR(clear[i * 4 + c] * clear[i * 4 + 3] / 255.0, black[i * 3 + c], 1.5) << "pixel " << i;
}

// 16bit（0〜32768）は 0〜255 に換算して縮小する
TEST(Resample, Converts16BitTo8BitScale) {
  const std::vector<uint16_t> px(4, 32768);
  const RgbImage img = Resample({px.data(), 4, 1, 1, 16, 2, 2}, 1, 1);
  ASSERT_EQ(img.w, 1);
  ASSERT_EQ(img.h, 1);
  EXPECT_FLOAT_EQ(img.px[0], 255);
  EXPECT_FLOAT_EQ(img.px[2], 255);
}
