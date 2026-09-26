#include "stackcore/raw_reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "stackcore/inflate.hpp"
#include "stackcore/lossless_jpeg.hpp"
#include "stackcore/mapped_file.hpp"

#include "../common/parallel_rows.hpp"

namespace stackcore {
namespace {

[[noreturn]] void fail(const std::string& format, const std::string& what) {
    throw std::runtime_error(format + ": " + what);
}

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

// ---- TIFF構造（CR2・DNG共通） -------------------------------------------------

struct Entry {
    std::uint16_t tag = 0;
    std::uint16_t type = 0;
    std::uint32_t count = 0;
    std::size_t value_offset = 0;  // 値そのものが置かれている位置（ファイル先頭から）
};

struct Ifd {
    std::vector<Entry> entries;
    std::size_t next = 0;
    const Entry* find(std::uint16_t tag) const {
        for (const Entry& e : entries) {
            if (e.tag == tag) return &e;
        }
        return nullptr;
    }
};

int type_size(std::uint16_t type) {
    switch (type) {
        case 1: case 2: case 6: case 7: return 1;
        case 3: case 8: return 2;
        case 4: case 9: case 11: case 13: return 4;
        case 5: case 10: case 12: return 8;
        default: return 0;
    }
}

struct Tiff {
    const std::uint8_t* d = nullptr;
    std::size_t n = 0;
    bool little = true;
    const char* format = "RAW";

    void need(std::size_t o, std::size_t size) const {
        if (o > n || size > n - o) fail(format, "ファイルが途中で切れています");
    }
    std::uint16_t u16(std::size_t o) const {
        need(o, 2);
        return little ? static_cast<std::uint16_t>(d[o] | (d[o + 1] << 8))
                      : static_cast<std::uint16_t>((d[o] << 8) | d[o + 1]);
    }
    std::uint32_t u32(std::size_t o) const {
        need(o, 4);
        return little ? (static_cast<std::uint32_t>(d[o]) | (static_cast<std::uint32_t>(d[o + 1]) << 8) |
                         (static_cast<std::uint32_t>(d[o + 2]) << 16) |
                         (static_cast<std::uint32_t>(d[o + 3]) << 24))
                      : ((static_cast<std::uint32_t>(d[o]) << 24) |
                         (static_cast<std::uint32_t>(d[o + 1]) << 16) |
                         (static_cast<std::uint32_t>(d[o + 2]) << 8) | d[o + 3]);
    }

    Ifd read_ifd(std::size_t offset) const {
        Ifd ifd;
        const std::uint16_t count = u16(offset);
        if (count > 4096) fail(format, "IFDの項目数が多すぎます");
        need(offset + 2, static_cast<std::size_t>(count) * 12 + 4);
        for (std::uint16_t i = 0; i < count; ++i) {
            const std::size_t e = offset + 2 + static_cast<std::size_t>(i) * 12;
            Entry t;
            t.tag = u16(e);
            t.type = u16(e + 2);
            t.count = u32(e + 4);
            const std::size_t bytes = static_cast<std::size_t>(type_size(t.type)) * t.count;
            t.value_offset = bytes <= 4 ? e + 8 : u32(e + 8);
            ifd.entries.push_back(t);
        }
        ifd.next = u32(offset + 2 + static_cast<std::size_t>(count) * 12);
        return ifd;
    }

    double value(const Entry& e, std::uint32_t i) const {
        const std::size_t size = static_cast<std::size_t>(type_size(e.type));
        if (size == 0) fail(format, "想定外のタグ型です");
        const std::size_t o = e.value_offset + i * size;
        need(o, size);
        switch (e.type) {
            case 1: case 7: return d[o];
            case 6: return static_cast<std::int8_t>(d[o]);
            case 3: return u16(o);
            case 8: return static_cast<std::int16_t>(u16(o));
            case 4: case 13: return u32(o);
            case 9: return static_cast<std::int32_t>(u32(o));
            case 5: {
                const std::uint32_t den = u32(o + 4);
                return den ? static_cast<double>(u32(o)) / den : 0.0;
            }
            case 10: {
                const std::int32_t den = static_cast<std::int32_t>(u32(o + 4));
                return den ? static_cast<double>(static_cast<std::int32_t>(u32(o))) / den : 0.0;
            }
            case 11: {
                const std::uint32_t u = u32(o);
                float f;
                std::memcpy(&f, &u, sizeof(f));
                return f;
            }
            case 12: {
                std::uint64_t u = little ? (static_cast<std::uint64_t>(u32(o + 4)) << 32) | u32(o)
                                         : (static_cast<std::uint64_t>(u32(o)) << 32) | u32(o + 4);
                double v;
                std::memcpy(&v, &u, sizeof(v));
                return v;
            }
            default: return 0.0;
        }
    }
    std::vector<double> values(const Entry& e) const {
        if (e.count > (1u << 26)) fail(format, "タグの値が多すぎます");
        std::vector<double> out(e.count);
        for (std::uint32_t i = 0; i < e.count; ++i) out[i] = value(e, i);
        return out;
    }
    double scalar(const Ifd& ifd, std::uint16_t tag, double fallback) const {
        const Entry* e = ifd.find(tag);
        if (!e || e->count == 0) return fallback;
        return value(*e, 0);
    }
    std::string ascii(const Ifd& ifd, std::uint16_t tag) const {
        const Entry* e = ifd.find(tag);
        if (!e || e->count == 0) return "";
        need(e->value_offset, e->count);
        std::string s(reinterpret_cast<const char*>(d + e->value_offset), e->count);
        const std::size_t z = s.find('\0');
        if (z != std::string::npos) s.resize(z);
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    }
};

Tiff open_tiff(const MappedFile& file, const char* format) {
    Tiff t;
    t.d = file.data();
    t.n = file.size();
    t.format = format;
    if (t.n < 16) fail(format, "ファイルが短すぎます");
    if (t.d[0] == 'I' && t.d[1] == 'I') t.little = true;
    else if (t.d[0] == 'M' && t.d[1] == 'M') t.little = false;
    else fail(format, "TIFF形式のバイトオーダーの印がありません");
    if (t.u16(2) != 42) fail(format, "TIFF形式の識別子がありません");
    return t;
}

// ---- 日時 --------------------------------------------------------------------

// 暦日 → 1970-01-01 からの日数（Howard Hinnant の days_from_civil）。
std::int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// "+09:00" → 540。読めなければ false。
bool parse_offset(const std::string& s, int& minutes) {
    if (s.size() < 6 || (s[0] != '+' && s[0] != '-') || s[3] != ':') return false;
    for (int i : {1, 2, 4, 5}) {
        if (s[static_cast<std::size_t>(i)] < '0' || s[static_cast<std::size_t>(i)] > '9') return false;
    }
    const int h = (s[1] - '0') * 10 + (s[2] - '0');
    const int m = (s[4] - '0') * 10 + (s[5] - '0');
    minutes = (s[0] == '-' ? -1 : 1) * (h * 60 + m);
    return true;
}

struct ExifInfo {
    std::string datetime, subsec, offset;
    std::size_t makernote = 0;
    std::uint32_t makernote_size = 0;
};

ExifInfo read_exif(const Tiff& t, const Ifd& ifd0) {
    ExifInfo info;
    const Entry* ptr = ifd0.find(34665);
    if (!ptr) return info;
    const Ifd exif = t.read_ifd(static_cast<std::size_t>(t.value(*ptr, 0)));
    info.datetime = t.ascii(exif, 36867);
    info.subsec = t.ascii(exif, 37521);
    info.offset = t.ascii(exif, 36881);
    if (const Entry* mn = exif.find(37500)) {
        info.makernote = mn->value_offset;
        info.makernote_size = mn->count;
    }
    return info;
}

// ---- Bayerの並び ----------------------------------------------------------------

// 2×2 の色（0=R, 1=G, 2=B）を左上から (x0, y0) ずらして見たときの並び。
SerColorId bayer_id(const int colors[2][2], int x0, int y0) {
    int c[2][2];
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) c[y][x] = colors[(y + y0) & 1][(x + x0) & 1];
    }
    if (c[0][0] == 0 && c[0][1] == 1 && c[1][0] == 1 && c[1][1] == 2) return SerColorId::BayerRGGB;
    if (c[0][0] == 1 && c[0][1] == 0 && c[1][0] == 2 && c[1][1] == 1) return SerColorId::BayerGRBG;
    if (c[0][0] == 1 && c[0][1] == 2 && c[1][0] == 0 && c[1][1] == 1) return SerColorId::BayerGBRG;
    if (c[0][0] == 2 && c[0][1] == 1 && c[1][0] == 1 && c[1][1] == 0) return SerColorId::BayerBGGR;
    return SerColorId::Mono;
}

// ---- CR2 ---------------------------------------------------------------------

struct Cr2Layout {
    Tiff tiff;
    std::size_t strip = 0, strip_size = 0;
    int slices[3] = {0, 0, 0};
    int colors[2][2] = {{0, 1}, {1, 2}};  // 生の座標の左上から見た色
    // SensorInfo（無ければ全体）。右・下は含む。
    int left = 0, top = 0, right = -1, bottom = -1;
    bool has_sensor_info = false;
    std::string camera;
    std::int64_t ticks = 0;
    LosslessJpegImage jpeg;  // ヘッダのみ
    int raw_width = 0, raw_height = 0;
};

Cr2Layout parse_cr2(const MappedFile& file) {
    Cr2Layout L;
    L.tiff = open_tiff(file, "CR2");
    const Tiff& t = L.tiff;
    if (t.d[8] != 'C' || t.d[9] != 'R' || t.d[10] != 2) fail("CR2", "Canon CR2（第2版）ではありません");

    const Ifd ifd0 = t.read_ifd(t.u32(4));
    const std::string make = t.ascii(ifd0, 271);
    const std::string model = t.ascii(ifd0, 272);
    L.camera = model.compare(0, make.size(), make) == 0 || make.empty() ? model : make + " " + model;

    // RAWの画素は4番目のIFD（IFD3）にある。ヘッダの12バイト目にもその位置が書いてある。
    std::size_t raw_ifd = t.u32(12);
    if (raw_ifd == 0) {
        std::size_t o = ifd0.next;
        for (int i = 1; i < 3 && o; ++i) o = t.read_ifd(o).next;
        raw_ifd = o;
    }
    if (raw_ifd == 0) fail("CR2", "RAWのIFDがありません");
    const Ifd raw = t.read_ifd(raw_ifd);
    const Entry* offsets = raw.find(273);
    const Entry* counts = raw.find(279);
    if (!offsets || !counts) fail("CR2", "RAWの画素の位置がありません");
    L.strip = static_cast<std::size_t>(t.value(*offsets, 0));
    L.strip_size = static_cast<std::size_t>(t.value(*counts, 0));
    t.need(L.strip, L.strip_size);
    if (const Entry* s = raw.find(0xC640)) {
        if (s->count >= 3) {
            for (int i = 0; i < 3; ++i) L.slices[i] = static_cast<int>(t.value(*s, static_cast<std::uint32_t>(i)));
        }
    }
    // CR2CFAPattern: 1=RGGB, 2=BGGR, 3=GBRG, 4=GRBG（無ければRGGB）。
    switch (static_cast<int>(t.scalar(raw, 0xC5E0, 1))) {
        case 2: L.colors[0][0] = 2; L.colors[0][1] = 1; L.colors[1][0] = 1; L.colors[1][1] = 0; break;
        case 3: L.colors[0][0] = 1; L.colors[0][1] = 2; L.colors[1][0] = 0; L.colors[1][1] = 1; break;
        case 4: L.colors[0][0] = 1; L.colors[0][1] = 0; L.colors[1][0] = 2; L.colors[1][1] = 1; break;
        default: break;
    }

    L.jpeg = probe_lossless_jpeg(t.d + L.strip, L.strip_size);
    const int jwide = L.jpeg.width * L.jpeg.components;
    // 生の幅: スライスがあればその合計、無ければ（帯0本＝1本の帯）その幅、
    // それも無ければ IFD3 の ImageWidth。JPEGの1行は生の画像の2行ぶんのこともある。
    if (L.slices[0]) {
        L.raw_width = L.slices[0] * L.slices[1] + L.slices[2];
    } else if (L.slices[2]) {
        L.raw_width = L.slices[2];
    } else {
        L.raw_width = static_cast<int>(t.scalar(raw, 256, jwide));
    }
    if (L.raw_width <= 0) fail("CR2", "RAWの幅がありません");
    const std::int64_t total = static_cast<std::int64_t>(jwide) * L.jpeg.height;
    if (total % L.raw_width != 0) fail("CR2", "スライスの幅が画素数と合いません");
    L.raw_height = static_cast<int>(total / L.raw_width);

    // メーカーノート（Canon は先頭から素のIFD。位置はファイル先頭から）。
    const ExifInfo exif = read_exif(t, ifd0);
    int tz_minutes = 0;
    bool tz_known = parse_offset(exif.offset, tz_minutes);
    if (exif.makernote && exif.makernote_size >= 2) {
        const Ifd mn = t.read_ifd(exif.makernote);
        if (const Entry* si = mn.find(0x00E0)) {
            // SensorInfo: [1]幅 [2]高さ [5]左 [6]上 [7]右 [8]下（右・下は含む）
            if (si->count >= 9) {
                const int w = static_cast<int>(t.value(*si, 1));
                const int h = static_cast<int>(t.value(*si, 2));
                const int l = static_cast<int>(t.value(*si, 5));
                const int tp = static_cast<int>(t.value(*si, 6));
                const int r = static_cast<int>(t.value(*si, 7));
                const int b = static_cast<int>(t.value(*si, 8));
                if (w == L.raw_width && h == L.raw_height && l >= 0 && tp >= 0 && r > l && b > tp &&
                    r < w && b < h) {
                    L.left = l;
                    L.top = tp;
                    L.right = r;
                    L.bottom = b;
                    L.has_sensor_info = true;
                }
            }
        }
        if (!tz_known) {
            // TimeInfo: [1]時差（分） [3]夏時間（分）
            if (const Entry* ti = mn.find(0x0035)) {
                if (ti->type == 4 || ti->type == 9) {
                    if (ti->count >= 4) {
                        tz_minutes = static_cast<int>(t.value(*ti, 1)) + static_cast<int>(t.value(*ti, 3));
                        tz_known = true;
                    }
                } else if (ti->type == 7 && ti->count >= 16) {
                    Entry as_long = *ti;
                    as_long.type = 9;
                    as_long.count = ti->count / 4;
                    tz_minutes = static_cast<int>(t.value(as_long, 1)) + static_cast<int>(t.value(as_long, 3));
                    tz_known = true;
                }
            }
        }
    }
    if (!L.has_sensor_info) {
        L.left = 0;
        L.top = 0;
        L.right = L.raw_width - 1;
        L.bottom = L.raw_height - 1;
    }
    if (tz_known && tz_minutes >= -14 * 60 && tz_minutes <= 14 * 60) {
        L.ticks = exif_datetime_to_ticks(exif.datetime, exif.subsec, tz_minutes);
    }
    return L;
}

// ロスレスJPEGの標本をスライスの並びから生の画像の座標へ戻す（dcraw と同じ並べ方）。
std::vector<std::uint16_t> decode_cr2_values(const Cr2Layout& L) {
    const LosslessJpegImage jpeg =
        decode_lossless_jpeg(L.tiff.d + L.strip, L.strip_size,
                             static_cast<std::size_t>(L.raw_width) * L.raw_height);
    const int W = L.raw_width, H = L.raw_height;
    std::vector<std::uint16_t> out(static_cast<std::size_t>(W) * H, 0);
    if (!L.slices[0]) {
        std::copy(jpeg.samples.begin(), jpeg.samples.begin() + static_cast<std::ptrdiff_t>(out.size()),
                  out.begin());
        return out;
    }
    // 縦長の帯（幅 slices[1] が slices[0] 本、最後に幅 slices[2] が1本）を
    // 左から順に、各帯の中は上から行ごとに詰めてある。
    const std::size_t band = static_cast<std::size_t>(L.slices[1]) * H;
    const std::size_t total = out.size();
    for (std::size_t k = 0; k < total; ++k) {
        std::size_t i = k / band;
        int width = L.slices[1];
        if (i >= static_cast<std::size_t>(L.slices[0])) {
            i = static_cast<std::size_t>(L.slices[0]);
            width = L.slices[2];
        }
        const std::size_t j = k - i * band;
        const std::size_t row = j / static_cast<std::size_t>(width);
        const std::size_t col = j % static_cast<std::size_t>(width) + i * L.slices[1];
        if (row < static_cast<std::size_t>(H) && col < static_cast<std::size_t>(W)) {
            out[row * W + col] = jpeg.samples[k];
        }
    }
    return out;
}

// 左（足りなければ上）の遮光部の、位相ごとの平均。
void masked_black(const std::vector<std::uint16_t>& v, int W, int H, const Cr2Layout& L,
                  double black[4]) {
    double sum[4] = {0, 0, 0, 0}, count[4] = {0, 0, 0, 0};
    const auto add = [&](int x0, int x1, int y0, int y1) {
        for (int y = std::max(0, y0); y < std::min(H, y1); ++y) {
            for (int x = std::max(0, x0); x < std::min(W, x1); ++x) {
                const int p = (y & 1) * 2 + (x & 1);
                sum[p] += v[static_cast<std::size_t>(y) * W + x];
                count[p] += 1.0;
            }
        }
    };
    // 境目のそばは光が漏れることがあるので、端から少し離す。
    if (L.left >= 16) {
        add(4, L.left - 4, L.top, L.bottom + 1);
    } else if (L.top >= 16) {
        add(L.left, L.right + 1, 4, L.top - 4);
    }
    for (int p = 0; p < 4; ++p) black[p] = count[p] > 0.0 ? sum[p] / count[p] : 0.0;
}

// ---- DNG ---------------------------------------------------------------------

struct DngRaw {
    Tiff tiff;
    Ifd ifd;
    std::string camera;
    std::int64_t ticks = 0;
    int width = 0, height = 0, spp = 1, bits = 16;
    int compression = 1, photometric = 0, predictor = 1, sample_format = 1;
    bool cfa = false;
    int colors[2][2] = {{0, 1}, {1, 2}};  // ActiveArea の左上から見た色
    // 切り抜き（生の座標）。
    int area_x = 0, area_y = 0, area_w = 0, area_h = 0;  // ActiveArea
    int crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0;  // 最終（ActiveArea込み）
};

double round_half_up(double v) { return std::floor(v + 0.5); }

DngRaw parse_dng(const MappedFile& file) {
    DngRaw R;
    R.tiff = open_tiff(file, "DNG");
    const Tiff& t = R.tiff;
    const Ifd ifd0 = t.read_ifd(t.u32(4));
    if (!ifd0.find(50706)) fail("DNG", "DNGVersion タグがありません（DNGではないTIFFです）");
    const std::string make = t.ascii(ifd0, 271);
    const std::string model = t.ascii(ifd0, 272);
    const std::string unique = t.ascii(ifd0, 50708);
    R.camera = !model.empty() ? (model.compare(0, make.size(), make) == 0 || make.empty() ? model
                                                                                          : make + " " + model)
                              : unique;

    // 候補: IFD0 とその後ろの連鎖、それぞれの SubIFD（1段）。
    std::vector<Ifd> all;
    all.push_back(ifd0);
    {
        std::size_t o = ifd0.next;
        for (int i = 0; i < 8 && o; ++i) {
            all.push_back(t.read_ifd(o));
            o = all.back().next;
        }
    }
    const std::size_t top_count = all.size();
    for (std::size_t i = 0; i < top_count; ++i) {
        if (const Entry* sub = all[i].find(330)) {
            for (std::uint32_t k = 0; k < std::min<std::uint32_t>(sub->count, 16); ++k) {
                all.push_back(t.read_ifd(static_cast<std::size_t>(t.value(*sub, k))));
            }
        }
    }
    const Ifd* best = nullptr;
    double best_pixels = 0.0;
    for (const Ifd& ifd : all) {
        if (static_cast<int>(t.scalar(ifd, 254, 0)) != 0) continue;  // 縮小版・マスク
        const int ph = static_cast<int>(t.scalar(ifd, 262, 0));
        if (ph != 32803 && ph != 34892) continue;
        const double pixels = t.scalar(ifd, 256, 0) * t.scalar(ifd, 257, 0);
        if (pixels > best_pixels) {
            best_pixels = pixels;
            best = &ifd;
        }
    }
    if (!best) fail("DNG", "RAW画像（CFA・LinearRaw）が見つかりません");
    R.ifd = *best;
    const Ifd& ifd = R.ifd;

    R.width = static_cast<int>(t.scalar(ifd, 256, 0));
    R.height = static_cast<int>(t.scalar(ifd, 257, 0));
    R.spp = static_cast<int>(t.scalar(ifd, 277, 1));
    R.bits = static_cast<int>(t.scalar(ifd, 258, 16));
    R.compression = static_cast<int>(t.scalar(ifd, 259, 1));
    R.photometric = static_cast<int>(t.scalar(ifd, 262, 0));
    R.predictor = static_cast<int>(t.scalar(ifd, 317, 1));
    R.sample_format = static_cast<int>(t.scalar(ifd, 339, 1));
    R.cfa = R.photometric == 32803;
    if (R.width <= 0 || R.height <= 0) fail("DNG", "画像の寸法がありません");
    if (R.bits < 1 || R.bits > 32) fail("DNG", "ビット深度に対応していません");
    if (static_cast<int>(t.scalar(ifd, 284, 1)) != 1) fail("DNG", "プレーナ配置には対応していません");
    if (R.compression == 34892) fail("DNG", "非可逆圧縮（lossy）のDNGには対応していません");
    if (R.compression == 52546) fail("DNG", "JPEG XL で圧縮されたDNGには対応していません");
    if (R.compression != 1 && R.compression != 7 && R.compression != 8 && R.compression != 32946) {
        fail("DNG", "対応していない圧縮形式です (" + std::to_string(R.compression) + ")");
    }
    if (R.cfa) {
        if (R.spp != 1) fail("DNG", "CFAなのに1画素に複数の標本があります");
        if (static_cast<int>(t.scalar(ifd, 50711, 1)) != 1) fail("DNG", "正方格子でないCFAには対応していません");
        const Entry* dim = ifd.find(33421);
        const Entry* pat = ifd.find(33422);
        if (!dim || !pat || dim->count < 2 || pat->count < 4) fail("DNG", "CFAの並びがありません");
        if (t.value(*dim, 0) != 2 || t.value(*dim, 1) != 2) {
            fail("DNG", "2×2のBayer以外のCFA（X-Transなど）には対応していません");
        }
        // CFAPlaneColor（既定 0,1,2 = R,G,B）で番号を色に直す。
        int plane_color[4] = {0, 1, 2, 3};
        if (const Entry* pc = ifd.find(50710)) {
            for (std::uint32_t i = 0; i < std::min<std::uint32_t>(pc->count, 4); ++i) {
                plane_color[i] = static_cast<int>(t.value(*pc, i));
            }
        }
        for (int i = 0; i < 4; ++i) {
            const int v = static_cast<int>(t.value(*pat, static_cast<std::uint32_t>(i)));
            if (v < 0 || v > 3 || plane_color[v] > 2) fail("DNG", "RGB以外のCFAには対応していません");
            R.colors[i / 2][i % 2] = plane_color[v];
        }
    } else if (R.spp != 1 && R.spp != 3) {
        fail("DNG", "LinearRaw は1標本か3標本のみ対応しています");
    }

    // ActiveArea（上・左・下・右）と、その中の DefaultCrop。
    R.area_x = 0;
    R.area_y = 0;
    R.area_w = R.width;
    R.area_h = R.height;
    if (const Entry* a = ifd.find(50829)) {
        if (a->count >= 4) {
            const int top = static_cast<int>(t.value(*a, 0)), left = static_cast<int>(t.value(*a, 1));
            const int bottom = static_cast<int>(t.value(*a, 2)), right = static_cast<int>(t.value(*a, 3));
            if (top >= 0 && left >= 0 && bottom > top && right > left && bottom <= R.height && right <= R.width) {
                R.area_x = left;
                R.area_y = top;
                R.area_w = right - left;
                R.area_h = bottom - top;
            }
        }
    }
    int cx = 0, cy = 0, cw = R.area_w, ch = R.area_h;
    const Entry* co = ifd.find(50719);
    const Entry* cs = ifd.find(50720);
    if (co && cs && co->count >= 2 && cs->count >= 2) {
        cx = static_cast<int>(round_half_up(t.value(*co, 0)));
        cy = static_cast<int>(round_half_up(t.value(*co, 1)));
        cw = static_cast<int>(round_half_up(t.value(*cs, 0)));
        ch = static_cast<int>(round_half_up(t.value(*cs, 1)));
        if (cx < 0 || cy < 0 || cw <= 0 || ch <= 0 || cx + cw > R.area_w || cy + ch > R.area_h) {
            cx = 0;
            cy = 0;
            cw = R.area_w;
            ch = R.area_h;
        }
    }
    R.crop_x = R.area_x + cx;
    R.crop_y = R.area_y + cy;
    R.crop_w = cw;
    R.crop_h = ch;

    const ExifInfo exif = read_exif(t, ifd0);
    int tz = 0;
    if (parse_offset(exif.offset, tz)) R.ticks = exif_datetime_to_ticks(exif.datetime, exif.subsec, tz);
    return R;
}

float half_to_float(std::uint16_t h) {
    const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
    const int exp = (h >> 10) & 0x1F;
    std::uint32_t mant = h & 0x3FFu;
    std::uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {  // 非正規化数を正規化する
            int e = -1;
            do {
                ++e;
                mant <<= 1;
            } while ((mant & 0x400u) == 0);
            mant &= 0x3FFu;
            bits = sign | (static_cast<std::uint32_t>(127 - 15 - e) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | (static_cast<std::uint32_t>(exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

float fp24_to_float(std::uint32_t v) {
    // 符号1・指数7（偏り63）・仮数16。
    const std::uint32_t sign = (v & 0x800000u) << 8;
    const int exp = static_cast<int>((v >> 16) & 0x7F);
    const std::uint32_t mant = v & 0xFFFFu;
    std::uint32_t bits;
    if (exp == 0 && mant == 0) bits = sign;
    else if (exp == 0x7F) bits = sign | 0x7F800000u | (mant << 7);
    else if (exp == 0) bits = sign;  // 非正規化数は0とみなす（天体のRAWでは出ない）
    else bits = sign | (static_cast<std::uint32_t>(exp - 63 + 127) << 23) | (mant << 7);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// タイル・ストリップ1つを展開して、生の標本（浮動小数点で持つ）を並べる。
// out は画像全体（width*height*spp）。chunk は (x0, y0) から cw×chunk_rows。
void decode_dng_chunk(const DngRaw& R, const std::uint8_t* src, std::size_t size, int x0, int y0,
                      int cw, int chunk_rows, std::vector<float>& out) {
    const int spp = R.spp;
    const int rows_here = std::min(chunk_rows, R.height - y0);
    const int cols_here = std::min(cw, R.width - x0);
    if (rows_here <= 0 || cols_here <= 0) return;
    const bool floating = R.sample_format == 3;
    const std::size_t row_samples = static_cast<std::size_t>(cw) * spp;

    const auto put = [&](int y, int x, int s, float v) {
        if (y >= rows_here || x >= cols_here) return;
        out[(static_cast<std::size_t>(y0 + y) * R.width + (x0 + x)) * spp + s] = v;
    };

    if (R.compression == 7) {
        if (floating) fail("DNG", "浮動小数点のロスレスJPEGには対応していません");
        const LosslessJpegImage jpeg = decode_lossless_jpeg(src, size, row_samples * chunk_rows * 4);
        // 標本は行優先に並んでいる（JPEGの1行がタイルの何行ぶんかは書き手によって違う）。
        const std::size_t total = std::min(jpeg.samples.size(), row_samples * static_cast<std::size_t>(chunk_rows));
        for (std::size_t k = 0; k < total; ++k) {
            const int y = static_cast<int>(k / row_samples);
            const std::size_t r = k % row_samples;
            put(y, static_cast<int>(r / spp), static_cast<int>(r % spp), jpeg.samples[k]);
        }
        return;
    }

    // 無圧縮・Deflate: バイト列にしてから標本を取り出す。
    const int bits = R.bits;
    const bool byte_aligned = bits == 8 || bits == 16 || bits == 24 || bits == 32;
    const std::size_t row_bytes = byte_aligned ? row_samples * (bits / 8) : (row_samples * bits + 7) / 8;
    const std::size_t need = row_bytes * static_cast<std::size_t>(chunk_rows);
    std::vector<std::uint8_t> buffer;
    const std::uint8_t* bytes = src;
    if (R.compression == 8 || R.compression == 32946) {
        buffer = inflate_zlib(src, size, need);
        if (buffer.size() < row_bytes * static_cast<std::size_t>(rows_here)) fail("DNG", "Deflateの展開結果が足りません");
        buffer.resize(std::max(buffer.size(), need), 0);
        bytes = buffer.data();
    } else if (size < row_bytes * static_cast<std::size_t>(rows_here)) {
        fail("DNG", "画素データが足りません");
    }

    // 予測子を戻す（Deflateのとき）。
    const int p = R.predictor;
    if (p != 1) {
        if (bytes != buffer.data()) {
            buffer.assign(bytes, bytes + need);
            bytes = buffer.data();
        }
        std::uint8_t* b = buffer.data();
        if (p == 2 || p == 34892 || p == 34893) {
            if (!byte_aligned || floating) fail("DNG", "この予測子とビット深度の組み合わせには対応していません");
            const int stride = (p == 2 ? 1 : (p == 34892 ? 2 : 4)) * spp;
            const int bps = bits / 8;
            for (int y = 0; y < rows_here; ++y) {
                std::uint8_t* row = b + static_cast<std::size_t>(y) * row_bytes;
                for (std::size_t i = static_cast<std::size_t>(stride); i < row_samples; ++i) {
                    std::uint8_t* cur = row + i * bps;
                    const std::uint8_t* prev = row + (i - stride) * bps;
                    if (bps == 1) {
                        cur[0] = static_cast<std::uint8_t>(cur[0] + prev[0]);
                    } else {
                        std::uint64_t a = 0, c = 0;
                        for (int k = 0; k < bps; ++k) {
                            const int shift = R.tiff.little ? 8 * k : 8 * (bps - 1 - k);
                            a |= static_cast<std::uint64_t>(cur[k]) << shift;
                            c |= static_cast<std::uint64_t>(prev[k]) << shift;
                        }
                        const std::uint64_t v = a + c;
                        for (int k = 0; k < bps; ++k) {
                            const int shift = R.tiff.little ? 8 * k : 8 * (bps - 1 - k);
                            cur[k] = static_cast<std::uint8_t>(v >> shift);
                        }
                    }
                }
            }
        } else if (p == 3 || p == 34894 || p == 34895) {
            // 浮動小数点予測子: バイトの差分を戻してから、上位バイトから順に分けて
            // 並べられたバイト列を標本ごとに組み直す（標本は上位バイトが先＝ビッグエンディアン）。
            if (!byte_aligned) fail("DNG", "浮動小数点予測子のビット深度が不正です");
            const std::size_t stride = static_cast<std::size_t>((p == 3 ? 1 : (p == 34894 ? 2 : 4)) * spp);
            const int bps = bits / 8;
            std::vector<std::uint8_t> tmp(row_bytes);
            for (int y = 0; y < rows_here; ++y) {
                std::uint8_t* row = b + static_cast<std::size_t>(y) * row_bytes;
                for (std::size_t i = stride; i < row_bytes; ++i) row[i] = static_cast<std::uint8_t>(row[i] + row[i - stride]);
                for (std::size_t i = 0; i < row_samples; ++i) {
                    for (int k = 0; k < bps; ++k) tmp[i * bps + k] = row[static_cast<std::size_t>(k) * row_samples + i];
                }
                std::memcpy(row, tmp.data(), row_bytes);
            }
        } else {
            fail("DNG", "対応していない予測子です (" + std::to_string(p) + ")");
        }
    }
    // 浮動小数点予測子を戻した後は、標本は常にビッグエンディアン。
    const bool big = !R.tiff.little || (p == 3 || p == 34894 || p == 34895);

    for (int y = 0; y < rows_here; ++y) {
        const std::uint8_t* row = bytes + static_cast<std::size_t>(y) * row_bytes;
        if (!byte_aligned) {
            // 10/12/14bit などは上位ビットから詰めてある（行ごとにバイト境界へ揃える）。
            std::uint64_t acc = 0;
            int have = 0;
            std::size_t pos = 0;
            for (std::size_t i = 0; i < row_samples; ++i) {
                while (have < bits) {
                    acc = (acc << 8) | row[pos++];
                    have += 8;
                }
                const std::uint32_t v = static_cast<std::uint32_t>(acc >> (have - bits)) & ((1u << bits) - 1u);
                have -= bits;
                put(y, static_cast<int>(i / spp), static_cast<int>(i % spp), static_cast<float>(v));
            }
            continue;
        }
        const int bps = bits / 8;
        for (std::size_t i = 0; i < row_samples; ++i) {
            const std::uint8_t* q = row + i * bps;
            std::uint32_t u = 0;
            for (int k = 0; k < bps; ++k) {
                const int shift = big ? 8 * (bps - 1 - k) : 8 * k;
                u |= static_cast<std::uint32_t>(q[k]) << shift;
            }
            float v;
            if (floating) {
                if (bits == 16) v = half_to_float(static_cast<std::uint16_t>(u));
                else if (bits == 24) v = fp24_to_float(u);
                else if (bits == 32) std::memcpy(&v, &u, sizeof(v));
                else fail("DNG", "浮動小数点のビット深度に対応していません");
            } else {
                v = static_cast<float>(u);
            }
            put(y, static_cast<int>(i / spp), static_cast<int>(i % spp), v);
        }
    }
}

std::vector<float> decode_dng_values(const DngRaw& R) {
    const Tiff& t = R.tiff;
    const Ifd& ifd = R.ifd;
    const bool tiled = ifd.find(322) != nullptr;
    const int cw = tiled ? static_cast<int>(t.scalar(ifd, 322, 0)) : R.width;
    const int ch = tiled ? static_cast<int>(t.scalar(ifd, 323, 0))
                         : static_cast<int>(std::min<double>(t.scalar(ifd, 278, R.height), R.height));
    if (cw <= 0 || ch <= 0) fail("DNG", "タイルまたはストリップの大きさが不正です");
    const Entry* offsets = ifd.find(tiled ? 324 : 273);
    const Entry* counts = ifd.find(tiled ? 325 : 279);
    if (!offsets || !counts || offsets->count != counts->count) fail("DNG", "画素データの位置がありません");
    const int across = tiled ? (R.width + cw - 1) / cw : 1;
    const int down = (R.height + ch - 1) / ch;
    if (offsets->count < static_cast<std::uint32_t>(across * down)) fail("DNG", "タイルの数が足りません");

    std::vector<float> out(static_cast<std::size_t>(R.width) * R.height * R.spp, 0.0f);
    // タイルは互いに独立なので並列に展開する（書き込む範囲が重ならない）。
    const int chunks = across * down;
    detail::parallel_rows(chunks, [&](int c0, int c1) {
        for (int c = c0; c < c1; ++c) {
            const std::size_t o = static_cast<std::size_t>(t.value(*offsets, static_cast<std::uint32_t>(c)));
            const std::size_t n = static_cast<std::size_t>(t.value(*counts, static_cast<std::uint32_t>(c)));
            t.need(o, n);
            decode_dng_chunk(R, t.d + o, n, (c % across) * cw, (c / across) * ch, cw, ch, out);
        }
    }, 1);
    return out;
}

// 黒・白で 0..1 にし、切り抜いて FrameBuffer に入れる。
void normalize_dng(const DngRaw& R, std::vector<float>& raw, FrameBuffer& out) {
    const Tiff& t = R.tiff;
    const Ifd& ifd = R.ifd;
    const int spp = R.spp;

    // LinearizationTable（整数の標本だけ）。
    if (const Entry* lt = ifd.find(50712)) {
        if (R.sample_format != 3 && lt->count > 0) {
            const std::vector<double> table = t.values(*lt);
            const std::size_t last = table.size() - 1;
            for (float& v : raw) {
                const std::size_t i = std::min(last, static_cast<std::size_t>(v < 0.0f ? 0.0f : v));
                v = static_cast<float>(table[i]);
            }
        }
    }

    // BlackLevel は ActiveArea の左上から繰り返す（BlackLevelRepeatDim、既定1×1）。
    int rr = 1, rc = 1;
    if (const Entry* d = ifd.find(50713)) {
        if (d->count >= 2) {
            rr = std::max(1, static_cast<int>(t.value(*d, 0)));
            rc = std::max(1, static_cast<int>(t.value(*d, 1)));
        }
    }
    std::vector<double> black(static_cast<std::size_t>(rr) * rc * spp, 0.0);
    if (const Entry* b = ifd.find(50714)) {
        const std::vector<double> v = t.values(*b);
        for (std::size_t i = 0; i < black.size(); ++i) black[i] = v.empty() ? 0.0 : v[std::min(i, v.size() - 1)];
    }
    std::vector<double> delta_h, delta_v;
    if (const Entry* e = ifd.find(50715)) delta_h = t.values(*e);
    if (const Entry* e = ifd.find(50716)) delta_v = t.values(*e);
    std::vector<double> white(static_cast<std::size_t>(spp), 0.0);
    {
        const double fallback = R.sample_format == 3 ? 1.0 : std::ldexp(1.0, R.bits) - 1.0;
        const Entry* w = ifd.find(50717);
        const std::vector<double> v = w ? t.values(*w) : std::vector<double>();
        for (int s = 0; s < spp; ++s) {
            white[static_cast<std::size_t>(s)] =
                v.empty() ? fallback : v[std::min(static_cast<std::size_t>(s), v.size() - 1)];
        }
    }

    const int W = R.crop_w, H = R.crop_h;
    out.reset(W, H, spp);
    out.set_source_bit_depth(R.sample_format == 3 ? 16 : std::min(16, R.bits));
    detail::parallel_rows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const int ry = R.crop_y + y;        // 生の座標
            const int ay = ry - R.area_y;       // ActiveArea の中の座標
            const double dv = ay >= 0 && static_cast<std::size_t>(ay) < delta_v.size() ? delta_v[static_cast<std::size_t>(ay)] : 0.0;
            for (int x = 0; x < W; ++x) {
                const int rx = R.crop_x + x;
                const int ax = rx - R.area_x;
                const double dh = ax >= 0 && static_cast<std::size_t>(ax) < delta_h.size() ? delta_h[static_cast<std::size_t>(ax)] : 0.0;
                for (int s = 0; s < spp; ++s) {
                    const double b = black[(static_cast<std::size_t>(ay % rr) * rc + static_cast<std::size_t>(ax % rc)) * spp + s] + dh + dv;
                    const double wv = white[static_cast<std::size_t>(s)];
                    const double v = raw[(static_cast<std::size_t>(ry) * R.width + rx) * spp + s];
                    double n = wv > b ? (v - b) / (wv - b) : 0.0;
                    if (!(n > 0.0)) n = 0.0;  // NaN も0へ
                    if (n > 1.0) n = 1.0;
                    out.row(s, y)[x] = static_cast<float>(n);
                }
            }
        }
    });
    out.invalidate_luma();
}

void fill_dng_info(const DngRaw& R, ImageFileInfo& info) {
    info.width = R.crop_w;
    info.height = R.crop_h;
    info.channels = R.spp;
    info.bit_depth = R.sample_format == 3 ? 16 : R.bits;
    // CFAPattern の並びは ActiveArea の左上から数える（DNG仕様。LibRaw も同じ読み方）。
    info.color = R.cfa ? bayer_id(R.colors, R.crop_x - R.area_x, R.crop_y - R.area_y)
                       : (R.spp == 3 ? SerColorId::RGB : SerColorId::Mono);
    info.format = R.cfa ? "DNG" : "DNG（LinearRaw）";
    info.camera = R.camera;
    info.timestamp_ticks = R.ticks;
}

void fill_cr2_info(const Cr2Layout& L, ImageFileInfo& info) {
    info.width = L.right - L.left + 1;
    info.height = L.bottom - L.top + 1;
    info.channels = 1;
    info.bit_depth = L.jpeg.precision;
    info.color = bayer_id(L.colors, L.left, L.top);
    info.format = "CR2";
    info.camera = L.camera;
    info.timestamp_ticks = L.ticks;
}

void check_size(const ImageFileInfo& info, const char* format) {
    if (info.width <= 0 || info.height <= 0) fail(format, "画像の寸法がありません");
    if (static_cast<double>(info.width) * info.height > 400.0e6) fail(format, "画像が大きすぎます");
}

}  // namespace

std::int64_t exif_datetime_to_ticks(const std::string& datetime, const std::string& subsec,
                                    int offset_minutes) {
    // "YYYY:MM:DD HH:MM:SS"
    if (datetime.size() < 19) return 0;
    const auto num = [&](std::size_t pos, std::size_t len, int& v) {
        v = 0;
        for (std::size_t i = pos; i < pos + len; ++i) {
            if (datetime[i] < '0' || datetime[i] > '9') return false;
            v = v * 10 + (datetime[i] - '0');
        }
        return true;
    };
    int Y, M, D, h, m, s;
    if (!num(0, 4, Y) || !num(5, 2, M) || !num(8, 2, D) || !num(11, 2, h) || !num(14, 2, m) ||
        !num(17, 2, s)) {
        return 0;
    }
    if (Y < 1900 || M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || m > 59 || s > 60) return 0;
    constexpr std::int64_t kTicksPerSecond = 10000000;
    // 1970-01-01 は 0001-01-01 から 719162 日後。
    const std::int64_t days = days_from_civil(Y, M, D) + 719162;
    std::int64_t ticks = (days * 86400 + h * 3600 + m * 60 + s - static_cast<std::int64_t>(offset_minutes) * 60) *
                         kTicksPerSecond;
    // SubSecTime は小数点以下の数字列。
    std::int64_t frac = 0, scale = kTicksPerSecond;
    for (char c : subsec) {
        if (c < '0' || c > '9') break;
        if (scale < 10) break;
        scale /= 10;
        frac += (c - '0') * scale;
    }
    return ticks + frac;
}

bool is_raw_image_path(const std::string& path) {
    const std::string ext = lower_extension(path);
    return ext == "cr2" || ext == "dng";
}

ImageFileInfo probe_raw_image(const std::string& path) {
    MappedFile file;
    file.open(path);
    ImageFileInfo info;
    const std::string ext = lower_extension(path);
    if (ext == "cr2") {
        const Cr2Layout L = parse_cr2(file);
        fill_cr2_info(L, info);
        check_size(info, "CR2");
    } else if (ext == "dng") {
        const DngRaw R = parse_dng(file);
        fill_dng_info(R, info);
        check_size(info, "DNG");
    } else {
        throw std::runtime_error("RAWではない拡張子です: " + path);
    }
    return info;
}

void read_raw_image(const std::string& path, FrameBuffer& out, ImageFileInfo& info) {
    MappedFile file;
    file.open(path);
    file.advise_sequential();
    const std::string ext = lower_extension(path);
    if (ext == "cr2") {
        const Cr2Layout L = parse_cr2(file);
        fill_cr2_info(L, info);
        check_size(info, "CR2");
        const std::vector<std::uint16_t> v = decode_cr2_values(L);
        double black[4];
        masked_black(v, L.raw_width, L.raw_height, L, black);
        const double white = std::ldexp(1.0, L.jpeg.precision) - 1.0;
        const int W = info.width, H = info.height;
        out.reset(W, H, 1);
        out.set_source_bit_depth(L.jpeg.precision);
        detail::parallel_rows(H, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                const int ry = L.top + y;
                const std::uint16_t* src = v.data() + static_cast<std::size_t>(ry) * L.raw_width + L.left;
                float* dst = out.row(0, y);
                for (int x = 0; x < W; ++x) {
                    const double b = black[(ry & 1) * 2 + ((L.left + x) & 1)];
                    double n = (src[x] - b) / (white - b);
                    if (n < 0.0) n = 0.0;
                    if (n > 1.0) n = 1.0;
                    dst[x] = static_cast<float>(n);
                }
            }
        });
        out.invalidate_luma();
    } else if (ext == "dng") {
        const DngRaw R = parse_dng(file);
        fill_dng_info(R, info);
        check_size(info, "DNG");
        std::vector<float> raw = decode_dng_values(R);
        normalize_dng(R, raw, out);
    } else {
        throw std::runtime_error("RAWではない拡張子です: " + path);
    }
}

RawSensorData read_cr2_sensor_data(const std::string& path) {
    MappedFile file;
    file.open(path);
    const Cr2Layout L = parse_cr2(file);
    RawSensorData d;
    d.width = L.raw_width;
    d.height = L.raw_height;
    d.values = decode_cr2_values(L);
    d.crop_x = L.left;
    d.crop_y = L.top;
    d.crop_width = L.right - L.left + 1;
    d.crop_height = L.bottom - L.top + 1;
    masked_black(d.values, d.width, d.height, L, d.black);
    d.bits = L.jpeg.precision;
    d.white = std::ldexp(1.0, d.bits) - 1.0;
    return d;
}

}  // namespace stackcore
