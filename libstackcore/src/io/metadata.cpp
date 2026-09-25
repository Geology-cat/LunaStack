#include "stackcore/metadata.hpp"

#include <algorithm>
#include <cstdio>

#include "stackcore/video_source.hpp"

namespace stackcore {
namespace {

constexpr std::int64_t kTicksPerSecond = 10000000;
constexpr std::int64_t kTicksPerDay = kTicksPerSecond * 86400;

struct Civil {
    int year, month, day, hour, minute, second, millisecond;
    std::int64_t sub_ticks;  // 秒未満の100ns単位
};

// 0001-01-01 からの日数を暦日に直す（Howard Hinnant の civil_from_days）。
Civil civil_from_ticks(std::int64_t ticks) {
    if (ticks < 0) ticks = 0;
    const std::int64_t days_since_0001 = ticks / kTicksPerDay;
    std::int64_t rem = ticks % kTicksPerDay;
    // 0001-01-01 は 1970-01-01 から 719162 日前。
    const std::int64_t z0 = days_since_0001 - 719162;
    const std::int64_t z = z0 + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = yoe + era * 400;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;
    Civil c;
    c.year = static_cast<int>(y + (m <= 2 ? 1 : 0));
    c.month = static_cast<int>(m);
    c.day = static_cast<int>(d);
    c.hour = static_cast<int>(rem / (kTicksPerSecond * 3600));
    rem %= kTicksPerSecond * 3600;
    c.minute = static_cast<int>(rem / (kTicksPerSecond * 60));
    rem %= kTicksPerSecond * 60;
    c.second = static_cast<int>(rem / kTicksPerSecond);
    c.sub_ticks = rem % kTicksPerSecond;
    c.millisecond = static_cast<int>(c.sub_ticks / 10000);
    return c;
}

}  // namespace

std::string ticks_to_iso8601(std::int64_t ticks) {
    const Civil c = civil_from_ticks(ticks);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03d", c.year, c.month, c.day,
                  c.hour, c.minute, c.second, c.millisecond);
    return buf;
}

std::string ticks_to_tiff_datetime(std::int64_t ticks) {
    const Civil c = civil_from_ticks(ticks);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%04d:%02d:%02d %02d:%02d:%02d", c.year, c.month, c.day,
                  c.hour, c.minute, c.second);
    return buf;
}

std::string ticks_to_winjupos(std::int64_t ticks) {
    // 分の10分の1（6秒単位）へ四捨五入してから書式化する。
    const std::int64_t unit = kTicksPerSecond * 6;
    const std::int64_t rounded = (ticks + unit / 2) / unit * unit;
    const Civil c = civil_from_ticks(rounded);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d-%02d%02d_%d", c.year, c.month, c.day, c.hour,
                  c.minute, c.second / 6);
    return buf;
}

bool mid_timestamp(const VideoSource& source, const std::vector<int>& indices,
                   std::int64_t& ticks) {
    if (!source.has_timestamps() || indices.empty()) return false;
    std::int64_t lo = 0, hi = 0;
    bool any = false;
    for (int index : indices) {
        if (index < 0 || index >= source.frame_count()) continue;
        const std::int64_t t = source.timestamp_ticks(index);
        if (t <= 0) continue;
        if (!any) {
            lo = hi = t;
            any = true;
        } else {
            lo = std::min(lo, t);
            hi = std::max(hi, t);
        }
    }
    if (!any) return false;
    ticks = lo + (hi - lo) / 2;
    return true;
}

}  // namespace stackcore
