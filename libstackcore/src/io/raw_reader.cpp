#include "stackcore/raw_reader.hpp"

#include <stdexcept>

#include "libraw_reader.hpp"

namespace stackcore {
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

// 暦日 → 1970-01-01 からの日数（Howard Hinnant の days_from_civil）。
std::int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
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
    return detail::libraw_extension(lower_extension(path));
}

ImageFileInfo probe_raw_image(const std::string& path) {
    if (!is_raw_image_path(path)) throw std::runtime_error("RAWではない拡張子です: " + path);
    return detail::libraw_probe(path);
}

void read_raw_image(const std::string& path, FrameBuffer& out, ImageFileInfo& info) {
    if (!is_raw_image_path(path)) throw std::runtime_error("RAWではない拡張子です: " + path);
    detail::libraw_read(path, out, info);
}

}  // namespace stackcore
