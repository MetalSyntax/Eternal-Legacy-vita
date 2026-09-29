/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/logger.h"

#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/*
 * Estandar de logs del toolkit (PORTING_PLAN.md seccion 6):
 *   <DATA_PATH>logs/eternallegacy_NNN.log, NNN de 001 a 999, siguiente indice en
 *   <DATA_PATH>logs/next.idx. Una ejecucion = un archivo.
 *
 * Las lineas se acumulan en un buffer en RAM y se vuelcan con log_flush() (errores,
 * pasos del boot y cada N frames desde main.c) -- abrir/cerrar el archivo por cada
 * linea congela la carga cuando el motor escanea miles de archivos (leccion de
 * GT-Racing-Motor-Academy-HD-vita, Fase 10).
 */
#define LOG_DIR       DATA_PATH "logs"
#define LOG_IDX_PATH  LOG_DIR "/next.idx"
#define LOG_SLUG      "eternallegacy"

static SceKernelLwMutexWork _log_mutex;
static atomic_bool _log_mutex_ready = ATOMIC_VAR_INIT(false);

// Buffer A is used to adjust the format string.
static char buffer_a[2048];
// Buffer B is used to compile the final log using the updated format string.
static char buffer_b[2048];

static char log_path[128];
static SceUID log_fd = -1;
static char pending[32 * 1024];
static size_t pending_len = 0;

static void _log_open(void) {
    sceIoMkdir(DATA_PATH, 0777);
    sceIoMkdir(LOG_DIR, 0777);

    int idx = 1;
    SceUID fd = sceIoOpen(LOG_IDX_PATH, SCE_O_RDONLY, 0);
    if (fd >= 0) {
        char buf[16] = {0};
        sceIoRead(fd, buf, sizeof(buf) - 1);
        sceIoClose(fd);
        idx = atoi(buf);
        if (idx < 1 || idx > 999) idx = 1;
    }

    sceClibSnprintf(log_path, sizeof(log_path), LOG_DIR "/" LOG_SLUG "_%03d.log", idx);

    int next = (idx >= 999) ? 1 : idx + 1;
    fd = sceIoOpen(LOG_IDX_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) {
        char buf[16];
        int len = sceClibSnprintf(buf, sizeof(buf), "%d\n", next);
        sceIoWrite(fd, buf, len);
        sceIoClose(fd);
    }

    log_fd = sceIoOpen(log_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
}

static void _log_flush_locked(void) {
    if (pending_len == 0) return;
    if (log_fd >= 0)
        sceIoWrite(log_fd, pending, pending_len);
    pending_len = 0;
}

static void _log_append_locked(const char *s) {
    size_t len = strlen(s);
    if (pending_len + len > sizeof(pending))
        _log_flush_locked();
    if (len > sizeof(pending))
        len = sizeof(pending);
    memcpy(pending + pending_len, s, len);
    pending_len += len;
}

static bool _log_lock(void) {
    if (!atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        int ret = sceKernelCreateLwMutex(&_log_mutex, "log_lock", 0, 0, NULL);
        if (ret < 0) {
            sceClibPrintf("Error: failed to create log mutex: 0x%x\n", ret);
            return false;
        }
        atomic_store_explicit(&_log_mutex_ready, true, memory_order_relaxed);
        _log_open();
    }
    sceKernelLockLwMutex(&_log_mutex, 1, NULL);
    return true;
}

void log_flush(void) {
    if (!_log_lock()) return;
    _log_flush_locked();
    sceKernelUnlockLwMutex(&_log_mutex, 1);
}

void _log_print(int t, const char* fmt, ...) {
    if (!_log_lock()) return;

    const char *tag;
    switch (t) {
        case LT_DEBUG:   tag = "debug";   break;
        case LT_INFO:    tag = "info";    break;
        case LT_WARN:    tag = "warning"; break;
        case LT_ERROR:   tag = "error";   break;
        case LT_FATAL:   tag = "fatal";   break;
        case LT_SUCCESS: tag = "success"; break;
        case LT_WAIT:    tag = "waiting"; break;
        default:
            sceKernelUnlockLwMutex(&_log_mutex, 1);
            return;
    }

    sceClibSnprintf(buffer_a, sizeof(buffer_a), "[%8u] %-7s %s\n",
                    (unsigned) (sceKernelGetProcessTimeLow() / 1000), tag, fmt);

    va_list list;
    va_start(list, fmt);
    sceClibVsnprintf(buffer_b, sizeof(buffer_b), buffer_a, list);
    va_end(list);

    sceClibPrintf("%s", buffer_b);
    _log_append_locked(buffer_b);

    if (t == LT_ERROR || t == LT_FATAL)
        _log_flush_locked();

    sceKernelUnlockLwMutex(&_log_mutex, 1);
}
