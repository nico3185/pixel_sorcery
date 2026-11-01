/* ps_portable.h — MSVC equivalents for the GCC/Clang extensions used elsewhere in
 * engine/src (weak symbols, __attribute__((constructor))). Not needed under GCC/Clang. */
#ifndef PS_PORTABLE_H
#define PS_PORTABLE_H

#if defined(_MSC_VER)

/* Runs f() before main(), like __attribute__((constructor)). MSVC has no such attribute;
 * this places a pointer to f in the CRT's user-initializer section (.CRT$XCU), which the
 * CRT startup code calls through before main(). /include forces the linker to keep it. */
#ifdef _WIN64
#define PS_SYM_PREFIX ""
#else
#define PS_SYM_PREFIX "_"
#endif
#pragma section(".CRT$XCU", read)
#define PS_INITIALIZER(f) \
    static void f(void); \
    __declspec(allocate(".CRT$XCU")) void (*f##_ps_ctor)(void) = f; \
    __pragma(comment(linker, "/include:" PS_SYM_PREFIX #f "_ps_ctor")) \
    static void f(void)

/* Emulates "strong symbol `name` overrides weak default" (how __attribute__((weak)) is used
 * elsewhere in engine/src): declares `name` as the real symbol other code calls/references,
 * and falls back to `default_name` at link time only if nothing else defines `name`. */
#define PS_ALTNAME(name, default_name) \
    __pragma(comment(linker, "/alternatename:" PS_SYM_PREFIX #name "=" PS_SYM_PREFIX #default_name))

#else /* GCC/Clang */

#define PS_INITIALIZER(f) \
    static void f(void) __attribute__((constructor)); \
    static void f(void)

#endif

/* Windows (MinGW and MSVC): mkdir takes no mode; exec's argument vector is const */
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define ps_mkdir(path, mode) _mkdir(path)
#define ps_execv(path, argv) _execv(path, (const char *const *)(argv))
#else
#include <sys/stat.h>
#include <unistd.h>
#define ps_mkdir(path, mode) mkdir(path, mode)
#define ps_execv(path, argv) execv(path, argv)
#endif

#endif /* PS_PORTABLE_H */
