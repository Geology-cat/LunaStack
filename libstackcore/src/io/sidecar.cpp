#include "stackcore/sidecar.hpp"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace stackcore {
namespace {

// 先頭16バイト。バージョンを分けているのは、形式を変えたときに
// 古いサイドカーを黙って読んで誤った結果を出さないため。
const char kMagic[12] = {'L', 'U', 'N', 'A', 'S', 'T', 'K', 'S', 'I', 'D', 'E', '1'};
// v1の参照画像は、疎なAP外周をS/Wから参照へ直接切り替えて作られており、
// タイル状アーティファクトが画像自体に焼き付いている可能性がある。
// バイナリ構造は同じでも意味的に安全ではないため、v2で再解析を必須にする。
constexpr std::uint32_t kVersion = 2;

struct Writer {
    std::FILE* f;
    explicit Writer(const std::string& path) {
        f = std::fopen(path.c_str(), "wb");
        if (!f) throw std::runtime_error("サイドカーを書けません: " + path);
    }
    ~Writer() {
        if (f) std::fclose(f);
    }
    void raw(const void* p, std::size_t n) {
        if (std::fwrite(p, 1, n, f) != n) {
            throw std::runtime_error("サイドカーの書き込みに失敗しました");
        }
    }
    void u32(std::uint32_t v) { raw(&v, 4); }
    void i32(std::int32_t v) { raw(&v, 4); }
    void i64(std::int64_t v) { raw(&v, 8); }
    void f64(double v) { raw(&v, 8); }
};

struct Reader {
    std::FILE* f;
    explicit Reader(const std::string& path) {
        f = std::fopen(path.c_str(), "rb");
        if (!f) throw std::runtime_error("サイドカーを読めません: " + path);
    }
    ~Reader() {
        if (f) std::fclose(f);
    }
    void raw(void* p, std::size_t n) {
        if (std::fread(p, 1, n, f) != n) {
            throw std::runtime_error("サイドカーが途中で終わっています（壊れている可能性があります）");
        }
    }
    std::uint32_t u32() {
        std::uint32_t v = 0;
        raw(&v, 4);
        return v;
    }
    std::int32_t i32() {
        std::int32_t v = 0;
        raw(&v, 4);
        return v;
    }
    std::int64_t i64() {
        std::int64_t v = 0;
        raw(&v, 8);
        return v;
    }
    double f64() {
        double v = 0.0;
        raw(&v, 8);
        return v;
    }
};

}  // namespace

void save_sidecar(const std::string& path, const AnalysisData& d) {
    Writer w(path);
    w.raw(kMagic, sizeof(kMagic));
    w.u32(kVersion);

    w.i64(d.source_size);
    w.i32(d.source_frames);
    w.i32(d.width);
    w.i32(d.height);
    w.i32(d.channels);

    w.i32(d.reference_index);
    w.f64(d.reference_mean);

    w.i32(static_cast<std::int32_t>(d.frames.size()));
    for (std::size_t i = 0; i < d.frames.size(); ++i) {
        const FrameInfo& fi = d.frames[i];
        w.i32(fi.index);
        w.f64(fi.quality);
        w.f64(fi.mean);
        w.i32(fi.dx);
        w.i32(fi.dy);
        w.f64(fi.similarity);
        w.i32(fi.accepted ? 1 : 0);
        w.i32(static_cast<std::int32_t>(fi.reason));
    }

    w.i32(d.ap_size);
    w.i32(d.ap_grid_step);
    w.i32(static_cast<std::int32_t>(d.points.size()));
    for (std::size_t i = 0; i < d.points.size(); ++i) {
        w.i32(d.points[i].cx);
        w.i32(d.points[i].cy);
        w.f64(d.points[i].mean_gradient);
        w.f64(d.points[i].mean_level);
        w.f64(d.points[i].min_eigenvalue);
    }

    w.i32(static_cast<std::int32_t>(d.analyzed_indices.size()));
    if (!d.analyzed_indices.empty()) {
        w.raw(d.analyzed_indices.data(), d.analyzed_indices.size() * sizeof(int));
    }

    w.i64(static_cast<std::int64_t>(d.matrix.size()));
    if (!d.matrix.empty()) {
        // LocalMatch は POD なのでそのまま書ける。
        w.raw(d.matrix.data(), d.matrix.size() * sizeof(LocalMatch));
    }

    w.i64(static_cast<std::int64_t>(d.reference.size()));
    if (!d.reference.empty()) {
        w.raw(d.reference.data(), d.reference.size() * sizeof(float));
    }
}

void load_sidecar(const std::string& path, AnalysisData& d) {
    Reader r(path);
    char magic[sizeof(kMagic)];
    r.raw(magic, sizeof(magic));
    if (std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
        throw std::runtime_error("サイドカーの識別子が違います: " + path);
    }
    const std::uint32_t version = r.u32();
    if (version != kVersion) {
        throw std::runtime_error("サイドカーのバージョンが違います (" +
                                 std::to_string(version) + " != " + std::to_string(kVersion) +
                                 ")。解析をやり直してください");
    }

    d = AnalysisData{};
    d.source_size = r.i64();
    d.source_frames = r.i32();
    d.width = r.i32();
    d.height = r.i32();
    d.channels = r.i32();
    d.reference_index = r.i32();
    d.reference_mean = r.f64();

    const std::int32_t frame_count = r.i32();
    if (frame_count < 0 || frame_count > 10000000) {
        throw std::runtime_error("サイドカー: フレーム数が不正です");
    }
    d.frames.resize(static_cast<std::size_t>(frame_count));
    for (std::int32_t i = 0; i < frame_count; ++i) {
        FrameInfo& fi = d.frames[static_cast<std::size_t>(i)];
        fi.index = r.i32();
        fi.quality = r.f64();
        fi.mean = r.f64();
        fi.dx = r.i32();
        fi.dy = r.i32();
        fi.similarity = r.f64();
        fi.accepted = r.i32() != 0;
        fi.reason = static_cast<RejectReason>(r.i32());
    }

    d.ap_size = r.i32();
    d.ap_grid_step = r.i32();
    const std::int32_t ap_count = r.i32();
    if (ap_count < 0 || ap_count > 1000000) {
        throw std::runtime_error("サイドカー: AP数が不正です");
    }
    d.points.resize(static_cast<std::size_t>(ap_count));
    for (std::int32_t i = 0; i < ap_count; ++i) {
        AlignmentPoint& p = d.points[static_cast<std::size_t>(i)];
        p.cx = r.i32();
        p.cy = r.i32();
        p.mean_gradient = r.f64();
        p.mean_level = r.f64();
        p.min_eigenvalue = r.f64();
    }

    const std::int32_t analyzed = r.i32();
    if (analyzed < 0 || analyzed > frame_count) {
        throw std::runtime_error("サイドカー: 解析フレーム数が不正です");
    }
    d.analyzed_indices.resize(static_cast<std::size_t>(analyzed));
    if (analyzed > 0) {
        r.raw(d.analyzed_indices.data(), d.analyzed_indices.size() * sizeof(int));
    }

    const std::int64_t matrix_size = r.i64();
    const std::int64_t expected =
        static_cast<std::int64_t>(ap_count) * static_cast<std::int64_t>(analyzed);
    if (matrix_size != expected) {
        throw std::runtime_error("サイドカー: 変位場の大きさが AP数×フレーム数 と合いません");
    }
    d.matrix.resize(static_cast<std::size_t>(matrix_size));
    if (matrix_size > 0) {
        r.raw(d.matrix.data(), d.matrix.size() * sizeof(LocalMatch));
    }

    const std::int64_t ref_size = r.i64();
    const std::int64_t ref_expected = static_cast<std::int64_t>(d.width) * d.height * d.channels;
    if (ref_size != 0 && ref_size != ref_expected) {
        throw std::runtime_error("サイドカー: 参照画像の大きさが寸法と合いません");
    }
    d.reference.resize(static_cast<std::size_t>(ref_size));
    if (ref_size > 0) {
        r.raw(d.reference.data(), d.reference.size() * sizeof(float));
    }
}

bool matches_source(const AnalysisData& d, std::int64_t source_size, int frames, int width,
                    int height, int channels, std::string& message) {
    if (d.source_size != source_size) {
        message = "入力ファイルのサイズが解析時と違います（" + std::to_string(d.source_size) +
                  " → " + std::to_string(source_size) + "）";
        return false;
    }
    if (d.source_frames != frames) {
        message = "フレーム数が解析時と違います（" + std::to_string(d.source_frames) + " → " +
                  std::to_string(frames) + "）";
        return false;
    }
    if (d.width != width || d.height != height || d.channels != channels) {
        message = "画像の寸法が解析時と違います";
        return false;
    }
    message.clear();
    return true;
}

}  // namespace stackcore
