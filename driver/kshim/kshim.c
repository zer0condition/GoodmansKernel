/* kshim.c - stdlib/stdio bodies wasm3 links against */
#include "kshim.h"
#include "stdio.h"
#include "stdlib.h"

FILE* const stdout = (FILE*)(void*)1;
FILE* const stderr = (FILE*)(void*)2;
FILE* const stdin  = (FILE*)(void*)3;

typedef struct {
    size_t size;
    unsigned char data[1];
} kshim_hdr;

#define HDR_OFF FIELD_OFFSET(kshim_hdr, data)

void* __cdecl malloc(size_t n)
{
    if (n == 0)
        return NULL;

    kshim_hdr* h = (kshim_hdr*)ExAllocatePoolWithTag(NonPagedPoolNx, n + HDR_OFF, GVM_POOL_TAG);
    if (!h)
        return NULL;

    h->size = n;
    return h->data;
}

void* __cdecl calloc(size_t n, size_t sz)
{
    size_t total = n * sz;
    if (sz != 0 && total / sz != n)
        return NULL;

    void* p = malloc(total);
    if (p)
        RtlZeroMemory(p, total);
    return p;
}

void __cdecl free(void* p)
{
    if (!p)
        return;
    kshim_hdr* h = (kshim_hdr*)((unsigned char*)p - HDR_OFF);
    ExFreePoolWithTag(h, GVM_POOL_TAG);
}

void* __cdecl realloc(void* p, size_t new_sz)
{
    if (!p)
        return malloc(new_sz);

    if (new_sz == 0) {
        free(p);
        return NULL;
    }

    kshim_hdr* h = (kshim_hdr*)((unsigned char*)p - HDR_OFF);
    size_t old_sz = h->size;
    if (old_sz >= new_sz)
        return p;

    void* np = malloc(new_sz);
    if (!np)
        return NULL;

    RtlCopyMemory(np, p, old_sz);
    free(p);
    return np;
}

void __cdecl abort(void)
{
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "[goodmans] abort\n");
    __fastfail(0);
}

void __cdecl exit(int status)
{
    UNREFERENCED_PARAMETER(status);
    abort();
}

int __cdecl printf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ULONG r = vDbgPrintExWithPrefix("[goodmans] ", DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, fmt, ap);
    va_end(ap);
    return (int)r;
}

int __cdecl fprintf(FILE* stream, const char* fmt, ...)
{
    UNREFERENCED_PARAMETER(stream);
    va_list ap;
    va_start(ap, fmt);
    ULONG r = vDbgPrintExWithPrefix("[goodmans] ", DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, fmt, ap);
    va_end(ap);
    return (int)r;
}

int __cdecl snprintf(char* buf, size_t sz, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    NTSTATUS s = RtlStringCbVPrintfA(buf, sz, fmt, ap);
    va_end(ap);
    if (!NT_SUCCESS(s))
        return -1;
    size_t len = 0;
    while (buf[len]) len++;
    return (int)len;
}

int __cdecl vsnprintf(char* buf, size_t sz, const char* fmt, va_list ap)
{
    NTSTATUS s = RtlStringCbVPrintfA(buf, sz, fmt, ap);
    if (!NT_SUCCESS(s))
        return -1;
    size_t len = 0;
    while (buf[len]) len++;
    return (int)len;
}

int __cdecl puts(const char* s)
{
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "[goodmans] %s\n", s ? s : "");
    return 0;
}

int __cdecl fputs(const char* s, FILE* stream)
{
    UNREFERENCED_PARAMETER(stream);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "[goodmans] %s", s ? s : "");
    return 0;
}

int __cdecl fputc(int c, FILE* stream)
{
    UNREFERENCED_PARAMETER(stream);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "%c", c);
    return c;
}

int __cdecl putchar(int c)
{
    return fputc(c, NULL);
}

int __cdecl fflush(FILE* stream)
{
    UNREFERENCED_PARAMETER(stream);
    return 0;
}

// m3_CallArgv wants strtoul/strtoull, base-10 or 0x hex
static int _kshim_digit(int c, int base)
{
    int v = -1;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') v = c - 'A' + 10;
    if (v < 0 || v >= base) return -1;
    return v;
}

unsigned long __cdecl strtoul(const char* nptr, char** endptr, int base)
{
    const char* p = nptr;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '+') p++;

    if (base == 0) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
        else if (p[0] == '0') { base = 8; p++; }
        else base = 10;
    } else if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }

    unsigned long acc = 0;
    int d;
    while ((d = _kshim_digit((unsigned char)*p, base)) >= 0) {
        acc = acc * (unsigned long)base + (unsigned long)d;
        p++;
    }
    if (endptr) *endptr = (char*)p;
    return acc;
}

unsigned long long __cdecl strtoull(const char* nptr, char** endptr, int base)
{
    const char* p = nptr;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '+') p++;

    if (base == 0) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
        else if (p[0] == '0') { base = 8; p++; }
        else base = 10;
    } else if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }

    unsigned long long acc = 0;
    int d;
    while ((d = _kshim_digit((unsigned char)*p, base)) >= 0) {
        acc = acc * (unsigned long long)base + (unsigned long long)d;
        p++;
    }
    if (endptr) *endptr = (char*)p;
    return acc;
}

// kshim_* aliases for direct kshim.h consumers
void* kshim_alloc(size_t n)                { return malloc(n); }
void* kshim_calloc(size_t n, size_t sz)    { return calloc(n, sz); }
void* kshim_realloc(void* p, size_t new_sz){ return realloc(p, new_sz); }
void  kshim_free(void* p)                  { free(p); }
void  kshim_abort(const char* msg)         { UNREFERENCED_PARAMETER(msg); abort(); }
int   kshim_printf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ULONG r = vDbgPrintExWithPrefix("[goodmans] ", DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, fmt, ap);
    va_end(ap);
    return (int)r;
}
int   kshim_snprintf(char* buf, size_t sz, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    NTSTATUS s = RtlStringCbVPrintfA(buf, sz, fmt, ap);
    va_end(ap);
    if (!NT_SUCCESS(s)) return -1;
    size_t len = 0; while (buf[len]) len++;
    return (int)len;
}
int   kshim_vsnprintf(char* buf, size_t sz, const char* fmt, va_list ap)
{
    NTSTATUS s = RtlStringCbVPrintfA(buf, sz, fmt, ap);
    if (!NT_SUCCESS(s)) return -1;
    size_t len = 0; while (buf[len]) len++;
    return (int)len;
}
