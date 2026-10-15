#include "effect.h"

#include <algorithm>
#include <cmath>

namespace hikariuni {
namespace {

constexpr double kPi = 3.14159265358979323846;

template<typename T>
RgbImage ResampleT(const PixelView& s, int dw, int dh, double toByte) {
  RgbImage out{dw, dh, std::vector<float>(size_t(dw) * dh * 3)};
  for (int y = 0; y < dh; y++) {
    const int sy0 = int(int64_t(y) * s.h / dh), sy1 = std::max(sy0 + 1, int(int64_t(y + 1) * s.h / dh));
    for (int x = 0; x < dw; x++) {
      const int sx0 = int(int64_t(x) * s.w / dw), sx1 = std::max(sx0 + 1, int(int64_t(x + 1) * s.w / dw));
      double sum[3] = {};
      for (int yy = sy0; yy < sy1; yy++) {
        auto row = reinterpret_cast<const T*>(static_cast<const char*>(s.data) + yy * s.rowBytes);
        for (int xx = sx0; xx < sx1; xx++)
          for (int c = 0; c < 3; c++)
            sum[c] += row[xx * s.planes + (s.colors == 1 ? 0 : c)];
      }
      const double n = double(sy1 - sy0) * (sx1 - sx0);
      for (int c = 0; c < 3; c++)
        out.px[(size_t(y) * dw + x) * 3 + c] = float(sum[c] / n * toByte);
    }
  }
  return out;
}

float& At(LightBuffer& b, int x, int y, int ch) {
  return b.rgb[(size_t(y - b.y0) * b.w + (x - b.x0)) * 3 + ch];
}

// (x0, y0)-(x1, y1) をバッファの範囲に切り詰める。空なら false
bool Clip(const LightBuffer& b, double x0, double y0, double x1, double y1, int& ix0, int& iy0, int& ix1, int& iy1) {
  ix0 = std::max(b.x0, int(std::floor(x0)));
  iy0 = std::max(b.y0, int(std::floor(y0)));
  ix1 = std::min(b.x0 + b.w, int(std::ceil(x1)) + 1);
  iy1 = std::min(b.y0 + b.h, int(std::ceil(y1)) + 1);
  return ix0 < ix1 && iy0 < iy1;
}

// 中心から外へ線形に消える円。中心の明るさは value
void Halo(LightBuffer& b, double x, double y, double r, const double value[3]) {
  int ix0, iy0, ix1, iy1;
  if (!Clip(b, x - r, y - r, x + r, y + r, ix0, iy0, ix1, iy1)) return;
  for (int py = iy0; py < iy1; py++)
    for (int px = ix0; px < ix1; px++) {
      const double d = std::hypot(px + 0.5 - x, py + 0.5 - y);
      if (d >= r) continue;
      for (int ch = 0; ch < 3; ch++)
        At(b, px, py, ch) += float(value[ch] * (1 - d / r));
    }
}

// lo..hi を「a <= v * dx + c <= b を満たす dx」で絞る（v がほぼ 0 なら c だけで判定）。空なら false
bool Narrow(double& lo, double& hi, double v, double c, double a, double b) {
  if (std::abs(v) < 1e-9) return a <= c && c <= b;
  const double p = (a - c) / v, q = (b - c) / v;
  lo = std::max(lo, std::min(p, q));
  hi = std::min(hi, std::max(p, q));
  return lo <= hi;
}

// 光芒 1 本。方向 (cs, sn)、中心の幅 width で先端に向かって細くなり、RGB ごとに長さ len[ch]（虹色のにじみ）。
// 長さ方向の明るさは value → 0.4 * value（25% 地点）→ 0。
// 幅方向は soft = 0 で縁がくっきり（1px のアンチエイリアス）、1 で幅 2 倍の山形（明るさの合計はほぼ同じ）。
// 外接矩形全体ではなく、各行で光芒が通る範囲だけを調べる
void Ray(LightBuffer& b, double x, double y, double cs, double sn, const double len[3], double width, double soft,
         const double value[3]) {
  const double L = std::max({len[0], len[1], len[2]});
  if (L < 1) return;
  const double hw = width / 2, reach = hw * (1 + soft) + 1; // 中心線から光が届く最大の距離
  int ix0, iy0, ix1, iy1;
  if (!Clip(b, std::min(x, x + cs * L) - reach, std::min(y, y + sn * L) - reach, std::max(x, x + cs * L) + reach,
            std::max(y, y + sn * L) + reach, ix0, iy0, ix1, iy1))
    return;
  for (int py = iy0; py < iy1; py++) {
    const double dy = py + 0.5 - y;
    double lo = ix0 + 0.5 - x, hi = ix1 - 0.5 - x; // この行で調べる dx の範囲
    if (!Narrow(lo, hi, -sn, dy * cs, -reach, reach) || !Narrow(lo, hi, cs, dy * sn, -0.5, L)) continue;
    const int px0 = std::max(ix0, int(std::floor(x + lo - 0.5))), px1 = std::min(ix1, int(std::ceil(x + hi - 0.5)) + 1);
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
        At(b, px, py, ch) += float(value[ch] * g * cover);
      }
    }
  }
}

template<typename T>
void ApplyT(const PixelView& in, void* out, int64_t outRowBytes, const std::vector<Light>& lights, const Params& p,
            float maxValue) {
  const float scale  = maxValue / 255;
  const double unit  = std::min(in.w, in.h) / 1000.0;
  const bool alpha   = in.alphaWritable && in.planes > in.colors; // 色の次のプレーンが透明度
  constexpr int kRow = 256;                                       // メモリを抑えるため 256 行ずつ描く
  for (int y0 = 0; y0 < in.h; y0 += kRow) {
    LightBuffer buf{0, y0, in.w, std::min(kRow, in.h - y0)};
    buf.rgb.assign(size_t(buf.w) * buf.h * 3, 0.f);
    Render(lights, p, 1.0, unit, buf);
    for (int y = 0; y < buf.h; y++) {
      auto srcRow       = reinterpret_cast<const T*>(static_cast<const char*>(in.data) + (y0 + y) * in.rowBytes);
      auto dstRow       = reinterpret_cast<T*>(static_cast<char*>(out) + (y0 + y) * outRowBytes);
      const float* line = &buf.rgb[size_t(y) * buf.w * 3];
      for (int x = 0; x < in.w; x++) {
        const T* src   = srcRow + x * in.planes;
        T* dst         = dstRow + x * in.planes;
        const float* l = line + x * 3;
        float add[3]   = {l[0] * scale, l[1] * scale, l[2] * scale}; // ホストの値の範囲に換算した光
        if (in.colors == 1) add[0] = (add[0] + add[1] + add[2]) / 3;
        int c = 0;
        if (!p.raysOnly) {
          for (; c < in.colors; c++)
            dst[c] = T(std::min(maxValue, src[c] + add[c] + 0.5f));
        } else if (alpha) {
          // 透過: 不透明度 = 光の強さ、色 = 光 / 不透明度（通常の合成で 色 × 不透明度 = 光になる）
          const float a = std::min(maxValue, *std::max_element(add, add + in.colors));
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
  }
}

} // namespace

RgbImage Resample(const PixelView& src, int dw, int dh) {
  return src.depth == 8 ? ResampleT<uint8_t>(src, dw, dh, 1.0) : ResampleT<uint16_t>(src, dw, dh, 255.0 / 32768);
}

RgbImage Resample(const RgbImage& src, int dw, int dh) {
  return ResampleT<float>({src.px.data(), int64_t(src.w) * 3 * int64_t(sizeof(float)), 3, 3, 0, src.w, src.h}, dw, dh,
                          1.0);
}

std::vector<Light> Detect(const RgbImage& img, const Params& p, double scale) {
  const int w = img.w, h = img.h, W = w + 1;
  const size_t n = size_t(w) * h;
  std::vector<float> lum(n), con(n);
  std::vector<double> integral(size_t(W) * (h + 1));
  for (size_t i = 0; i < n; i++)
    lum[i] = 0.2126f * img.px[i * 3] + 0.7152f * img.px[i * 3 + 1] + 0.0722f * img.px[i * 3 + 2];
  for (int y = 0; y < h; y++) {
    double row = 0;
    for (int x = 0; x < w; x++) {
      row += lum[size_t(y) * w + x];
      integral[size_t(y + 1) * W + x + 1] = integral[size_t(y) * W + x + 1] + row;
    }
  }

  // 局所コントラスト = 輝度 - 周囲（半径 R の箱）の平均。背景を測る範囲は光源より十分広く取る
  const double maxD = p.size * std::min(w, h) / 1000;
  const int R       = std::max(3, int(std::lround(maxD * 1.5)));
  for (int y = 0; y < h; y++) {
    const int y0 = std::max(0, y - R), y1 = std::min(h, y + R + 1);
    for (int x = 0; x < w; x++) {
      const int x0 = std::max(0, x - R), x1 = std::min(w, x + R + 1);
      const size_t i    = size_t(y) * w + x;
      const double mean = (integral[size_t(y1) * W + x1] - integral[size_t(y0) * W + x1] -
                           integral[size_t(y1) * W + x0] + integral[size_t(y0) * W + x0]) /
                          (double(x1 - x0) * (y1 - y0));
      con[i]            = lum[i] >= p.thr ? float(lum[i] - mean) : 0.f;
    }
  }

  // コントラストの高い画素を 4 近傍でつないで光源候補にし、それっぽくないものを捨てる
  std::vector<uint8_t> seen(n);
  std::vector<int> stack;
  std::vector<Light> out;
  for (size_t start = 0; start < n; start++) {
    if (seen[start] || con[start] < p.contrast) continue;
    double cnt = 0, sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0, r = 0, g = 0, b = 0, e = 0, sl = 0;
    seen[start] = 1;
    stack.push_back(int(start));
    while (!stack.empty()) {
      const int q = stack.back();
      stack.pop_back();
      const int x = q % w, y = q / w;
      cnt++, sx += x, sy += y, sxx += double(x) * x, syy += double(y) * y, sxy += double(x) * y;
      r += img.px[size_t(q) * 3], g += img.px[size_t(q) * 3 + 1], b += img.px[size_t(q) * 3 + 2];
      e += con[q], sl += lum[q];
      auto visit = [&](int v) {
        if (!seen[v] && con[v] >= p.contrast) seen[v] = 1, stack.push_back(v);
      };
      if (x > 0) visit(q - 1);
      if (x < w - 1) visit(q + 1);
      if (y > 0) visit(q - w);
      if (y < h - 1) visit(q + w);
    }
    const double mx = sx / cnt, my = sy / cnt;
    const double vx = sxx / cnt - mx * mx + 1.0 / 12, vy = syy / cnt - my * my + 1.0 / 12, cv = sxy / cnt - mx * my;
    const double t = (vx + vy) / 2, d = std::hypot((vx - vy) / 2, cv);      // 共分散の固有値 t ± d
    if (2 * std::sqrt(cnt / kPi) > maxD || (t + d) / (t - d) > 9) continue; // 大きすぎる / 縦横比 3 超

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

    const double m = std::max({r, g, b}); // 色相を保ったまま最大チャンネルを 1 に
    out.push_back({(mx + 0.5) / scale, (my + 0.5) / scale, e, float(r / m), float(g / m), float(b / m)});
  }
  std::stable_sort(out.begin(), out.end(), [](const Light& a, const Light& c) { return a.e > c.e; });
  // 最も強い光源を 1 にそろえる。どれを除外しても、ほかの光源の大きさが変わらないように
  const double eMax = out.empty() ? 1 : out.front().e;
  for (Light& l : out)
    l.e /= eMax;
  return out;
}

std::vector<Light> Pick(const std::vector<Light>& lights, const Params& p, double unit) {
  std::vector<Light> out;
  const double gap = p.gap * unit;
  for (const Light& s : lights) {
    if (out.size() >= size_t(p.max)) break;
    if (std::all_of(out.begin(), out.end(), [&](const Light& o) { return std::hypot(o.x - s.x, o.y - s.y) >= gap; }))
      out.push_back(s);
  }
  return out;
}

std::vector<Light> Arrange(const std::vector<Light>& detected, const Params& p, double unit, const Edits& edits) {
  std::vector<Light> out = Pick(detected, p, unit);
  const double radius    = 5 * unit;
  auto removed           = [&](const Light& l) {
    return std::any_of(edits.removed.begin(), edits.removed.end(),
                       [&](const Point& q) { return std::hypot(l.x - q.x, l.y - q.y) < radius; });
  };
  out.erase(std::remove_if(out.begin(), out.end(), removed), out.end());
  out.insert(out.end(), edits.added.begin(), edits.added.end());
  return out;
}

Light MakeLight(const RgbImage& img, double scale, double x, double y) {
  // 検出用画像で (x, y) の周り 2px のうち、最も明るい画素の色を使う
  const int cx = int(x * scale), cy = int(y * scale);
  const float* best = nullptr;
  float bestLum     = -1;
  for (int py = std::max(0, cy - 2); py <= std::min(img.h - 1, cy + 2); py++)
    for (int px = std::max(0, cx - 2); px <= std::min(img.w - 1, cx + 2); px++) {
      const float* c  = &img.px[(size_t(py) * img.w + px) * 3];
      const float lum = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
      if (lum > bestLum) bestLum = lum, best = c;
    }
  Light light{x, y, 1, 1, 1, 1};
  const float m = best ? std::max({best[0], best[1], best[2]}) : 0.f;
  if (m >= 64) light.r = best[0] / m, light.g = best[1] / m, light.b = best[2] / m; // 暗い場所なら白のまま
  return light;
}

void Render(const std::vector<Light>& lights, const Params& p, double k, double unit, LightBuffer& buf) {
  const int rays    = std::max(1, int(p.rays));
  const double f[3] = {1 + p.disp * 0.2, 1, 1 - p.disp * 0.2}; // R を長く B を短くすると先端が虹色ににじむ
  for (const Light& s : lights) {
    const double wt = std::sqrt(std::clamp(s.e, 0.0, 1.0)), x = s.x * k, y = s.y * k;
    const double a    = std::min(1.0, p.power * (0.3 + 0.7 * wt));
    const double c[3] = {s.r * 255.0 * a, s.g * 255.0 * a, s.b * 255.0 * a};
    const double R    = p.glow * unit * wt;
    if (R >= 1) {
      const double halo[3] = {c[0] * 0.6, c[1] * 0.6, c[2] * 0.6};
      Halo(buf, x, y, R, halo);
    }
    const double width  = p.width * unit * (0.3 + 0.7 * wt);
    const double len[3] = {p.len * unit * wt * f[0], p.len * unit * wt * f[1], p.len * unit * wt * f[2]};
    for (int i = 0; i < rays; i++) {
      const double ang = (p.angle + i * 360.0 / rays) * kPi / 180;
      Ray(buf, x, y, std::cos(ang), std::sin(ang), len, width, p.soft, c);
    }
  }
}

void Apply(const PixelView& in, void* out, int64_t outRowBytes, const std::vector<Light>& lights, const Params& p) {
  if (in.depth == 8) ApplyT<uint8_t>(in, out, outRowBytes, lights, p, 255.f);
  else ApplyT<uint16_t>(in, out, outRowBytes, lights, p, 32768.f);
}

} // namespace hikariuni
