/* numfmt.c — number <-> text for Lua, the same on every platform (single precision; the
 * nano build has no double-precision printf/strtod). */
#include <stdint.h>
#include <string.h>
#include "numfmt.h"

static inline uint32_t u_from_f(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* Decimal string -> float (the Lua lexer and tonumber). */
float ab_strtof(const char *s, char **end) {
    const char *p = s;
    float v = 0.0f;
    int neg = 0, digits = 0, exp10 = 0;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') p++;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        const char *q = p + 2;
        uint32_t h = 0;
        int n = 0;
        for (;; q++, n++) {
            int d = *q >= '0' && *q <= '9' ? *q - '0' : *q >= 'a' && *q <= 'f' ? *q - 'a' + 10 :
                    *q >= 'A' && *q <= 'F' ? *q - 'A' + 10 : -1;
            if (d < 0) break;
            h = h * 16u + (uint32_t)d;
        }
        if (n) { if (end) *end = (char *)q; return neg ? -(float)h : (float)h; }
    }
    /* accumulate up to 9 significant digits in an integer, the rest only shift the exponent */
    {
        uint32_t mant = 0;
        int sig = 0;
        for (; *p >= '0' && *p <= '9'; p++, digits++) {
            if (sig < 9) { mant = mant * 10u + (uint32_t)(*p - '0'); if (mant) sig++; }
            else exp10++;
        }
        if (*p == '.') {
            p++;
            for (; *p >= '0' && *p <= '9'; p++, digits++) {
                if (sig < 9) { mant = mant * 10u + (uint32_t)(*p - '0'); if (mant) sig++; exp10--; }
            }
        }
        if (!digits) { if (end) *end = (char *)s; return 0.0f; }
        v = (float)mant;
    }
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        int eneg = 0, e = 0, n = 0;
        if (*q == '-' || *q == '+') eneg = *q++ == '-';
        for (; *q >= '0' && *q <= '9'; q++, n++) if (e < 1000) e = e * 10 + (*q - '0');
        if (n) { exp10 += eneg ? -e : e; p = q; }
    }
    {
        /* scale in double steps to keep rounding error below half an ulp of a float */
        double d = (double)v, t = 10.0;
        int e = exp10 < 0 ? -exp10 : exp10;
        double scale = 1.0;
        while (e) { if (e & 1) scale *= t; t *= t; e >>= 1; if (scale > 1e60) break; }
        d = exp10 < 0 ? d / scale : d * scale;
        v = (float)d;
    }
    if (end) *end = (char *)p;
    return neg ? -v : v;
}

/* float -> shortest "%.7g"-style text (what Lua's tostring prints). Returns the length. */
int ab_fmt_float(char *out, float x) {
    char *o = out;
    uint32_t u = u_from_f(x);
    if (x != x) { memcpy(out, "nan", 4); return 3; }
    if (u >> 31) { *o++ = '-'; x = -x; }
    if (x > 3.0e38f) { memcpy(o, "inf", 4); return (int)(o - out) + 3; }
    if (x == 0.0f) { *o++ = '0'; *o = 0; return (int)(o - out); }
    {
        /* 7 significant digits: d.dddddd x 10^e */
        int e = 0, i, n;
        double d = (double)x;
        char dig[8];
        uint32_t m;
        while (d >= 10.0) { d /= 10.0; e++; }
        while (d < 1.0) { d *= 10.0; e--; }
        m = (uint32_t)(d * 1000000.0 + 0.5);
        if (m >= 10000000u) { m /= 10u; e++; }
        for (i = 6; i >= 0; i--) { dig[i] = (char)('0' + m % 10u); m /= 10u; }
        n = 7;
        while (n > 1 && dig[n - 1] == '0') n--;         /* %g drops trailing zeros */
        if (e < -4 || e >= 7) {
            *o++ = dig[0];
            if (n > 1) { *o++ = '.'; for (i = 1; i < n; i++) *o++ = dig[i]; }
            *o++ = 'e';
            *o++ = e < 0 ? '-' : '+';
            if (e < 0) e = -e;
            *o++ = (char)('0' + e / 10);
            *o++ = (char)('0' + e % 10);
        } else if (e >= 0) {
            for (i = 0; i <= e; i++) *o++ = i < n ? dig[i] : '0';
            if (n > e + 1) { *o++ = '.'; for (i = e + 1; i < n; i++) *o++ = dig[i]; }
        } else {
            *o++ = '0'; *o++ = '.';
            for (i = -1; i > e; i--) *o++ = '0';
            for (i = 0; i < n; i++) *o++ = dig[i];
        }
    }
    *o = 0;
    return (int)(o - out);
}
