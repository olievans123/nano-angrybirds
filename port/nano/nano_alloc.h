#ifndef AB_NANO_ALLOC_H
#define AB_NANO_ALLOC_H
#include <stdint.h>
typedef struct {
    uint32_t allocations, probes, copied, failures;
} PortAllocStats;
void port_alloc_stats_reset(void);
PortAllocStats port_alloc_stats(void);
uint32_t port_arena_free_bytes(void);   /* bytes on the free lists right now */
#endif
