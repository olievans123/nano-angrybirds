/* nano_cxx.cpp — the C++ runtime the port needs on the nano (no libstdc++/libsupc++):
 * Box2D and the engine are built with -fno-exceptions -fno-rtti -fno-threadsafe-statics. */
#include <stddef.h>
#include <stdlib.h>

void *operator new(size_t n) { return malloc(n); }
void *operator new[](size_t n) { return malloc(n); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

extern "C" {
void __cxa_pure_virtual(void) { abort(); }
/* Static objects are never destroyed: the app's memory is dropped as a whole. */
int __aeabi_atexit(void *obj, void (*fn)(void *), void *dso) { (void)obj; (void)fn; (void)dso; return 0; }
int __cxa_atexit(void (*fn)(void *), void *obj, void *dso) { (void)obj; (void)fn; (void)dso; return 0; }
void *__dso_handle;

/* Constructors of static objects (the default linker script collects them in
 * .init_array); called once per launch after .bss/.data are reset. */
typedef void (*ctor_fn)(void);
extern ctor_fn __init_array_start[], __init_array_end[];
void ab_run_static_ctors(void)
{
    for (ctor_fn *f = __init_array_start; f < __init_array_end; f++)
        (*f)();
}
}
