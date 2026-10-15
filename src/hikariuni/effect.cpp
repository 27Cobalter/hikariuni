#include "effect.h"

#include <algorithm>
#include <cmath>
#include <execution>
#include <functional>
#include <numbers>
#include <optional>

namespace hikariuni {
namespace {

using Rgb = std::array<double, 3>;

// [0, h) を rows 行ずつの帯に分け、帯ごとに fn(y0, y1) を並列に呼ぶ（帯どうしは同じ場所に書かないこと）
template<typename F>
void ForEachStrip(int h, int rows, F&& fn) {
  std::vector<int> starts;
  for (int y = 0; y < h; y += rows)
    starts.push_back(y);
  std::for_each(std::execution::par, starts.begin(), starts.end(), [&](int y0) { fn(y0, std::min(h, y0 + rows)); });
}

// dw × dh への面積平均の縮小のうち、行 [y0, y1) だけを作る
template<typename T>
RgbImage ResampleT(const PixelView& s, int dw, int dh, int y0, int y1, double toByte) {
  RgbImage out{dw, y1 - y0, std::vector<float>(size_t(dw) * (y1 - y0) * 3)};
  const auto dst = out.view();
  for (int y = y0; y < y1; y++) {
    const int sy0 = int(int64_t(y) * s.h / dh), sy1 = std::max(sy0 + 1, int(int64_t(y + 1) * s.h / dh));
    for (int x = 0; x < dw; x++) {
      const int sx0 = int(int64_t(x) * s.w / dw), sx1 = std::max(sx0 + 1, int(int64_t(x + 1) * s.w / dw));
      Rgb sum{};
      for (int yy = sy0; yy < sy1; yy++) {
        auto row = reinterpret_cast<const T*>(static_cast<const char*>(s.data) + yy * s.rowBytes);
        for (int xx = sx0; xx < sx1; xx++)
          for (int c = 0; c < 3; c++)
            sum[c] += row[xx * s.planes + (s.colors == 1 ? 0 : c)];
      }
      const double n = double(sy1 - sy0) * (sx1 - sx0);
      for (int c = 0; c < 3; c++)
        dst[y - y0, x, c] = float(sum[c] / n * toByte);
    }
  }
  return out;
}

uint32_t Pack(float r, float g, float b) {
  auto byte = [](float v) { return uint32_t(std::clamp(v + 0.5f, 0.f, 255.f)); };
  return byte(r) << 16 | byte(g) << 8 | byte(b);
}

// バッファの中の矩形 [x0, x1) × [y0, y1)
struct Box {
  int x0, y0, x1, y1;
};

// (x0, y0)-(x1, y1) をバッファの範囲に切り詰める。空なら nullopt
std::optional<Box> Clip(const LightBuffer& b, double x0, double y0, double x1, double y1) {
  const Box box{std::max(b.x0, int(std::floor(x0))), std::max(b.y0, int(std::floor(y0))),
                std::min(b.x0 + b.w, int(std::ceil(x1)) + 1), std::min(b.y0 + b.h, int(std::ceil(y1)) + 1)};
  if (box.x0 >= box.x1 || box.y0 >= box.y1) return std::nullopt;
  return box;
}

// 中心から外へ線形に消える円。中心の明るさは value
void Halo(LightBuffer& b, double x, double y, double r, const Rgb& value) {
  const auto box = Clip(b, x - r, y - r, x + r, y + r);
  if (!box) return;
  const auto v = b.view();
  for (int py = box->y0; py < box->y1; py++)
    for (int px = box->x0; px < box->x1; px++) {
      const double d = std::hypot(px + 0.5 - x, py + 0.5 - y);
      if (d >= r) continue;
      for (int ch = 0; ch < 3; ch++)
        v[py - b.y0, px - b.x0, ch] += float(value[ch] * (1 - d / r));
    }
}

// lo..hi を「a <= v * dx + c <= b を満たす dx」で絞る（v がほぼ 0 なら c だけで判定）。空なら false
bool Narrow(double& lo, double& hi, double v, double c, double a, double b) {
  if (std::abs(v) < 1e-9) return a <= c && c <= b;
  const auto [p, q] = std::minmax({(a - c) / v, (b - c) / v}); // 初期化子リスト版は値で返す（参照がぶら下がらない）
  lo                = std::max(lo, p);
  hi                = std::min(hi, q);
  return lo <= hi;
}

// 光芒 1 本。方向 (cs, sn)、中心の幅 width で先端に向かって細くなり、RGB ごとに長さ len[ch]（虹色のにじみ）。
// 長さ方向の明るさは value → 0.4 * value（25% 地点）→ 0。
// 幅方向は soft = 0 で縁がくっきり（1px のアンチエイリアス）、1 で幅 2 倍の山形（明るさの合計はほぼ同じ）。
// 外接矩形全体ではなく、各行で光芒が通る範囲だけを調べる
void Ray(LightBuffer& b, double x, double y, double cs, double sn, const Rgb& len, double width, double soft,
         const Rgb& value) {
  const double L = std::ranges::max(len);
  if (L < 1) return;
  const double hw = width / 2, reach = hw * (1 + soft) + 1; // 中心線から光が届く最大の距離
  const auto box = Clip(b, std::min(x, x + cs * L) - reach, std::min(y, y + sn * L) - reach,
                        std::max(x, x + cs * L) + reach, std::max(y, y + sn * L) + reach);
  if (!box) return;
  const auto v = b.view();
  for (int py = box->y0; py < box->y1; py++) {
    const double dy = py + 0.5 - y;
    double lo = box->x0 + 0.5 - x, hi = box->x1 - 0.5 - x; // この行で調べる dx の範囲
    if (!Narrow(lo, hi, -sn, dy * cs, -reach, reach) || !Narrow(lo, hi, cs, dy * sn, -0.5, L)) continue;
    const int px0 = std::max(box->x0, int(std::floor(x + lo - 0.5)));
    const int px1 = std::min(box->x1, int(std::ceil(x + hi - 0.5)) + 1);
    for (int px = px0; px < px1; px++) {
      const double dx = px + 0.5 - x;
      const double t = dx * cs + dy * sn, s = std::abs(dy * cs - dx * sn);
      if (t < -0.5) continue;
      for (int ch = 0; ch < 3; ch++) {
        if (len[ch] < 1 || t >= len[ch]) continue;
        const double u = std::max(0.0, t) / len[ch], hwt = hw * (1 - u);
        const double hard = std::clamp(hwt + 0.5 - s, 0.0, 1.0);
        const double tent = std::clamp(1 - s / (hwt * (1 + soft) + 0.5), 0.0, 1.0);
        double cover      = (hard + (tent - hard) * soft) * std::clamp(t + 0.5, 0.0, 1.0);
        if (hwt < 0.5) cover *= 2 * hwt; // 1px より細い部分は幅に比例して暗くする
        if (cover <= 0) continue;
        const double g = u < 0.25 ? 1 - 2.4 * u : 0.4 * (1 - u) / 0.75;
        v[py - b.y0, px - b.x0, ch] += float(value[ch] * g * cover);
      }
    }
  }
}

template<typename T>
void ApplyT(const PixelView& in, void* out, int64_t outRowBytes, const std::vector<Light>& lights, const Params& p,
            float maxValue) {
  const float scale = maxValue / 255;
  const double unit = std::min(in.w, in.h) / 1000.0;
  const bool alpha  = in.alphaWritable && in.planes > in.colors; // 色の次のプレーンが透明度
  // メモリを抑えるため 64 行ずつの帯に分けて描き、帯は並列に処理する
  ForEachStrip(in.h, 64, [&](int y0, int y1) {
    LightBuffer buf{0, y0, in.w, y1 - y0, std::vector<float>(size_t(in.w) * (y1 - y0) * 3)};
    Render(lights, p, 1.0, unit, buf);
    const auto light = buf.view();
    for (int y = 0; y < buf.h; y++) {
      auto srcRow = reinterpret_cast<const T*>(static_cast<const char*>(in.data) + (y0 + y) * in.rowBytes);
      auto dstRow = reinterpret_cast<T*>(static_cast<char*>(out) + (y0 + y) * outRowBytes);
      for (int x = 0; x < in.w; x++) {
        const T* src = srcRow + x * in.planes;
        T* dst       = dstRow + x * in.planes;
        std::array add{light[y, x, 0] * scale, light[y, x, 1] * scale, light[y, x, 2] * scale}; // ホストの値の範囲
        if (in.colors == 1) add[0] = (add[0] + add[1] + add[2]) / 3;
        const auto colors = std::span(add).first(in.colors);
        int c             = 0;
        if (!p.raysOnly) {
          for (; c < in.colors; c++)
            dst[c] = T(std::min(maxValue, src[c] + add[c] + 0.5f));
        } else if (alpha) {
          // 透過: 不透明度 = 光の強さ、色 = 光 / 不透明度（通常の合成で 色 × 不透明度 = 光になる）
          const float a = std::min(maxValue, std::ranges::max(colors));
          for (; c < in.colors; c++)
            dst[c] = a > 0 ? T(std::min(maxValue, add[c] * maxValue / a + 0.5f)) : T(0);
          dst[c++] = T(a + 0.5f);
        } else {
          for (; c < in.colors; c++)
            dst[c] = T(std::min(maxValue, add[c] + 0.5f)); // 透明度がなければ黒背景
        }
        for (; c < in.planes; c++)
          dst[c] = src[c];
      }
    }
  });
}

} // namespace

RgbImage ResampleRows(const PixelView& src, int dw, int dh, int y0, int y1) {
  return src.depth == 8 ? ResampleT<uint8_t>(src, dw, dh, y0, y1, 1.0)
                        : ResampleT<uint16_t>(src, dw, dh, y0, y1, 255.0 / 32768);
}

RgbImage Resample(const PixelView& src, int dw, int dh) {
  return ResampleRows(src, dw, dh, 0, dh);
}

std::vector<uint32_t> PreviewBase(const PixelView& image, int w, int h) {
  std::vector<uint32_t> out(size_t(w) * h);
  ForEachStrip(h, 64, [&](int y0, int y1) {
    const RgbImage part = ResampleRows(image, w, h, y0, y1);
    const auto px       = part.view();
    for (int y = y0; y < y1; y++)
      for (int x = 0; x < w; x++)
        out[size_t(y) * w + x] = Pack(px[y - y0, x, 0], px[y - y0, x, 1], px[y - y0, x, 2]);
  });
  return out;
}

void ComposePreview(std::span<const uint32_t> base, int w, int h, double k, double unit,
                    const std::vector<Light>& lights, const Params& p, std::span<uint32_t> out) {
  ForEachStrip(h, 64, [&](int y0, int y1) {
    LightBuffer buf{0, y0, w, y1 - y0, std::vector<float>(size_t(w) * (y1 - y0) * 3)};
    Render(lights, p, k, unit * k, buf);
    const auto light = buf.view();
    for (int y = y0; y < y1; y++)
      for (int x = 0; x < w; x++) {
        const size_t i   = size_t(y) * w + x;
        const uint32_t b = p.raysOnly ? 0 : base[i]; // 光芒のみなら透明な部分を黒で見せる
        out[i]           = Pack(float(b >> 16 & 255) + light[y - y0, x, 0], float(b >> 8 & 255) + light[y - y0, x, 1],
                                float(b & 255) + light[y - y0, x, 2]);
      }
  });
}

RgbImage Resample(const RgbImage& src, int dw, int dh) {
  const PixelView view{.data     = src.px.data(),
                       .rowBytes = int64_t(src.w) * 3 * int64_t(sizeof(float)),
                       .planes   = 3,
                       .colors   = 3,
                       .depth    = 0, // ResampleT<float> では使わない
                       .w        = src.w,
                       .h        = src.h};
  return ResampleT<float>(view, dw, dh, 0, dh, 1.0);
}

namespace {

// 輝度と局所コントラスト（輝度 - 周囲（半径 R の箱）の平均。明るさ未満の画素は 0）。maxD は最大サイズ（px）
struct ContrastMap {
  std::vector<float> lum, con;
  double maxD;
  int R;
};

ContrastMap Contrast(const RgbImage& img, const Params& p) {
  const int w = img.w, h = img.h, W = w + 1;
  const size_t n = size_t(w) * h;
  const auto px  = img.view();
  ContrastMap m{std::vector<float>(n), std::vector<float>(n), p.size * std::min(w, h) / 1000, 0};
  std::vector<double> integral(size_t(W) * (h + 1));
  for (int y = 0; y < h; y++) {
    double row = 0;
    for (int x = 0; x < w; x++) {
      const size_t i = size_t(y) * w + x;
      m.lum[i]       = 0.2126f * px[y, x, 0] + 0.7152f * px[y, x, 1] + 0.0722f * px[y, x, 2];
      row += m.lum[i];
      integral[size_t(y + 1) * W + x + 1] = integral[size_t(y) * W + x + 1] + row;
    }
  }
  m.R = std::max(3, int(std::lround(m.maxD * 1.5))); // 背景を測る範囲は光源より十分広く取る
  for (int y = 0; y < h; y++) {
    const int y0 = std::max(0, y - m.R), y1 = std::min(h, y + m.R + 1);
    for (int x = 0; x < w; x++) {
      const int x0 = std::max(0, x - m.R), x1 = std::min(w, x + m.R + 1);
      const size_t i    = size_t(y) * w + x;
      const double mean = (integral[size_t(y1) * W + x1] - integral[size_t(y0) * W + x1] -
                           integral[size_t(y1) * W + x0] + integral[size_t(y0) * W + x0]) /
                          (double(x1 - x0) * (y1 - y0));
      m.con[i]          = m.lum[i] >= p.thr ? float(m.lum[i] - mean) : 0.f;
    }
  }
  return m;
}

// seed から、con が threshold 以上の画素を 4 近傍でたどり、各画素の添字で fn を呼ぶ。たどった画素は seen に印を付ける
template<typename F>
void Flood(const std::vector<float>& con, int w, int h, int seed, double threshold, std::vector<uint8_t>& seen,
           F&& fn) {
  std::vector<int> stack{seed};
  seen[seed] = 1;
  while (!stack.empty()) {
    const int q = stack.back();
    stack.pop_back();
    fn(q);
    const int x = q % w, y = q / w;
    auto visit = [&](int v) {
      if (!seen[v] && con[v] >= threshold) seen[v] = 1, stack.push_back(v);
    };
    if (x > 0) visit(q - 1);
    if (x < w - 1) visit(q + 1);
    if (y > 0) visit(q - w);
    if (y < h - 1) visit(q + w);
  }
}

} // namespace

std::vector<Light> Detect(const RgbImage& img, const Params& p, double scale) {
  const int w = img.w, h = img.h;
  const size_t n                 = size_t(w) * h;
  const auto px                  = img.view();
  const auto [lum, con, maxD, R] = Contrast(img, p);

  // コントラストの高い画素を 4 近傍でつないで光源候補にし、それっぽくないものを捨てる
  std::vector<uint8_t> seen(n);
  std::vector<Light> out;
  for (size_t start = 0; start < n; start++) {
    if (seen[start] || con[start] < p.contrast) continue;
    double cnt = 0, sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0, e = 0, sl = 0;
    Rgb color{};
    Flood(con, w, h, int(start), p.contrast, seen, [&](int q) {
      const int x = q % w, y = q / w;
      cnt++, sx += x, sy += y, sxx += double(x) * x, syy += double(y) * y, sxy += double(x) * y;
      for (int c = 0; c < 3; c++)
        color[c] += px[y, x, c];
      e += con[q], sl += lum[q];
    });
    const double mx = sx / cnt, my = sy / cnt;
    const double vx = sxx / cnt - mx * mx + 1.0 / 12, vy = syy / cnt - my * my + 1.0 / 12, cv = sxy / cnt - mx * my;
    const double t = (vx + vy) / 2, d = std::hypot((vx - vy) / 2, cv);                   // 共分散の固有値 t ± d
    if (2 * std::sqrt(cnt / std::numbers::pi) > maxD || (t + d) / (t - d) > 9) continue; // 大きすぎる / 縦横比 3 超

    // 周囲に同じくらい明るい画素が多い = 花びらの先端など、大きな物体の一部
    const double level = 0.85 * sl / cnt;
    const int cx = int(std::lround(mx)), cy = int(std::lround(my));
    double bright = -cnt, area = 0;
    for (int y = std::max(0, cy - R); y <= std::min(h - 1, cy + R); y++)
      for (int x = std::max(0, cx - R); x <= std::min(w - 1, cx + R); x++) {
        area++;
        if (lum[size_t(y) * w + x] >= level) bright++;
      }
    if (bright > area * 0.1) continue;

    const double m = std::ranges::max(color); // 色相を保ったまま最大チャンネルを 1 に
    out.push_back(
        {(mx + 0.5) / scale, (my + 0.5) / scale, e, float(color[0] / m), float(color[1] / m), float(color[2] / m)});
  }
  std::ranges::stable_sort(out, std::greater{}, &Light::e);
  return out;
}

std::vector<Light> Pick(const std::vector<Light>& lights, const Params& p, double unit) {
  std::vector<Light> out;
  const double gap = p.gap * unit;
  for (const Light& s : lights) {
    if (out.size() >= size_t(p.max)) break;
    if (std::ranges::all_of(out, [&](const Light& o) { return std::hypot(o.x - s.x, o.y - s.y) >= gap; }))
      out.push_back(s);
  }
  return out;
}

std::vector<Light> Arrange(const std::vector<Light>& detected, const Params& p, double unit, const Edits& edits) {
  std::vector<Light> out = Pick(detected, p, unit);
  std::erase_if(out, [&](const Light& l) {
    return std::ranges::any_of(edits.removed,
                               [&](const Point& q) { return std::hypot(l.x - q.x, l.y - q.y) < 5 * unit; });
  });
  const size_t picked = out.size();
  out.append_range(edits.added);
  // 強さは検出した全候補のうち最も強いものを 1 にそろえる。どれを除外しても、ほかの光源の大きさが変わらないように。
  // 手動の光源は、コントラストのない場所に置いても見えるよう kMinManual 以上にする
  const double eMax = detected.empty() ? 1 : std::ranges::max(detected, {}, &Light::e).e;
  for (size_t i = 0; i < out.size(); i++)
    out[i].e = i < picked ? out[i].e / eMax : std::max(kMinManual, out[i].e / eMax);
  return out;
}

Light MakeLight(const RgbImage& img, const Params& p, double scale, double x, double y) {
  // 検出用画像で (x, y) の周り 2px のうち最も明るい画素を起点にする
  // ponytail: クリックのたびに画像全体のコントラストを計算する（2000px で十数 ms）。重ければ周りだけにする
  const auto map = Contrast(img, p);
  const int cx = std::clamp(int(x * scale), 0, img.w - 1), cy = std::clamp(int(y * scale), 0, img.h - 1);
  int seed = cy * img.w + cx;
  for (int py = std::max(0, cy - 2); py <= std::min(img.h - 1, cy + 2); py++)
    for (int qx = std::max(0, cx - 2); qx <= std::min(img.w - 1, cx + 2); qx++)
      if (const int i = py * img.w + qx; map.lum[i] > map.lum[seed]) seed = i;
  Light light{x, y, 0, 1, 1, 1};
  // 強さは検出と同じく、起点からコントラストの高い画素をたどった合計。検出の閾値に届かない光は起点のコントラストまで下げてたどる
  if (const float c0 = map.con[seed]; c0 > 0) {
    std::vector<uint8_t> seen(map.con.size());
    Flood(map.con, img.w, img.h, seed, std::min(p.contrast, double(c0)), seen, [&](int q) { light.e += map.con[q]; });
  }
  const auto px = img.view();
  const int sx = seed % img.w, sy = seed / img.w;
  const Rgb c{px[sy, sx, 0], px[sy, sx, 1], px[sy, sx, 2]};
  if (const double m = std::ranges::max(c); m >= 64) // 暗い場所なら白のまま
    light.r = float(c[0] / m), light.g = float(c[1] / m), light.b = float(c[2] / m);
  return light;
}

void Render(const std::vector<Light>& lights, const Params& p, double k, double unit, LightBuffer& buf) {
  const int rays = std::max(1, int(p.rays));
  const Rgb f{1 + p.disp * 0.2, 1, 1 - p.disp * 0.2}; // R を長く B を短くすると先端が虹色ににじむ
  for (const Light& s : lights) {
    const double wt = std::sqrt(std::clamp(s.e, 0.0, 1.0)), x = s.x * k, y = s.y * k;
    const double a = std::min(1.0, p.power * (0.3 + 0.7 * wt));
    const Rgb c{s.r * 255.0 * a, s.g * 255.0 * a, s.b * 255.0 * a};
    if (const double R = p.glow * unit * wt; R >= 1) Halo(buf, x, y, R, {c[0] * 0.6, c[1] * 0.6, c[2] * 0.6});
    const double width = p.width * unit * (0.3 + 0.7 * wt);
    const double len   = p.len * unit * wt;
    for (int i = 0; i < rays; i++) {
      const double ang = (p.angle + i * 360.0 / rays) * std::numbers::pi / 180;
      Ray(buf, x, y, std::cos(ang), std::sin(ang), {len * f[0], len * f[1], len * f[2]}, width, p.soft, c);
    }
  }
}

void Apply(const PixelView& in, void* out, int64_t outRowBytes, const std::vector<Light>& lights, const Params& p) {
  if (in.depth == 8) ApplyT<uint8_t>(in, out, outRowBytes, lights, p, 255.f);
  else ApplyT<uint16_t>(in, out, outRowBytes, lights, p, 32768.f);
}

} // namespace hikariuni
