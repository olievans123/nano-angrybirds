/* Exercise the actual nano allocator under ASan/UBSan with a 2 MiB arena.
 * Direct inclusion also allows checking every boundary tag and free-list link. */
#include <assert.h>
#define AB_ALLOC_TEST 1
#include "../nano/nano_alloc.c"

void *port_test_aux_block;
static uint8_t backing[PORT_ARENA_MIN] __attribute__((aligned(64)));
static int backing_used;
uint32_t hb_os_heap_free(void) { return (8u << 20) - (backing_used ? sizeof(backing) : 0); }
void *hb_os_alloc(uint32_t n) {
    if (backing_used || n > sizeof(backing)) return NULL;
    backing_used = 1;
    return backing;
}
void hb_os_free(void *p) { assert(p == backing); backing_used = 0; }

static void validate(void)
{
    static uint8_t seen[PORT_ARENA_MIN / 16];
    memset(seen, 0, sizeof(seen));
    uint32_t prev = 0, off = 0, free_count = 0, listed = 0;
    int prev_free = 0;
    while (off < sArenaSize) {
        ArenaBlk *b = block_at(off);
        uint32_t n = block_size(b);
        assert(n >= sizeof(ArenaBlk) && n <= sArenaSize - off);
        assert(b->prev_size == prev);
        int is_free = !(b->size_flags & USED);
        assert(!(prev_free && is_free));
        if (is_free) { seen[off / 16] = 1; free_count++; }
        prev_free = is_free;
        prev = n; off += n;
    }
    assert(off == sArenaSize);
    for (unsigned bin = 0; bin < BIN_COUNT; bin++) {
        uint32_t last = NONE;
        for (off = sBins[bin]; off != NONE; off = block_at(off)->next_free) {
            assert(off < sArenaSize && !(off & 15u));
            ArenaBlk *b = block_at(off);
            assert(seen[off / 16] == 1 && b->prev_free == last);
            assert(bin_for(block_size(b)) == bin);
            seen[off / 16] = 2; listed++; last = off;
            assert(listed <= free_count); /* cycles/duplicates */
        }
    }
    assert(listed == free_count);
}

static uint32_t rng = 17483;
static uint32_t random_u32(void) { rng = rng * 1664525u + 1013904223u; return rng; }
typedef struct { uint8_t *p; uint32_t size; uint8_t value; } Slot;
static Slot slots[512];
static void check(const Slot *s) {
    for (uint32_t i = 0; i < s->size; i++) assert(s->p[i] == s->value);
}

int main(void)
{
    uint8_t *a = malloc(128), *b = malloc(4096), *guard = malloc(128);
    assert(a && b && guard && sArenaSize == PORT_ARENA_MIN);
    memset(a, 0x57, 128);
    free(b);
    assert(realloc(a, 2048) == a); /* adjacent free block can be used in place */
    for (int i = 0; i < 128; i++) assert(a[i] == 0x57);
    assert(realloc(a, 32) == a); /* shrinking never needs an allocation */
    validate(); free(guard); free(a); validate();
    assert(port_arena_largest_free() == PORT_ARENA_MIN);
    assert(!malloc(SIZE_MAX) && !calloc(SIZE_MAX, 2) && !memalign(24, 10));
    a = malloc(128); memset(a, 0x24, 128);
    assert(!realloc(a, SIZE_MAX));
    for (int i = 0; i < 128; i++) assert(a[i] == 0x24);
    assert(realloc(a, 0) == NULL);
    /* Lua requires shrinking to succeed even when there is no free memory. */
    a = malloc(1024); memset(a, 0x19, 1024);
    unsigned filled = 0;
    while (filled < 512 && (slots[filled].p = malloc(8192))) filled++;
    assert(!malloc(8192));
    assert(realloc(a, 256) == a);
    for (int i = 0; i < 256; i++) assert(a[i] == 0x19);
    free(a);
    for (unsigned i = 0; i < filled; i++) { free(slots[i].p); slots[i].p = NULL; }
    validate();
    assert(port_arena_largest_free() == PORT_ARENA_MIN);
    port_alloc_stats_reset();
    for (int step = 0; step < 100000; step++) {
        Slot *s = &slots[(random_u32() >> 12) % 512];
        if (s->p) check(s);
        if (!s->p) {
            uint32_t n = (random_u32() >> 8) % 8192 + 1;
            uint32_t align = 16u << ((random_u32() >> 15) % 7);
            s->p = memalign(align, n);
            if (s->p) {
                assert(!((uintptr_t)s->p & (align - 1)));
                s->size = n; s->value = (uint8_t)(random_u32() >> 8);
                memset(s->p, s->value, n);
            }
        } else if (random_u32() & 1) {
            uint32_t n = (random_u32() >> 8) % 12000 + 1;
            uint8_t *p = realloc(s->p, n);
            if (p) {
                for (uint32_t i = 0; i < (n < s->size ? n : s->size); i++) assert(p[i] == s->value);
                s->p = p; s->size = n; memset(p, s->value, n);
            } else check(s); /* failed realloc preserves the original */
        } else { free(s->p); s->p = NULL; }
        if (!(step % 100)) validate();
    }
    for (unsigned i = 0; i < 512; i++) if (slots[i].p) { check(&slots[i]); free(slots[i].p); }
    validate();
    assert(port_arena_largest_free() == PORT_ARENA_MIN);
    PortAllocStats stats = port_alloc_stats();
    printf("allocator: 100000 randomized operations PASS; %u allocations, %u probes\n",
           stats.allocations, stats.probes);
}
