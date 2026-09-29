/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  mem.h
 * @brief Implementations and wrappers for memory-related functions.
 */

#ifndef SOLOADER_MEM_H
#define SOLOADER_MEM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

#define MAP_FAILED (void*)-1

void *sceClibMemclr(void *dst, size_t len);

void *memcpy_soloader(void *dst, const void *src, size_t len);
void *memmove_soloader(void *dst, const void *src, size_t len);
void *memset_soloader(void *dst, int c, size_t len);

void *malloc_soloader(size_t size);
void *calloc_soloader(size_t n, size_t size);
void *realloc_soloader(void *ptr, size_t size);
void heap_log_snapshot(const char *tag);

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offs);

int munmap(void *addr, size_t length);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_MEM_H
