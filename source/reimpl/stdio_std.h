#ifndef SOLOADER_STDIO_STD_H
#define SOLOADER_STDIO_STD_H

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// __sF de Bionic (3 FILE de 0x54 bytes): stdout/stderr del .so van al logger.
extern uint8_t bionic_sF[];

int vfprintf_soloader(FILE * f, const char * fmt, va_list ap);
int fprintf_soloader(FILE * f, const char * fmt, ...);
int fputs_soloader(const char * s, FILE * f);
size_t fwrite_soloader(const void * p, size_t size, size_t count, FILE * f);
int fputc_soloader(int c, FILE * f);
int putc_soloader(int c, FILE * f);
int fflush_soloader(FILE * f);
int vprintf_soloader(const char * fmt, va_list ap);
int printf_soloader(const char * fmt, ...);
int puts_soloader(const char * s);
int putchar_soloader(int c);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_STDIO_STD_H
