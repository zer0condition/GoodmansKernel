/* kshim.h - interface for kshim.c and driver TUs */
#pragma once

#include <ntddk.h>
#include <ntstrsafe.h>

#define GVM_POOL_TAG 'MVoG'

#ifdef __cplusplus
extern "C" {
#endif

void* kshim_alloc(size_t n);
void* kshim_calloc(size_t n, size_t sz);
void* kshim_realloc(void* p, size_t new_sz);
void  kshim_free(void* p);
void  kshim_abort(const char* msg);
int   kshim_printf(const char* fmt, ...);
int   kshim_snprintf(char* buf, size_t sz, const char* fmt, ...);
int   kshim_vsnprintf(char* buf, size_t sz, const char* fmt, va_list ap);

#ifdef __cplusplus
}
#endif
