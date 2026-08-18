/* gvm_ext.cpp - WinDbg extension for inspecting Goodmans state.
 *
 * bang commands:
 *   !gvm.modules            enumerate loaded wasm modules from the driver
 *   !gvm.info <id>          detailed info on one module (name/size/hash/caps)
 *   !gvm.mem  <id> <off> <len>   dump guest linear memory
 *   !gvm.help
 *
 * load with:
 *   .load path\to\gvm_ext.dll
 *
 * requires:
 *   - Goodmans.sys loaded with symbols (Goodmans.pdb) in .sympath
 *   - live kernel debugging OR full/kernel minidump
 */
#include <windows.h>
#include <dbgeng.h>
#include <stdio.h>
#include <string.h>

extern "C" {
    IDebugClient*        g_client   = nullptr;
    IDebugControl*       g_control  = nullptr;
    IDebugSymbols*       g_symbols  = nullptr;
    IDebugDataSpaces*    g_data     = nullptr;
}

// mirror of gvm_module layout. must stay in sync with driver/inc/gvm.h
#pragma pack(push, 8)
struct gvm_module_dbg {
    ULONG          id;
    UCHAR          used;
    UCHAR          _pad0[7];
    ULONG64        env;
    ULONG64        runtime;
    ULONG64        module;
    ULONG64        wasm_bytes;
    ULONG          wasm_size;
    ULONG          _pad1;
    ULONG64        hash;
    volatile LONG  refcount;
    ULONG          _pad2;
    ULONG64        call_mutex[6];      // KMUTEX opaque
    volatile LONG64 pool_used;
    LONG64         pool_budget;
    ULONG          caps;
    ULONG          _pad3;
    volatile ULONG64 exec_deadline_qpc;
    CHAR           name[64];
};
#pragma pack(pop)

#define GVM_MAX_MODULES 32

static HRESULT read_kmem(ULONG64 addr, void* buf, ULONG size)
{
    if (!g_data) return E_FAIL;
    ULONG got = 0;
    HRESULT hr = g_data->ReadVirtual(addr, buf, size, &got);
    if (SUCCEEDED(hr) && got != size) return E_FAIL;
    return hr;
}

static HRESULT resolve_symbol(const char* name, ULONG64* out)
{
    if (!g_symbols) return E_FAIL;
    return g_symbols->GetOffsetByName(name, out);
}

extern "C" __declspec(dllexport) HRESULT CALLBACK
DebugExtensionInitialize(PULONG version, PULONG flags)
{
    *version = DEBUG_EXTENSION_VERSION(1, 0);
    *flags   = 0;

    if (DebugCreate(__uuidof(IDebugClient), (void**)&g_client) != S_OK)
        return E_FAIL;

    g_client->QueryInterface(__uuidof(IDebugControl),    (void**)&g_control);
    g_client->QueryInterface(__uuidof(IDebugSymbols),    (void**)&g_symbols);
    g_client->QueryInterface(__uuidof(IDebugDataSpaces), (void**)&g_data);
    return S_OK;
}

extern "C" __declspec(dllexport) void CALLBACK
DebugExtensionUninitialize(void)
{
    if (g_data)     g_data->Release();
    if (g_symbols)  g_symbols->Release();
    if (g_control)  g_control->Release();
    if (g_client)   g_client->Release();
    g_data = nullptr; g_symbols = nullptr; g_control = nullptr; g_client = nullptr;
}

static void printf_ext(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    char buf[1024];
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (g_control) g_control->Output(DEBUG_OUTPUT_NORMAL, "%s", buf);
}

static void print_cap_string(char* out, size_t out_sz, ULONG caps)
{
    static const char* names[] = {
        "ALLOC","READ","WRITE","MSR_R","MSR_W","PHYS","CPUID","CB","HCALL","INTRO"
    };
    out[0] = 0;
    if (caps == 0xFFFFFFFFu) { strncpy_s(out, out_sz, "ALL", _TRUNCATE); return; }
    for (int i = 0; i < 10; i++) {
        if (caps & (1u << i)) {
            if (out[0]) strncat_s(out, out_sz, "|", _TRUNCATE);
            strncat_s(out, out_sz, names[i], _TRUNCATE);
        }
    }
}

extern "C" __declspec(dllexport) HRESULT CALLBACK
modules(IDebugClient* client, PCSTR args)
{
    UNREFERENCED_PARAMETER(client);
    UNREFERENCED_PARAMETER(args);

    ULONG64 base = 0;
    if (FAILED(resolve_symbol("Goodmans!g_modules", &base))) {
        printf_ext("[!] cannot resolve Goodmans!g_modules. is the .pdb loaded?\n");
        return E_FAIL;
    }

    printf_ext("%-4s %-6s %-10s %-10s %-16s %-24s %s\n",
               "id","wasm","pool","budget","hash","caps","name");
    printf_ext("---- ------ ---------- ---------- ---------------- ------------------------ ----\n");

    unsigned int active = 0;
    for (unsigned int i = 0; i < GVM_MAX_MODULES; i++) {
        gvm_module_dbg m = {};
        if (FAILED(read_kmem(base + i * sizeof(m), &m, sizeof(m)))) break;
        if (!m.used) continue;
        active++;
        char capstr[128] = {};
        print_cap_string(capstr, sizeof(capstr), m.caps);
        printf_ext("%-4u %-6u %-10lld %-10lld %016llx %-24s %s\n",
                   m.id, m.wasm_size, m.pool_used, m.pool_budget, m.hash, capstr, m.name);
    }
    printf_ext("\n(%u active)\n", active);
    return S_OK;
}

extern "C" __declspec(dllexport) HRESULT CALLBACK
info(IDebugClient* client, PCSTR args)
{
    UNREFERENCED_PARAMETER(client);
    if (!args || !*args) { printf_ext("usage: !gvm.info <id>\n"); return E_INVALIDARG; }

    unsigned int id = (unsigned int)strtoul(args, nullptr, 0);
    if (id == 0 || id > GVM_MAX_MODULES) { printf_ext("bad id\n"); return E_INVALIDARG; }

    ULONG64 base = 0;
    if (FAILED(resolve_symbol("Goodmans!g_modules", &base))) return E_FAIL;

    gvm_module_dbg m = {};
    ULONG64 slot = base + (id - 1) * sizeof(m);
    if (FAILED(read_kmem(slot, &m, sizeof(m)))) return E_FAIL;
    if (!m.used) { printf_ext("slot %u is free\n", id); return S_OK; }

    char capstr[128] = {};
    print_cap_string(capstr, sizeof(capstr), m.caps);

    printf_ext("id:            %u\n", m.id);
    printf_ext("name:          %s\n", m.name);
    printf_ext("wasm size:     %u bytes\n", m.wasm_size);
    printf_ext("wasm bytes ka: 0x%016llx\n", m.wasm_bytes);
    printf_ext("hash:          %016llx\n", m.hash);
    printf_ext("refcount:      %d\n", m.refcount);
    printf_ext("pool used:     %lld / %lld\n", m.pool_used, m.pool_budget);
    printf_ext("caps:          %08x  (%s)\n", m.caps, capstr);
    printf_ext("env ka:        0x%016llx\n", m.env);
    printf_ext("runtime ka:    0x%016llx\n", m.runtime);
    printf_ext("module ka:     0x%016llx\n", m.module);
    return S_OK;
}

extern "C" __declspec(dllexport) HRESULT CALLBACK
mem(IDebugClient* client, PCSTR args)
{
    UNREFERENCED_PARAMETER(client);
    if (!args || !*args) {
        printf_ext("usage: !gvm.mem <id> <off> <len>\n");
        return E_INVALIDARG;
    }

    unsigned int id, off, len;
    if (sscanf_s(args, "%u %x %x", &id, &off, &len) != 3) {
        if (sscanf_s(args, "%u %u %u", &id, &off, &len) != 3) {
            printf_ext("bad args\n"); return E_INVALIDARG;
        }
    }
    if (id == 0 || id > GVM_MAX_MODULES || len == 0 || len > 4096) {
        printf_ext("bad args (len max 4096)\n"); return E_INVALIDARG;
    }

    ULONG64 base = 0;
    if (FAILED(resolve_symbol("Goodmans!g_modules", &base))) return E_FAIL;

    gvm_module_dbg m = {};
    if (FAILED(read_kmem(base + (id - 1) * sizeof(m), &m, sizeof(m))) || !m.used || !m.runtime) {
        printf_ext("slot not loaded\n"); return E_FAIL;
    }

    // wasm3 stores linear memory at runtime->memory.mallocated + offset.
    // walking the M3Runtime struct requires matching wasm3's layout exactly.
    // for portability, emit a KD-side memory command using a symbolic offset
    // once the exact struct is resolved via .lookup_field.
    printf_ext("[!] guest linear memory walk requires wasm3 pdb. read runtime@0x%016llx manually with dt or !address\n",
               m.runtime);
    printf_ext("    len=%u off=%u\n", len, off);
    return S_OK;
}

extern "C" __declspec(dllexport) HRESULT CALLBACK
help(IDebugClient* client, PCSTR args)
{
    UNREFERENCED_PARAMETER(client);
    UNREFERENCED_PARAMETER(args);
    printf_ext("Goodmans WinDbg extension\n");
    printf_ext("  !gvm.modules              enumerate loaded modules\n");
    printf_ext("  !gvm.info    <id>         detailed info for module id\n");
    printf_ext("  !gvm.mem     <id> <off> <len>  dump guest linear memory (needs wasm3 pdb)\n");
    printf_ext("  !gvm.help                 this help\n");
    printf_ext("\nrequires Goodmans.pdb in .sympath.\n");
    return S_OK;
}
