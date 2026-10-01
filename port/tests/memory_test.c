/* Bounds, alignment, overlap, and byte preservation for the app's libc wrappers.
 * Also compiled into the ARM replay: no comparison uses wrapped libc routines. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>

void *__wrap_memcpy(void *, const void *, size_t);
void *__wrap_memmove(void *, const void *, size_t);
void *__wrap_memset(void *, int, size_t);

static uint8_t original[83200], actual[83200], expected[83200];
static uint32_t random_state = 0x12345678;
static uint32_t next_random(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}
static void reset(size_t length) {
    for (size_t i = 0; i < length; i++)
        actual[i] = expected[i] = original[i] = (uint8_t)next_random();
}
static void verify(size_t length) {
    for (size_t i = 0; i < length; i++) assert(actual[i] == expected[i]);
}
int memory_test(void) {
    unsigned cases = 0;
    for (size_t n = 0; n <= 257; n++) {
        for (size_t src = 0; src < 32; src++) {
            for (size_t dst = 0; dst < 32; dst++) {
                reset(320);
                for (size_t i = 0; i < n; i++) expected[dst + i] = original[src + i];
                assert(__wrap_memmove(actual + dst, actual + src, n) == actual + dst);
                verify(320); cases++;
            }
            reset(320);
            int value = (int)(n * 31 + src);
            for (size_t i = 0; i < n; i++) expected[src + i] = (uint8_t)value;
            assert(__wrap_memset(actual + src, value, n) == actual + src);
            verify(320); cases++;
        }
        /* Exact allocation ends let ASan detect even a one-byte overread. */
        for (size_t src = 0; src < 4; src++) {
            uint8_t *s = malloc(n + src + 1);
            assert(s);
            for (size_t i = 0; i < n; i++) s[src + 1 + i] = (uint8_t)(i * 13);
            for (size_t dst = 0; dst < 4; dst++) {
                uint8_t *d = malloc(n + dst + 1);
                assert(d);
                for (size_t i = 0; i <= dst; i++) d[i] = 0xa5;
                assert(__wrap_memcpy(d + dst + 1, s + src + 1, n) == d + dst + 1);
                for (size_t i = 0; i < n; i++) assert(d[dst + 1 + i] == (uint8_t)(i * 13));
                for (size_t i = 0; i <= dst; i++) assert(d[i] == 0xa5);
                free(d); cases++;
            }
            free(s);
        }
    }
    for (int trial = 0; trial < 200; trial++) {
        size_t src = next_random() % 512, dst = next_random() % 512;
        size_t n = trial < 4 ? 81920 : next_random() % 81921;
        reset(sizeof actual);
        for (size_t i = 0; i < n; i++) expected[dst + i] = original[src + i];
        assert(__wrap_memmove(actual + dst, actual + src, n) == actual + dst);
        verify(sizeof actual); cases++;
    }
    printf("memory wrappers: %u cases PASS\n", cases);
    return 0;
}
#ifndef AB_MEMORY_NO_MAIN
int main(void) { return memory_test(); }
#endif
