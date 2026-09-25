// GUI自己検証用の合成SERを作る（ctest の gui_selftest が使う）。
//
// 縞模様のある惑星状の円盤を、フレームごとに少しずつ動かし、局所的に歪ませ、
// ノイズを乗せて書き出す。品質評価・アライメント（位置合わせ領域の自動配置）・
// スタックの全工程が通る程度に「本物らしい」ことが目的で、精度の検証には使わない。

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace {

void put32(std::vector<std::uint8_t>& b, std::int32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

void put64(std::vector<std::uint8_t>& b, std::int64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

void put_text(std::vector<std::uint8_t>& b, const char* s, std::size_t n) {
    std::size_t i = 0;
    for (; s[i] && i < n; ++i) b.push_back(static_cast<std::uint8_t>(s[i]));
    for (; i < n; ++i) b.push_back(0);
}

std::uint32_t hash(std::uint32_t v) {
    v ^= v >> 16;
    v *= 0x7feb352dU;
    v ^= v >> 15;
    v *= 0x846ca68bU;
    v ^= v >> 16;
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "使い方: make_scene_ser <出力.ser> [フレーム数]\n");
        return 2;
    }
    const int w = 192, h = 160;
    const int frames = argc > 2 ? std::atoi(argv[2]) : 30;
    std::vector<std::uint8_t> out;
    put_text(out, "LUCAM-RECORDER", 14);
    put32(out, 0);
    put32(out, 100);  // RGB（カラー経路とチャンネル合わせも通す）
    put32(out, 1);
    put32(out, w);
    put32(out, h);
    put32(out, 8);
    put32(out, frames);
    put_text(out, "LunaStack test", 40);
    put_text(out, "synthetic", 40);
    put_text(out, "synthetic", 40);
    const std::int64_t t0 = 637613382273000000LL;  // 2021-07-08 10:50:27.3 UTC
    put64(out, t0);
    put64(out, t0);
    for (int f = 0; f < frames; ++f) {
        const double sx = 2.5 * std::sin(f * 0.7), sy = 2.0 * std::cos(f * 0.45);
        const double blur = 0.6 + 0.8 * ((hash(static_cast<std::uint32_t>(f) * 7919u) & 255) / 255.0);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                // 局所的な揺らぎ（シーイング）で模様を少し歪ませる。
                const double wx = x - sx + 0.8 * std::sin(y * 0.09 + f);
                const double wy = y - sy + 0.8 * std::cos(x * 0.07 + f * 1.3);
                const double d = std::hypot(wx - w / 2.0, wy - h / 2.0);
                const double edge = std::max(0.0, std::min(1.0, (58.0 - d) / (2.0 * blur)));
                const double bands = 0.62 + 0.22 * std::sin(wy * 0.33) + 0.08 * std::sin((wx + wy) * 0.21 / blur);
                const double v = 0.03 + edge * bands;
                for (int c = 0; c < 3; ++c) {
                    const double tint = c == 0 ? 1.0 : (c == 1 ? 0.92 : 0.75);
                    const double noise = ((hash(static_cast<std::uint32_t>((f * h + y) * w + x) * 3u + c) & 1023) / 1023.0 - 0.5) * 0.04;
                    const int value = static_cast<int>(std::lround(std::max(0.0, std::min(1.0, v * tint + noise)) * 255.0));
                    out.push_back(static_cast<std::uint8_t>(value));
                }
            }
        }
    }
    for (int f = 0; f < frames; ++f) put64(out, t0 + static_cast<std::int64_t>(f) * 100000LL);
    std::FILE* fp = std::fopen(argv[1], "wb");
    if (!fp) return 1;
    const bool ok = std::fwrite(out.data(), 1, out.size(), fp) == out.size();
    std::fclose(fp);
    return ok ? 0 : 1;
}
