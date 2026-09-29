#ifndef SOLOADER_WAITPROF_H
#define SOLOADER_WAITPROF_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    WP_USLEEP, WP_NANOSLEEP, WP_YIELD, WP_MUTEX, WP_COND, WP_COND_TIMED,
    WP_SEM, WP_FOPEN, WP_GETSTAT, WP_FREAD, WP_FCLOSE,
    WP_KIND_COUNT
};

uint64_t waitprof_now(void);
unsigned waitprof_caller(void * lr);
void waitprof_add(int kind, unsigned caller, uint64_t us);
void waitprof_dump(const char * tag, unsigned frames, unsigned max_frame_ms);
void waitprof_frame(unsigned frame_ms);
void waitprof_start(void);

// Mide `expr` y lo acumula como `kind` a nombre de quien llamo al wrapper actual.
#define WAITPROF(kind, expr) do { \
        uint64_t _wp_t0 = waitprof_now(); \
        expr; \
        waitprof_add((kind), waitprof_caller(__builtin_return_address(0)), waitprof_now() - _wp_t0); \
    } while (0)

int usleep_soloader(unsigned usec);
int nanosleep_soloader(const struct timespec * req, struct timespec * rem);
int sched_yield_soloader(void);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_WAITPROF_H
