/* main.c - user-mode client */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../shared/goodmans_ioctl.h"

static HANDLE open_dev(void)
{
    HANDLE h = CreateFileA(GVM_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        fprintf(stderr, "CreateFile(%s) failed: %lu\n", GVM_USER_PATH, GetLastError());
    return h;
}

static int cmd_load(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "load <wasm-path>\n"); return 1; }
    const char* path = argv[2];

    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "open %s: %lu\n", path, GetLastError());
        return 1;
    }
    LARGE_INTEGER sz;
    GetFileSizeEx(f, &sz);
    if (sz.QuadPart <= 0 || sz.QuadPart > (16LL << 20)) {
        fprintf(stderr, "bad size %lld\n", sz.QuadPart);
        CloseHandle(f);
        return 1;
    }

    size_t buf_sz = sizeof(gvm_load_in) + (size_t)sz.QuadPart;
    unsigned char* buf = (unsigned char*)malloc(buf_sz);
    if (!buf) { CloseHandle(f); return 1; }

    gvm_load_in* in = (gvm_load_in*)buf;
    memset(in, 0, sizeof(*in));
    in->wasm_size   = (unsigned int)sz.QuadPart;
    in->stack_bytes = 0;
    strncpy_s(in->name, GVM_MAX_MODULE_NAME, path, _TRUNCATE);

    DWORD rd = 0;
    ReadFile(f, buf + sizeof(gvm_load_in), (DWORD)sz.QuadPart, &rd, NULL);
    CloseHandle(f);

    HANDLE dev = open_dev();
    if (dev == INVALID_HANDLE_VALUE) { free(buf); return 1; }

    gvm_load_out out = { 0 };
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(dev, IOCTL_GVM_LOAD_MODULE,
                              buf, (DWORD)buf_sz,
                              &out, sizeof(out), &ret, NULL);
    CloseHandle(dev);
    free(buf);

    if (!ok) { fprintf(stderr, "ioctl failed: %lu\n", GetLastError()); return 1; }
    if (out.status != 0) { fprintf(stderr, "load status=%d: %s\n", out.status, out.err_msg); return 1; }
    printf("module_id=%u (%s)\n", out.module_id, out.err_msg);
    return 0;
}

static int cmd_call(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "call [--timeout=ms] <module-id> <export> [arg1..arg8]\n");
        return 1;
    }

    unsigned int timeout_ms = 0;
    int base = 2;
    if (argc > base && strncmp(argv[base], "--timeout=", 10) == 0) {
        timeout_ms = (unsigned int)strtoul(argv[base] + 10, NULL, 0);
        base++;
    }
    if (argc <= base) { fprintf(stderr, "missing module id\n"); return 1; }

    gvm_call_in in = { 0 };
    in.module_id  = (unsigned int)strtoul(argv[base], NULL, 0);
    in.timeout_ms = timeout_ms;
    if (argc > base + 1) strncpy_s(in.export_name, GVM_MAX_EXPORT_NAME, argv[base + 1], _TRUNCATE);
    in.argc = 0;
    for (int i = base + 2; i < argc && in.argc < GVM_MAX_ARGS; i++)
        in.argv[in.argc++] = _strtoui64(argv[i], NULL, 0);

    HANDLE dev = open_dev();
    if (dev == INVALID_HANDLE_VALUE) return 1;

    gvm_call_out out = { 0 };
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(dev, IOCTL_GVM_CALL_EXPORT,
                              &in, sizeof(in),
                              &out, sizeof(out), &ret, NULL);
    CloseHandle(dev);

    if (!ok) { fprintf(stderr, "ioctl failed: %lu\n", GetLastError()); return 1; }
    if (out.status != 0) { fprintf(stderr, "call status=%d: %s\n", out.status, out.err_msg); return 1; }
    printf("rv=0x%llx (%llu)\n", out.rv, out.rv);
    return 0;
}

static int cmd_unload(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "unload <module-id>\n"); return 1; }
    gvm_unload_in in = { 0 };
    in.module_id = (unsigned int)strtoul(argv[2], NULL, 0);

    HANDLE dev = open_dev();
    if (dev == INVALID_HANDLE_VALUE) return 1;

    DWORD ret = 0;
    BOOL ok = DeviceIoControl(dev, IOCTL_GVM_UNLOAD_MODULE,
                              &in, sizeof(in), NULL, 0, &ret, NULL);
    CloseHandle(dev);

    if (!ok) { fprintf(stderr, "ioctl failed: %lu\n", GetLastError()); return 1; }
    printf("unloaded\n");
    return 0;
}

static int cmd_modules(void)
{
    HANDLE dev = open_dev();
    if (dev == INVALID_HANDLE_VALUE) return 1;

    gvm_list_out* out = (gvm_list_out*)malloc(sizeof(gvm_list_out));
    if (!out) { CloseHandle(dev); return 1; }
    memset(out, 0, sizeof(*out));

    DWORD ret = 0;
    BOOL ok = DeviceIoControl(dev, IOCTL_GVM_LIST_MODULES,
                              NULL, 0, out, sizeof(*out), &ret, NULL);
    CloseHandle(dev);

    if (!ok) { fprintf(stderr, "ioctl failed: %lu\n", GetLastError()); free(out); return 1; }

    printf("%-4s  %-8s  %-8s  %-8s  %-16s  %s\n",
           "id", "wasm", "mem", "pool", "hash", "name");
    for (unsigned int i = 0; i < out->count; i++) {
        gvm_module_entry* e = &out->entries[i];
        printf("%-4u  %-6uB   %-4uKB    %-6lluB  %016llx  %s\n",
               e->id, e->wasm_size, e->mem_pages * 64,
               e->pool_bytes, e->hash, e->name);
    }
    printf("(%u module%s)\n", out->count, out->count == 1 ? "" : "s");
    free(out);
    return 0;
}

static int cmd_info(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "info <module-id>\n"); return 1; }

    gvm_info_in in = { 0 };
    in.module_id = (unsigned int)strtoul(argv[2], NULL, 0);

    HANDLE dev = open_dev();
    if (dev == INVALID_HANDLE_VALUE) return 1;

    gvm_info_out* out = (gvm_info_out*)malloc(sizeof(gvm_info_out));
    if (!out) { CloseHandle(dev); return 1; }
    memset(out, 0, sizeof(*out));

    DWORD ret = 0;
    BOOL ok = DeviceIoControl(dev, IOCTL_GVM_MODULE_INFO,
                              &in, sizeof(in),
                              out, sizeof(*out), &ret, NULL);
    CloseHandle(dev);

    if (!ok) { fprintf(stderr, "ioctl failed: %lu\n", GetLastError()); free(out); return 1; }
    if (out->status != 0) { fprintf(stderr, "info status=%d: %s\n", out->status, out->err_msg); free(out); return 1; }

    printf("id:        %u\n", out->base.id);
    printf("name:      %s\n", out->base.name);
    printf("wasm size: %u bytes\n", out->base.wasm_size);
    printf("mem pages: %u (%u KB)\n", out->base.mem_pages, out->base.mem_pages * 64);
    printf("hash:      %016llx\n", out->base.hash);
    free(out);
    return 0;
}

static int cmd_unload_all(void)
{
    HANDLE dev = open_dev();
    if (dev == INVALID_HANDLE_VALUE) return 1;

    DWORD ret = 0;
    BOOL ok = DeviceIoControl(dev, IOCTL_GVM_UNLOAD_ALL,
                              NULL, 0, NULL, 0, &ret, NULL);
    CloseHandle(dev);

    if (!ok) { fprintf(stderr, "ioctl failed: %lu\n", GetLastError()); return 1; }
    printf("all modules unloaded\n");
    return 0;
}

static int write_file(const char* path, const char* contents)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { fprintf(stderr, "cannot create %s: %lu\n", path, GetLastError()); return 1; }
    DWORD w = 0; WriteFile(h, contents, (DWORD)strlen(contents), &w, NULL);
    CloseHandle(h);
    return 0;
}

static int cmd_new(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "new <module-name>\n"); return 1; }
    const char* name = argv[2];
    if (!CreateDirectoryA(name, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        fprintf(stderr, "cannot create dir %s: %lu\n", name, GetLastError());
        return 1;
    }
    char cpath[MAX_PATH], bpath[MAX_PATH];
    _snprintf_s(cpath, sizeof(cpath), _TRUNCATE, "%s\\%s.c", name, name);
    _snprintf_s(bpath, sizeof(bpath), _TRUNCATE, "%s\\build.cmd", name);

    char src[2048];
    _snprintf_s(src, sizeof(src), _TRUNCATE,
        "/* %s.c - goodmans wasm guest generated by `goodmans new`\n"
        " * declare capabilities in GVM_MANIFEST, use gvm_* helpers from the SDK.\n"
        " */\n"
        "\n"
        "#include \"gvm.h\"\n"
        "\n"
        "GVM_MANIFEST(GVM_CAP_ALLOC | GVM_CAP_READ_KMEM | GVM_CAP_INTROSPECT);\n"
        "\n"
        "GVM_EXPORT(hello)\n"
        "u32 hello(u32 x)\n"
        "{\n"
        "    // write to the driver debug ring - shows up in the GUI Log tab\n"
        "    static const char msg[] = \"hello from guest\";\n"
        "    gvm_dbg_print_raw(msg, sizeof(msg) - 1);\n"
        "    return x * 2 + 1;\n"
        "}\n"
        "\n"
        "GVM_EXPORT(current_pid)\n"
        "u32 current_pid(void) { return gvm_process_id(); }\n"
        "\n"
        "GVM_EXPORT(run_all)\n"
        "u32 run_all(void)\n"
        "{\n"
        "    (void)hello(21);\n"
        "    return current_pid();\n"
        "}\n",
        name);

    char build[1024];
    _snprintf_s(build, sizeof(build), _TRUNCATE,
        "@echo off\r\n"
        "setlocal\r\n"
        "cd /d \"%%~dp0\"\r\n"
        "set CLANG=%%LLVM_HOME%%\\bin\\clang.exe\r\n"
        "if not exist \"%%CLANG%%\" set CLANG=C:\\Program Files\\LLVM\\bin\\clang.exe\r\n"
        "if not exist \"%%CLANG%%\" ( echo [X] clang not found ^& exit /b 1 )\r\n"
        "set SDK=..\\guest_sdk\r\n"
        "\"%%CLANG%%\" -O2 --target=wasm32 -nostdlib -fno-builtin -I \"%%SDK%%\" ^\r\n"
        "  -Wl,--no-entry -Wl,--export-dynamic -Wl,--allow-undefined -Wl,--strip-all ^\r\n"
        "  -o %s.wasm %s.c\r\n"
        "if errorlevel 1 exit /b 1\r\n"
        "echo built %s.wasm\r\n",
        name, name, name);

    // put a copy of gvm.h next to the source for offline builds; if user has
    // the repo checked out, they can point SDK= to guest_sdk instead.
    if (write_file(cpath, src)) return 1;
    if (write_file(bpath, build)) return 1;
    printf("created %s\\%s.c\n", name, name);
    printf("created %s\\build.cmd\n", name);
    printf("\nnext:\n  cd %s\n  build.cmd\n  goodmans load %s.wasm\n", name, name);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "goodmans - Goodmans kernel VM CLI\n\n"
            "  goodmans load       <wasm-path>\n"
            "  goodmans call       [--timeout=ms] <module-id> <export> [arg1..arg8]\n"
            "  goodmans unload     <module-id>\n"
            "  goodmans modules\n"
            "  goodmans info       <module-id>\n"
            "  goodmans unload-all\n"
            "  goodmans new        <module-name>     scaffold a new guest\n");
        return 1;
    }
    if (!strcmp(argv[1], "load"))       return cmd_load(argc, argv);
    if (!strcmp(argv[1], "call"))       return cmd_call(argc, argv);
    if (!strcmp(argv[1], "unload"))     return cmd_unload(argc, argv);
    if (!strcmp(argv[1], "modules"))    return cmd_modules();
    if (!strcmp(argv[1], "info"))       return cmd_info(argc, argv);
    if (!strcmp(argv[1], "unload-all")) return cmd_unload_all();
    if (!strcmp(argv[1], "new"))        return cmd_new(argc, argv);

    fprintf(stderr, "unknown cmd: %s\n", argv[1]);
    return 1;
}
