#include "stackcore/debayer.hpp"

#include <algorithm>
#include <stdexcept>

namespace stackcore {
namespace {

// CFA上の座標 (x, y) に実際に置かれている色を返す（0=R, 1=G, 2=B）。
int cfa_color_at(SerColorId pattern, int x, int y) {
    const int px = x & 1;
    const int py = y & 1;
    switch (pattern) {
        case SerColorId::BayerRGGB:  // 行0: R G / 行1: G B
            return py == 0 ? (px == 0 ? 0 : 1) : (px == 0 ? 1 : 2);
        case SerColorId::BayerGRBG:  // 行0: G R / 行1: B G
            return py == 0 ? (px == 0 ? 1 : 0) : (px == 0 ? 2 : 1);
        case SerColorId::BayerGBRG:  // 行0: G B / 行1: R G
            return py == 0 ? (px == 0 ? 1 : 2) : (px == 0 ? 0 : 1);
        case SerColorId::BayerBGGR:  // 行0: B G / 行1: G R
            return py == 0 ? (px == 0 ? 2 : 1) : (px == 0 ? 1 : 0);
        default:
            return -1;
    }
}

}  // namespace

void debayer_bilinear(const FrameBuffer& cfa, SerColorId pattern, FrameBuffer& out) {
    if (!is_supported_bayer(pattern)) {
        throw std::runtime_error(std::string("デバイヤー: 未対応のパターンです (") +
                                 to_string(pattern) + ")");
    }
    if (cfa.channels() != 1) {
        throw std::invalid_argument("デバイヤー: 入力は1chのCFA画像である必要があります");
    }

    const int w = cfa.width();
    const int h = cfa.height();
    out.reset(w, h, 3);
    out.set_source_bit_depth(cfa.source_bit_depth());

    for (int y = 0; y < h; ++y) {
        float* dst[3] = {out.row(0, y), out.row(1, y), out.row(2, y)};

        for (int x = 0; x < w; ++x) {
            const int own = cfa_color_at(pattern, x, y);

            // 自分の位置にある色は実測値をそのまま使い、
            // それ以外は3x3近傍にある同色サンプルの平均で埋める。
            // Bayer配列ではこれが通常のbilinear補間と一致する
            // （Gは十字4点、対角位置のR/Bは4点、同一行のR/Bは2点の平均になる）。
            float sum[3] = {0.0f, 0.0f, 0.0f};
            int count[3] = {0, 0, 0};

            const int y0 = y > 0 ? y - 1 : 0;
            const int y1 = y + 1 < h ? y + 1 : h - 1;
            const int x0 = x > 0 ? x - 1 : 0;
            const int x1 = x + 1 < w ? x + 1 : w - 1;

            for (int ny = y0; ny <= y1; ++ny) {
                const float* src = cfa.row(0, ny);
                for (int nx = x0; nx <= x1; ++nx) {
                    if (nx == x && ny == y) continue;
                    const int c = cfa_color_at(pattern, nx, ny);
                    sum[c] += src[nx];
                    count[c] += 1;
                }
            }

            const float own_value = cfa.row(0, y)[x];
            for (int c = 0; c < 3; ++c) {
                if (c == own) {
                    dst[c][x] = own_value;
                } else if (count[c] > 0) {
                    dst[c][x] = sum[c] / static_cast<float>(count[c]);
                } else {
                    dst[c][x] = own_value;  // 1x1画像などの退化ケース
                }
            }
        }
    }

    out.invalidate_luma();
}

const char* to_string(DebayerMethod method) {
    return method == DebayerMethod::MalvarHeCutler ? "mhc" : "bilinear";
}

void debayer_mhc(const FrameBuffer& cfa, SerColorId pattern, FrameBuffer& out) {
    if (!is_supported_bayer(pattern)) {
        throw std::runtime_error(std::string("デバイヤー: 未対応のパターンです (") +
                                 to_string(pattern) + ")");
    }
    if (cfa.channels() != 1) {
        throw std::invalid_argument("デバイヤー: 入力は1chのCFA画像である必要があります");
    }
    const int w = cfa.width();
    const int h = cfa.height();
    out.reset(w, h, 3);
    out.set_source_bit_depth(cfa.source_bit_depth());
    if (w < 3 || h < 3) {  // 5x5の核が意味を持たない極小画像は従来方式で埋める
        debayer_bilinear(cfa, pattern, out);
        return;
    }

    // 端は鏡映で折り返す（-1→1, w→w-2）。折り返しても配列の偶奇が保たれるので、
    // 境界でも正しい色の画素を参照できる。値を複製するclampでは偶奇がずれる。
    const auto mirror = [](int v, int n) {
        if (v < 0) v = -v;
        if (v >= n) v = 2 * (n - 1) - v;
        return std::min(std::max(v, 0), n - 1);
    };
    const auto at = [&](int x, int y) -> float {
        return cfa.row(0, mirror(y, h))[mirror(x, w)];
    };

    for (int y = 0; y < h; ++y) {
        float* dst[3] = {out.row(0, y), out.row(1, y), out.row(2, y)};
        for (int x = 0; x < w; ++x) {
            const int own = cfa_color_at(pattern, x, y);
            const float c = at(x, y);
            const float n1 = at(x, y - 1) + at(x, y + 1);          // 上下
            const float e1 = at(x - 1, y) + at(x + 1, y);          // 左右
            const float n2 = at(x, y - 2) + at(x, y + 2);
            const float e2 = at(x - 2, y) + at(x + 2, y);
            const float diag = at(x - 1, y - 1) + at(x + 1, y - 1) + at(x - 1, y + 1) +
                               at(x + 1, y + 1);
            float rgb[3] = {0.0f, 0.0f, 0.0f};
            rgb[own] = c;
            if (own == 1) {
                // G画素。左右に並ぶ色と上下に並ぶ色を別の核で求める。
                const int horizontal = cfa_color_at(pattern, x + 1, y);  // 左右の色
                const int vertical = cfa_color_at(pattern, x, y + 1);    // 上下の色
                const float h_value = (5.0f * c + 4.0f * e1 - diag - e2 + 0.5f * n2) / 8.0f;
                const float v_value = (5.0f * c + 4.0f * n1 - diag - n2 + 0.5f * e2) / 8.0f;
                rgb[horizontal] = h_value;
                rgb[vertical] = v_value;
            } else {
                // R/B画素。Gは十字、反対色は対角から求める。
                const int other = own == 0 ? 2 : 0;
                rgb[1] = (4.0f * c + 2.0f * (n1 + e1) - (n2 + e2)) / 8.0f;
                rgb[other] = (6.0f * c + 2.0f * diag - 1.5f * (n2 + e2)) / 8.0f;
            }
            for (int k = 0; k < 3; ++k) {
                dst[k][x] = std::min(1.0f, std::max(0.0f, rgb[k]));
            }
        }
    }
    out.invalidate_luma();
}

void debayer(const FrameBuffer& cfa, SerColorId pattern, DebayerMethod method, FrameBuffer& out) {
    if (method == DebayerMethod::MalvarHeCutler) {
        debayer_mhc(cfa, pattern, out);
    } else {
        debayer_bilinear(cfa, pattern, out);
    }
}

}  // namespace stackcore
