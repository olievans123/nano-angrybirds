#ifndef AB_NUMFMT_H
#define AB_NUMFMT_H
#ifdef __cplusplus
extern "C" {
#endif
/* Decimal (or 0x hex) text -> float; *end is set past the number (or to s if none). */
float ab_strtof(const char *s, char **end);
/* float -> text as printf("%.7g") would print it. Returns the length. `out` needs 24 bytes. */
int ab_fmt_float(char *out, float x);
#ifdef __cplusplus
}
#endif
#endif
