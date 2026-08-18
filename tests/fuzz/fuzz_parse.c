/* fuzz_parse.c - libfuzzer target for m3_ParseModule.
 *
 * exercises the wasm3 parser with attacker-controlled bytes.
 * kernel driver embeds the same parser; parser bugs here = potential
 * bugcheck when someone loads a crafted .wasm via IOCTL_GVM_LOAD_MODULE.
 *
 * build (clang):
 *   clang -O2 -g -fsanitize=fuzzer,address                                  \
 *         -DM3_IMPLEMENT_ERROR_STRINGS                                       \
 *         -I../../driver/wasm3                                               \
 *         fuzz_parse.c ../../driver/wasm3/m3_*.c                             \
 *         -o fuzz_parse.exe
 *
 * run:
 *   fuzz_parse.exe corpus/ -max_len=65536
 *
 * seed corpus:
 *   mkdir corpus && cp ../../sample_guest/sample_guest.wasm corpus/
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "wasm3.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 8 || size > (16u * 1024u * 1024u)) return 0;

    IM3Environment env = m3_NewEnvironment();
    if (!env) return 0;

    IM3Runtime rt = m3_NewRuntime(env, 64 * 1024, NULL);
    if (!rt) { m3_FreeEnvironment(env); return 0; }

    IM3Module mod = NULL;
    M3Result r = m3_ParseModule(env, &mod, data, (uint32_t)size);
    if (!r && mod) {
        m3_LoadModule(rt, mod);
        // no m3_Call here. parse+load exercises the surface we care about.
        // if wasm3 signals load failure, module is not owned by us to free.
    }

    m3_FreeRuntime(rt);
    m3_FreeEnvironment(env);
    return 0;
}
