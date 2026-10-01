#ifndef AB_NANO_PERF_H
#define AB_NANO_PERF_H
#include <stdint.h>
extern uint32_t port_perf_entry_us; /* integer-only entry setup, included in the measured gap */
void port_perf_level_loaded(void);
void port_perf_flush(void); /* before starting a frame's GL work */
void port_perf_frame(uint32_t interval, uint32_t cpu, uint32_t game,
                     uint32_t submit, uint32_t physics, uint32_t io, uint32_t upload,
                     uint32_t gap);
/* Frame pacing: the spacing to hold frames to (0 = none) and the platform's usual time
 * between the end of one frame's work and the next callback. */
uint32_t port_perf_pace_us(void);
uint32_t port_perf_gap_estimate(void);
#endif
