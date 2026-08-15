#include <cmath>
#include <vector>

#include "microtest.hpp"
#include "stackcore/frame_selector.hpp"

using stackcore::similarity_outlier_threshold;

namespace {

// 正常フレームの類似度は狭い範囲に固まる。実データ（木星SER）の実測に合わせた分布。
std::vector<double> normal_population(std::size_t n) {
    std::vector<double> v;
    v.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        // 0.9935〜0.9955 を行き来する程度のばらつき。
        // i は符号なしなので、int にしてから引くこと（符号なしのまま 0-2 とすると
        // 桁が回り込んで巨大な値になる）。
        const int offset = static_cast<int>(i % 5) - 2;
        v.push_back(0.9945 + offset * 0.0005);
    }
    return v;
}

}  // namespace

MT_TEST(selector_サンプルが少なすぎるときは判定しない) {
    const std::vector<double> few(5, 0.99);
    MT_CHECK(similarity_outlier_threshold(few, 6.0) < 0.0);
}

MT_TEST(selector_実データ相当の分布でテアリングだけを切る) {
    // 正常フレーム100枚に、実測されたテアリングフレームの値を混ぜる。
    std::vector<double> v = normal_population(100);
    const double torn[] = {0.749, 0.865, 0.894, 0.931, 0.985};
    for (double t : torn) v.push_back(t);

    const double threshold = similarity_outlier_threshold(v, 6.0);

    for (double t : torn) {
        if (!(t < threshold)) {
            microtest::fail("テアリングフレーム " + microtest::mt_str(t) +
                            " がしきい値 " + microtest::mt_str(threshold) + " を上回っている");
        }
    }
    const std::vector<double> normals = normal_population(100);
    for (std::size_t i = 0; i < normals.size(); ++i) {
        if (normals[i] < threshold) {
            microtest::fail("正常フレーム " + microtest::mt_str(normals[i]) +
                            " がしきい値 " + microtest::mt_str(threshold) + " で切られている");
        }
    }
}

MT_TEST(selector_全フレームが同一でも正常フレームを切らない) {
    // MADが0になる場合。しきい値が中央値に貼り付くと全滅するので、
    // 下限（floor_margin）が効いていなければならない。
    const std::vector<double> v(64, 0.994);
    const double threshold = similarity_outlier_threshold(v, 6.0);
    MT_CHECK(threshold < 0.994);
    MT_CHECK_NEAR(threshold, 0.992, 1e-9);
}

MT_TEST(selector_外れ値が多数混ざってもしきい値が壊れない) {
    // MADは外れ値に「引きずられにくい」だけで完全に不変ではない。
    // 外れ値が増えると偏差配列の中央位置がずれるため、しきい値は多少動く。
    // 求めるのは不変性ではなく、動いた先でも判定が壊れていないことである。
    // （平均と標準偏差で同じことをすると、外れ値20個でしきい値が
    //   正常フレームより下に吹き飛び、判定そのものが無意味になる。）
    std::vector<double> a = normal_population(100);
    std::vector<double> b = a;
    for (int i = 0; i < 20; ++i) b.push_back(0.70);

    const double ta = similarity_outlier_threshold(a, 6.0);
    const double tb = similarity_outlier_threshold(b, 6.0);

    if (!(std::fabs(ta - tb) < 0.01)) {
        microtest::fail("外れ値の混入でしきい値が動きすぎ: " + microtest::mt_str(ta) +
                        " → " + microtest::mt_str(tb));
    }
    // どちらのしきい値でも、外れ値は切られ正常フレームは残る。
    const std::vector<double> normals = normal_population(100);
    for (double t : {ta, tb}) {
        MT_CHECK(0.70 < t);
        for (std::size_t i = 0; i < normals.size(); ++i) MT_CHECK(normals[i] >= t);
    }
}

MT_TEST(selector_kを大きくするとしきい値は緩くなる) {
    const std::vector<double> v = normal_population(200);
    const double strict = similarity_outlier_threshold(v, 3.0);
    const double loose = similarity_outlier_threshold(v, 12.0);
    MT_CHECK(loose <= strict);
}
