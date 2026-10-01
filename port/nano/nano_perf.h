#ifndef AB_NANO_PERF_H
#define AB_NANO_PERF_H
#include <stdint.h>
void port_perf_level_loaded(void);
void port_perf_flush(void); /* before starting a frame's GL work */
void port_perf_draw(float width, float height);
void port_perf_frame(uint32_t interval, uint32_t cpu, uint32_t game,
                     uint32_t submit, uint32_t physics, uint32_t io, uint32_t upload,
                     uint32_t gap);
#endif
