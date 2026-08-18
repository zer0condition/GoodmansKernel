/* stdio.h - kshim decls, bodies in kshim.c */
#pragma once

#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void FILE;

extern FILE* const stdout;
extern FILE* const stderr;
extern FILE* const stdin;

int  __cdecl printf(const char* fmt, ...);
int  __cdecl fprintf(FILE* stream, const char* fmt, ...);
int  __cdecl snprintf(char* buf, size_t sz, const char* fmt, ...);
int  __cdecl vsnprintf(char* buf, size_t sz, const char* fmt, va_list ap);
int  __cdecl puts(const char* s);
int  __cdecl fputs(const char* s, FILE* stream);
int  __cdecl fputc(int c, FILE* stream);
int  __cdecl putchar(int c);
int  __cdecl fflush(FILE* stream);

#ifdef __cplusplus
}
#endif
