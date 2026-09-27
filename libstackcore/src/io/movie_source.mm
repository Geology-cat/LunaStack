#include "movie_source.hpp"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "stackcore/raw_reader.hpp"
#include "stackcore/video_source.hpp"

// MRC（手動の参照カウント）で書く。アプリ本体と同じ流儀。

namespace stackcore {
namespace detail {
namespace {

std::string lower_extension(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return ext;
}

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("動画（AVFoundation）: " + what); }

std::string fourcc_name(FourCharCode code) {
    char s[5] = {static_cast<char>((code >> 24) & 0xFF), static_cast<char>((code >> 16) & 0xFF),
                 static_cast<char>((code >> 8) & 0xFF), static_cast<char>(code & 0xFF), 0};
    const std::string f(s);
    if (f == "avc1" || f == "avc3") return "H.264";
    if (f == "hvc1" || f == "hev1") return "HEVC";
    if (f.compare(0, 2, "ap") == 0) return "ProRes";
    if (f == "jpeg" || f == "mjpa" || f == "mjpb") return "Motion JPEG";
    return f;
}

// QuickTime の作成日時 "2024-01-19T02:24:06+0900"（または "+09:00"）→ UTC の ticks。
// 時差の書かれていない日時は使わない（現地時刻を UTC と取り違えないため）。
std::int64_t creation_ticks(AVAsset* asset) {
    NSArray* items = [AVMetadataItem metadataItemsFromArray:[asset metadata]
                                       filteredByIdentifier:AVMetadataIdentifierQuickTimeMetadataCreationDate];
    for (AVMetadataItem* item in items) {
        NSString* value = [item stringValue];
        if (!value) continue;
        const std::string s([value UTF8String]);
        // YYYY-MM-DDTHH:MM:SS[.sss](+|-)HH[:]MM
        if (s.size() < 24 || s[4] != '-' || s[7] != '-' || s[10] != 'T') continue;
        std::string datetime = s.substr(0, 19);
        datetime[4] = ':';
        datetime[7] = ':';
        datetime[10] = ' ';
        std::size_t pos = 19;
        std::string subsec;
        if (pos < s.size() && s[pos] == '.') {
            ++pos;
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') subsec += s[pos++];
        }
        if (pos >= s.size() || (s[pos] != '+' && s[pos] != '-')) continue;
        const int sign = s[pos] == '-' ? -1 : 1;
        std::string digits;
        for (std::size_t i = pos + 1; i < s.size(); ++i) {
            if (s[i] >= '0' && s[i] <= '9') digits += s[i];
        }
        if (digits.size() != 4) continue;
        const int minutes = sign * (std::stoi(digits.substr(0, 2)) * 60 + std::stoi(digits.substr(2, 2)));
        const std::int64_t t = exif_datetime_to_ticks(datetime, subsec, minutes);
        if (t > 0) return t;
    }
    return 0;
}

class MovieSource : public VideoSource {
public:
    explicit MovieSource(const std::string& path) : path_(path) {
        @autoreleasepool {
            NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
            asset_ = [[AVURLAsset alloc] initWithURL:url
                                             options:@{AVURLAssetPreferPreciseDurationAndTimingKey : @YES}];
            NSArray* tracks = [asset_ tracksWithMediaType:AVMediaTypeVideo];
            if ([tracks count] == 0) fail("映像のトラックがありません");
            track_ = [[tracks objectAtIndex:0] retain];
            fps_ = [track_ nominalFrameRate];
            NSArray* formats = [track_ formatDescriptions];
            if ([formats count] > 0) {
                CMFormatDescriptionRef fd = (CMFormatDescriptionRef)[formats objectAtIndex:0];
                codec_ = fourcc_name(CMFormatDescriptionGetMediaSubType(fd));
            }
            build_frame_table();
            // 大きさは実際にデコードした1枚目で決める（naturalSize は端数や回転を含みうる）。
            std::vector<std::uint8_t> bgra;
            int w = 0, h = 0;
            decode_locked(0, bgra, w, h);
            width_ = w;
            height_ = h;
            ticks0_ = creation_ticks(asset_);
        }
    }

    ~MovieSource() override {
        stop_reader();
        [track_ release];
        [asset_ release];
    }

    int width() const override { return width_; }
    int height() const override { return height_; }
    int frame_count() const override { return static_cast<int>(pts_.size()); }
    SerColorId color_id() const override { return SerColorId::RGB; }
    int bit_depth() const override { return 8; }
    bool has_timestamps() const override { return ticks0_ > 0; }
    std::int64_t timestamp_ticks(int index) const override {
        if (ticks0_ <= 0 || index < 0 || index >= frame_count()) return 0;
        // 作成日時（撮影の始め）に、そのフレームの表示時刻を足す。
        const double seconds = CMTimeGetSeconds(CMTimeSubtract(pts_[static_cast<std::size_t>(index)], pts_[0]));
        return ticks0_ + static_cast<std::int64_t>(seconds * 1.0e7 + 0.5);
    }

    void read_frame(int index, FrameBuffer& out) const override {
        if (index < 0 || index >= frame_count()) throw std::out_of_range("動画: フレーム番号が範囲外です");
        std::vector<std::uint8_t> bgra;
        int w = 0, h = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            @autoreleasepool {
                decode_locked(index, bgra, w, h);
            }
        }
        if (w != width_ || h != height_) fail("途中でフレームの大きさが変わりました");
        if (out.width() != w || out.height() != h || out.channels() != 3) out.reset(w, h, 3);
        const float scale = 1.0f / 255.0f;
        for (int y = 0; y < h; ++y) {
            const std::uint8_t* s = bgra.data() + static_cast<std::size_t>(y) * w * 4;
            float* r = out.row(0, y);
            float* g = out.row(1, y);
            float* b = out.row(2, y);
            for (int x = 0; x < w; ++x) {
                b[x] = s[x * 4 + 0] * scale;
                g[x] = s[x * 4 + 1] * scale;
                r[x] = s[x * 4 + 2] * scale;
            }
        }
        out.set_source_bit_depth(8);
        out.invalidate_luma();
    }

    FrameStats frame_stats(int index) const override {
        FrameBuffer f;
        read_frame(index, f);
        double lo = 1.0, hi = 0.0, sum = 0.0;
        std::size_t n = 0;
        for (int c = 0; c < 3; ++c) {
            for (int y = 0; y < f.height(); ++y) {
                const float* row = f.row(c, y);
                for (int x = 0; x < f.width(); ++x) {
                    lo = std::min(lo, static_cast<double>(row[x]));
                    hi = std::max(hi, static_cast<double>(row[x]));
                    sum += row[x];
                    ++n;
                }
            }
        }
        FrameStats s;
        s.min_value = static_cast<std::uint32_t>(lo * 255.0 + 0.5);
        s.max_value = static_cast<std::uint32_t>(hi * 255.0 + 0.5);
        s.mean_value = n ? sum / n * 255.0 : 0.0;
        return s;
    }

    std::string describe() const override {
        char buf[200];
        std::snprintf(buf, sizeof(buf), "%s, %.2f fps（AVFoundation で読み込み。OSの版で画素が変わることがあります）",
                      codec_.empty() ? "動画" : codec_.c_str(), fps_);
        return buf;
    }
    const char* format_name() const override {
        const std::string ext = lower_extension(path_);
        return ext == "mp4" ? "MP4" : (ext == "m4v" ? "M4V" : "MOV");
    }
    // 内部でデコーダを順番に使う（読む順番が番号順から大きく外れなければ、並列に頼まれてもよい）。
    bool supports_concurrent_reads() const override { return true; }

private:
    // 圧縮されたままのサンプルを走査して、表示時刻の一覧を作る（デコードはしない）。
    void build_frame_table() {
        NSError* error = nil;
        AVAssetReader* reader = [[[AVAssetReader alloc] initWithAsset:asset_ error:&error] autorelease];
        if (!reader) fail("読めません");
        AVAssetReaderTrackOutput* output =
            [[[AVAssetReaderTrackOutput alloc] initWithTrack:track_ outputSettings:nil] autorelease];
        [output setAlwaysCopiesSampleData:NO];
        if (![reader canAddOutput:output]) fail("映像を取り出せません");
        [reader addOutput:output];
        if (![reader startReading]) fail("読み始められません");
        for (;;) {
            @autoreleasepool {
                CMSampleBufferRef sample = [output copyNextSampleBuffer];
                if (!sample) break;
                // 編集リスト（B フレームの遅延の補正など）を反映した表示時刻を使う。
                // 生の表示時刻だと、デコードした画像の時刻とずれて番号が合わなくなる。
                // サンプル数0のバッファは目印（区切り・終わり）なので数えない。
                if (CMSampleBufferGetNumSamples(sample) <= 0) {
                    CFRelease(sample);
                    continue;
                }
                CMItemCount count = 0;
                if (CMSampleBufferGetOutputSampleTimingInfoArray(sample, 0, nullptr, &count) == noErr && count > 0) {
                    std::vector<CMSampleTimingInfo> timing(static_cast<std::size_t>(count));
                    if (CMSampleBufferGetOutputSampleTimingInfoArray(sample, count, timing.data(), &count) == noErr) {
                        for (CMItemCount i = 0; i < count; ++i) {
                            if (CMTIME_IS_VALID(timing[static_cast<std::size_t>(i)].presentationTimeStamp)) {
                                pts_.push_back(timing[static_cast<std::size_t>(i)].presentationTimeStamp);
                            }
                        }
                    }
                } else {
                    const CMTime t = CMSampleBufferGetOutputPresentationTimeStamp(sample);
                    if (CMTIME_IS_VALID(t) && CMSampleBufferGetNumSamples(sample) > 0) pts_.push_back(t);
                }
                CFRelease(sample);
            }
        }
        if ([reader status] == AVAssetReaderStatusFailed) fail("映像を最後まで読めません");
        // トラックの表示範囲（編集リストで切った範囲）の外の時刻は表示されないので除く。
        const CMTimeRange range = [track_ timeRange];
        if (CMTIMERANGE_IS_VALID(range) && CMTIME_IS_NUMERIC(range.duration) && CMTimeGetSeconds(range.duration) > 0) {
            const CMTime end = CMTimeRangeGetEnd(range);
            pts_.erase(std::remove_if(pts_.begin(), pts_.end(),
                                      [&](const CMTime& t) {
                                          return CMTimeCompare(t, range.start) < 0 || CMTimeCompare(t, end) >= 0;
                                      }),
                       pts_.end());
        }
        std::sort(pts_.begin(), pts_.end(), [](const CMTime& a, const CMTime& b) { return CMTimeCompare(a, b) < 0; });
        pts_.erase(std::unique(pts_.begin(), pts_.end(),
                               [](const CMTime& a, const CMTime& b) { return CMTimeCompare(a, b) == 0; }),
                   pts_.end());
        if (pts_.empty()) fail("フレームがありません");
    }

    // 表示時刻 → フレーム番号。
    int index_of(CMTime t) const {
        auto it = std::lower_bound(pts_.begin(), pts_.end(), t,
                                   [](const CMTime& a, const CMTime& b) { return CMTimeCompare(a, b) < 0; });
        if (it != pts_.end() && CMTimeCompare(*it, t) == 0) return static_cast<int>(it - pts_.begin());
        return -1;
    }

    void stop_reader() const {
        if (reader_) {
            [reader_ cancelReading];
            [reader_ release];
            reader_ = nil;
        }
        [output_ release];
        output_ = nil;
    }

    // index の位置から読み直せるデコーダを用意する（直前のキーフレームからの分は AVFoundation が内部で読む）。
    void restart(int index) const {
        stop_reader();
        pending_.clear();
        pending_bytes_ = 0;
        NSError* error = nil;
        reader_ = [[AVAssetReader alloc] initWithAsset:asset_ error:&error];
        if (!reader_) fail("読めません");
        NSDictionary* settings = @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)};
        output_ = [[AVAssetReaderTrackOutput alloc] initWithTrack:track_ outputSettings:settings];
        [output_ setAlwaysCopiesSampleData:NO];
        [reader_ addOutput:output_];
        [reader_ setTimeRange:CMTimeRangeFromTimeToTime(pts_[static_cast<std::size_t>(index)], kCMTimePositiveInfinity)];
        if (![reader_ startReading]) fail("読み始められません");
        next_ = index;
    }

    // 次の1枚をデコードする。フレーム番号と画素（BGRA）を返す。尽きたら -1。
    int decode_next(std::vector<std::uint8_t>& bgra, int& w, int& h) const {
        for (;;) {
            CMSampleBufferRef sample = [output_ copyNextSampleBuffer];
            if (!sample) return -1;
            CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample);
            if (!image) {
                CFRelease(sample);
                continue;
            }
            const int index = index_of(CMSampleBufferGetOutputPresentationTimeStamp(sample));
            CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
            w = static_cast<int>(CVPixelBufferGetWidth(image));
            h = static_cast<int>(CVPixelBufferGetHeight(image));
            const std::size_t stride = CVPixelBufferGetBytesPerRow(image);
            const std::uint8_t* base = static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddress(image));
            bgra.resize(static_cast<std::size_t>(w) * h * 4);
            for (int y = 0; y < h; ++y) {
                std::memcpy(bgra.data() + static_cast<std::size_t>(y) * w * 4, base + static_cast<std::size_t>(y) * stride,
                            static_cast<std::size_t>(w) * 4);
            }
            CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
            CFRelease(sample);
            if (index < 0) continue;  // 表示しないサンプル
            return index;
        }
    }

    // index のフレームを取り出す（mutex を持った状態で呼ぶ）。
    //
    // 番号順に読まれる限りはデコーダをそのまま進める。少し先を頼まれたら途中のフレームを
    // 上限まで取っておく（並列のワーカーが番号を前後して取りに来ても、読み直さずに済む）。
    // 離れた番号を頼まれたときだけ、その位置から読み直す。
    void decode_locked(int index, std::vector<std::uint8_t>& bgra, int& w, int& h) const {
        auto hit = pending_.find(index);
        if (hit != pending_.end()) {
            bgra.swap(hit->second.bytes);
            w = hit->second.w;
            h = hit->second.h;
            pending_bytes_ -= bgra.size();
            pending_.erase(hit);
            return;
        }
        if (!reader_ || index < next_ || index > next_ + 32) restart(index);
        for (;;) {
            std::vector<std::uint8_t> bytes;
            int fw = 0, fh = 0;
            const int got = decode_next(bytes, fw, fh);
            if (got < 0) {
                // 途中で尽きた（時刻の丸めなど）。その位置から読み直して1回だけやり直す。
                restart(index);
                const int again = decode_next(bytes, fw, fh);
                if (again != index) fail("フレーム " + std::to_string(index + 1) + " を取り出せません");
                bgra.swap(bytes);
                w = fw;
                h = fh;
                next_ = index + 1;
                return;
            }
            next_ = got + 1;
            if (got == index) {
                bgra.swap(bytes);
                w = fw;
                h = fh;
                return;
            }
            if (got > index) {
                // 頼んだフレームを飛び越えた（表示しないフレームなど）。その位置から読み直す。
                restart(index);
                continue;
            }
            // 頼まれた番号より手前（あとで頼まれるかもしれない）。上限までは取っておく。
            if (pending_bytes_ + bytes.size() <= kPendingBudget) {
                pending_bytes_ += bytes.size();
                Pending p;
                p.bytes.swap(bytes);
                p.w = fw;
                p.h = fh;
                pending_[got] = std::move(p);
            }
        }
    }

    struct Pending {
        std::vector<std::uint8_t> bytes;
        int w = 0, h = 0;
    };
    static constexpr std::size_t kPendingBudget = 256u << 20;

    std::string path_;
    AVURLAsset* asset_ = nil;
    AVAssetTrack* track_ = nil;
    std::vector<CMTime> pts_;
    std::string codec_;
    float fps_ = 0.0f;
    int width_ = 0, height_ = 0;
    std::int64_t ticks0_ = 0;

    mutable std::mutex mutex_;
    mutable AVAssetReader* reader_ = nil;
    mutable AVAssetReaderTrackOutput* output_ = nil;
    mutable int next_ = 0;
    mutable std::map<int, Pending> pending_;
    mutable std::size_t pending_bytes_ = 0;
};

}  // namespace

bool is_movie_path(const std::string& path) {
    const std::string ext = lower_extension(path);
    return ext == "mov" || ext == "mp4" || ext == "m4v";
}

std::unique_ptr<VideoSource> open_movie(const std::string& path) {
    return std::unique_ptr<VideoSource>(new MovieSource(path));
}

}  // namespace detail
}  // namespace stackcore
