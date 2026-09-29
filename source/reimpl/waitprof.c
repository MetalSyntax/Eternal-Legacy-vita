/*
 * waitprof.c -- perfilador de esperas/IO del .so (Bug #007).
 *
 * Log 010: tras el menu el motor abre ~1800 voces .wav de a una, ~85 ms por archivo
 * (open->close = 11 ms, el resto sin ningun log), sin correlacion con el tamaño y sin
 * llegar nunca a 300 frames: la carga corre dentro de nativeRender y algo la frena.
 * En vez de adivinar, cada wrapper de espera (usleep/nanosleep/sched_yield, mutex
 * bloqueado, cond wait, sem wait) y de IO (fopen/fread/getstat) acumula el tiempo por
 * (tipo, direccion de retorno dentro del .so). Cada ~2 s el main loop vuelca el top.
 *
 * Costo en el caso sano: dos lecturas de reloj + un lock corto por llamada.
 */

#include "reimpl/waitprof.h"

#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

#define WP_SLOTS 96
#define WP_TOP   12
#define WAITPROF_PERIOD_MS 2000

typedef struct {
    unsigned caller; // offset dentro del .so (0 = fuera del .so)
    int kind;
    uint64_t us;
    unsigned calls;
} wp_entry;

static const char * const s_kind_names[WP_KIND_COUNT] = {
    "usleep", "nanosleep", "sched_yield", "mutex", "cond_wait", "cond_timedwait",
    "sem_wait", "fopen", "getstat", "fread", "fclose",
};

static wp_entry s_wp[WP_SLOTS];
static unsigned s_dropped = 0;
static pthread_mutex_t s_wp_mutex = PTHREAD_MUTEX_INITIALIZER;

uint64_t waitprof_now(void) {
    return sceKernelGetProcessTimeWide();
}

unsigned waitprof_caller(void * lr) {
    uintptr_t a = (uintptr_t) lr;
    if (so_mod.text_base && a >= so_mod.text_base && a < so_mod.text_base + so_mod.text_size)
        return (unsigned) (a - so_mod.text_base);
    return 0;
}

void waitprof_add(int kind, unsigned caller, uint64_t us) {
    pthread_mutex_lock(&s_wp_mutex);
    wp_entry * free_slot = NULL;
    for (int i = 0; i < WP_SLOTS; i++) {
        wp_entry * e = &s_wp[i];
        if (e->calls && e->kind == kind && e->caller == caller) {
            e->us += us;
            e->calls++;
            pthread_mutex_unlock(&s_wp_mutex);
            return;
        }
        if (!e->calls && !free_slot) free_slot = e;
    }
    if (free_slot) {
        free_slot->kind = kind;
        free_slot->caller = caller;
        free_slot->us = us;
        free_slot->calls = 1;
    } else {
        s_dropped++;
    }
    pthread_mutex_unlock(&s_wp_mutex);
}

static volatile unsigned s_frames = 0;
static volatile unsigned s_max_frame_ms = 0;

void waitprof_frame(unsigned frame_ms) {
    s_frames++;
    if (frame_ms > s_max_frame_ms) s_max_frame_ms = frame_ms;
}

void waitprof_dump(const char * tag, unsigned frames, unsigned max_frame_ms) {
    wp_entry snap[WP_SLOTS];
    unsigned dropped;

    pthread_mutex_lock(&s_wp_mutex);
    memcpy(snap, s_wp, sizeof(snap));
    memset(s_wp, 0, sizeof(s_wp));
    dropped = s_dropped;
    s_dropped = 0;
    pthread_mutex_unlock(&s_wp_mutex);

    l_info("waitprof[%s]: %u frames, frame max %u ms%s", tag, frames, max_frame_ms,
           dropped ? " (tabla llena, hay entradas perdidas)" : "");

    // Top WP_TOP por tiempo total (seleccion simple, la tabla es chica).
    for (int n = 0; n < WP_TOP; n++) {
        int best = -1;
        for (int i = 0; i < WP_SLOTS; i++) {
            if (snap[i].calls && (best < 0 || snap[i].us > snap[best].us)) best = i;
        }
        if (best < 0 || snap[best].us < 1000) break;
        wp_entry * e = &snap[best];
        l_info("  %-14s .so+0x%06x  %6u llamadas  %7u ms  (%u us/llamada)",
               s_kind_names[e->kind], e->caller, e->calls, (unsigned) (e->us / 1000),
               (unsigned) (e->us / e->calls));
        e->calls = 0;
    }
}

int usleep_soloader(unsigned usec) {
    int ret;
    WAITPROF(WP_USLEEP, ret = usleep(usec));
    return ret;
}

int nanosleep_soloader(const struct timespec * req, struct timespec * rem) {
    int ret;
    WAITPROF(WP_NANOSLEEP, ret = nanosleep(req, rem));
    return ret;
}

int sched_yield_soloader(void) {
    int ret;
    WAITPROF(WP_YIELD, ret = sched_yield());
    return ret;
}

// Hilo de volcado: el main loop puede quedar minutos dentro de un nativeRender (la carga
// del menu corre ahi), asi que el volcado no puede depender de que termine un frame.
static void * waitprof_thread(void * arg) {
    (void) arg;
    for (;;) {
        sceKernelDelayThread(WAITPROF_PERIOD_MS * 1000);
        unsigned frames = s_frames, max_ms = s_max_frame_ms;
        s_frames = 0;
        s_max_frame_ms = 0;

        // Silencio si la ventana fue sana: frames completos y ninguno largo.
        if (max_ms > 100 || frames < WAITPROF_PERIOD_MS / 50) {
            waitprof_dump("2s", frames, max_ms);
            log_flush();
        } else {
            pthread_mutex_lock(&s_wp_mutex);
            memset(s_wp, 0, sizeof(s_wp));
            pthread_mutex_unlock(&s_wp_mutex);
        }
    }
    return NULL;
}

void waitprof_start(void) {
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16 * 1024);
    pthread_create(&t, &attr, waitprof_thread, NULL);
    pthread_attr_destroy(&attr);
}
