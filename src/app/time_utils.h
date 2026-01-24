#pragma once

#include <time.h>

namespace time_utils {

static constexpr time_t kValidTimeThreshold = 1609459200; // 2021-01-01T00:00:00Z

// Returns true when the system time has been synchronized.
bool is_time_valid();

// Portable UTC mktime (temporarily sets TZ=UTC0).
time_t timegm_portable(struct tm *tm);

// Parse UTC timestamps in the filename format YYYYMMDDTHHMMSSZ.
// Returns false for malformed values so we won't delete valid images by mistake.
bool parse_utc_timestamp(const char *ts, time_t *out_epoch);

} // namespace time_utils