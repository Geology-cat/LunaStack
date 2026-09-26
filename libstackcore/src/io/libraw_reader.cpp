#include "libraw_reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>

#include "libraw/libraw.h"
#include "stackcore/mapped_file.hpp"
#include "stackcore/raw_reader.hpp"

#include "../common/parallel_rows.hpp"

namespace stackcore {
namespace detail {
namespace {

std::string upper_extension(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
    for (char& c : ext) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return ext;
}

[[noreturn]] void fail(const std::string& path, const std::string& what) {
    throw std::runtime_error(upper_extension(path) + ": " + what);
}

// ---- 撮影時刻（EXIF とメーカーノートを LibRaw の解析中に拾う） --------------------
//
// LibRaw 自身の timestamp は、撮影地の現地時刻を**この Mac の**時間帯で解釈した値なので
// 使わない。EXIF の日時の文字列と時差を自分で集め、時差が分かるときだけ UTC にする。
struct TimeTags {
    LibRaw* raw = nullptr;
    std::string datetime, subsec, offset;
    int tz_minutes = 0;
    bool tz_known = false;
};

std::string read_string(void* ifp, int len) {
    if (len <= 0 || len > 64) return "";
    char buf[65] = {};
    static_cast<LibRaw_abstract_datastream*>(ifp)->read(buf, 1, static_cast<size_t>(len));
    std::string s(buf, strnlen(buf, static_cast<size_t>(len)));
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    return s;
}

long read_int(const unsigned char* p, int bytes, unsigned ord, bool is_signed) {
    unsigned long v = 0;
    for (int i = 0; i < bytes; ++i) {
        const int k = ord == 0x4949 ? i : bytes - 1 - i;
        v |= static_cast<unsigned long>(p[k]) << (8 * i);
    }
    if (is_signed && bytes < static_cast<int>(sizeof(long)) && (v >> (8 * bytes - 1)) & 1u) {
        return static_cast<long>(v) - (1L << (8 * bytes));
    }
    return static_cast<long>(v);
}

void exif_callback(void* context, int tag, int type, int len, unsigned int ord, void* ifp, INT64) {
    (void)type;
    TimeTags* t = static_cast<TimeTags*>(context);
    if ((tag >> 16) != 0) return;  // 別のIFD（GPS・Kodak など）の同じ番号は見ない
    switch (tag & 0xFFFF) {
        case 0x9003: if (t->datetime.empty()) t->datetime = read_string(ifp, len); break;  // DateTimeOriginal
        case 0x9291: if (t->subsec.empty()) t->subsec = read_string(ifp, len); break;      // SubSecTimeOriginal
        case 0x9011: if (t->offset.empty()) t->offset = read_string(ifp, len); break;      // OffsetTimeOriginal
        default: break;
    }
    (void)ord;
}

void makernote_callback(void* context, int tag, int type, int len, unsigned int ord, void* ifp, INT64) {
    TimeTags* t = static_cast<TimeTags*>(context);
    if (t->tz_known || (tag >> 16) != 0) return;
    const char* make = t->raw->imgdata.idata.make;
    unsigned char buf[16] = {};
    if (std::strncmp(make, "Canon", 5) == 0 && (tag & 0xFFFF) == 0x0035 && (type == 4 || type == 9 || type == 7)) {
        // TimeInfo: int32 ×4。[1] が時差（分）で、夏時間のぶんも含む（[3] の夏時間を足さない。
        // EOS R6 の実物で TimeZone=120・DaylightSavings=60 のとき EXIF の時差は +02:00）。
        const int bytes = type == 7 ? len : len * 4;
        if (bytes < 16) return;
        static_cast<LibRaw_abstract_datastream*>(ifp)->read(buf, 1, 16);
        t->tz_minutes = static_cast<int>(read_int(buf + 4, 4, ord, true));
        t->tz_known = true;
    }
    // ほかの社のメーカーノートの時差は、実物で確かめられていないので使わない
    // （EXIF の OffsetTimeOriginal があればそれを使う）。
}

bool parse_offset(const std::string& s, int& minutes) {
    if (s.size() < 6 || (s[0] != '+' && s[0] != '-') || s[3] != ':') return false;
    for (int i : {1, 2, 4, 5}) {
        if (s[static_cast<std::size_t>(i)] < '0' || s[static_cast<std::size_t>(i)] > '9') return false;
    }
    minutes = (s[0] == '-' ? -1 : 1) * (((s[1] - '0') * 10 + (s[2] - '0')) * 60 + (s[4] - '0') * 10 + (s[5] - '0'));
    return true;
}

// ---- LibRaw を開く -----------------------------------------------------------------

struct Opened {
    MappedFile file;
    std::unique_ptr<LibRaw> raw;
    TimeTags times;
};

void open_raw(const std::string& path, Opened& o) {
    o.file.open(path);
    o.raw.reset(new LibRaw(0));
    o.times.raw = o.raw.get();
    o.raw->set_exifparser_handler(exif_callback, &o.times);
    o.raw->set_makernotes_handler(makernote_callback, &o.times);
    // メモリに割り付けたファイルをそのまま渡す（パスの文字コードにも左右されない）。
    const int rc = o.raw->open_buffer(o.file.data(), o.file.size());
    if (rc != LIBRAW_SUCCESS) fail(path, std::string("読めませんでした（") + libraw_strerror(rc) + "）");
    const libraw_iparams_t& id = o.raw->imgdata.idata;
    if (o.raw->imgdata.rawdata.ioparams.fuji_width) fail(path, "斜め配列（SuperCCD）のセンサーには対応していません");
    if (id.is_foveon) fail(path, "Foveon のセンサーには対応していません");
    if (id.filters > 1000 && id.colors != 3) fail(path, "RGB以外のカラーフィルター（CMYGなど）には対応していません");
    if (id.filters != 0 && id.filters != 9 && id.filters <= 1000) fail(path, "このカラーフィルターの並びには対応していません");
    if (id.filters == 0 && id.colors != 1 && id.colors != 3) fail(path, "この色の数には対応していません");
}

// 切り抜く範囲（LibRaw の見える範囲の座標）。機種の既定の範囲（CR2 の SensorInfo・
// DNG の DefaultCrop など）があればそれ、無ければ見える範囲全体。
struct Crop {
    int x = 0, y = 0, w = 0, h = 0;
};

Crop crop_of(const LibRaw& raw) {
    const libraw_image_sizes_t& S = raw.imgdata.sizes;
    Crop c;
    c.w = S.width;
    c.h = S.height;
    const libraw_raw_inset_crop_t& in = S.raw_inset_crops[0];
    const int x = static_cast<int>(in.cleft) - S.left_margin;
    const int y = static_cast<int>(in.ctop) - S.top_margin;
    if (in.cwidth > 0 && in.cheight > 0 && in.cleft != 0xFFFF && in.ctop != 0xFFFF && x >= 0 && y >= 0 &&
        x + in.cwidth <= S.width && y + in.cheight <= S.height) {
        c.x = x;
        c.y = y;
        c.w = in.cwidth;
        c.h = in.cheight;
    }
    return c;
}

SerColorId bayer_of(LibRaw& raw, const Crop& crop) {
    int c[2][2];
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            const int v = raw.COLOR(crop.y + y, crop.x + x);
            c[y][x] = v == 3 ? 1 : v;
        }
    }
    if (c[0][0] == 0 && c[0][1] == 1 && c[1][0] == 1 && c[1][1] == 2) return SerColorId::BayerRGGB;
    if (c[0][0] == 1 && c[0][1] == 0 && c[1][0] == 2 && c[1][1] == 1) return SerColorId::BayerGRBG;
    if (c[0][0] == 1 && c[0][1] == 2 && c[1][0] == 0 && c[1][1] == 1) return SerColorId::BayerGBRG;
    if (c[0][0] == 2 && c[0][1] == 1 && c[1][0] == 1 && c[1][1] == 0) return SerColorId::BayerBGGR;
    return SerColorId::Mono;
}

void fill_info(const std::string& path, Opened& o, ImageFileInfo& info) {
    LibRaw& r = *o.raw;
    const libraw_iparams_t& id = r.imgdata.idata;
    const Crop crop = crop_of(r);
    info.width = crop.w;
    info.height = crop.h;
    if (info.width <= 0 || info.height <= 0) fail(path, "画像の寸法がありません");
    if (static_cast<double>(info.width) * info.height > 400.0e6) fail(path, "画像が大きすぎます");
    const bool xtrans = id.filters == 9;
    if (id.filters > 1000) {
        info.channels = 1;
        info.color = bayer_of(r, crop);
        if (info.color == SerColorId::Mono) fail(path, "Bayerの並びが読めません");
    } else if (xtrans || id.colors == 3) {
        info.channels = 3;  // X-Trans はここで色補間してRGBで返す
        info.color = SerColorId::RGB;
    } else {
        info.channels = 1;
        info.color = SerColorId::Mono;
    }
    unsigned bits = r.imgdata.color.raw_bps;
    if (bits == 0 || bits > 16) {
        bits = 1;
        while (bits < 16 && (1u << bits) <= r.imgdata.color.maximum) ++bits;
    }
    info.bit_depth = static_cast<int>(bits);
    info.format = upper_extension(path) + (xtrans ? "（X-Trans）" : (id.filters == 0 ? "（色補間済み）" : ""));
    const std::string make = id.make, model = id.model;
    info.camera = model.compare(0, make.size(), make) == 0 || make.empty() ? model : make + " " + model;
    int tz = 0;
    bool tz_known = parse_offset(o.times.offset, tz);
    if (!tz_known && o.times.tz_known) {
        tz = o.times.tz_minutes;
        tz_known = true;
    }
    info.timestamp_ticks = 0;
    if (tz_known && tz >= -14 * 60 && tz <= 14 * 60) {
        info.timestamp_ticks = exif_datetime_to_ticks(o.times.datetime, o.times.subsec, tz);
    }
}

// 飽和点（生の値）。メーカーノートの線形の上限があればそれ、無ければ機種の最大値。
double white_of(const LibRaw& r) {
    const libraw_colordata_t& c = r.imgdata.color;
    const double maximum = c.maximum;
    const double linear = c.linear_max[0];
    return linear > 0.0 && linear < maximum ? linear : maximum;
}

// X-Trans（6×6）の簡単な色補間。無い色は近傍（3×3、無ければ5×5）の同じ色の平均で埋める。
// 線形の平均だけなので、リニアのまま・決定論的に求まる。
void demosaic_xtrans(const std::vector<float>& cfa, const int pattern[6][6], int w, int h, FrameBuffer& out) {
    out.reset(w, h, 3);
    parallel_rows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* dst[3] = {out.row(0, y), out.row(1, y), out.row(2, y)};
            for (int x = 0; x < w; ++x) {
                const int own = pattern[y % 6][x % 6];
                for (int c = 0; c < 3; ++c) {
                    if (c == own) {
                        dst[c][x] = cfa[static_cast<std::size_t>(y) * w + x];
                        continue;
                    }
                    double sum = 0.0;
                    int count = 0;
                    for (int radius = 1; radius <= 2 && count == 0; ++radius) {
                        for (int dy = -radius; dy <= radius; ++dy) {
                            const int yy = y + dy;
                            if (yy < 0 || yy >= h) continue;
                            for (int dx = -radius; dx <= radius; ++dx) {
                                const int xx = x + dx;
                                if (xx < 0 || xx >= w || pattern[yy % 6][xx % 6] != c) continue;
                                sum += cfa[static_cast<std::size_t>(yy) * w + xx];
                                ++count;
                            }
                        }
                    }
                    dst[c][x] = count ? static_cast<float>(sum / count) : 0.0f;
                }
            }
        }
    });
    out.invalidate_luma();
}

}  // namespace

bool libraw_extension(const std::string& ext) {
    static const char* const kExtensions[] = {
        "cr2", "cr3", "crw", "dng", "nef", "nrw", "arw", "srf", "sr2", "raf", "orf", "rw2", "raw", "rwl",
        "pef", "srw", "3fr", "fff", "iiq", "erf", "kdc", "dcr", "mrw", "mos", "mef", "gpr",
    };
    for (const char* e : kExtensions) {
        if (ext == e) return true;
    }
    return false;
}

ImageFileInfo libraw_probe(const std::string& path) {
    Opened o;
    open_raw(path, o);
    ImageFileInfo info;
    fill_info(path, o, info);
    return info;
}

void libraw_read(const std::string& path, FrameBuffer& out, ImageFileInfo& info) {
    Opened o;
    open_raw(path, o);
    fill_info(path, o, info);
    LibRaw& r = *o.raw;
    int rc = r.unpack();
    if (rc != LIBRAW_SUCCESS) fail(path, std::string("展開できませんでした（") + libraw_strerror(rc) + "）");
    // 黒は展開したあとに決まる形式がある（遮光部から測るなど）。白も黒を引いた後の値にする。
    const double black = r.imgdata.color.black;
    double white_raw = white_of(r);
    // 浮動小数点のDNGは LibRaw が整数へ直すときに倍率 fnorm を掛けている。白レベルを超える値
    // （HDR）があると、その画像の最大値に合わせて縮めるので、そのままだと1枚ごとに明るさの
    // 基準が変わる。DNG の WhiteLevel に同じ倍率を掛けた値を白にして、基準を画像によらず一定にする
    // （白を超えたところは1で切る）。
    const libraw_colordata_t& color = r.imgdata.color;
    if (color.fnorm > 0.0f && color.dng_levels.dng_whitelevel[0] > 0) {
        white_raw = static_cast<double>(color.dng_levels.dng_whitelevel[0]) * color.fnorm;
    }
    // 見える範囲を切り出しながら黒を引く（LibRaw の dcraw_process と同じ引き方。色補間はしない）。
    rc = r.raw2image_ex(1);
    if (rc != LIBRAW_SUCCESS) fail(path, std::string("画素を取り出せませんでした（") + libraw_strerror(rc) + "）");
    const libraw_image_sizes_t& S = r.imgdata.sizes;
    const int stride = S.iwidth;
    if (S.iwidth != S.width || S.iheight != S.height) fail(path, "画像の寸法が食い違っています");
    const Crop crop = crop_of(r);
    const int W = crop.w, H = crop.h;
    if (W != info.width || H != info.height) fail(path, "画像の寸法が食い違っています");
    // raw2image_ex の後の maximum は黒を引いた後の値。linear_max は生の値なので黒を引く。
    double white = r.imgdata.color.maximum;
    if (color.fnorm > 0.0f && white_raw - black > 0.0) {
        white = white_raw - black;
    } else if (white_raw - black > 0.0 && white_raw - black < white) {
        white = white_raw - black;
    }
    if (!(white > 0.0)) fail(path, "白レベルが分かりません");
    const float scale = static_cast<float>(1.0 / white);
    const libraw_iparams_t& id = r.imgdata.idata;
    unsigned short (*image)[4] = r.imgdata.image;
    if (!image) fail(path, "画素がありません");
    // 切り抜いた範囲の (x, y) にあたる画素。
    const auto at = [&](int x, int y) -> unsigned short* {
        return image[static_cast<std::size_t>(crop.y + y) * stride + (crop.x + x)];
    };

    const auto norm = [scale](unsigned short v) {
        const float n = v * scale;
        return n > 1.0f ? 1.0f : n;
    };
    if (id.filters > 1000 || (id.filters == 0 && id.colors == 1)) {
        // Bayer（またはモノクロ）: 画素ごとに、その位置の色の値だけが入っている。
        out.reset(W, H, 1);
        parallel_rows(H, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                float* dst = out.row(0, y);
                for (int x = 0; x < W; ++x) {
                    const int c = id.filters ? r.COLOR(crop.y + y, crop.x + x) : 0;
                    dst[x] = norm(at(x, y)[c]);
                }
            }
        });
        out.invalidate_luma();
    } else if (id.filters == 9) {
        int pattern[6][6];
        for (int y = 0; y < 6; ++y) {
            for (int x = 0; x < 6; ++x) {
                const int c = r.COLOR(crop.y + y, crop.x + x);
                pattern[y][x] = c == 3 ? 1 : c;
            }
        }
        std::vector<float> cfa(static_cast<std::size_t>(W) * H);
        parallel_rows(H, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                for (int x = 0; x < W; ++x) {
                    cfa[static_cast<std::size_t>(y) * W + x] = norm(at(x, y)[pattern[y % 6][x % 6]]);
                }
            }
        });
        demosaic_xtrans(cfa, pattern, W, H, out);
    } else {
        // 色補間済み（3色）。
        out.reset(W, H, 3);
        parallel_rows(H, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                for (int c = 0; c < 3; ++c) {
                    float* dst = out.row(c, y);
                    for (int x = 0; x < W; ++x) dst[x] = norm(at(x, y)[c]);
                }
            }
        });
        out.invalidate_luma();
    }
    out.set_source_bit_depth(std::min(16, info.bit_depth));
}

}  // namespace detail
}  // namespace stackcore
