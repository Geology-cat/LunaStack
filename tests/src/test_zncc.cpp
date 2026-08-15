#include <cstdio>
#include <cmath>
#include <cstdint>
#include <vector>

#include "microtest.hpp"
#include "stackcore/zncc_matcher.hpp"

using stackcore::MatchResult;
using stackcore::ZnccMatcher;

namespace {

struct Lcg {
    std::uint32_t s;
    explicit Lcg(std::uint32_t seed) : s(seed) {}
    double next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<double>((s >> 8) & 0xFFFF) / 65535.0;
    }
};

// 連続関数として定義したシーン。任意の実数座標で評価できる。
//
// 真値はここから直接標本化して作る。リサンプラを通すと、
// リサンプラの誤差とマッチャの誤差が混ざって切り分けられなくなる。
//
// **三角関数の重ね合わせは使えない。** 周期的な模様は相関に本質的な多義性を持ち、
// 別の周期に一致してしまう位置にもピークが立つ。実際に最初はそれで
// 最大11pxのずれが出た。ガウス斑点を擬似乱数の位置に多数置くことで、
// 連続・非周期・帯域制限（σで決まる）のすべてを満たす。
struct Blobs {
    struct Blob {
        double x, y, a;
    };
    std::vector<Blob> list;
    double sigma = 1.8;

    Blobs() {
        Lcg rng(20260809u);
        // テンプレート位置(40..110)と探索範囲を十分覆う範囲に撒く。
        for (int i = 0; i < 420; ++i) {
            Blob b;
            b.x = rng.next() * 200.0;
            b.y = rng.next() * 200.0;
            b.a = 0.25 + 0.75 * rng.next();
            list.push_back(b);
        }
    }

    double at(double x, double y) const {
        const double inv = 1.0 / (2.0 * sigma * sigma);
        double v = 0.0;
        for (std::size_t i = 0; i < list.size(); ++i) {
            const double dx = x - list[i].x, dy = y - list[i].y;
            const double r2 = dx * dx + dy * dy;
            // 遠い斑点は寄与しない。打ち切りで不連続にならないよう
            // 十分遠い（5σ）ところで切る。
            if (r2 > 25.0 * sigma * sigma) continue;
            v += list[i].a * std::exp(-r2 * inv);
        }
        return v;
    }
};

const Blobs& blobs() {
    static const Blobs b;
    return b;
}

double scene(double x, double y) { return 0.15 + 0.25 * blobs().at(x, y); }

// (x0, y0) を左上として size x size を標本化する。小数座標を受け付ける。
std::vector<float> sample(int size, double x0, double y0, double gain = 1.0,
                          double offset = 0.0, double noise = 0.0, std::uint32_t seed = 1) {
    std::vector<float> v(static_cast<std::size_t>(size) * size);
    Lcg rng(seed);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            double s = scene(x0 + x, y0 + y) * gain + offset;
            if (noise > 0.0) s += noise * (rng.next() - 0.5);
            v[static_cast<std::size_t>(y) * size + x] = static_cast<float>(s);
        }
    }
    return v;
}

}  // namespace

MT_TEST(zncc_同一のテンプレートはスコア1で変位ゼロ) {
    const int t = 32, r = 8;
    ZnccMatcher m(t, r);
    const std::vector<float> tmpl = sample(t, 40.0, 50.0);
    MT_CHECK(m.set_template(tmpl.data(), t));

    // 探索領域はテンプレートの周囲に r ぶんの余白を付けたもの。
    const std::vector<float> search = sample(t + 2 * r, 40.0 - r, 50.0 - r);
    const MatchResult res = m.match(search.data(), t + 2 * r);

    MT_CHECK_EQ(res.peak_dx, 0);
    MT_CHECK_EQ(res.peak_dy, 0);
    MT_CHECK_NEAR(res.score, 1.0, 1e-4);
    MT_CHECK(!res.at_search_limit);
}

MT_TEST(zncc_整数変位を符号込みで復元する) {
    const int t = 32, r = 8;
    ZnccMatcher m(t, r);
    const std::vector<float> tmpl = sample(t, 40.0, 50.0);
    MT_CHECK(m.set_template(tmpl.data(), t));

    // 探索領域の中身を (+5, -3) ずらす＝テンプレートの内容が右下方向に +5, -3 の
    // 位置で見つかるようにする。
    const std::vector<float> search = sample(t + 2 * r, 40.0 - r - 5, 50.0 - r + 3);
    const MatchResult res = m.match(search.data(), t + 2 * r);
    MT_CHECK_EQ(res.peak_dx, 5);
    MT_CHECK_EQ(res.peak_dy, -3);
}

MT_TEST(zncc_サブピクセル変位をRMS0_1px以下で復元する) {
    // M2の受け入れ条件そのもの。
    // 真値は連続関数から直接標本化しており、リサンプラを経由していない。
    const int t = 48, r = 8;
    ZnccMatcher m(t, r);

    double sum_sq = 0.0, worst = 0.0;
    int count = 0;

    for (int i = 0; i < 7; ++i) {
        for (int j = 0; j < 7; ++j) {
            const double true_dx = -3.0 + i * 1.0 + (i % 3) * 0.17 - 0.25;
            const double true_dy = -3.0 + j * 1.0 + (j % 4) * 0.13 - 0.31;

            const std::vector<float> tmpl = sample(t, 60.0, 70.0);
            if (!m.set_template(tmpl.data(), t)) continue;

            const std::vector<float> search =
                sample(t + 2 * r, 60.0 - r - true_dx, 70.0 - r - true_dy);
            const MatchResult res = m.match(search.data(), t + 2 * r);

            const double ex = res.dx - true_dx;
            const double ey = res.dy - true_dy;
            sum_sq += ex * ex + ey * ey;
            const double e = std::sqrt(ex * ex + ey * ey);
            if (e > worst) worst = e;
            ++count;
        }
    }

    const double rms = std::sqrt(sum_sq / (2.0 * count));
    std::printf("           サブピクセル精度: RMS %.4f px / 最大 %.4f px (%d 通り)\n", rms,
                worst, count);
    if (!(rms <= 0.1)) {
        microtest::fail("RMS誤差が0.1pxを超えている: " + microtest::mt_str(rms));
    }
}

MT_TEST(zncc_明るさとコントラストが変わっても結果が変わらない) {
    // ZNCCを使う理由そのもの。薄雲や透明度変動で明るさが変わっても
    // 相関が崩れないこと（仕様書 §4.6）。
    //
    // 「スコアが1に近い」ではなく「明るさを変えても**同じ結果**になる」ことを見る。
    // 小数変位では探索窓のどれもテンプレートと厳密には一致しないので、
    // 明るさ変化がなくてもスコアは1未満になる。invariance の主張は
    // 変換前後で一致することであって、絶対値が1であることではない。
    const int t = 32, r = 8;
    ZnccMatcher m(t, r);
    const std::vector<float> tmpl = sample(t, 40.0, 50.0);
    MT_CHECK(m.set_template(tmpl.data(), t));

    const MatchResult plain =
        m.match(sample(t + 2 * r, 40.0 - r - 2.4, 50.0 - r - 1.6).data(), t + 2 * r);
    const MatchResult scaled =
        m.match(sample(t + 2 * r, 40.0 - r - 2.4, 50.0 - r - 1.6, 0.6, 0.15).data(),
                t + 2 * r);

    // 厳密な演算なら完全一致するが、相関はfloat32で計算しているため
    // 0.6倍して丸めたぶんの差が残る。1e-4 px は「同じ結果」と言ってよい水準。
    MT_CHECK_NEAR(scaled.dx, plain.dx, 1e-4);
    MT_CHECK_NEAR(scaled.dy, plain.dy, 1e-4);
    MT_CHECK_NEAR(scaled.score, plain.score, 1e-4);
    MT_CHECK_NEAR(plain.dx, 2.4, 0.12);
    MT_CHECK_NEAR(plain.dy, 1.6, 0.12);
}

MT_TEST(zncc_ノイズが乗ってもスコアが下がるだけで変位は保たれる) {
    const int t = 48, r = 8;
    ZnccMatcher m(t, r);
    const std::vector<float> tmpl = sample(t, 40.0, 50.0);
    MT_CHECK(m.set_template(tmpl.data(), t));

    const std::vector<float> search =
        sample(t + 2 * r, 40.0 - r - 1.3, 50.0 - r + 2.7, 1.0, 0.0, 0.06, 99);
    const MatchResult res = m.match(search.data(), t + 2 * r);

    MT_CHECK_NEAR(res.dx, 1.3, 0.15);
    MT_CHECK_NEAR(res.dy, -2.7, 0.15);
    MT_CHECK(res.score < 0.999);  // ノイズぶんスコアは下がる
    MT_CHECK(res.score > 0.8);
}

MT_TEST(zncc_一様なテンプレートは拒否される) {
    const int t = 32, r = 8;
    ZnccMatcher m(t, r);
    const std::vector<float> flat(static_cast<std::size_t>(t) * t, 0.42f);
    MT_CHECK(!m.set_template(flat.data(), t));
}

MT_TEST(zncc_探索範囲を超える変位は信用できない結果として現れる) {
    // 真の変位が探索範囲の外にあるとき、返る変位は必ず間違っている。
    // 仕様書 §4.6 は「相関ピーク値が閾値未満、**または**変位が探索半径上限に
    // 張り付いた場合」を無効とする。どちらか一方だけでは捕まらない場合があるので、
    // 両方を合わせて初めて検出できることをここで固定する。
    const int t = 32, r = 4;
    ZnccMatcher m(t, r);
    const std::vector<float> tmpl = sample(t, 40.0, 50.0);
    MT_CHECK(m.set_template(tmpl.data(), t));

    const std::vector<float> search = sample(t + 2 * r, 40.0 - r - 10.0, 50.0 - r);
    const MatchResult res = m.match(search.data(), t + 2 * r);

    std::printf("           範囲外(10px)のとき: score=%.3f 縁=%s dx=%.2f\n", res.score,
                res.at_search_limit ? "はい" : "いいえ", res.dx);
    if (!(res.at_search_limit || res.score < 0.9)) {
        microtest::fail("範囲外の変位が信用できない結果として検出されない: score=" +
                        microtest::mt_str(res.score));
    }
}

MT_TEST(zncc_正しく合う場合は高いスコアで縁にも張り付かない) {
    // 上のテストの対照。合う場合と合わない場合が分離していなければ、
    // どちらの判定条件も意味を持たない。
    const int t = 32, r = 4;
    ZnccMatcher m(t, r);
    const std::vector<float> tmpl = sample(t, 40.0, 50.0);
    MT_CHECK(m.set_template(tmpl.data(), t));

    const std::vector<float> search = sample(t + 2 * r, 40.0 - r - 1.5, 50.0 - r - 0.5);
    const MatchResult res = m.match(search.data(), t + 2 * r);
    MT_CHECK(!res.at_search_limit);
    MT_CHECK(res.score > 0.95);
}

MT_TEST(zncc_2次曲面フィットは対称な山の頂点を中央に置く) {
    double v[3][3];
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
            const double dx = i - 1, dy = j - 1;
            v[j][i] = 1.0 - 0.1 * (dx * dx + dy * dy);
        }
    }
    double dx = 9.0, dy = 9.0;
    stackcore::quadratic_subpixel(v, dx, dy);
    MT_CHECK_NEAR(dx, 0.0, 1e-12);
    MT_CHECK_NEAR(dy, 0.0, 1e-12);
}

MT_TEST(zncc_2次曲面フィットはずれた山の頂点を当てる) {
    // 頂点 (0.3, -0.4) の放物面をサンプルして、そこへ戻せるか。
    const double px = 0.3, py = -0.4;
    double v[3][3];
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
            const double dx = (i - 1) - px, dy = (j - 1) - py;
            v[j][i] = 1.0 - 0.08 * dx * dx - 0.11 * dy * dy;
        }
    }
    double dx = 0.0, dy = 0.0;
    stackcore::quadratic_subpixel(v, dx, dy);
    MT_CHECK_NEAR(dx, px, 1e-9);
    MT_CHECK_NEAR(dy, py, 1e-9);
}

MT_TEST(zncc_2次曲面フィットは3x3の外へ外挿しない) {
    // 極値が遠くにある（＝ピーク検出が失敗している）場合に、
    // でたらめな大きい補正値を返さないこと。
    double v[3][3];
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) v[j][i] = 0.5 + 0.3 * (i - 1);  // 単調な斜面
    }
    double dx = 0.0, dy = 0.0;
    stackcore::quadratic_subpixel(v, dx, dy);
    MT_CHECK(dx >= -1.0 && dx <= 1.0);
    MT_CHECK(dy >= -1.0 && dy <= 1.0);
}
