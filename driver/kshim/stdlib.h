/* stdlib.h - kshim decls */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

void* __cdecl malloc(size_t n);
void* __cdecl calloc(size_t n, size_t sz);
void* __cdecl realloc(void* p, size_t new_sz);
void  __cdecl free(void* p);
void  __cdecl abort(void);
void  __cdecl exit(int status);

#define _abs64(x) ((x) < 0 ? -(x) : (x))

#ifdef __cplusplus
}
#endif
