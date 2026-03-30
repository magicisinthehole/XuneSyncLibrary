#pragma once

#ifdef _WIN32

#include <ctime>

// gmtime_s has reversed parameter order vs POSIX gmtime_r
inline struct tm* gmtime_r(const time_t* timer, struct tm* buf) {
    return gmtime_s(buf, timer) == 0 ? buf : nullptr;
}

#endif
