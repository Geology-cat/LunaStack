// LunaStack CLI
//
// エンジン（libstackcore）をコマンドラインから駆動する。
// GUIより先にこちらを完成させ、各段の出力を数値と画像で検証できるようにする
// （実装計画書 §1「エンジン先行・CLIファースト」）。

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <memory>
#include <utility>
#include <string>
#include <vector>

#include "stackcore/debayer.hpp"
#include "stackcore/frame_selector.hpp"
#include "stackcore/fits_writer.hpp"
#include "stackcore/global_aligner.hpp"
#include "stackcore/quality.hpp"
#include "stackcore/ser_decoder.hpp"
#include "stackcore/simple_stacker.hpp"
#include "stackcore/tiff_writer.hpp"
#include "stackcore/avi_decoder.hpp"
#include "stackcore/video_source.hpp"
#include "stackcore/map_pipeline.hpp"
#include "stackcore/png_writer.hpp"
#include "stackcore/sidecar.hpp"
#include "stackcore/drizzle.hpp"
#include "stackcore/wavelet.hpp"

namespace {

using stackcore::AlignMode;
using stackcore::ByteOrder;
using stackcore::FrameBuffer;
using stackcore::SerColorId;
using stackcore::SerDecoder;
using stackcore::VideoSource;

const char* const kVersion = "0.8.0 (段階処理・32bit FITS出力)";

struct Options {
    std::string command;
    std::string input;
    std::string output;
    int frame = 0;
    int bit_depth = 0;  // 0 = ヘッダの値を使う
    ByteOrder endian = ByteOrder::Auto;
    bool as_float = false;
    bool raw_cfa = false;

    // stack コマンド用
    double top_percent = 25.0;
    AlignMode mode = AlignMode::Auto;
    double min_similarity = -1.0;  // 負なら既定値を使う
    int max_shift = 0;             // 0で自動
    int limit = 0;                 // 0で全フレーム
    double outlier_k = 6.0;        // 類似度の外れ値判定（中央値からの σ 倍）
    stackcore::QualityMetric quality_metric = stackcore::QualityMetric::GradientEnergy;

    // mapstack コマンド用
    int ap_size = 0;               // 0で自動提案
    int search_radius = 16;
    double ap_top_percent = 10.0;
    int ap_top_count = 0;
    double ap_gradient_ratio = 0.6;
    double ap_level_ratio = 0.15;
    double min_score = 0.5;
    int reference_passes = 2;
    std::string sidecar;      // 保存/読み込み先（空で使わない）
    bool reuse_sidecar = false;  // 既存のサイドカーから再スタックする
    bool low_memory = false;
    bool normalize_brightness = true;
    stackcore::StackMode stack_mode = stackcore::StackMode::Mean;
    double sigma_clip_threshold = 2.0;

    // 後処理（M6）
    std::string sharpen;   // レイヤーごとの係数をカンマ区切りで。空で無効
    std::string denoise;   // 同上
    int wavelet_layers = 6;
    double stretch_black = -1.0;  // 負で無効
    double stretch_white = 1.0;
    double stretch_gamma = 1.0;

    // Drizzle（M5）
    double drizzle = 1.0;
    double pixfrac = 0.9;
};

bool ends_with_ci(const std::string& value, const char* suffix) {
    const std::size_t length = std::strlen(suffix);
    if (value.size() < length) return false;
    for (std::size_t i = 0; i < length; ++i) {
        char a = value[value.size() - length + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

bool output_is_png(const Options& opts) { return ends_with_ci(opts.output, ".png"); }
bool output_is_fits(const Options& opts) {
    return ends_with_ci(opts.output, ".fit") || ends_with_ci(opts.output, ".fits");
}

const char* output_format_name(const Options& opts) {
    if (output_is_png(opts)) return "16bit PNG";
    if (output_is_fits(opts)) return "32bit float FITS";
    return opts.as_float ? "32bit float TIFF" : "16bit TIFF";
}

void write_output_image(const Options& opts, const FrameBuffer& image) {
    if (output_is_fits(opts)) {
        stackcore::write_fits_float32(opts.output, image);
        return;
    }
    if (output_is_png(opts)) {
        if (opts.as_float) {
            throw std::invalid_argument("PNGと--floatは同時に指定できません（PNGは16bit整数です）");
        }
        stackcore::write_png16(opts.output, image);
        return;
    }
    const stackcore::TiffFormat format =
        opts.as_float ? stackcore::TiffFormat::Float32 : stackcore::TiffFormat::UInt16;
    stackcore::write_tiff(opts.output, image, format);
}

// "1.5,1.3,1.0" のような文字列をレイヤーごとの値に直す。
// 指定が足りない分は fill で埋める（後ろのレイヤーほど触らないのが普通なので）。
bool parse_layer_values(const std::string& text, int layers, double fill,
                        std::vector<double>& out) {
    out.assign(static_cast<std::size_t>(layers), fill);
    if (text.empty()) return true;
    std::size_t pos = 0;
    int index = 0;
    while (pos <= text.size() && index < layers) {
        const std::size_t comma = text.find(',', pos);
        const std::string token =
            text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (!token.empty()) {
            char* end = nullptr;
            const double v = std::strtod(token.c_str(), &end);
            if (end == nullptr || *end != '\0') return false;
            out[static_cast<std::size_t>(index)] = v;
        }
        ++index;
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return true;
}

// スタック結果に後処理を掛ける（仕様書 §4.10）。
// 何も指定がなければ何もしない。
bool apply_post_processing(const Options& opts, FrameBuffer& image) {
    const bool want_wavelet = !opts.sharpen.empty() || !opts.denoise.empty();
    const bool want_stretch = opts.stretch_black >= 0.0;
    if (!want_wavelet && !want_stretch) return true;

    if (want_wavelet) {
        std::vector<double> sharpen, denoise;
        if (!parse_layer_values(opts.sharpen, opts.wavelet_layers, 1.0, sharpen) ||
            !parse_layer_values(opts.denoise, opts.wavelet_layers, 0.0, denoise)) {
            std::fprintf(stderr, "エラー: --sharpen / --denoise はカンマ区切りの数値です\n");
            return false;
        }

        stackcore::WaveletSharpener w;
        w.analyze(image, opts.wavelet_layers);

        std::vector<stackcore::WaveletLayerParams> params(
            static_cast<std::size_t>(opts.wavelet_layers));
        std::printf("\n[ウェーブレット後処理] %d レイヤー\n", opts.wavelet_layers);
        for (int j = 0; j < opts.wavelet_layers; ++j) {
            params[static_cast<std::size_t>(j)].sharpen = sharpen[static_cast<std::size_t>(j)];
            params[static_cast<std::size_t>(j)].denoise = denoise[static_cast<std::size_t>(j)];
            std::printf("  レイヤー%d (約%dpx): sharpen %.2f / denoise %.2f / ノイズσ %.5f\n",
                        j, 1 << (j + 1), sharpen[static_cast<std::size_t>(j)],
                        denoise[static_cast<std::size_t>(j)], w.layer_noise(j));
        }

        FrameBuffer out;
        w.synthesize(params, out);
        out.set_source_bit_depth(image.source_bit_depth());
        image = std::move(out);
    }

    if (want_stretch) {
        FrameBuffer out;
        stackcore::stretch_histogram(image, opts.stretch_black, opts.stretch_white,
                                     opts.stretch_gamma, out);
        std::printf("\n[ヒストグラムストレッチ] 黒点 %.3f / 白点 %.3f / ガンマ %.2f\n",
                    opts.stretch_black, opts.stretch_white, opts.stretch_gamma);
        image = std::move(out);
    }
    return true;
}

void print_usage() {
    std::printf(
        "LunaStack CLI %s — 月・惑星スタッキングエンジン\n"
        "入力: SER v3 / AVI（非圧縮・MJPEG）。どちらも自前デコーダで読む\n"
        "\n"
        "使い方:\n"
        "  stackcli info <file.ser|file.avi> [オプション]\n"
        "      ヘッダと実測値を表示する（デコーダの診断用）\n"
        "\n"
        "  stackcli extract <file.ser|file.avi> -f <番号> -o <出力.tif|png|fits> [オプション]\n"
        "      指定フレームをTIFF・PNG・32bit float FITSに書き出す\n"
        "\n"
        "  stackcli stack <file.ser|file.avi> -o <出力.tif|fits> [オプション]\n"
        "      グローバルアライメント＋品質選択＋単純平均スタック (M1)\n"
        "      切り出しは整数変位のみ。サブピクセル補間はM2以降\n"
        "\n"
        "  stackcli mapstack <file.ser|file.avi> -o <出力.tif|fits> [オプション]\n"
        "      MAP局所アライメント＋オーバーラップ窓合成スタック (M2)\n"
        "      APごとに別のフレームを選ぶ spatial lucky imaging\n"
        "\n"
        "  stackcli version\n"
        "\n"
        "mapstack のオプション (stack のオプションも使える):\n"
        "  --ap-size <px>            APサイズ 32|48|64|96|128|200 (既定: 自動提案)\n"
        "  --ap-top <割合>           AP別に採用するフレームの割合 (既定: 10)\n"
        "  --ap-count <枚数>         割合ではなくAP別の採用枚数を直接指定\n"
        "  --quality gradient|frequency\n"
        "                            品質指標（既定: gradient）\n"
        "  --search-radius <px>      局所探索の半径 (既定: 16)\n"
        "  --min-score <値>          ZNCCの下限。下回るAPは近傍から補間 (既定: 0.5)\n"
        "  --ap-gradient <比>        AP採用の勾配しきい値。全体平均に対する比 (既定: 0.6)\n"
        "  --low-memory              読み終えたフレームのページをカーネルに返す\n"
        "                            最大RSSを抑える。低RAM機向け (仕様書 §7.3)\n"
        "  --no-normalize            輝度正規化を無効にする（既定は有効）\n"
        "  --stack-mode mean|weighted|sigma\n"
        "                            加算方式（単純平均／品質重み付き／σクリップ）\n"
        "  --sigma <値>              σクリップ閾値（既定: 2.0）\n"
        "  --sidecar <path.lstk>     解析結果をサイドカーに保存する\n"
        "  --reuse-sidecar           サイドカーから解析結果を読み、加算だけやり直す\n"
        "                            選択率(--ap-top)を変えて何度も試すときに使う\n"
        "  --ref-passes <N>          参照の反復精密化の回数 (既定: 2)\n"
        "                            1=精密化なし。2以上でMAPの結果を新しい参照にして通し直す\n"
        "  --ap-level <比>           AP採用の輝度しきい値。最大輝度に対する比 (既定: 0.15)\n"
        "\n"
        "Drizzle のオプション (stack / mapstack 共通、仕様書 §4.9):\n"
        "  --drizzle <倍率>          1.0(無効) / 1.5 / 2.0 / 3.0 (既定: 1.0)\n"
        "                            **アンダーサンプリングのときだけ効く。**\n"
        "                            焦点距離が十分長ければ解像度は上がらず、\n"
        "                            処理時間とメモリが増えるだけになる\n"
        "  --pixfrac <値>            入力画素を縮めてから落とす割合 (既定: 0.9)\n"
        "\n"
        "後処理のオプション (stack / mapstack 共通、仕様書 §4.10):\n"
        "  --sharpen <値,値,...>     ウェーブレットのレイヤー別Sharpen係数 (0〜3、既定1.0)\n"
        "                            細かいレイヤーから順に指定する。例: --sharpen 1.6,1.3,1.1\n"
        "  --denoise <値,値,...>     レイヤー別Denoise (0〜1、既定0)。例: --denoise 0.5,0.3\n"
        "  --wavelet-layers <N>      分解レイヤー数 (既定: 6)\n"
        "  --stretch <黒,白,ガンマ>  ヒストグラムストレッチ。例: --stretch 0.0,0.35,1.8\n"
        "\n"
        "stack のオプション:\n"
        "  --top <割合>              品質上位何%%を加算するか (既定: 25)\n"
        "  --mode auto|planet|lunar  対象の種類 (既定: auto)\n"
        "                            planet=閾値二値化＋重心で粗位置→位相相関\n"
        "                            lunar =全面の位相相関\n"
        "  --min-similarity <値>     参照との類似度(ZNCC)の下限。下回ると追跡失敗として除外\n"
        "  --max-shift <画素>        許容する変位の上限 (既定: 短辺の1/4)\n"
        "  --outlier-k <値>          類似度の外れ値判定の厳しさ (既定: 6.0)\n"
        "                            中央値から MAD 換算で何σ離れたら除外するか\n"
        "  --limit <N>               先頭Nフレームだけ処理する (動作確認用)\n"
        "\n"
        "共通オプション:\n"
        "  --endian auto|little|big  16bitサンプルのバイトオーダー (既定: auto)\n"
        "                            SERヘッダのフラグは信用できないため自動判定する\n"
        "  --bit-depth <N>           正規化に使うビット深度を上書きする\n"
        "                            「16bitと書いてあるが中身は12bit」への対処\n"
        "  -f, --frame <N>           フレーム番号 (0始まり)\n"
        "  -o, --output <path>       出力ファイル（.fits/.fit で32bit float FITS）\n"
        "      --float               32bit float TIFFで書き出す (既定は16bit)\n"
        "      --raw-cfa             Bayerをデバイヤーせず生のCFAのまま出力する\n",
        kVersion);
}

bool parse_int(const char* text, int& out) {
    char* end = nullptr;
    const long v = std::strtol(text, &end, 10);
    if (end == text || end == nullptr || *end != '\0') return false;
    out = static_cast<int>(v);
    return true;
}

long long file_size_bytes(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return -1;
    return static_cast<long long>(st.st_size);
}

std::string human_size(long long bytes) {
    if (bytes < 0) return "不明";
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.2f %s", v, units[u]);
    return std::string(buf);
}

// SERのタイムスタンプは .NET の tick（0001-01-01 からの100ns単位）。
std::string format_ticks(std::int64_t ticks) {
    if (ticks <= 0) return "(未設定)";
    const std::int64_t kTicksAtUnixEpoch = 621355968000000000LL;
    const std::int64_t unix_100ns = ticks - kTicksAtUnixEpoch;
    const std::time_t t = static_cast<std::time_t>(unix_100ns / 10000000LL);
    std::tm tm_buf;
    if (::gmtime_r(&t, &tm_buf) == nullptr) return "(不正な値)";
    char buf[64];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf) == 0) return "(不正な値)";
    return std::string(buf);
}

int bit_width_of(std::uint32_t max_value) {
    int bits = 0;
    while (max_value > 0) {
        ++bits;
        max_value >>= 1;
    }
    return bits;
}

const char* byte_order_name(ByteOrder order) {
    switch (order) {
        case ByteOrder::Little: return "little";
        case ByteOrder::Big:    return "big";
        case ByteOrder::Auto:   return "auto";
    }
    return "?";
}

void print_histogram(const FrameBuffer& frame) {
    const int kBins = 16;
    long counts[16] = {0};
    long total = 0;
    for (int c = 0; c < frame.channels(); ++c) {
        for (int y = 0; y < frame.height(); ++y) {
            const float* row = frame.row(c, y);
            for (int x = 0; x < frame.width(); ++x) {
                float v = row[x];
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                int bin = static_cast<int>(v * kBins);
                if (bin >= kBins) bin = kBins - 1;
                counts[bin] += 1;
                total += 1;
            }
        }
    }
    if (total == 0) return;

    long peak = 1;
    for (int i = 0; i < kBins; ++i) {
        if (counts[i] > peak) peak = counts[i];
    }

    std::printf("\n[輝度ヒストグラム] 正規化後 0.0〜1.0 を16分割\n");
    for (int i = 0; i < kBins; ++i) {
        const int bar = static_cast<int>(40.0 * static_cast<double>(counts[i]) /
                                         static_cast<double>(peak));
        std::printf("  %4.2f-%4.2f |", static_cast<double>(i) / kBins,
                    static_cast<double>(i + 1) / kBins);
        for (int k = 0; k < bar; ++k) std::printf("#");
        std::printf(" %5.1f%%\n",
                    100.0 * static_cast<double>(counts[i]) / static_cast<double>(total));
    }
}

// AVI用の info。SERとは持っている情報が違うので表示も分けている。
int command_info_avi(const Options& opts) {
    stackcore::AviDecoder decoder;
    decoder.open(opts.input);
    const stackcore::AviDecoder::Header& h = decoder.header();

    std::printf("ファイル: %s\n", opts.input.c_str());
    std::printf("  サイズ: %s\n", human_size(file_size_bytes(opts.input)).c_str());

    std::printf("\n[ヘッダ]\n");
    std::printf("  形式          : AVI（自前RIFFパーサ。OS付属デコーダは使わない）\n");
    std::printf("  圧縮          : %s\n", h.compression_name.c_str());
    std::printf("  fccHandler    : %s\n", h.handler.c_str());
    std::printf("  画像サイズ    : %d x %d\n", h.width, h.height);
    std::printf("  ビット深度    : %d bit/pixel → %s %d bit/sample\n", h.bit_count,
                stackcore::to_string(decoder.color_id()), decoder.bit_depth());
    std::printf("  格納の向き    : %s (biHeight %s)\n", h.top_down ? "上から下" : "下から上",
                h.top_down ? "負またはFourCC形式" : "正");
    if (decoder.is_mjpeg()) {
        std::printf("  1行のバイト数 : 該当なし（フレーム単位のJPEG圧縮）\n");
    } else {
        std::printf("  1行のバイト数 : %zu\n", h.row_bytes);
    }
    std::printf("  フレーム数    : %d\n", decoder.frame_count());
    std::printf("  フレームレート: %.3f fps\n", h.fps);
    std::printf("  1フレーム     : %s\n",
                human_size(static_cast<long long>(decoder.frame_bytes(0))).c_str());

    std::printf("\n[サンプルフレームの生サンプル値]\n");
    const int last = decoder.frame_count() - 1;
    int indices[3] = {0, last / 2, last};
    for (int i = 0; i < 3; ++i) {
        if (i > 0 && indices[i] == indices[i - 1]) continue;
        const stackcore::FrameStats s = decoder.frame_stats(indices[i]);
        std::printf("  frame %6d: min=%5u  max=%5u  mean=%8.1f  実効ビット幅=%d\n", indices[i],
                    s.min_value, s.max_value, s.mean_value, bit_width_of(s.max_value));
    }

    FrameBuffer frame;
    decoder.read_frame(last / 2, frame);
    print_histogram(frame);
    return 0;
}

int command_info(const Options& opts) {
    SerDecoder decoder;
    decoder.open(opts.input, opts.endian);
    if (opts.bit_depth > 0) decoder.set_bit_depth_override(opts.bit_depth);

    const stackcore::SerHeader& h = decoder.header();

    std::printf("ファイル: %s\n", opts.input.c_str());
    std::printf("  サイズ: %s\n", human_size(file_size_bytes(opts.input)).c_str());

    std::printf("\n[ヘッダ]\n");
    std::printf("  識別子        : %s%s\n", h.file_id.c_str(),
                h.file_id == "LUCAM-RECORDER" ? "" : "  ← 標準の識別子と異なります");
    std::printf("  カラー形式    : %s (%d)%s\n", stackcore::to_string(h.color_id),
                static_cast<int>(h.color_id),
                stackcore::is_bayer(h.color_id)
                    ? (stackcore::is_supported_bayer(h.color_id) ? "  [Bayer]"
                                                                 : "  [Bayer・未対応パターン]")
                    : "");
    std::printf("  画像サイズ    : %d x %d\n", h.width, h.height);
    std::printf("  ビット深度    : %d bit/plane  (%d バイト/サンプル, %d プレーン)\n",
                h.pixel_depth, decoder.bytes_per_sample(), decoder.planes());
    std::printf("  フレーム数    : %d\n", h.frame_count);
    std::printf("  1フレーム     : %s\n",
                human_size(static_cast<long long>(decoder.frame_bytes())).c_str());
    if (!h.observer.empty())   std::printf("  観測者        : %s\n", h.observer.c_str());
    if (!h.instrument.empty()) std::printf("  機材          : %s\n", h.instrument.c_str());
    if (!h.telescope.empty())  std::printf("  望遠鏡        : %s\n", h.telescope.c_str());
    std::printf("  撮影日時(UTC) : %s\n", format_ticks(h.datetime_utc).c_str());
    std::printf("  タイムスタンプ: %s\n", decoder.has_timestamps() ? "あり" : "なし");

    if (decoder.bytes_per_sample() == 2) {
        std::printf("\n[バイトオーダー]\n");
        std::printf("  ヘッダの主張  : %s\n", h.header_little_endian ? "little" : "big");
        std::printf("  採用          : %s%s\n", byte_order_name(decoder.resolved_byte_order()),
                    opts.endian == ByteOrder::Auto ? " (自動判定)" : " (手動指定)");
        if (decoder.byte_order_differs_from_header()) {
            std::printf("  ※ ヘッダの主張と食い違っています。これは珍しくありません\n"
                        "     （キャプチャソフト間でこのフラグの解釈が割れているため）。\n"
                        "     画像が破綻して見える場合は --endian で明示指定してください。\n");
        }
    }

    // 先頭・中央・末尾のフレームを実測する。
    std::vector<int> sample_indices;
    sample_indices.push_back(0);
    if (h.frame_count > 2) sample_indices.push_back(h.frame_count / 2);
    if (h.frame_count > 1) sample_indices.push_back(h.frame_count - 1);

    std::printf("\n[サンプルフレームの生サンプル値]\n");
    std::uint32_t overall_max = 0;
    for (std::size_t i = 0; i < sample_indices.size(); ++i) {
        const int index = sample_indices[i];
        const stackcore::FrameStats s = decoder.frame_stats(index);
        overall_max = s.max_value > overall_max ? s.max_value : overall_max;
        std::printf("  frame %6d: min=%5u  max=%5u  mean=%8.1f  実効ビット幅=%d\n", index,
                    s.min_value, s.max_value, s.mean_value, bit_width_of(s.max_value));
    }

    const int observed_bits = bit_width_of(overall_max);
    if (decoder.bytes_per_sample() == 2 && h.pixel_depth >= 13 && overall_max > 0 &&
        observed_bits <= 12) {
        std::printf(
            "\n  ⚠ ヘッダは %d bit と主張していますが、実測値は %d bit 幅に収まっています。\n"
            "     12bitデータが16bitコンテナに入っている可能性があります。\n"
            "     画像が暗すぎる場合は --bit-depth %d を試してください。\n",
            h.pixel_depth, observed_bits, observed_bits <= 12 ? 12 : observed_bits);
    }

    FrameBuffer frame;
    decoder.read_frame(sample_indices.empty() ? 0 : sample_indices[sample_indices.size() / 2],
                       frame);
    print_histogram(frame);

    return 0;
}

int command_extract(const Options& opts) {
    if (opts.output.empty()) {
        std::fprintf(stderr, "エラー: 出力ファイルを -o で指定してください\n");
        return 2;
    }

    stackcore::OpenOptions open_opts;
    open_opts.endian = opts.endian;
    open_opts.bit_depth_override = opts.bit_depth;
    const std::unique_ptr<VideoSource> source = stackcore::open_video(opts.input, open_opts);

    if (opts.frame < 0 || opts.frame >= source->frame_count()) {
        std::fprintf(stderr, "エラー: フレーム番号 %d は範囲外です (0..%d)\n", opts.frame,
                     source->frame_count() - 1);
        return 2;
    }

    FrameBuffer frame;
    source->read_frame(opts.frame, frame);

    const SerColorId color = source->color_id();
    const bool want_debayer =
        !opts.raw_cfa && stackcore::is_bayer(color) && stackcore::is_supported_bayer(color);

    FrameBuffer rgb;
    const FrameBuffer* to_write = &frame;
    if (want_debayer) {
        stackcore::debayer_bilinear(frame, color, rgb);
        to_write = &rgb;
    } else if (!opts.raw_cfa && stackcore::is_bayer(color)) {
        std::fprintf(stderr,
                     "注意: %s は未対応のBayerパターンのため、生のCFAのまま出力します\n",
                     stackcore::to_string(color));
    }

    write_output_image(opts, *to_write);

    std::printf("書き出しました: %s\n", opts.output.c_str());
    std::printf("  入力形式      : %s\n", source->format_name());
    std::printf("  入力の詳細    : %s\n", source->describe().c_str());
    std::printf("  フレーム      : %d / %d\n", opts.frame, source->frame_count());
    std::printf("  画像          : %d x %d x %dch\n", to_write->width(), to_write->height(),
                to_write->channels());
    std::printf("  形式          : %s\n", output_format_name(opts));
    std::printf("  正規化深度    : %d bit\n", source->bit_depth());
    if (want_debayer) {
        std::printf("  デバイヤー    : bilinear (%s)\n", stackcore::to_string(color));
    }
    return 0;
}

// SERから1フレーム読み、必要ならデバイヤーする。
// 結果が入っているバッファ（cfa か rgb のどちらか）を返す。
// バッファは呼び出し側で使い回してもらうため、毎フレーム確保しなおさない。
const FrameBuffer* read_prepared(const VideoSource& source, int index, bool raw_cfa,
                                 FrameBuffer& cfa, FrameBuffer& rgb) {
    source.read_frame(index, cfa);
    const SerColorId color = source.color_id();
    if (!raw_cfa && stackcore::is_bayer(color) && stackcore::is_supported_bayer(color)) {
        stackcore::debayer_bilinear(cfa, color, rgb);
        return &rgb;
    }
    return &cfa;
}

struct FrameMeta {
    int index = 0;
    double quality = 0.0;
    double mean = 0.0;
    int dx = 0;
    int dy = 0;
    double similarity = 0.0;
    bool accepted = false;
    stackcore::RejectReason reason = stackcore::RejectReason::None;
};

void print_progress(const char* label, int done, int total) {
    if (total <= 0) return;
    const int step = total < 10 ? 1 : total / 10;
    if (done % step != 0 && done != total) return;
    std::printf("\r  %s %d/%d (%d%%)", label, done, total, done * 100 / total);
    std::fflush(stdout);
}

int command_stack(const Options& opts) {
    if (opts.output.empty()) {
        std::fprintf(stderr, "エラー: 出力ファイルを -o で指定してください\n");
        return 2;
    }
    if (opts.stack_mode != stackcore::StackMode::Mean) {
        std::fprintf(stderr,
                     "エラー: --stack-mode weighted|sigma は mapstack で使用してください\n");
        return 2;
    }
    if (!(opts.top_percent > 0.0 && opts.top_percent <= 100.0)) {
        std::fprintf(stderr, "エラー: --top は 0 より大きく 100 以下の値です\n");
        return 2;
    }

    stackcore::OpenOptions open_opts;
    open_opts.endian = opts.endian;
    open_opts.bit_depth_override = opts.bit_depth;
    const std::unique_ptr<VideoSource> source = stackcore::open_video(opts.input, open_opts);
    if (opts.low_memory) source->set_low_memory(true);

    const int total = opts.limit > 0 && opts.limit < source->frame_count()
                          ? opts.limit
                          : source->frame_count();
    if (total < 1) {
        std::fprintf(stderr, "エラー: 処理するフレームがありません\n");
        return 2;
    }

    std::printf("入力: %s\n", opts.input.c_str());
    std::printf("  形式: %s (%s)\n", source->format_name(), source->describe().c_str());
    std::printf("  %d x %d, %s, %d bit, %d フレーム中 %d フレームを処理\n", source->width(),
                source->height(), stackcore::to_string(source->color_id()),
                source->bit_depth(), source->frame_count(), total);

    FrameBuffer cfa, rgb;
    stackcore::QualityWorkspace ws;
    std::vector<FrameMeta> meta(static_cast<std::size_t>(total));

    // --- パス1: 全フレームの品質と平均輝度 --------------------------------
    // 参照フレームを「最も品質の高いフレーム」にしたいので、
    // アライメントより先に品質を知る必要がある。
    std::printf("[1/3] 品質評価\n");
    for (int i = 0; i < total; ++i) {
        const FrameBuffer* f = read_prepared(*source, i, opts.raw_cfa, cfa, rgb);
        FrameMeta& m = meta[static_cast<std::size_t>(i)];
        m.index = i;
        m.quality = stackcore::quality_score(*f, opts.quality_metric, ws);
        m.mean = stackcore::mean_luma(*f);
        print_progress("フレーム", i + 1, total);
    }
    std::printf("\n");

    // 参照フレームは「品質が中央値のフレーム」にする。
    //
    // 最高品質のフレームを選んではいけない。勾配エネルギーは「シャープさ」と
    // 「引き裂かれた段差」を区別できず、テアリングを起こした壊れフレームが
    // 最高スコアを取るためである。実データ（木星SER 4617フレーム）では
    // 品質上位20フレームがすべて構造的な異常フレームであり、
    // 最高品質を参照にすると壊れたフレームを基準に全体を合わせることになった。
    // 中央値なら定義上ふつうのフレームであり、グローバルアライメントは
    // 平行移動しか求めないので参照の鋭さは推定精度にほとんど影響しない。
    std::vector<double> sorted_quality;
    sorted_quality.reserve(meta.size());
    for (std::size_t i = 0; i < meta.size(); ++i) sorted_quality.push_back(meta[i].quality);
    std::sort(sorted_quality.begin(), sorted_quality.end());
    const double median_quality = sorted_quality[sorted_quality.size() / 2];

    int ref_index = 0;
    double best_distance = -1.0;
    for (int i = 0; i < total; ++i) {
        const double d = std::fabs(meta[static_cast<std::size_t>(i)].quality - median_quality);
        if (best_distance < 0.0 || d < best_distance) {
            best_distance = d;
            ref_index = i;
        }
    }
    const double ref_mean = meta[static_cast<std::size_t>(ref_index)].mean;
    std::printf("  参照フレーム: %d (品質 %.6g = 中央値)\n", ref_index,
                meta[static_cast<std::size_t>(ref_index)].quality);

    // --- パス2: 全フレームのグローバルアライメント ------------------------
    FrameBuffer ref_cfa, ref_rgb;
    const FrameBuffer* reference =
        read_prepared(*source, ref_index, opts.raw_cfa, ref_cfa, ref_rgb);

    stackcore::GlobalAlignSettings settings;
    settings.mode = opts.mode;
    settings.max_shift = opts.max_shift;
    if (opts.min_similarity >= 0.0) settings.min_similarity = opts.min_similarity;

    stackcore::GlobalAligner aligner(*reference, settings);
    std::printf("[2/3] グローバルアライメント (モード: %s, 変位上限: %d px, "
                "類似度下限: %.1f)\n",
                stackcore::to_string(aligner.mode()), aligner.max_shift(),
                settings.min_similarity);

    int rejected_correlation = 0, rejected_shift = 0;
    for (int i = 0; i < total; ++i) {
        const FrameBuffer* f = read_prepared(*source, i, opts.raw_cfa, cfa, rgb);
        const stackcore::GlobalAlignResult r = aligner.align(*f);
        FrameMeta& m = meta[static_cast<std::size_t>(i)];
        m.dx = r.dx;
        m.dy = r.dy;
        m.similarity = r.similarity;
        m.accepted = r.accepted;
        m.reason = r.reason;
        if (!r.accepted) {
            if (r.reason == stackcore::RejectReason::LowCorrelation) ++rejected_correlation;
            else ++rejected_shift;
        }
        print_progress("フレーム", i + 1, total);
    }
    std::printf("\n");

    // --- 構造的な外れ値の除外 ---------------------------------------------
    // 類似度の絶対値だけでは足りない。テアリングを起こしたフレームは
    // 「参照とまるで違う」わけではなく（実測ZNCC 0.72〜0.985）、
    // 絶対しきい値を安全側に置くと素通りする。
    // 一方で正常フレームの類似度は極めて狭い範囲に固まる（実測 0.993〜1.000）ため、
    // **分布からの外れ具合**で見れば明確に分離できる。
    // 中央絶対偏差(MAD)を使うのは、外れ値自身に引きずられない散らばりの尺度だから。
    //
    // なぜこれが必要か: 勾配エネルギーは「シャープさ」と「引き裂かれた段差」を
    // 区別できず、壊れたフレームほど高いスコアを付ける。実データでは
    // 品質上位20フレームがすべて構造的な異常フレームだった。
    // つまり品質順の選択は、この網がないと壊れたフレームから順に採ってしまう。
    int rejected_outlier = 0;
    {
        std::vector<double> sims;
        sims.reserve(meta.size());
        for (std::size_t i = 0; i < meta.size(); ++i) {
            if (meta[i].accepted) sims.push_back(meta[i].similarity);
        }
        const double threshold =
            stackcore::similarity_outlier_threshold(sims, opts.outlier_k);
        if (threshold >= 0.0) {
            std::vector<double> tmp = sims;
            std::sort(tmp.begin(), tmp.end());
            std::printf("  類似度: 中央値 %.4f, 外れ値しきい値 %.4f (中央値 - %.1f σ)\n",
                        tmp[tmp.size() / 2], threshold, opts.outlier_k);
            for (std::size_t i = 0; i < meta.size(); ++i) {
                if (meta[i].accepted && meta[i].similarity < threshold) {
                    meta[i].accepted = false;
                    meta[i].reason = stackcore::RejectReason::StructuralOutlier;
                    ++rejected_outlier;
                }
            }
        }
    }

    std::printf("  除外: 類似度不足 %d 件 / 変位超過 %d 件 / 構造的外れ値 %d 件\n",
                rejected_correlation, rejected_shift, rejected_outlier);

    // --- 選択 -------------------------------------------------------------
    // 追跡に成功したフレームだけを品質降順に並べ、その上位N%を採る。
    // 「全体のN%」ではなく「採用可能なフレームのN%」にすることで、
    // 追跡失敗が多い動画でも加算枚数が痩せない。
    std::vector<FrameMeta> usable;
    usable.reserve(meta.size());
    for (std::size_t i = 0; i < meta.size(); ++i) {
        if (meta[i].accepted) usable.push_back(meta[i]);
    }
    if (usable.empty()) {
        std::fprintf(stderr,
                     "エラー: 追跡に成功したフレームが1枚もありません。"
                     "--mode や --min-similarity を見直してください\n");
        return 1;
    }

    // 品質降順。同点はフレーム番号昇順で決める（決定論性の要件）。
    std::sort(usable.begin(), usable.end(), [](const FrameMeta& a, const FrameMeta& b) {
        if (a.quality != b.quality) return a.quality > b.quality;
        return a.index < b.index;
    });

    int keep = static_cast<int>(usable.size() * opts.top_percent / 100.0 + 0.5);
    if (keep < 1) keep = 1;
    if (keep > static_cast<int>(usable.size())) keep = static_cast<int>(usable.size());
    usable.resize(static_cast<std::size_t>(keep));

    // 加算はフレーム番号の昇順に固定する。float64でも加算は非結合であり、
    // 順序が変わると結果がビット単位で変わって再現性を失う（実装計画書 §4.2）。
    std::sort(usable.begin(), usable.end(),
              [](const FrameMeta& a, const FrameMeta& b) { return a.index < b.index; });

    std::printf("  採用可能 %d フレーム中、品質上位 %.1f%% の %d フレームを加算\n",
                static_cast<int>(meta.size()) - rejected_correlation - rejected_shift -
                    rejected_outlier,
                opts.top_percent, keep);

    // --- パス3: 加算 ------------------------------------------------------
    const bool use_drizzle = opts.drizzle > 1.0001 || opts.drizzle < 0.9999;
    std::printf("[3/3] スタック%s\n",
                use_drizzle ? "（Drizzle）" : "");

    FrameBuffer result;
    stackcore::StackStats stats;
    stackcore::DrizzleStats dstats;

    if (use_drizzle) {
        stackcore::Drizzle drizzle(reference->width(), reference->height(),
                                   reference->channels(), opts.drizzle, opts.pixfrac);
        for (std::size_t i = 0; i < usable.size(); ++i) {
            const FrameMeta& m = usable[i];
            const FrameBuffer* f = read_prepared(*source, m.index, opts.raw_cfa, cfa, rgb);
            const double gain = opts.normalize_brightness && m.mean > 1e-9
                                    ? ref_mean / m.mean
                                    : 1.0;
            // グローバル変位は整数だが、Drizzleは小数変位を受け付ける。
            // M1のグローバル段が整数までしか出さないので、ここでは整数のまま渡す。
            // 実際にDrizzleが効くのは、フレームごとの変位が
            // サブピクセルで散っている場合である。
            drizzle.add(*f, -static_cast<double>(m.dx), -static_cast<double>(m.dy), gain);
            print_progress("フレーム", static_cast<int>(i) + 1, keep);
        }
        std::printf("\n");
        drizzle.finish(result, dstats);
        result.set_source_bit_depth(reference->source_bit_depth());
    } else {
        stackcore::SimpleStacker stacker(reference->width(), reference->height(),
                                         reference->channels());
        for (std::size_t i = 0; i < usable.size(); ++i) {
            const FrameMeta& m = usable[i];
            const FrameBuffer* f = read_prepared(*source, m.index, opts.raw_cfa, cfa, rgb);
            // 輝度正規化。参照フレームの明るさに揃える。
            const double gain = opts.normalize_brightness && m.mean > 1e-9
                                    ? ref_mean / m.mean
                                    : 1.0;
            stacker.add(*f, m.dx, m.dy, gain);
            print_progress("フレーム", static_cast<int>(i) + 1, keep);
        }
        std::printf("\n");
        stacker.finish(result, stats);
        result.set_source_bit_depth(reference->source_bit_depth());
    }

    if (!apply_post_processing(opts, result)) return 2;

    write_output_image(opts, result);

    std::printf("\n書き出しました: %s\n", opts.output.c_str());
    std::printf("  画像          : %d x %d x %dch (%s)\n", result.width(), result.height(),
                result.channels(), output_format_name(opts));
    std::printf("  加算枚数      : %d\n", stats.frames);
    std::printf("  画素あたり加算: 最小 %u / 最大 %u\n", stats.min_coverage,
                stats.max_coverage);
    std::printf("  平均後の最大値: %.4f\n", stats.max_value);
    if (stats.clipped > 0) {
        std::printf("  ※ %zu 画素が1.0を超えて切り詰められました"
                    "（輝度正規化が過剰な可能性があります）\n",
                    stats.clipped);
    }
    std::printf("\n  注意: M1の切り出しは整数変位のみです。サブピクセル補間（Lanczos3）は\n"
                "  M2以降の担当であり、その分だけ原理的にぼけます（実装計画書 §4.3）。\n");
    return 0;
}

int command_mapstack(const Options& opts) {
    if (opts.output.empty()) {
        std::fprintf(stderr, "エラー: 出力ファイルを -o で指定してください\n");
        return 2;
    }

    stackcore::OpenOptions open_opts;
    open_opts.endian = opts.endian;
    open_opts.bit_depth_override = opts.bit_depth;
    const std::unique_ptr<VideoSource> source = stackcore::open_video(opts.input, open_opts);
    if (opts.low_memory) source->set_low_memory(true);

    std::printf("入力: %s\n", opts.input.c_str());
    std::printf("  形式: %s (%s)\n", source->format_name(), source->describe().c_str());
    std::printf("  %d x %d, %s, %d bit, %d フレーム%s\n", source->width(), source->height(),
                stackcore::to_string(source->color_id()), source->bit_depth(),
                source->frame_count(), opts.low_memory ? " [低メモリモード]" : "");

    stackcore::MapStackSettings settings;
    settings.global.align.mode = opts.mode;
    settings.global.align.max_shift = opts.max_shift;
    if (opts.min_similarity >= 0.0) settings.global.align.min_similarity = opts.min_similarity;
    settings.global.outlier_k = opts.outlier_k;
    settings.global.limit = opts.limit;
    settings.global.quality_metric = opts.quality_metric;
    settings.reference_top_percent = opts.top_percent;
    settings.ap.ap_size = opts.ap_size;
    settings.ap.gradient_ratio = opts.ap_gradient_ratio;
    settings.ap.level_ratio = opts.ap_level_ratio;
    settings.local.search_radius = opts.search_radius;
    settings.local.min_score = opts.min_score;
    settings.ap_top_percent = opts.ap_top_percent;
    settings.ap_top_count = opts.ap_top_count;
    settings.normalize_brightness = opts.normalize_brightness;
    settings.stack_mode = opts.stack_mode;
    settings.sigma_clip_threshold = opts.sigma_clip_threshold;
    settings.reference_passes = opts.reference_passes;
    settings.drizzle_scale = opts.drizzle;
    settings.pixfrac = opts.pixfrac;
    settings.raw_cfa = opts.raw_cfa;

    std::string last_stage;
    const stackcore::ProgressFn progress = [&last_stage](const char* stage, int done,
                                                         int total) -> bool {
        if (last_stage != stage) {
            if (!last_stage.empty()) std::printf("\n");
            std::printf("[%s]\n", stage);
            last_stage = stage;
        }
        const int step = total < 10 ? 1 : total / 10;
        if (done % step != 0 && done != total) return true;
        std::printf("\r  %d/%d (%d%%)   ", done, total, total > 0 ? done * 100 / total : 0);
        std::fflush(stdout);
        return true;  // CLIは中断しない
    };

    stackcore::MapStackReport report;
    FrameBuffer result;

    if (opts.reuse_sidecar) {
        if (opts.sidecar.empty()) {
            std::fprintf(stderr, "エラー: --reuse-sidecar には --sidecar でパスを指定してください\n");
            return 2;
        }
        stackcore::AnalysisData analysis;
        stackcore::load_sidecar(opts.sidecar, analysis);

        std::string message;
        if (!stackcore::matches_source(analysis, file_size_bytes(opts.input),
                                       source->frame_count(), source->width(),
                                       source->height(),
                                       stackcore::is_bayer(source->color_id()) && !opts.raw_cfa
                                           ? 3
                                           : (source->color_id() == SerColorId::RGB ||
                                                      source->color_id() == SerColorId::BGR
                                                  ? 3
                                                  : 1),
                                       message)) {
            std::fprintf(stderr,
                         "エラー: サイドカーがこの入力に対応していません。%s\n"
                         "解析からやり直してください（--reuse-sidecar を外す）\n",
                         message.c_str());
            return 2;
        }
        std::printf("\nサイドカーから解析結果を読みました: %s\n", opts.sidecar.c_str());
        std::printf("  AP %d 個 / 解析済み %d フレーム（解析はやり直しません）\n",
                    static_cast<int>(analysis.points.size()),
                    static_cast<int>(analysis.analyzed_indices.size()));
        result = stackcore::stack_from_analysis(*source, settings, analysis, progress, report);
        std::printf("\n");
    } else {
        if (!opts.sidecar.empty()) {
            stackcore::AnalysisData analysis =
                stackcore::analyze_map_stack(*source, settings, progress, report);
            analysis.source_size = file_size_bytes(opts.input);
            stackcore::save_sidecar(opts.sidecar, analysis);
            result = stackcore::stack_from_analysis(*source, settings, analysis, progress, report);
            std::printf("\n解析結果を保存しました: %s\n", opts.sidecar.c_str());
        } else {
            result = stackcore::run_map_stack(*source, settings, progress, report);
        }
        std::printf("\n");
    }

    const stackcore::GlobalStageReport& g = report.global;
    if (!opts.reuse_sidecar) {
        std::printf("\n[グローバル段]\n");
        std::printf("  参照フレーム  : %d (品質の中央値)\n", g.reference_index);
        std::printf("  モード        : %s / 変位上限 %d px\n", stackcore::to_string(g.mode),
                    g.max_shift);
        std::printf("  除外          : 類似度不足 %d / 変位超過 %d / 構造的外れ値 %d\n",
                    g.rejected_low_similarity, g.rejected_shift, g.rejected_outlier);
        std::printf("  参照画像      : 上位 %.1f%% の %d フレームを単純平均\n",
                    opts.top_percent, report.reference_frames);
    }

    std::printf("\n[MAP段]\n");
    std::printf("  APサイズ      : %d px (格子間隔 %d px = 50%%オーバーラップ)\n",
                report.ap_size, report.ap_grid_step);
    std::printf("  AP数          : %d\n", report.ap_count);
    std::printf("  解析フレーム  : %d\n", report.frames_analyzed);
    std::printf("  AP×フレーム   : %lld 組\n", report.ap_frame_pairs);
    if (report.ap_frame_pairs > 0 && !opts.reuse_sidecar) {
        std::printf("    信頼度不足で近傍から補間: %lld (%.2f%%)\n", report.invalid_matches,
                    100.0 * report.invalid_matches / report.ap_frame_pairs);
        std::printf("    隣接との差でクリップ    : %lld (%.2f%%)\n", report.clipped_matches,
                    100.0 * report.clipped_matches / report.ap_frame_pairs);
        const long long frame_passes = report.alignment_frame_passes;
        if (frame_passes > 0) {
            std::printf("    共通変位へ安全退避      : %lld / %lld フレーム×パス (%.2f%%)\n",
                        report.consensus_fallback_frames, frame_passes,
                        100.0 * report.consensus_fallback_frames / frame_passes);
        }
        std::printf("    空間的不整合の外れAP    : %lld (%.2f%%)\n",
                    report.consensus_outlier_matches,
                    100.0 * report.consensus_outlier_matches / report.ap_frame_pairs);
    }
    if (opts.ap_top_count > 0) {
        std::printf("  AP毎の採用    : 上位 %d フレーム\n", report.frames_per_ap);
    } else {
        std::printf("  AP毎の採用    : 上位 %.1f%% の %d フレーム\n", opts.ap_top_percent,
                    report.frames_per_ap);
    }
    if (report.passes_run > 0) {
        std::printf("  参照の精密化  : %d 回通した\n", report.passes_run);
    }
    const char* stack_mode = opts.stack_mode == stackcore::StackMode::QualityWeighted
                                 ? "品質重み付き平均"
                                 : (opts.stack_mode == stackcore::StackMode::SigmaClip
                                        ? "σクリップ"
                                        : "単純平均");
    std::printf("  加算方式      : %s\n", stack_mode);

    const stackcore::WindowedStackStats& st = report.stack;
    std::printf("\n[窓合成]\n");
    if (opts.drizzle > 1.0001 || opts.drizzle < 0.9999) {
        std::printf("  Drizzle       : %.2f倍 / pixfrac %.2f\n", opts.drizzle, opts.pixfrac);
    }
    std::printf("  重み          : 最小 %.4f / 最大 %.4f\n", st.min_weight, st.max_weight);
    std::printf("  未被覆画素    : %zu (参照画像で補填)\n", st.uncovered_pixels);
    if (st.weak_pixels > 0) {
        std::printf("  重み不足画素  : %zu (参照画像で補填)\n", st.weak_pixels);
    }
    if (st.fallback_blended_pixels > 0) {
        std::printf("  AP外周の混合  : %zu (参照画像へ滑らかに接続)\n",
                    st.fallback_blended_pixels);
    }
    std::printf("  平均後の最大値: %.4f\n", st.max_value);
    if (st.clipped > 0) {
        std::printf("  ※ %zu 画素が1.0を超えて切り詰められました\n", st.clipped);
    }

    // FrameBuffer はコピーできない（アラインメント確保を持つため）ので
    // 後処理はその場で掛ける。
    FrameBuffer final_image = std::move(result);
    if (!apply_post_processing(opts, final_image)) return 2;

    write_output_image(opts, final_image);

    std::printf("\n書き出しました: %s\n", opts.output.c_str());
    std::printf("  画像          : %d x %d x %dch (%s)\n", final_image.width(),
                final_image.height(), final_image.channels(),
                output_format_name(opts));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    Options opts;
    opts.command = argv[1];

    if (opts.command == "version" || opts.command == "--version") {
        std::printf("LunaStack CLI %s\n", kVersion);
        return 0;
    }
    if (opts.command == "help" || opts.command == "--help" || opts.command == "-h") {
        print_usage();
        return 0;
    }

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_next = (i + 1) < argc;

        if (arg == "--endian" && has_next) {
            const std::string v = argv[++i];
            if (v == "auto") opts.endian = ByteOrder::Auto;
            else if (v == "little") opts.endian = ByteOrder::Little;
            else if (v == "big") opts.endian = ByteOrder::Big;
            else {
                std::fprintf(stderr, "エラー: --endian は auto|little|big のいずれかです\n");
                return 2;
            }
        } else if (arg == "--bit-depth" && has_next) {
            if (!parse_int(argv[++i], opts.bit_depth) || opts.bit_depth < 1 ||
                opts.bit_depth > 16) {
                std::fprintf(stderr, "エラー: --bit-depth は1..16の整数です\n");
                return 2;
            }
        } else if ((arg == "-f" || arg == "--frame") && has_next) {
            if (!parse_int(argv[++i], opts.frame)) {
                std::fprintf(stderr, "エラー: --frame には整数を指定してください\n");
                return 2;
            }
        } else if ((arg == "-o" || arg == "--output") && has_next) {
            opts.output = argv[++i];
        } else if (arg == "--top" && has_next) {
            char* end = nullptr;
            opts.top_percent = std::strtod(argv[++i], &end);
            if (end == nullptr || *end != '\0') {
                std::fprintf(stderr, "エラー: --top には数値を指定してください\n");
                return 2;
            }
        } else if (arg == "--mode" && has_next) {
            const std::string v = argv[++i];
            if (v == "auto") opts.mode = AlignMode::Auto;
            else if (v == "planet") opts.mode = AlignMode::Planet;
            else if (v == "lunar") opts.mode = AlignMode::Lunar;
            else {
                std::fprintf(stderr, "エラー: --mode は auto|planet|lunar のいずれかです\n");
                return 2;
            }
        } else if (arg == "--min-similarity" && has_next) {
            char* end = nullptr;
            opts.min_similarity = std::strtod(argv[++i], &end);
            if (end == nullptr || *end != '\0' || opts.min_similarity < 0.0) {
                std::fprintf(stderr, "エラー: --min-similarity には0以上の数値を指定してください\n");
                return 2;
            }
        } else if (arg == "--max-shift" && has_next) {
            if (!parse_int(argv[++i], opts.max_shift) || opts.max_shift < 0) {
                std::fprintf(stderr, "エラー: --max-shift には0以上の整数を指定してください\n");
                return 2;
            }
        } else if (arg == "--ap-size" && has_next) {
            if (!parse_int(argv[++i], opts.ap_size) || opts.ap_size < 8) {
                std::fprintf(stderr, "エラー: --ap-size には8以上の整数を指定してください\n");
                return 2;
            }
        } else if (arg == "--search-radius" && has_next) {
            if (!parse_int(argv[++i], opts.search_radius) || opts.search_radius < 1) {
                std::fprintf(stderr, "エラー: --search-radius には1以上の整数を指定してください\n");
                return 2;
            }
        } else if (arg == "--ap-top" && has_next) {
            char* end = nullptr;
            opts.ap_top_percent = std::strtod(argv[++i], &end);
            if (end == nullptr || *end != '\0' || opts.ap_top_percent <= 0.0 ||
                opts.ap_top_percent > 100.0) {
                std::fprintf(stderr, "エラー: --ap-top は0より大きく100以下の値です\n");
                return 2;
            }
        } else if (arg == "--ap-count" && has_next) {
            if (!parse_int(argv[++i], opts.ap_top_count) || opts.ap_top_count < 1) {
                std::fprintf(stderr, "エラー: --ap-count は1以上の整数で指定してください\n");
                return 2;
            }
        } else if (arg == "--quality" && has_next) {
            const std::string value = argv[++i];
            if (value == "gradient") {
                opts.quality_metric = stackcore::QualityMetric::GradientEnergy;
            } else if (value == "frequency") {
                opts.quality_metric = stackcore::QualityMetric::FrequencyBandPowerRatio;
            } else {
                std::fprintf(stderr,
                             "エラー: --quality は gradient|frequency のいずれかです\n");
                return 2;
            }
        } else if (arg == "--min-score" && has_next) {
            char* end = nullptr;
            opts.min_score = std::strtod(argv[++i], &end);
            if (end == nullptr || *end != '\0') {
                std::fprintf(stderr, "エラー: --min-score には数値を指定してください\n");
                return 2;
            }
        } else if (arg == "--ap-gradient" && has_next) {
            char* end = nullptr;
            opts.ap_gradient_ratio = std::strtod(argv[++i], &end);
            if (end == nullptr || *end != '\0' || opts.ap_gradient_ratio < 0.0) {
                std::fprintf(stderr, "エラー: --ap-gradient には0以上の数値を指定してください\n");
                return 2;
            }
        } else if (arg == "--sidecar" && has_next) {
            opts.sidecar = argv[++i];
        } else if (arg == "--drizzle" && has_next) {
            char* end = nullptr;
            opts.drizzle = std::strtod(argv[++i], &end);
            if (end == nullptr || *end != '\0' || opts.drizzle <= 0.0 || opts.drizzle > 4.0) {
                std::fprintf(stderr, "エラー: --drizzle は0より大きく4以下の値です\n");
                return 2;
            }
        } else if (arg == "--pixfrac" && has_next) {
            char* end = nullptr;
            opts.pixfrac = std::strtod(argv[++i], &end);
            if (end == nullptr || *end != '\0' || opts.pixfrac <= 0.0 || opts.pixfrac > 1.0) {
                std::fprintf(stderr, "エラー: --pixfrac は0より大きく1以下の値です\n");
                return 2;
            }
        } else if (arg == "--sharpen" && has_next) {
            opts.sharpen = argv[++i];
        } else if (arg == "--denoise" && has_next) {
            opts.denoise = argv[++i];
        } else if (arg == "--wavelet-layers" && has_next) {
            if (!parse_int(argv[++i], opts.wavelet_layers) || opts.wavelet_layers < 1 ||
                opts.wavelet_layers > 12) {
                std::fprintf(stderr, "エラー: --wavelet-layers は1..12です\n");
                return 2;
            }
        } else if (arg == "--stretch" && has_next) {
            std::vector<double> v;
            if (!parse_layer_values(argv[++i], 3, -1.0, v) || v[0] < 0.0 || v[1] <= v[0] ||
                v[2] <= 0.0) {
                std::fprintf(stderr,
                             "エラー: --stretch は 黒,白,ガンマ の3つ（黒<白、ガンマ>0）です\n");
                return 2;
            }
            opts.stretch_black = v[0];
            opts.stretch_white = v[1];
            opts.stretch_gamma = v[2];
        } else if (arg == "--low-memory") {
            opts.low_memory = true;
        } else if (arg == "--no-normalize") {
            opts.normalize_brightness = false;
        } else if (arg == "--stack-mode" && has_next) {
            const std::string value = argv[++i];
            if (value == "mean") opts.stack_mode = stackcore::StackMode::Mean;
            else if (value == "weighted") {
                opts.stack_mode = stackcore::StackMode::QualityWeighted;
            } else if (value == "sigma") {
                opts.stack_mode = stackcore::StackMode::SigmaClip;
            } else {
                std::fprintf(stderr,
                             "エラー: --stack-mode は mean|weighted|sigma のいずれかです\n");
                return 2;
            }
        } else if (arg == "--sigma" && has_next) {
            char* end = nullptr;
            opts.sigma_clip_threshold = std::strtod(argv[++i], &end);
            if (end == nullptr || *end != '\0' || opts.sigma_clip_threshold <= 0.0) {
                std::fprintf(stderr, "エラー: --sigma は0より大きい数値です\n");
                return 2;
            }
        } else if (arg == "--reuse-sidecar") {
            opts.reuse_sidecar = true;
        } else if (arg == "--ref-passes" && has_next) {
            if (!parse_int(argv[++i], opts.reference_passes) || opts.reference_passes < 1) {
                std::fprintf(stderr, "エラー: --ref-passes には1以上の整数を指定してください\n");
                return 2;
            }
        } else if (arg == "--ap-level" && has_next) {
            char* end = nullptr;
            opts.ap_level_ratio = std::strtod(argv[++i], &end);
            if (end == nullptr || *end != '\0' || opts.ap_level_ratio < 0.0) {
                std::fprintf(stderr, "エラー: --ap-level には0以上の数値を指定してください\n");
                return 2;
            }
        } else if (arg == "--outlier-k" && has_next) {
            char* end = nullptr;
            opts.outlier_k = std::strtod(argv[++i], &end);
            if (end == nullptr || *end != '\0' || opts.outlier_k <= 0.0) {
                std::fprintf(stderr, "エラー: --outlier-k には正の数値を指定してください\n");
                return 2;
            }
        } else if (arg == "--limit" && has_next) {
            if (!parse_int(argv[++i], opts.limit) || opts.limit < 1) {
                std::fprintf(stderr, "エラー: --limit には1以上の整数を指定してください\n");
                return 2;
            }
        } else if (arg == "--float") {
            opts.as_float = true;
        } else if (arg == "--raw-cfa") {
            opts.raw_cfa = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "エラー: 不明なオプション: %s\n", arg.c_str());
            return 2;
        } else if (opts.input.empty()) {
            opts.input = arg;
        } else {
            std::fprintf(stderr, "エラー: 入力ファイルが複数指定されています: %s\n", arg.c_str());
            return 2;
        }
    }

    if (opts.input.empty()) {
        std::fprintf(stderr, "エラー: 入力ファイルを指定してください\n\n");
        print_usage();
        return 2;
    }

    try {
        if (opts.command == "info") {
            // SERとして開ければSER用の詳しい表示、そうでなければAVIとして試す。
            // 拡張子だけで決めないのは、拡張子が実態と食い違うファイルがあるため。
            try {
                return command_info(opts);
            } catch (const std::exception& ser_error) {
                try {
                    return command_info_avi(opts);
                } catch (const std::exception&) {
                    throw ser_error;  // 元のSERとしてのエラーを見せる
                }
            }
        }
        if (opts.command == "extract") return command_extract(opts);
        if (opts.command == "stack") return command_stack(opts);
        if (opts.command == "mapstack") return command_mapstack(opts);
        std::fprintf(stderr, "エラー: 不明なコマンド: %s\n\n", opts.command.c_str());
        print_usage();
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "エラー: %s\n", e.what());
        return 1;
    }
}
