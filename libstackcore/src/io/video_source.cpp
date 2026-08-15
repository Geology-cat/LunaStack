#include "stackcore/video_source.hpp"

#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "stackcore/avi_decoder.hpp"

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

std::unique_ptr<VideoSource> open_video(const std::string& path, const OpenOptions& options) {
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

}  // namespace stackcore
