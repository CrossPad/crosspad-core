#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace crosspad {

/// Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's days_from_civil).
constexpr int64_t daysFromCivil(int y, int m, int d) {
    y -= m <= 2 ? 1 : 0;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<int64_t>(era) * 146097 + doe - 719468;
}

/// "Fri, 02 Oct 2026 18:00:00 GMT" (RFC 9110 IMF-fixdate, the only form a
/// server still sends) to unix seconds. False for anything else.
inline bool parseHttpDate(const char* text, int64_t& unixSeconds) {
    static constexpr char kMonths[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    constexpr int kSecondsPerDay = 86400;
    if (!text) return false;
    char weekday[4] = {}, month[4] = {};
    int day = 0, year = 0, hour = 0, minute = 0, second = 0;
    // "%3s," reads "Fri" and leaves the comma for the literal that follows it.
    if (std::sscanf(text, "%3s, %d %3s %d %d:%d:%d GMT", weekday, &day, month, &year, &hour,
                    &minute, &second) != 7)
        return false;
    const char* m = std::strlen(month) == 3 ? std::strstr(kMonths, month) : nullptr;
    if (!m || (m - kMonths) % 3 != 0) return false;
    if (day < 1 || day > 31 || year < 1970 || year > 9999 || hour > 23 || minute > 59 || second > 60 ||
        hour < 0 || minute < 0 || second < 0)
        return false;
    const int mon = static_cast<int>(m - kMonths) / 3 + 1;
    unixSeconds = daysFromCivil(year, mon, day) * kSecondsPerDay + hour * 3600 + minute * 60 + second;
    return true;
}

} // namespace crosspad
