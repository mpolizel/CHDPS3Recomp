/*
 * port_prx.c - the game's own PRX modules (characters, enemies, shots, DLC).
 *
 * Castlevania: HoD loads its modules at run time with
 * sys_prx_load_module("/dev_bdvd/PS3_GAME/USRDIR/.../dllsoma.ppu.sprx") and
 * then sys_prx_start_module(id, 4, argp, &modres): module_start fills the
 * structure argp points at with the module's entry points. ps3recomp had no
 * handler for these imports, so they were faked as CELL_OK and the game waited
 * forever on a structure nobody filled in.
 *
 * Every module was relocated at its own guest base and lifted offline
 * (tools/build_prx.py -> recompiled_prx/). Loading one here copies its
 * relocated image to that base and registers its lifted functions; starting it
 * calls module_start through the guest-call path. The modules import a dozen
 * sys_libc functions, which are implemented at the bottom of this file.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include "ps3emu/nid.h"
#include "ppu_context.h"
#include "ppu_memory.h"
#include "port_prx_table.h"

extern void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
extern void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
extern int ppu_guest_call_ct_checked(uint32_t code, uint32_t toc,
    uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t* result);

#define PRX_ERROR_UNKNOWN_MODULE 0x8001112Eu   /* CELL_PRX_ERROR_UNKNOWN_MODULE */
#define PRX_ID_BASE              0x23000000u   /* ids look like the kernel's */

static unsigned char s_loaded[1024];

static void read_guest_str(u32 ea, char* out, size_t n)
{
    size_t i = 0;
    if (ea) for (; i + 1 < n; i++) { out[i] = (char)vm_read8(ea + (u32)i); if (!out[i]) break; }
    out[i] = 0;
}

/* "/dev_bdvd/PS3_GAME/USRDIR/dynamic/.../x.ppu.sprx" -> table index, or -1. */
static int find_module(const char* guest_path)
{
    const char* rel = strstr(guest_path, "USRDIR/");
    rel = rel ? rel + 7 : guest_path;
    char want[256];
    snprintf(want, sizeof want, "%s", rel);
    size_t n = strlen(want);
    if (n > 5 && strcmp(want + n - 5, ".sprx") == 0) strcpy(want + n - 5, ".prx");
    for (uint32_t i = 0; i < chd_prx_module_count; i++)
        if (strcmp(chd_prx_modules[i].path, want) == 0) return (int)i;
    return -1;
}

static const chd_prx_module* module_by_id(u32 id)
{
    if (id < PRX_ID_BASE || id - PRX_ID_BASE >= chd_prx_module_count) return NULL;
    return &chd_prx_modules[id - PRX_ID_BASE];
}

static int load_image(const chd_prx_module* m)
{
    const char* dir = getenv("PS3_PRX_DIR");
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir ? dir : "game/prx", m->image_file);
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[port_prx] cannot open %s\n", path); return 0; }
    /* Read into a host buffer, then copy: guest pages are committed on first
     * touch by the runtime's fault handler, which a kernel-mode ReadFile into
     * an untouched page never reaches (fread returned 0, the image stayed zero). */
    uint8_t* buf = (uint8_t*)malloc(m->size);
    if (!buf) { fclose(f); return 0; }
    size_t got = fread(buf, 1, m->size, f);
    fclose(f);
    if (got < m->size) memset(buf + got, 0, m->size - got);
    memcpy(vm_base + m->base, buf, m->size);
    free(buf);
    printf("[port_prx] image %s: read %zu/%u bytes, module_start OPD word = 0x%08X\n",
           path, got, m->size, m->module_start ? vm_read32(m->module_start) : 0);
    for (uint64_t i = 0; i < *m->func_count; i++)
        ppu_register_function(m->funcs[i].addr, (void (*)(ppu_context*))m->funcs[i].func);
    return 1;
}

/* sys_prx_id_t sys_prx_load_module(const char* path, sys_prx_flags_t flags, sys_prx_load_module_option_t* opt) */
static void prx_load_module(ppu_context* ctx)
{
    char path[256];
    read_guest_str((u32)ctx->gpr[3], path, sizeof path);
    const int i = find_module(path);
    if (i < 0) {
        printf("[port_prx] load '%s': not a lifted module\n", path);
        ctx->gpr[3] = PRX_ERROR_UNKNOWN_MODULE;
        return;
    }
    const chd_prx_module* m = &chd_prx_modules[i];
    if (!s_loaded[i]) {
        if (!load_image(m)) { ctx->gpr[3] = PRX_ERROR_UNKNOWN_MODULE; return; }
        s_loaded[i] = 1;
    }
    printf("[port_prx] load %s -> id 0x%X (base 0x%08X, %llu functions)\n",
           m->path, PRX_ID_BASE + i, m->base, (unsigned long long)*m->func_count);
    ctx->gpr[3] = PRX_ID_BASE + (u32)i;
}

/* int sys_prx_start_module(sys_prx_id_t id, size_t args, void* argp, int* modres,
 *                          sys_prx_flags_t flags, sys_prx_start_module_option_t* opt) */
static void prx_start_module(ppu_context* ctx)
{
    const u32 id = (u32)ctx->gpr[3];
    const u64 args = ctx->gpr[4];
    const u32 argp = (u32)ctx->gpr[5], modres = (u32)ctx->gpr[6];
    const chd_prx_module* m = module_by_id(id);
    if (!m) { ctx->gpr[3] = PRX_ERROR_UNKNOWN_MODULE; return; }
    uint64_t res = 0;
    const u32 before0 = argp ? vm_read32(argp) : 0, before4 = argp ? vm_read32(argp + 4) : 0;
    if (m->module_start) {
        const u32 code = vm_read32(m->module_start), toc = vm_read32(m->module_start + 4);
        const int ok = ppu_guest_call_ct_checked(code, toc, args, argp, 0, 0, 0, 0, 0, 0, &res);
        printf("[port_prx] module_start OPD 0x%08X -> code 0x%08X toc 0x%08X, call ok=%d\n",
               m->module_start, code, toc, ok);
    }
    if (modres) vm_write32(modres, (u32)res);
    printf("[port_prx] start %s -> module_start returned %d; argp=0x%08X [%08X %08X] -> [%08X %08X]\n",
           m->path, (int)(int32_t)res, argp, before0, before4,
           argp ? vm_read32(argp) : 0, argp ? vm_read32(argp + 4) : 0);
    ctx->gpr[3] = CELL_OK;
}

static void prx_stop_module(ppu_context* ctx)
{
    const u32 modres = (u32)ctx->gpr[6];
    if (modres) vm_write32(modres, 0);
    ctx->gpr[3] = module_by_id((u32)ctx->gpr[3]) ? CELL_OK : PRX_ERROR_UNKNOWN_MODULE;
}

static void prx_unload_module(ppu_context* ctx)
{
    const u32 id = (u32)ctx->gpr[3];
    if (!module_by_id(id)) { ctx->gpr[3] = PRX_ERROR_UNKNOWN_MODULE; return; }
    s_loaded[id - PRX_ID_BASE] = 0;   /* a reload starts from a fresh image */
    ctx->gpr[3] = CELL_OK;
}

/* The EBOOT registers its own library for modules to import; none of this
 * title's modules import from it. */
static void prx_register_library(ppu_context* ctx) { ctx->gpr[3] = CELL_OK; }

/* ---- sys_libc, as imported by the modules -------------------------------- */

static float read_f32(u32 ea) { u32 v = vm_read32(ea); float f; memcpy(&f, &v, 4); return f; }
static void write_f32(u32 ea, float f) { u32 v; memcpy(&v, &f, 4); vm_write32(ea, v); }

/* Dinkumware classification codes returned by _FD* */
#define FP_ZERO_CODE    0
#define FP_FINITE_CODE (-1)
#define FP_INF_CODE     1
#define FP_NAN_CODE     2

static int classify(float x)
{
    if (isnan(x)) return FP_NAN_CODE;
    if (isinf(x)) return FP_INF_CODE;
    return x == 0.0f ? FP_ZERO_CODE : FP_FINITE_CODE;
}

static void libc_memcpy(ppu_context* ctx)
{
    const u32 d = (u32)ctx->gpr[3], s = (u32)ctx->gpr[4], n = (u32)ctx->gpr[5];
    if (n) memmove(vm_base + d, vm_base + s, n);
    ctx->gpr[3] = d;
}
static void libc_strlen(ppu_context* ctx) { ctx->gpr[3] = strlen((const char*)(vm_base + (u32)ctx->gpr[3])); }
static void libc_strcmp(ppu_context* ctx)
{
    ctx->gpr[3] = (u64)(s64)strcmp((const char*)(vm_base + (u32)ctx->gpr[3]),
                                   (const char*)(vm_base + (u32)ctx->gpr[4]));
}
/* short _FDscale(float* px, long lexp): *px *= 2^lexp */
static void libc_fdscale(ppu_context* ctx)
{
    const u32 px = (u32)ctx->gpr[3];
    float x = read_f32(px);
    const int c = classify(x);
    if (c == FP_FINITE_CODE) { x = ldexpf(x, (int)(s64)ctx->gpr[4]); write_f32(px, x); }
    ctx->gpr[3] = (u64)(s64)classify(x);
}
/* short _FDunscale(short* pex, float* px): *px = mantissa in [0.5,1), *pex = exponent */
static void libc_fdunscale(ppu_context* ctx)
{
    const u32 pex = (u32)ctx->gpr[3], px = (u32)ctx->gpr[4];
    float x = read_f32(px);
    const int c = classify(x);
    int e = 0;
    if (c == FP_FINITE_CODE) { x = frexpf(x, &e); write_f32(px, x); }
    vm_write16(pex, (u16)(s16)e);
    ctx->gpr[3] = (u64)(s64)c;
}
static void libc_geterrno(ppu_context* ctx) { ctx->gpr[3] = 0; }
static void libc_fputs(ppu_context* ctx)
{
    printf("[guest] %s", (const char*)(vm_base + (u32)ctx->gpr[3]));
    ctx->gpr[3] = 0;
}
static void libc_fflush(ppu_context* ctx) { fflush(stdout); ctx->gpr[3] = 0; }
static void libc_abort(ppu_context* ctx)
{
    fprintf(stderr, "[port_prx] guest abort() (lr=0x%08X)\n", (u32)ctx->lr);
    ctx->gpr[3] = 0;
}
static void libc_syslock(ppu_context* ctx) { ctx->gpr[3] = 0; }

void port_prx_register(void)
{
    ps3_hle_register_ctx(ps3_compute_nid("sys_prx_load_module"), "sys_prx_load_module", prx_load_module);
    ps3_hle_register_ctx(ps3_compute_nid("sys_prx_start_module"), "sys_prx_start_module", prx_start_module);
    ps3_hle_register_ctx(ps3_compute_nid("sys_prx_stop_module"), "sys_prx_stop_module", prx_stop_module);
    ps3_hle_register_ctx(ps3_compute_nid("sys_prx_unload_module"), "sys_prx_unload_module", prx_unload_module);
    ps3_hle_register_ctx(ps3_compute_nid("sys_prx_register_library"), "sys_prx_register_library", prx_register_library);

    ps3_hle_register_ctx(0x831D70A5u, "memcpy", libc_memcpy);
    ps3_hle_register_ctx(0x5B162B7Fu, "memmove", libc_memcpy);
    ps3_hle_register_ctx(0x2F45D39Cu, "strlen", libc_strlen);
    ps3_hle_register_ctx(0x3D85D6F8u, "strcmp", libc_strcmp);
    ps3_hle_register_ctx(0xD4912EE3u, "_FDscale", libc_fdscale);
    ps3_hle_register_ctx(0x296BC72Fu, "_FDunscale", libc_fdunscale);
    ps3_hle_register_ctx(0x44115DD0u, "_Geterrno", libc_geterrno);
    ps3_hle_register_ctx(0x4D348427u, "fputs", libc_fputs);
    ps3_hle_register_ctx(0x814D8CB0u, "fflush", libc_fflush);
    ps3_hle_register_ctx(0x8A6830E7u, "abort", libc_abort);
    ps3_hle_register_ctx(0x7028DEA9u, "_Locksyslock", libc_syslock);
    ps3_hle_register_ctx(0x34E7C97Eu, "_Unlocksyslock", libc_syslock);
}
