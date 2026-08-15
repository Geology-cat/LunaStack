// 全フレームを順に読んでmmapの挙動とスループットを測る使い捨てツール。
#include <chrono>
#include <cstdio>
#include <stdexcept>

#include "stackcore/ser_decoder.hpp"

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: sweep <file.ser>\n"); return 2; }
    try {
        stackcore::SerDecoder dec;
        dec.open(argv[1]);
        const int n = dec.frame_count();
        stackcore::FrameBuffer fb;

        double global_min = 1e9, global_max = -1e9, mean_sum = 0.0;
        std::int64_t first_ts = 0, last_ts = 0;
        const auto t0 = std::chrono::steady_clock::now();

        for (int i = 0; i < n; ++i) {
            dec.read_frame(i, fb);
            const auto s = dec.frame_stats(i);
            if (s.min_value < global_min) global_min = s.min_value;
            if (s.max_value > global_max) global_max = s.max_value;
            mean_sum += s.mean_value;
            if (dec.has_timestamps()) {
                if (i == 0) first_ts = dec.timestamp_ticks(0);
                if (i == n - 1) last_ts = dec.timestamp_ticks(i);
            }
        }

        const auto t1 = std::chrono::steady_clock::now();
        const double sec = std::chrono::duration<double>(t1 - t0).count();
        const double bytes = static_cast<double>(dec.frame_bytes()) * n;

        std::printf("全 %d フレーム読み込み成功\n", n);
        std::printf("  所要 %.2f 秒 / %.2f GB → %.1f MB/s, %.0f フレーム/秒\n",
                    sec, bytes / 1e9, bytes / 1e6 / sec, n / sec);
        std::printf("  生サンプル値 min=%.0f max=%.0f, 平均の平均=%.2f\n",
                    global_min, global_max, mean_sum / n);
        if (first_ts && last_ts) {
            const double span = static_cast<double>(last_ts - first_ts) / 1e7;
            std::printf("  タイムスタンプ長 %.2f 秒（%.1f fps 相当）\n", span, (n - 1) / span);
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "エラー: %s\n", e.what());
        return 1;
    }
}
