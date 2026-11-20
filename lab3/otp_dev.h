#ifndef OTP_DEV_H
#define OTP_DEV_H

#include <stdint.h>
#include <devctl.h>

namespace otp {

#define OTP_DEVICE_PATH "/dev/otp"

// Максимальный размер обрабатываемых данных
const uint32_t kMaxTextSize = 1024;

// x(n+1) = (a * x(n) + c) mod m, x(0) = seed
struct LcgParams
{
    uint32_t seed;
    uint32_t a;
    uint32_t c;
    uint32_t m;
};

struct OtpContext
{
    LcgParams lcg;
    uint32_t  size;
    uint8_t   text[kMaxTextSize];
};

}

#define CYPHER_TEXT  __DIOTF(_DCMD_MISC, 1, otp::OtpContext)

#endif // OTP_DEV_H
