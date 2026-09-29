/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/mem.h"
#include "utils/logger.h"

#include <string.h>
#include <malloc.h>
#include <psp2/kernel/clib.h>
void *sceClibMemclr(void *dst, size_t len) {
    return sceClibMemset(dst, 0, len);
}

/*
 * El .so llama a memcpy/memmove/memset directamente (PLT) para parsear archivos
 * (glitch::res::File::Init y cia). Un tamaño calculado con underflow (size - 60 con
 * size < 60) o un buffer de un malloc fallido llega acá como memcpy(NULL, heap, -4)
 * = data abort en SceLibKernel sin ninguna pista en el log (Bug #003/#004: R0=0,
 * R1=heap, R2=0xFFFFFFFC, PC=SceLibKernel+0x38). Estos wrappers no cambian el caso
 * sano (una comparación + salto) pero convierten el crash silencioso en un error
 * logueado y flusheado: la copia se salta y el juego sigue para que el log muestre
 * QUÉ estaba cargando cuando pasó.
 */
#define MEM_GUARD_MAX (128 * 1024 * 1024)

void *memcpy_soloader(void *dst, const void *src, size_t len) {
    if (len == 0) return dst; // memcpy(dst, NULL, 0) benigno (params de material sin default)
    if (!dst || !src || len > MEM_GUARD_MAX) {
        void *lr = __builtin_return_address(0);
        unsigned so_off = (unsigned) lr >= 0x98000000 ? (unsigned) lr - 0x98000000 : 0;
        l_error("memcpy(%p, %p, 0x%x): RECHAZADO (dst/src NULL o len absurdo) desde PC=%p (.so+0x%x)",
                dst, src, (unsigned) len, lr, so_off);
        log_flush();
        return dst;
    }
    return sceClibMemcpy(dst, src, len);
}

void *memmove_soloader(void *dst, const void *src, size_t len) {
    if (!dst || !src || len > MEM_GUARD_MAX) {
        l_error("memmove(%p, %p, 0x%x): RECHAZADO (dst/src NULL o len absurdo)", dst, src, (unsigned) len);
        log_flush();
        return dst;
    }
    return sceClibMemmove(dst, src, len);
}

void *memset_soloader(void *dst, int c, size_t len) {
    if (!dst || len > MEM_GUARD_MAX) {
        l_error("memset(%p, 0x%x, 0x%x): RECHAZADO (dst NULL o len absurdo)", dst, (unsigned) c, (unsigned) len);
        log_flush();
        return dst;
    }
    return sceClibMemset(dst, c, len);
}

/*
 * GlitchAlloc(size) del motor es un malloc directo: cuando devuelve NULL el juego
 * sigue y el crash aparece 20 instrucciones despues en un memcpy mudo (Bug #004).
 * Estos wrappers no cambian el caso sano; ante un NULL loguean el tamaño pedido y una
 * foto del heap (mallinfo) para distinguir "size basura por header mal parseado"
 * (size gigante, heap con lugar) de "heap agotado/corrupto" (size sano, heap lleno).
 */
static void log_alloc_fail(const char *fn, size_t size, void *caller) {
    struct mallinfo mi = mallinfo();
    // Quién pidió: si el caller está en el .so, offset para resolver con objdump.
    // (El __builtin_return_address se captura en el wrapper, no acá: esta función
    // helper reportaría su propio sitio de llamada dentro del loader.)
    unsigned so_off = (unsigned) caller >= 0x98000000 ? (unsigned) caller - 0x98000000 : 0;
    l_error("%s(0x%x): NULL (heap: arena=%d usados=%d libres=%d) desde PC=%p (.so+0x%x)",
            fn, (unsigned) size, mi.arena, mi.uordblks, mi.fordblks, caller, so_off);
    log_flush();
}

void *malloc_soloader(size_t size) {
    void *p = malloc(size);
    if (!p && size) log_alloc_fail("malloc", size, __builtin_return_address(0));
    return p;
}

void *calloc_soloader(size_t n, size_t size) {
    void *p = calloc(n, size);
    if (!p && n && size) log_alloc_fail("calloc", n * size, __builtin_return_address(0));
    return p;
}

void *realloc_soloader(void *ptr, size_t size) {
    void *p = realloc(ptr, size);
    if (!p && size) log_alloc_fail("realloc", size, __builtin_return_address(0));
    return p;
}

void heap_log_snapshot(const char *tag) {
    struct mallinfo mi = mallinfo();
    l_info("heap[%s]: arena=%d usados=%d libres=%d", tag, mi.arena, mi.uordblks, mi.fordblks);
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offs) {
    l_warn("mmap(%p, %i, %i, %i, %i, %li)", addr, length, prot, flags, fd, offs);

    if (length <= 0) {
        return MAP_FAILED;
    }
    void* ret= malloc(length);
    memset(ret, 0, length);
    return ret;
}

int munmap(void *addr, size_t length) {
    if (addr) free(addr);
    return 0;
}
