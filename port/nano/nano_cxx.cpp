/* nano_cxx.cpp — the C++ runtime the port needs on the nano (no libstdc++/libsupc++):
 * Box2D and the engine are built with -fno-exceptions -fno-rtti -fno-threadsafe-statics. */
#include <stddef.h>
#include <stdlib.h>

void *operator new(size_t n) {
    void *p = malloc(n);
    if (!p) abort();
    return p;
}
void *operator new[](size_t n) { return operator new(n); }
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

}
