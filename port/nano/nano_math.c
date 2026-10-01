/* nano_math.c — the libm/libc number functions the port needs on the nano.
 *
 * newlib's prebuilt copies of several of these address their constants with movw/movt
 * pairs, which the NanoApps loader doesn't rebase (tools/mkrelocapp.py rejects them).
 * These are plain C built with -mword-relocations, single precision throughout (the
 * game's Lua numbers are floats), so no newlib math member is linked at all. */
#include <stdint.h>
#include <string.h>

static inline float f_from_u(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t u_from_f(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

#define F_NAN f_from_u(0x7fc00000u)
#define F_INF f_from_u(0x7f800000u)

float fabsf(float x) { return f_from_u(u_from_f(x) & 0x7fffffffu); }

float sqrtf(float x) {
#if defined(__arm__) && defined(__ARM_FP) && !defined(__thumb__)
    float r;
    __asm__("vsqrt.f32 %0, %1" : "=t"(r) : "t"(x));
    return r;
#else
    if (!(x > 0.0f)) return x == 0.0f ? x : F_NAN;
    if (x > 3.0e38f) return x;
    float y = f_from_u(0x1fbd1df5u + (u_from_f(x) >> 1));
    y = 0.5f * (y + x / y);
    y = 0.5f * (y + x / y);
    y = 0.5f * (y + x / y);
    return y;
#endif
}

float floorf(float x) {
    if (!(fabsf(x) < 8388608.0f)) return x;          /* already integral, inf or NaN */
    float t = (float)(int32_t)x;
    return t > x ? t - 1.0f : t;
}

float ceilf(float x) {
    if (!(fabsf(x) < 8388608.0f)) return x;
    float t = (float)(int32_t)x;
    return t < x ? t + 1.0f : t;
}

float fmodf(float x, float y) {
    if (y == 0.0f || x != x || y != y || fabsf(x) > 3.0e38f) return F_NAN;
    float ax = fabsf(x), ay = fabsf(y);
    if (ax < ay) return x;
    /* exact for the ranges the game uses (quotients far below 2^24) */
    if (ax / ay < 8388608.0f) {
        float q = (float)(int32_t)(ax / ay);
        float r = ax - q * ay;
        if (r < 0.0f) r += ay;
        if (r >= ay) r -= ay;
        return x < 0.0f ? -r : r;
    }
    while (ax >= ay) {
        float t = ay;
        while (t * 2.0f <= ax) t *= 2.0f;
        ax -= t;
    }
    return x < 0.0f ? -ax : ax;
}

/* sin/cos on [-pi/4, pi/4] (minimax, ~1e-7 absolute). */
static float k_sin(float x) {
    float z = x * x;
    return x + x * z * (-1.6666654611e-1f + z * (8.3321608736e-3f + z * -1.9515295891e-4f));
}
static float k_cos(float x) {
    float z = x * x;
    return 1.0f + z * (-0.5f + z * (4.1666645683e-2f + z * (-1.3887316255e-3f + z * 2.4433157240e-5f)));
}

/* Reduce by multiples of pi/2 (Cody-Waite, three-part pi/2): fine for |x| < ~1e5. */
static int reduce(float x, float *r) {
    const float inv_pio2 = 6.3661974669e-01f;
    const float pio2_1 = 1.5707855225e+00f, pio2_2 = 1.0804334124e-05f, pio2_3 = 6.0770999344e-11f;
    float k = x * inv_pio2;
    int n = (int)(k >= 0.0f ? k + 0.5f : k - 0.5f);
    float fn = (float)n;
    *r = ((x - fn * pio2_1) - fn * pio2_2) - fn * pio2_3;
    return n;
}

float sinf(float x) {
    float r;
    switch (reduce(x, &r) & 3) {
        case 0: return k_sin(r);
        case 1: return k_cos(r);
        case 2: return -k_sin(r);
        default: return -k_cos(r);
    }
}

float cosf(float x) {
    float r;
    switch (reduce(x, &r) & 3) {
        case 0: return k_cos(r);
        case 1: return -k_sin(r);
        case 2: return -k_cos(r);
        default: return k_sin(r);
    }
}

float tanf(float x) {
    float c = cosf(x);
    return c == 0.0f ? F_INF : sinf(x) / c;
}

/* atan (Cephes atanf: reduce to |x| <= tan(pi/8), then an odd polynomial; ~1e-7). */
float atanf(float x) {
    const float pio2 = 1.5707963268f, pio4 = 0.7853981634f;
    float ax = fabsf(x), y, z;
    if (ax != ax) return x;
    if (ax > 2.414213562f) { y = pio2; ax = -(1.0f / ax); }
    else if (ax > 0.4142135624f) { y = pio4; ax = (ax - 1.0f) / (ax + 1.0f); }
    else y = 0.0f;
    z = ax * ax;
    y += (((8.05374449538e-2f * z - 1.38776856032e-1f) * z + 1.99777106478e-1f) * z - 3.33329491539e-1f) * z * ax + ax;
    return x < 0.0f ? -y : y;
}

float atan2f(float y, float x) {
    const float pi = 3.1415926536f, pio2 = 1.5707963268f;
    if (x != x || y != y) return F_NAN;
    if (x == 0.0f) {
        if (y == 0.0f) return (u_from_f(x) >> 31) ? (u_from_f(y) >> 31 ? -pi : pi) : y;
        return y > 0.0f ? pio2 : -pio2;
    }
    float a = atanf(y / x);
    if (x > 0.0f) return a;
    return y >= 0.0f && !(u_from_f(y) >> 31) ? a + pi : a - pi;
}

static float asin_small(float x) {
    float z = x * x;
    float p = z * (1.6666586697e-01f + z * (-4.2743422091e-02f + z * -8.6563630030e-03f));
    float q = 1.0f + z * -7.0662963390e-01f;
    return x + x * (p / q);
}

float acosf(float x) {
    const float pi = 3.1415926536f, pio2 = 1.5707963268f;
    float ax = fabsf(x);
    if (ax > 1.0f) return F_NAN;
    if (ax <= 0.5f) return pio2 - asin_small(x);
    float s = sqrtf((1.0f - ax) * 0.5f);
    float a = 2.0f * asin_small(s);
    return x > 0.0f ? a : pi - a;
}

float asinf(float x) { return 1.5707963268f - acosf(x); }

/* 2^k scaling */
float ldexpf(float x, int e) {
    while (e > 60) { x *= 1.152921504606846976e18f; e -= 60; }
    while (e < -60) { x *= 8.673617379884035e-19f; e += 60; }
    return x * f_from_u((uint32_t)(e + 127) << 23);
}

float frexpf(float x, int *e) {
    uint32_t u = u_from_f(x);
    int ex = (int)((u >> 23) & 0xff);
    *e = 0;
    if (ex == 0xff || x == 0.0f) return x;
    if (ex == 0) {                         /* subnormal */
        float m = frexpf(x * 16777216.0f, e);
        *e -= 24;
        return m;
    }
    *e = ex - 126;
    return f_from_u((u & 0x807fffffu) | (126u << 23));
}

float logf(float x) {
    const float ln2 = 0.6931471806f;
    int e;
    if (x != x || x < 0.0f) return F_NAN;
    if (x == 0.0f) return -F_INF;
    if (x > 3.0e38f) return x;
    float m = frexpf(x, &e);               /* m in [0.5, 1) */
    if (m < 0.70710678f) { m *= 2.0f; e--; }
    float s = (m - 1.0f) / (m + 1.0f), z = s * s;
    /* ln(m) = 2 atanh(s) */
    float r = 2.0f * s * (1.0f + z * (0.33333334f + z * (0.2f + z * (0.14285715f + z * 0.11111111f))));
    return r + (float)e * ln2;
}

float expf(float x) {
    const float ln2 = 0.6931471806f, inv_ln2 = 1.4426950409f;
    if (x != x) return x;
    if (x > 88.7f) return F_INF;
    if (x < -103.0f) return 0.0f;
    float k = floorf(x * inv_ln2 + 0.5f);
    float r = x - k * ln2;                 /* |r| <= ln2/2 */
    float p = 1.0f + r * (1.0f + r * (0.5f + r * (0.16666667f + r * (0.041666668f + r * (0.0083333338f + r * 0.0013888889f)))));
    return ldexpf(p, (int)k);
}

float log10f(float x) { return logf(x) * 0.4342944819f; }

float powf(float x, float y) {
    if (y == 0.0f) return 1.0f;
    if (x != x || y != y) return F_NAN;
    /* integer exponents (the common case in game scripts): exact repeated squaring */
    if (y == floorf(y) && fabsf(y) < 1024.0f) {
        int n = (int)fabsf(y);
        float r = 1.0f, b = x;
        while (n) { if (n & 1) r *= b; b *= b; n >>= 1; }
        return y < 0.0f ? 1.0f / r : r;
    }
    if (x < 0.0f) return F_NAN;
    if (x == 0.0f) return y > 0.0f ? 0.0f : F_INF;
    return expf(y * logf(x));
}

float sinhf(float x) { float e = expf(x); return 0.5f * (e - 1.0f / e); }
float coshf(float x) { float e = expf(x); return 0.5f * (e + 1.0f / e); }
float tanhf(float x) { float e = expf(2.0f * x); return (e - 1.0f) / (e + 1.0f); }

float modff(float x, float *ip) {
    float t = x < 0.0f ? ceilf(x) : floorf(x);
    *ip = t;
    return x - t;
}
