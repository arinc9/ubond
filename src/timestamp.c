#include "timestamp.h"

inline uint64_t
ubond_timestamp64(ev_tstamp now)
{
    uint64_t _now = now * 1000.0;
    return _now;
}

inline uint16_t
ubond_timestamp16(uint64_t now)
{
    uint16_t ts = now % 65536;
    if (ts == (uint16_t)-1) {
        ts++;
    }
    return ts;
}

inline uint16_t
ubond_timestamp16_diff(uint16_t tsnew, uint16_t tsold)
{
    int32_t diff = tsnew - tsold;
    if (diff < 0) {
        diff += 65536;
    }
    return diff;
}

inline uint64_t
ubond_timestamp64_diff(uint64_t tsnew, uint64_t tsold)
{
    return (int64_t)(tsnew - tsold);
}
