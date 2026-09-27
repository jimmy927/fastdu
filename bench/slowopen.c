/*
 * macOS, benchmarks only: loaded into fastdu with DYLD_INSERT_LIBRARIES, it
 * times every open and openat and, at exit, prints on standard error how many
 * took longer than 4 s, how long they took together, and the errno of each —
 * the App Data stalls, which end in EINTR after 5 s. fastdu itself is not
 * changed.
 *
 *   cc -dynamiclib -o slowopen.dylib bench/slowopen.c
 *   DYLD_INSERT_LIBRARIES=./slowopen.dylib ./fastdu -d 0 ~
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static atomic_int slow;
static atomic_llong slowNanos;
static atomic_int slowEintr;

static long long now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static void count(long long started, int result) {
    long long took = now() - started;
    if (took < 4000000000LL) return;
    atomic_fetch_add(&slow, 1);
    atomic_fetch_add(&slowNanos, took);
    if (result < 0 && errno == EINTR) atomic_fetch_add(&slowEintr, 1);
}

static int timedOpen(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    long long started = now();
    int result = open(path, flags, mode);
    count(started, result);
    return result;
}

static int timedOpenat(int fd, const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    long long started = now();
    int result = openat(fd, path, flags, mode);
    count(started, result);
    return result;
}

__attribute__((used, section("__DATA,__interpose"))) static struct {
    const void *replacement, *original;
} interposed[] = {
    {(const void *)timedOpen, (const void *)open},
    {(const void *)timedOpenat, (const void *)openat},
};

__attribute__((destructor)) static void summary(void) {
    fprintf(stderr, "slow opens: %d, %.1f s together, %d ended in EINTR\n", atomic_load(&slow),
            atomic_load(&slowNanos) / 1e9, atomic_load(&slowEintr));
}
