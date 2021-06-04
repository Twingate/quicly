#pragma once

#if defined(_WIN32)
    #include <Winsock2.h>
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/udp.h>
    #include <netdb.h>
    #include <sys/time.h>
    #include <pthread.h>
#endif

// WIN32 redundant?
#if defined(_WINDOWS) || defined(WIN32)
# define __thread __declspec(thread)

struct iovec {
    void *iov_base;
    size_t iov_len;
};
#endif

#ifdef __GNUC__
#define clz(x) __builtin_clz(x)
#define clzll(x) __builtin_clzll(x)
#define popcountl(x) __builtin_popcountl(x)
#else
inline int clz(uint32_t x) {
    unsigned long r = 0;
    _BitScanReverse(&r, x);
    FMT_ASSERT(x != 0, "");
    // Static analysis complains about using uninitialized data
    // "r", but the only way that can happen is if "x" is 0,
    // which the callers guarantee to not happen.
    //FMT_SUPPRESS_MSC_WARNING(6102)
    return 31 ^ (int)(r);
}

inline int clzll(uint64_t x) {
    unsigned long r = 0;
#  ifdef _WIN64
    _BitScanReverse64(&r, x);
#  else
    // Scan the high 32 bits.
    if (_BitScanReverse(&r, (uint32_t)(x >> 32))) return 63 ^ (r + 32);
    // Scan the low 32 bits.
    _BitScanReverse(&r, (uint32_t)(x));
#  endif
    FMT_ASSERT(x != 0, "");
    //FMT_SUPPRESS_MSC_WARNING(6102)  // Suppress a bogus static analysis warning.
    return 63 ^ (int)(r);
}

static inline int popcountl(uint64_t x)
{
    // See https://en.wikipedia.org/wiki/Hamming_weight.
    x = x - ((x >> 1) & 0x55555555);
    x = (x & 0x33333333) + ((x >> 2) & 0x33333333);
    x = (x + (x >> 4)) & 0x0F0F0F0F;
    return (x * 0x01010101) >> 24;
}
#endif
