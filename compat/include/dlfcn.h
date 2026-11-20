#pragma once
// Horizon has no dynamic loader; everything is linked statically.
#ifdef __cplusplus
extern "C" {
#endif
#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_GLOBAL 4
#define RTLD_LOCAL 0
static inline void* dlopen(const char* f, int m) { (void)f; (void)m; return 0; }
static inline void* dlsym(void* h, const char* s) { (void)h; (void)s; return 0; }
static inline int dlclose(void* h) { (void)h; return 0; }
static inline const char* dlerror(void) { return "dl unsupported"; }
#ifdef __cplusplus
}
#endif
