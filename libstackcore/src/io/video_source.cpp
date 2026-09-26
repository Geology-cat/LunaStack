#include "stackcore/video_source.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "stackcore/avi_decoder.hpp"
#include "stackcore/image_reader.hpp"

namespace stackcore {
namespace {

class SerSource : public VideoSource {
public:
    SerSource(const std::string& path, const OpenOptions& o) {
        decoder_.open(path, o.endian);
        if (o.bit_depth_override > 0) decoder_.set_bit_depth_override(o.bit_depth_override);
        decoder_.advise_sequential();
    }

    int width() const override { return decoder_.header().width; }
    int height() const override { return decoder_.header().height; }
    int frame_count() const override { return decoder_.frame_count(); }
    SerColorId color_id() const override { return decoder_.header().color_id; }
    int bit_depth() const override { return decoder_.effective_bit_depth(); }
    bool has_timestamps() const override { return decoder_.has_timestamps(); }
    std::int64_t timestamp_ticks(int index) const override {
        return decoder_.has_timestamps() ? decoder_.timestamp_ticks(index) : 0;
    }
    void read_frame(int index, FrameBuffer& out) const override {
        decoder_.read_frame(index, out);
    }
    FrameStats frame_stats(int index) const override { return decoder_.frame_stats(index); }
    const char* format_name() const override { return "SER v3"; }
    bool byte_order_suspect() const override {
        return decoder_.byte_order_differs_from_header();
    }
    void set_low_memory(bool on) override {
        low_memory_ = on;
        // 256MBごとに張り直す。実測で最大RSSがほぼこの値に収まる。
        decoder_.set_reclaim_budget(on ? 256u * 1024u * 1024u : 0u);
    }
    bool supports_concurrent_reads() const override { return !low_memory_; }

    std::string describe() const override {
        const char* order = "auto";
        switch (decoder_.resolved_byte_order()) {
            case ByteOrder::Little: order = "little"; break;
            case ByteOrder::Big: order = "big"; break;
            case ByteOrder::Auto: order = "auto"; break;
        }
        std::string s = std::string("バイトオーダー ") + order;
        if (decoder_.byte_order_differs_from_header()) s += "（ヘッダの主張と相違）";
        return s;
    }

    const SerDecoder& decoder() const { return decoder_; }

private:
    SerDecoder decoder_;
    bool low_memory_ = false;
};

class AviSource : public VideoSource {
public:
    explicit AviSource(const std::string& path) {
        decoder_.open(path);
        decoder_.advise_sequential();
    }

    int width() const override { return decoder_.header().width; }
    int height() const override { return decoder_.header().height; }
    int frame_count() const override { return decoder_.frame_count(); }
    SerColorId color_id() const override { return decoder_.color_id(); }
    int bit_depth() const override { return decoder_.bit_depth(); }
    // AVIのフレーム時刻はフレームレートから計算できるが、SERのような
    // フレームごとの実測タイムスタンプではない。持っていないものを
    // 持っていると言わないため false を返す。
    bool has_timestamps() const override { return false; }
    std::int64_t timestamp_ticks(int) const override { return 0; }
    void read_frame(int index, FrameBuffer& out) const override {
        decoder_.read_frame(index, out);
    }
    FrameStats frame_stats(int index) const override { return decoder_.frame_stats(index); }
    const char* format_name() const override {
        return decoder_.is_mjpeg() ? "AVI (MJPEG)" : "AVI (非圧縮)";
    }
    void set_low_memory(bool on) override {
        low_memory_ = on;
        decoder_.set_reclaim_budget(on ? 256u * 1024u * 1024u : 0u);
    }
    bool supports_concurrent_reads() const override { return !low_memory_; }

    std::string describe() const override {
        if (decoder_.is_mjpeg()) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "MJPEG, %.2f fps", decoder_.header().fps);
            return std::string(buf);
        }
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s, %s, %.2f fps, 1行 %zu バイト",
                      decoder_.header().compression_name.c_str(),
                      decoder_.header().top_down ? "上から下" : "下から上",
                      decoder_.header().fps, decoder_.header().row_bytes);
        return std::string(buf);
    }

private:
    AviDecoder decoder_;
    bool low_memory_ = false;
};

// 静止画連番。1ファイル＝1フレームとして扱う。
//
// 寸法と色形式は先頭の1枚で決め、以降の画像が食い違えば読んだ時点で例外にする
// （黙って縮めたり切り捨てたりすると、どのフレームが壊れていたか分からなくなる）。
class ImageSequenceSource : public VideoSource {
public:
    explicit ImageSequenceSource(const std::vector<std::string>& files) : files_(files) {
        if (files_.empty()) throw std::runtime_error("静止画連番: 画像がありません");
        info_ = probe_image_file(files_[0]);
        if (info_.channels != 1 && info_.channels != 3) {
            throw std::runtime_error("静止画連番: 1chまたは3chの画像のみ対応しています");
        }
        // カメラのRAWは1枚ごとに撮影時刻（UTC）を持つ。全部そろっているときだけ使う
        // （WinJUPOS向けの中央時刻・FITSのDATE-OBS）。ヘッダだけなので速い。
        if (info_.timestamp_ticks > 0) {
            ticks_.assign(files_.size(), 0);
            ticks_[0] = info_.timestamp_ticks;
            for (std::size_t i = 1; i < files_.size(); ++i) {
                try {
                    ticks_[i] = probe_image_file(files_[i]).timestamp_ticks;
                } catch (const std::exception&) {
                    ticks_[i] = 0;  // 壊れた1枚は読むときに分かる
                }
                if (ticks_[i] <= 0) {
                    ticks_.clear();
                    break;
                }
            }
        }
    }

    int width() const override { return info_.width; }
    int height() const override { return info_.height; }
    int frame_count() const override { return static_cast<int>(files_.size()); }
    SerColorId color_id() const override { return info_.color; }
    int bit_depth() const override { return std::min(16, info_.bit_depth); }
    bool has_timestamps() const override { return !ticks_.empty(); }
    std::int64_t timestamp_ticks(int index) const override {
        if (ticks_.empty() || index < 0 || index >= frame_count()) return 0;
        return ticks_[static_cast<std::size_t>(index)];
    }

    void read_frame(int index, FrameBuffer& out) const override {
        if (index < 0 || index >= frame_count()) {
            throw std::out_of_range("静止画連番: フレーム番号が範囲外です");
        }
        ImageFileInfo info;
        read_image_file(files_[static_cast<std::size_t>(index)], out, info);
        if (info.width != info_.width || info.height != info_.height ||
            info.channels != info_.channels) {
            throw std::runtime_error("静止画連番: 寸法が先頭の画像と違います: " +
                                     files_[static_cast<std::size_t>(index)]);
        }
        out.set_source_bit_depth(bit_depth());
    }

    FrameStats frame_stats(int index) const override {
        FrameBuffer frame;
        read_frame(index, frame);
        const double scale = static_cast<double>((1u << std::min(16, info_.bit_depth)) - 1u);
        FrameStats stats;
        double lo = 1.0, hi = 0.0, sum = 0.0;
        std::size_t n = 0;
        for (int c = 0; c < frame.channels(); ++c) {
            for (int y = 0; y < frame.height(); ++y) {
                const float* row = frame.row(c, y);
                for (int x = 0; x < frame.width(); ++x) {
                    lo = std::min(lo, static_cast<double>(row[x]));
                    hi = std::max(hi, static_cast<double>(row[x]));
                    sum += row[x];
                    ++n;
                }
            }
        }
        stats.min_value = static_cast<std::uint32_t>(lo * scale + 0.5);
        stats.max_value = static_cast<std::uint32_t>(hi * scale + 0.5);
        stats.mean_value = n > 0 ? sum / n * scale : 0.0;
        return stats;
    }

    std::string describe() const override {
        std::string s = std::to_string(files_.size()) + "枚の" + info_.format + "（" +
                        std::to_string(info_.bit_depth) + "bit";
        if (!info_.camera.empty()) s += "、" + info_.camera;
        return s + "）";
    }
    const char* format_name() const override { return "静止画連番"; }

private:
    std::vector<std::string> files_;
    ImageFileInfo info_;
    std::vector<std::int64_t> ticks_;  // 撮影時刻（全部そろっているときだけ）
};

// 前処理（フレーム範囲・色形式の指定・デバイヤー方式・ダーク/フラット補正）を
// 掛けるラッパー。パイプラインは前処理の存在を知らなくてよい。
class PreparedSource : public VideoSource {
public:
    PreparedSource(std::unique_ptr<VideoSource> base, const OpenOptions& o)
        : base_(std::move(base)), options_(o) {
        const int total = base_->frame_count();
        start_ = std::max(0, o.frame_start);
        const int end = o.frame_end > 0 ? std::min(o.frame_end, total) : total;
        if (start_ >= end) {
            throw std::runtime_error("フレーム範囲が空です（" + std::to_string(o.frame_start) +
                                     "〜" + std::to_string(o.frame_end) + " / 全" +
                                     std::to_string(total) + "フレーム）");
        }
        count_ = end - start_;
        if (o.override_color) {
            if (o.color_override != SerColorId::Mono && !is_supported_bayer(o.color_override)) {
                throw std::runtime_error("色形式の指定はモノクロか RGGB/GRBG/GBRG/BGGR のみです");
            }
            if (base_->color_id() == SerColorId::RGB || base_->color_id() == SerColorId::BGR) {
                throw std::runtime_error("カラー（RGB）の入力にはBayer配列を指定できません");
            }
        }
        if (o.calibration && !o.calibration->empty()) {
            const auto check = [&](const FrameBuffer& f, const char* name) {
                if (!f.empty() && (f.width() != base_->width() || f.height() != base_->height())) {
                    throw std::runtime_error(std::string("キャリブレーション: ") + name +
                                             "の寸法（" + std::to_string(f.width()) + "×" +
                                             std::to_string(f.height()) + "）が入力と違います");
                }
            };
            check(o.calibration->dark, "ダーク");
            check(o.calibration->flat, "フラット");
        }
    }

    int width() const override { return base_->width(); }
    int height() const override { return base_->height(); }
    int frame_count() const override { return count_; }
    SerColorId color_id() const override {
        return options_.override_color ? options_.color_override : base_->color_id();
    }
    int bit_depth() const override { return base_->bit_depth(); }
    bool has_timestamps() const override { return base_->has_timestamps(); }
    std::int64_t timestamp_ticks(int index) const override {
        return base_->timestamp_ticks(start_ + index);
    }
    void read_frame(int index, FrameBuffer& out) const override {
        if (index < 0 || index >= count_) {
            throw std::out_of_range("フレーム番号が範囲外です");
        }
        base_->read_frame(start_ + index, out);
        if (options_.calibration) apply_calibration(out, *options_.calibration);
    }
    FrameStats frame_stats(int index) const override { return base_->frame_stats(start_ + index); }
    std::string describe() const override {
        std::string s = base_->describe();
        if (start_ != 0 || count_ != base_->frame_count()) {
            s += " / 範囲 " + std::to_string(start_ + 1) + "〜" + std::to_string(start_ + count_);
        }
        if (options_.calibration && !options_.calibration->dark.empty()) s += " / ダーク補正";
        if (options_.calibration && !options_.calibration->flat.empty()) s += " / フラット補正";
        return s;
    }
    const char* format_name() const override { return base_->format_name(); }
    bool byte_order_suspect() const override { return base_->byte_order_suspect(); }
    void set_low_memory(bool on) override { base_->set_low_memory(on); }
    bool supports_concurrent_reads() const override { return base_->supports_concurrent_reads(); }
    DebayerMethod debayer_method() const override { return options_.debayer; }
    int original_index(int index) const override { return base_->original_index(start_ + index); }

private:
    std::unique_ptr<VideoSource> base_;
    OpenOptions options_;
    int start_ = 0;
    int count_ = 0;
};

bool ends_with_ci(const std::string& s, const char* suffix) {
    const std::size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        char a = s[s.size() - n + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

}  // namespace

static std::unique_ptr<VideoSource> open_container(const std::string& path, const OpenOptions& options) {
    // 拡張子で見当をつけるが、それだけで決めない。
    // 拡張子が違っていても中身で開ければ開く（キャプチャソフトによっては
    // SERを .avi として保存する事故がある）。
    const bool looks_avi = ends_with_ci(path, ".avi");

    std::string first_error;
    if (looks_avi) {
        try {
            return std::unique_ptr<VideoSource>(new AviSource(path));
        } catch (const std::exception& e) {
            first_error = e.what();
        }
        try {
            return std::unique_ptr<VideoSource>(new SerSource(path, options));
        } catch (const std::exception&) {
            throw std::runtime_error(first_error);
        }
    }

    try {
        return std::unique_ptr<VideoSource>(new SerSource(path, options));
    } catch (const std::exception& e) {
        first_error = e.what();
    }
    try {
        return std::unique_ptr<VideoSource>(new AviSource(path));
    } catch (const std::exception&) {
        throw std::runtime_error(first_error);
    }
}

bool is_directory_path(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::unique_ptr<VideoSource> open_raw_video(const std::string& path, const OpenOptions& options) {
    if (!options.sequence_files.empty()) {
        return std::unique_ptr<VideoSource>(new ImageSequenceSource(options.sequence_files));
    }
    if (is_directory_path(path)) {
        return std::unique_ptr<VideoSource>(new ImageSequenceSource(list_image_sequence(path)));
    }
    if (is_supported_image_path(path)) {
        // 静止画1枚も「1フレームの連番」として開ける（ダーク・フラットの原本など）。
        return std::unique_ptr<VideoSource>(
            new ImageSequenceSource(std::vector<std::string>(1, path)));
    }
    return open_container(path, options);
}

std::unique_ptr<VideoSource> open_video(const std::string& path, const OpenOptions& options) {
    std::unique_ptr<VideoSource> base = open_raw_video(path, options);
    if (!options.has_preprocessing()) return base;
    return std::unique_ptr<VideoSource>(new PreparedSource(std::move(base), options));
}

}  // namespace stackcore
