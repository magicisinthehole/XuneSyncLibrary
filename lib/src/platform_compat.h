#pragma once

#ifdef _WIN32

#include <ctime>

inline struct tm* gmtime_r(const time_t* timer, struct tm* buf) {
    return gmtime_s(buf, timer) == 0 ? buf : nullptr;
}

#endif
