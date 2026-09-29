/*
 * stdio_std.c -- stdin/stdout/stderr de Bionic para el .so (Bug #008).
 *
 * En Bionic `stdout` es `&__sF[1]` y `stderr` es `&__sF[2]`, con FILE de 0x54 bytes
 * (CScriptManager::OutputError hace `fprintf(__sF + 0x54, ...)`). El loader exportaba un
 * array de FILE de newlib (otro tamaño) y los f* van a SceLibc: los errores de Lua
 * ("Script syntex error", "%s ----> %s") terminaban en un FILE* basura y se perdian.
 *
 * Aca __sF es un bloque opaco del tamaño de Bionic y toda funcion f... o printf que reciba uno de
 * esos punteros se redirige al logger, linea por linea. Cualquier otro FILE* sigue a libc.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#define RAW_IO(fn) sceLibcBridge_##fn
#else
#define RAW_IO(fn) fn
#endif

#include "reimpl/stdio_std.h"
#include "utils/logger.h"

#define BIONIC_FILE_SIZE 0x54

uint8_t bionic_sF[3 * BIONIC_FILE_SIZE] __attribute__((aligned(8)));

static char s_line[3][512];
static size_t s_line_len[3];
static pthread_mutex_t s_std_mutex = PTHREAD_MUTEX_INITIALIZER;

static int std_idx(const void * f) {
    const uint8_t * p = (const uint8_t *) f;
    if (p < bionic_sF || p >= bionic_sF + sizeof(bionic_sF)) return -1;
    return (int) ((p - bionic_sF) / BIONIC_FILE_SIZE);
}

// Asume s_std_mutex tomado.
static void std_emit(int idx) {
    if (!s_line_len[idx]) return;
    s_line[idx][s_line_len[idx]] = '\0';
    if (idx == 2) {
        l_warn("[stderr] %s", s_line[idx]);
        log_flush();
    } else {
        l_info("[stdout] %s", s_line[idx]);
    }
    s_line_len[idx] = 0;
}

static void std_write(int idx, const char * s, size_t len) {
    pthread_mutex_lock(&s_std_mutex);
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n') {
            std_emit(idx);
        } else if (s[i] != '\r') {
            if (s_line_len[idx] >= sizeof(s_line[0]) - 1) std_emit(idx);
            s_line[idx][s_line_len[idx]++] = s[i];
        }
    }
    pthread_mutex_unlock(&s_std_mutex);
}

static int std_vprintf(int idx, const char * fmt, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n > 0) std_write(idx, buf, (size_t) n < sizeof(buf) ? (size_t) n : sizeof(buf) - 1);
    return n;
}

int vfprintf_soloader(FILE * f, const char * fmt, va_list ap) {
    int idx = std_idx(f);
    if (idx >= 0) return std_vprintf(idx, fmt, ap);
    return RAW_IO(vfprintf)(f, fmt, ap);
}

int fprintf_soloader(FILE * f, const char * fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vfprintf_soloader(f, fmt, ap);
    va_end(ap);
    return ret;
}

int fputs_soloader(const char * s, FILE * f) {
    int idx = std_idx(f);
    if (idx >= 0) {
        std_write(idx, s, strlen(s));
        return 1;
    }
    return RAW_IO(fputs)(s, f);
}

size_t fwrite_soloader(const void * p, size_t size, size_t count, FILE * f) {
    int idx = std_idx(f);
    if (idx >= 0) {
        std_write(idx, (const char *) p, size * count);
        return count;
    }
    return RAW_IO(fwrite)(p, size, count, f);
}

int fputc_soloader(int c, FILE * f) {
    int idx = std_idx(f);
    if (idx >= 0) {
        char ch = (char) c;
        std_write(idx, &ch, 1);
        return (unsigned char) c;
    }
    return RAW_IO(fputc)(c, f);
}

int putc_soloader(int c, FILE * f) {
    int idx = std_idx(f);
    if (idx >= 0) return fputc_soloader(c, f);
    return RAW_IO(putc)(c, f);
}

int fflush_soloader(FILE * f) {
    int idx = std_idx(f);
    if (idx >= 0) {
        pthread_mutex_lock(&s_std_mutex);
        std_emit(idx);
        pthread_mutex_unlock(&s_std_mutex);
        return 0;
    }
    return RAW_IO(fflush)(f);
}

int vprintf_soloader(const char * fmt, va_list ap) {
    return std_vprintf(1, fmt, ap);
}

int printf_soloader(const char * fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = std_vprintf(1, fmt, ap);
    va_end(ap);
    return ret;
}

int puts_soloader(const char * s) {
    std_write(1, s, strlen(s));
    std_write(1, "\n", 1);
    return 1;
}

int putchar_soloader(int c) {
    char ch = (char) c;
    std_write(1, &ch, 1);
    return (unsigned char) c;
}
