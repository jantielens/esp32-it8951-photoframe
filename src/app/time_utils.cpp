#include "time_utils.h"

#include <cstdlib>
#include <cstring>

namespace time_utils {

bool is_time_valid() {
    time_t now = time(nullptr);
    return now >= kValidTimeThreshold;
}

time_t timegm_portable(struct tm *tm) {
    const char *tz = getenv("TZ");
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t t = mktime(tm);
    if (tz) {
        setenv("TZ", tz, 1);
    } else {
        unsetenv("TZ");
    }
    tzset();
    return t;
}

bool parse_utc_timestamp(const char *ts, time_t *out_epoch) {
    if (!ts || !out_epoch) return false;
    if (strlen(ts) != 16) return false;
    for (int i = 0; i < 16; i++) {
        const char c = ts[i];
        if (i == 8) {
            if (c != 'T') return false;
        } else if (i == 15) {
            if (c != 'Z') return false;
        } else if (c < '0' || c > '9') {
            return false;
        }
    }

    const int year = (ts[0] - '0') * 1000 + (ts[1] - '0') * 100 + (ts[2] - '0') * 10 + (ts[3] - '0');
    const int mon = (ts[4] - '0') * 10 + (ts[5] - '0');
    const int day = (ts[6] - '0') * 10 + (ts[7] - '0');
    const int hour = (ts[9] - '0') * 10 + (ts[10] - '0');
    const int min = (ts[11] - '0') * 10 + (ts[12] - '0');
    const int sec = (ts[13] - '0') * 10 + (ts[14] - '0');

    struct tm tm = {};
    tm.tm_year = year - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
    tm.tm_isdst = 0;

    const time_t epoch = timegm_portable(&tm);
    if (epoch <= 0) return false;
    *out_epoch = epoch;
    return true;
}

} // namespace time_utils