// 加算アキュムレータの精度測定（M2の受け入れ条件）。
//
// 仕様書 §4.8 は「32bit floatなら誤差は無視できると決めつけない（要測定）」と
// している。0〜1に正規化した値を数百〜数万フレーム加算すると、和が大きくなるにつれ
// 1ulpも大きくなり、平均値の誤差が16bit出力の1LSBに達しうるためである。
//
// ここで比較するのは4通り:
//   float32 逐次加算   … 素朴な実装
//   float32 対加算     … 2の冪ごとに部分和を畳む。追加バッファは O(log N) で済む
//   float32 + Neumaier … 補正項を持つ。アキュムレータと同サイズの追加バッファが要る
//   float64 逐次加算   … 精度は最良だがメモリが2倍
//
// 判定基準: 16bit出力の1LSB = 1/65535 ≈ 1.526e-5。
// 平均値の誤差がこれを超えるなら、その方式は16bit出力で目に見える誤差を生む。
//
// ビルド:
//   c++ -std=c++17 -O2 tools/accum_precision.cpp -o /tmp/accum_precision
//   /tmp/accum_precision

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr double kLsb16 = 1.0 / 65535.0;

// 対加算。部分和を「同じ深さのものどうし」で畳むので、
// 和の大きさが揃ったまま加算され、誤差の成長が O(log N) に落ちる。
// 追加メモリは深さぶんのスタックだけ（画素あたり数十バイトではなく、
// 実装上は画素ごとに小さな配列）。
class PairwiseF32 {
public:
    void add(float v) {
        float carry = v;
        std::uint64_t mask = count_;
        std::size_t level = 0;
        while (mask & 1) {
            carry = stack_[level] + carry;
            ++level;
            mask >>= 1;
        }
        if (level >= stack_.size()) stack_.resize(level + 1, 0.0f);
        stack_[level] = carry;
        ++count_;
    }
    double total() const {
        // 端数の部分和を小さい方から足す。
        float s = 0.0f;
        for (std::size_t i = 0; i < stack_.size(); ++i) {
            if ((count_ >> i) & 1) s = s + stack_[i];
        }
        return static_cast<double>(s);
    }

private:
    std::vector<float> stack_;
    std::uint64_t count_ = 0;
};

// Neumaier（Kahan-Babuskaの改良版）。補正項を別に持つ。
class NeumaierF32 {
public:
    void add(float v) {
        const float t = sum_ + v;
        if (std::fabs(sum_) >= std::fabs(v)) {
            c_ += (sum_ - t) + v;
        } else {
            c_ += (v - t) + sum_;
        }
        sum_ = t;
    }
    double total() const { return static_cast<double>(sum_) + static_cast<double>(c_); }

private:
    float sum_ = 0.0f;
    float c_ = 0.0f;
};

struct Result {
    double naive32, pairwise32, neumaier32, naive64;
};

// 同一の値をN回加算したときの平均値の誤差。
// 受け入れ条件が指定している「同一フレームをN枚スタックする」ケース。
Result measure_identical(double value, int n) {
    float s32 = 0.0f;
    double s64 = 0.0;
    PairwiseF32 pw;
    NeumaierF32 nm;
    const float v32 = static_cast<float>(value);

    for (int i = 0; i < n; ++i) {
        s32 += v32;
        s64 += value;
        pw.add(v32);
        nm.add(v32);
    }
    const double inv = 1.0 / n;
    Result r;
    r.naive32 = std::fabs(static_cast<double>(s32) * inv - value);
    r.pairwise32 = std::fabs(pw.total() * inv - value);
    r.neumaier32 = std::fabs(nm.total() * inv - value);
    r.naive64 = std::fabs(s64 * inv - value);
    return r;
}

// ばらつきのある値を加算したときの誤差。
// 実際のスタックはフレームごとにノイズと輝度正規化のゲインが乗るため、
// 同一値の加算より現実に近い。解析解は float64 の Neumaier で作る。
Result measure_spread(double center, double spread, int n, std::uint32_t seed) {
    float s32 = 0.0f;
    double s64 = 0.0;
    PairwiseF32 pw;
    NeumaierF32 nm;

    // 参照値は float64 + Neumaier で求める（これを解析解の代用とする）。
    double ref_sum = 0.0, ref_c = 0.0;

    std::uint32_t rng = seed;
    for (int i = 0; i < n; ++i) {
        rng = rng * 1664525u + 1013904223u;
        const double u = static_cast<double>((rng >> 8) & 0xFFFF) / 65535.0;
        const double value = center + spread * (u - 0.5);
        const float v32 = static_cast<float>(value);

        s32 += v32;
        s64 += static_cast<double>(v32);
        pw.add(v32);
        nm.add(v32);

        const double t = ref_sum + static_cast<double>(v32);
        if (std::fabs(ref_sum) >= std::fabs(static_cast<double>(v32))) {
            ref_c += (ref_sum - t) + static_cast<double>(v32);
        } else {
            ref_c += (static_cast<double>(v32) - t) + ref_sum;
        }
        ref_sum = t;
    }

    const double truth = (ref_sum + ref_c) / n;
    const double inv = 1.0 / n;
    Result r;
    r.naive32 = std::fabs(static_cast<double>(s32) * inv - truth);
    r.pairwise32 = std::fabs(pw.total() * inv - truth);
    r.neumaier32 = std::fabs(nm.total() * inv - truth);
    r.naive64 = std::fabs(s64 * inv - truth);
    return r;
}

void print_row(const char* label, const Result& r) {
    std::printf("  %-16s %10.3e %10.3e %10.3e %10.3e\n", label, r.naive32 / kLsb16,
                r.pairwise32 / kLsb16, r.neumaier32 / kLsb16, r.naive64 / kLsb16);
}

}  // namespace

int main() {
    std::printf("加算アキュムレータの精度測定\n");
    std::printf("単位: 16bit出力の1LSB (= 1/65535 ≈ %.3e) を1とした比\n", kLsb16);
    std::printf("1.0 を超えると16bit出力で1階調以上ずれる\n\n");

    const int counts[] = {100, 1000, 10000, 30000};

    std::printf("【1】同一の値を N 回加算（受け入れ条件が指定するケース）\n");
    for (double v : {0.25, 0.5, 0.9}) {
        std::printf("\n  値 = %.2f\n", v);
        std::printf("  %-16s %10s %10s %10s %10s\n", "N", "f32逐次", "f32対加算", "f32補正",
                    "f64逐次");
        for (int n : counts) {
            char label[32];
            std::snprintf(label, sizeof(label), "%d", n);
            print_row(label, measure_identical(v, n));
        }
    }

    std::printf("\n【2】ばらつきのある値を N 回加算（実際のスタックに近い）\n");
    std::printf("  中心 0.5、振れ幅 ±0.1\n");
    std::printf("  %-16s %10s %10s %10s %10s\n", "N", "f32逐次", "f32対加算", "f32補正",
                "f64逐次");
    for (int n : counts) {
        char label[32];
        std::snprintf(label, sizeof(label), "%d", n);
        print_row(label, measure_spread(0.5, 0.2, n, 12345u));
    }

    // ---- 2段階加算 -------------------------------------------------------
    // オーバーラップ窓合成の構造そのものを利用する方式。
    // 加算の大半は「1つのAP内で選択フレームを足し込む」ところで起きる。
    // APは64x64程度と小さいので、この局所バッファだけ float64 にしても
    // 数十KBしか増えない。全体バッファへの加算は、50%オーバーラップなら
    // 1画素あたりAP4枚ぶんの4回しか起きないので float32 で足りる。
    std::printf("\n【5】2段階加算（AP内 float64 → 全体 float32、1画素あたりAP4枚）\n");
    std::printf("  %-16s %10s %10s\n", "AP内のN", "全体誤差", "（参考）f32逐次");
    for (int n : counts) {
        const double v = 0.9;  // 最も誤差が出やすい側の値
        // AP内: float64 で n 回加算 → 平均
        double local = 0.0;
        for (int i = 0; i < n; ++i) local += v;
        const float local_mean = static_cast<float>(local / n);
        // 全体: 4回だけ float32 で加算
        float total = 0.0f;
        for (int i = 0; i < 4; ++i) total += local_mean;
        const double got = static_cast<double>(total) / 4.0;

        float naive = 0.0f;
        for (int i = 0; i < n * 4; ++i) naive += static_cast<float>(v);
        const double naive_mean = static_cast<double>(naive) / (n * 4);

        std::printf("  %-16d %10.3e %10.3e\n", n, std::fabs(got - v) / kLsb16,
                    std::fabs(naive_mean - v) / kLsb16);
    }

    std::printf("\n【3】最悪ケースの探索（同一値・N=30000で最も誤差が出る値）\n");
    double worst_v = 0.0, worst_err = 0.0;
    for (int i = 1; i < 1000; ++i) {
        const double v = i / 1000.0;
        const double e = measure_identical(v, 30000).naive32;
        if (e > worst_err) {
            worst_err = e;
            worst_v = v;
        }
    }
    std::printf("  値 %.3f のとき f32逐次 の誤差 %.3f LSB\n", worst_v, worst_err / kLsb16);

    std::printf("\n【4】メモリ（出力バッファ S と重み W の合計）\n");
    struct Case {
        const char* name;
        long long w, h;
    };
    const Case cases[] = {{"1080p x1", 1920, 1080},
                          {"4K x1", 3840, 2160},
                          {"4K x2 Drizzle", 7680, 4320},
                          {"4K x3 Drizzle", 11520, 6480}};
    std::printf("  %-16s %12s %12s %12s\n", "", "f32 (S+W)", "f32+補正", "f64 (S+W)");
    for (const Case& c : cases) {
        const double px = static_cast<double>(c.w) * c.h;
        const double f32 = (px * 3 * 4 + px * 4) / 1e9;
        const double f32c = (px * 3 * 4 * 2 + px * 4) / 1e9;
        const double f64 = (px * 3 * 8 + px * 8) / 1e9;
        std::printf("  %-16s %9.2f GB %9.2f GB %9.2f GB\n", c.name, f32, f32c, f64);
    }
    std::printf("\n  ワーキングセット上限: 既定4GB / 設定で2GBまで引き下げ可（仕様書 §7.3）\n");
    return 0;
}
