# Goodmans

![Demo](demo.gif)

Signed WDM driver embedding wasm3. Loads unsigned `.wasm` modules, gives
them nt/hal FFI. Write kernel logic in C/Rust, compile to `wasm32`, load,
iterate. HVCI-compliant.

## Features

- wasm3 in the kernel, 64KB expanded stack, no JIT
- Direct FFI to any nt/hal export by name
- Per-module capability manifest
- Per-slot KMUTEX, per-slot pool budget
- SEH-guarded kernel VA read/write
- InfinityHook trampoline
- Process/image notify callbacks
- Watchdog + trace/log rings
- Qt6 GUI

## Build

Needs VS 2022 + WDK 10.0.26100.0, Qt 6.9, clang wasm32.

```
build.cmd Release
```

## Install

```
cd deploy
gen_cert.cmd
install.cmd
```

Needs `bcdedit /set testsigning on` if not signed.


## IOCTLs

| Code | Purpose |
|------|---------|
| `LOAD_MODULE` | load .wasm, link imports |
| `CALL_EXPORT` | invoke export, up to 8 args |
| `UNLOAD_MODULE` | tear down runtime |
| `LIST_MODULES` / `MODULE_INFO` | enumerate |
| `UNLOAD_ALL` / `FORCE_UNLOAD` | bulk teardown |
| `TAIL_LOG` / `TAIL_TRACE` | poll rings |
| `TRACE_CTL` | toggle trace |
| `READ_GUEST` | read guest linear memory |
| `NOTIFY_STOP` | deregister callbacks |

## Host Imports

| Import | Sig |
|--------|-----|
| `host_dbg_print` | `v(ii)` |
| `host_alloc` / `host_free` | `I(i)` / `v(I)` |
| `host_read_u8/32/64` | `i/I(I)` |
| `host_write_u64` | `v(II)` |
| `host_read_bytes` / `host_write_bytes` | `i(Iii)` |
| `host_current_irql` | `i()` |
| `host_process_id` / `host_thread_id` | `i()` |
| `host_current_process` | `I()` |
| `host_cpuid` | `v(iii)` |
| `host_rdtsc` | `I()` |
| `host_readmsr` / `host_writemsr` | `I(i)` / `i(iI)` |
| `host_phys_read` / `host_phys_write` | `i(Iii)` |
| `host_call` | `I(iiiIIIIIIII)` |
| `host_ih_trampoline` / `host_ih_configure` / `host_ih_quiesce` | |
| `host_notify_enable` / `host_notify_poll` / `host_dispatch_start` / `host_dispatch_stop` | |
| `host_mem_base` / `host_mem_size` | `I()` / `i()` |
| `host_make_unistr` / `host_free_unistr` | |

Guest bindings in `guest_sdk/gvm.h`.

## Capabilities

| Bit | Name | Grants |
|-----|------|--------|
| 0 | `ALLOC` | host_alloc, host_free |
| 1 | `READ_KMEM` | host_read_* |
| 2 | `WRITE_KMEM` | host_write_* |
| 3 | `MSR_READ` | host_readmsr |
| 4 | `MSR_WRITE` | host_writemsr |
| 5 | `PHYSMEM` | host_phys_* |
| 6 | `CPUID_TSC` | host_cpuid, host_rdtsc |
| 7 | `CALLBACKS` | process/image notify |
| 8 | `HOSTCALL` | host_call |
| 9 | `INTROSPECT` | pid/tid/irql/current_process |

## Limits

- no f32/f64 in guests
- wasm memory bounds checks compiled out
- one caller per module
- 16MB ioctl payload
- 32 module slots

## Credits

- wasm3 by Volodymyr Shymanskyy and Steven Massey (MIT). https://github.com/wasm3/wasm3
- InfinityHook by everdox (MIT). https://github.com/everdox/InfinityHook
- Qt 6 (LGPL v3)

## License

MIT.
