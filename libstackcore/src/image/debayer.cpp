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

namespace {

// 1画素ぶんの bilinear 補間（汎用。画像の縁の画素に使う）。
//
// 自分の位置にある色は実測値をそのまま使い、それ以外は3x3近傍にある
// 同色サンプルの平均で埋める。Bayer配列ではこれが通常のbilinear補間と一致する
// （Gは十字4点、対角位置のR/Bは4点、同一行のR/Bは2点の平均になる）。
// 縁では近傍が欠けるので、ある分だけで平均する。
void bilinear_pixel(const FrameBuffer& cfa, SerColorId pattern, int x, int y, int w, int h,
                    float* const dst[3]) {
    const int own = cfa_color_at(pattern, x, y);
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
        float* const dst[3] = {out.row(0, y), out.row(1, y), out.row(2, y)};

        if (y == 0 || y == h - 1 || w < 3) {
            for (int x = 0; x < w; ++x) bilinear_pixel(cfa, pattern, x, y, w, h, dst);
            continue;
        }
        bilinear_pixel(cfa, pattern, 0, y, w, h, dst);
        bilinear_pixel(cfa, pattern, w - 1, y, w, h, dst);

        // 内側の画素は近傍8点がすべて揃っているので、色の並びごとに決まった式で求める。
        // 色補間はフレームを読むたびに全画素へ掛かり、局所アライメントと加算の
        // 律速になっていた（1画素ずつ近傍の色を調べる汎用処理の約10倍速い）。
        //
        // **足す順序は汎用処理と同じ（上の行→同じ行→下の行、左→右）にしてある。**
        // 0.0f から足し始めるのも同じで、出力は汎用処理とビット単位で一致する。
        const float* up = cfa.row(0, y - 1);
        const float* cur = cfa.row(0, y);
        const float* down = cfa.row(0, y + 1);
        const int row_color[2] = {cfa_color_at(pattern, 0, y), cfa_color_at(pattern, 1, y)};

        for (int parity = 0; parity < 2; ++parity) {
            const int own = row_color[parity];
            int x = parity == 0 ? 2 : 1;
            if (own == 1) {
                // G画素。左右に並ぶ色と上下に並ぶ色は別々で、どちらも2点の平均。
                const int horizontal = row_color[1 - parity];
                const int vertical = 2 - horizontal;
                float* dh = dst[horizontal];
                float* dv = dst[vertical];
                float* dg = dst[1];
                for (; x < w - 1; x += 2) {
                    float sh = 0.0f;
                    sh += cur[x - 1];
                    sh += cur[x + 1];
                    float sv = 0.0f;
                    sv += up[x];
                    sv += down[x];
                    dh[x] = sh / 2.0f;
                    dv[x] = sv / 2.0f;
                    dg[x] = cur[x];
                }
            } else {
                // R/B画素。Gは十字の4点、反対色は対角の4点の平均。
                float* dn = dst[own];
                float* dg = dst[1];
                float* dopp = dst[2 - own];
                for (; x < w - 1; x += 2) {
                    float sg = 0.0f;
                    sg += up[x];
                    sg += cur[x - 1];
                    sg += cur[x + 1];
                    sg += down[x];
                    float so = 0.0f;
                    so += up[x - 1];
                    so += up[x + 1];
                    so += down[x - 1];
                    so += down[x + 1];
                    dn[x] = cur[x];
                    dg[x] = sg / 4.0f;
                    dopp[x] = so / 4.0f;
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
