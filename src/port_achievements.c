/*
 * port_achievements.c - Recomp Achievements (RecompLauncher) from the game's
 * own PS3 trophies.
 *
 * Castlevania: Harmony of Despair unlocks its trophies through
 * sceNpTrophyUnlockTrophy, which ps3recomp implements (and saves to
 * gamedata/trophies/NPWR01823.json). This passes each new unlock on to
 * RecompLauncher as a native achievement, with the trophy id. The SDK
 * (ext/rl-achievements, free for games under the Recomp Achievements
 * License) talks to the launcher over the pipe RecompLauncher names in
 * RL_SESSION; started any other way, every call here does nothing.
 *
 * At start it also sends:
 *   - the trophy list (id, name, description) read from the player's own
 *     TROPHY.TRP, so the launcher has names without a list of its own;
 *   - the trophies already unlocked in earlier sessions ("earlier": kept, but
 *     never Ranked).
 * And it reports each save load (cellSaveDataAutoLoad2), which the launcher's
 * Ranked rules use.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include "ppu_context.h"
#include "ppu_memory.h"
#include "rl_achievements.h"

extern void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));

/* ps3recomp's implementations, called by the wrappers below. Pointer
 * arguments are guest addresses, as the HLE dispatcher passes them. */
extern s32 sceNpTrophyUnlockTrophy(s32 context, s32 handle, s32 trophy_id, void* platinum_id);
extern s32 cellSaveDataAutoLoad2(u32 version, const char* dir_name, u32 err_dialog, void* set_buf,
                                 void* func_stat, void* func_file, u32 container, void* userdata);

#define NID_sceNpTrophyUnlockTrophy 0x8CEEDD21u
#define NID_cellSaveDataAutoLoad2   0xFBD5C856u

#define TROPHY_FILE "game/vfs/PS3_GAME/TROPDIR/NPWR01823_00/TROPHY.TRP"
#define UNLOCKS_FILE "gamedata/trophies/NPWR01823.json"

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

static char* read_file(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (len > 0 && len < (64 << 20)) ? (char*)malloc((size_t)len + 1) : NULL;
    if (buf && fread(buf, 1, (size_t)len, f) == (size_t)len) {
        buf[len] = 0;
        if (n) *n = (size_t)len;
    } else {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

/* One entry of a TRP package (header 0x40, then 0x40-byte entries:
 * name[32], offset, size). */
static char* trp_entry(const char* trp, size_t len, const char* want, size_t* out_len)
{
    const uint8_t* d = (const uint8_t*)trp;
    if (len < 0x40 || be32(d) != 0xDCA24D00u) return NULL;
    uint32_t count = be32(d + 0x10), esize = be32(d + 0x14);
    for (uint32_t i = 0; i < count && 0x40 + (size_t)(i + 1) * esize <= len; ++i) {
        const uint8_t* e = d + 0x40 + (size_t)i * esize;
        if (strncmp((const char*)e, want, 32) != 0) continue;
        uint64_t off = be64(e + 32), sz = be64(e + 40);
        if (off + sz > len) return NULL;
        char* out = (char*)malloc((size_t)sz + 1);
        if (!out) return NULL;
        memcpy(out, trp + off, (size_t)sz);
        out[sz] = 0;
        *out_len = (size_t)sz;
        return out;
    }
    return NULL;
}

/* The text between <tag> and </tag> after p, XML entities decoded. */
static int xml_text(const char* p, const char* end, const char* tag, char* out, size_t cap)
{
    char open[32], close[32];
    snprintf(open, sizeof open, "<%s>", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    const char* a = strstr(p, open);
    if (!a || a >= end) return 0;
    a += strlen(open);
    const char* b = strstr(a, close);
    if (!b || b > end) return 0;
    size_t o = 0;
    while (a < b && o + 1 < cap) {
        static const struct { const char* ent; char ch; } ents[] = {
            {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
        int done = 0;
        for (size_t k = 0; k < sizeof ents / sizeof ents[0]; ++k) {
            size_t n = strlen(ents[k].ent);
            if ((size_t)(b - a) >= n && strncmp(a, ents[k].ent, n) == 0) {
                out[o++] = ents[k].ch; a += n; done = 1; break;
            }
        }
        if (!done) out[o++] = *a++;
    }
    out[o] = 0;
    return 1;
}

/* Trophy grade to a gamerscore the launcher's scale maps to its points
 * (up to 10 -> 5, up to 30 -> 10, up to 75 -> 25): bronze 5, silver 10,
 * gold 25. The platinum is left out, as the spec asks (this game has none). */
static uint32_t grade_score(char ttype)
{
    switch (ttype) {
    case 'B': return 10;
    case 'S': return 30;
    case 'G': return 75;
    default:  return 0;
    }
}

static int define_trophies(void)
{
    size_t trp_len = 0, sfm_len = 0;
    char* trp = read_file(TROPHY_FILE, &trp_len);
    if (!trp) return 0;
    char* sfm = trp_entry(trp, trp_len, "TROP.SFM", &sfm_len);
    free(trp);
    if (!sfm) return 0;
    int n = 0;
    for (const char* p = strstr(sfm, "<trophy "); p; p = strstr(p + 1, "<trophy ")) {
        const char* end = strstr(p, "</trophy>");
        if (!end) break;
        int id = -1;
        char ttype = 0;
        const char* q = strstr(p, "id=\"");
        if (q && q < end) id = atoi(q + 4);
        q = strstr(p, "ttype=\"");
        if (q && q < end) ttype = q[7];
        char name[256], detail[512];
        if (id < 0 || ttype == 'P' || !xml_text(p, end, "name", name, sizeof name)) continue;
        if (!xml_text(p, end, "detail", detail, sizeof detail)) detail[0] = 0;
        rl_ach_native_define((uint32_t)id, name, detail, grade_score(ttype));
        ++n;
    }
    free(sfm);
    return n;
}

/* Trophies unlocked before this session, from ps3recomp's own save. */
static void send_earlier(void)
{
    char* j = read_file(UNLOCKS_FILE, NULL);
    if (!j) return;
    for (const char* p = strstr(j, "\"id\":"); p; p = strstr(p + 1, "\"id\":"))
        rl_ach_native_earlier((uint32_t)atoi(p + 5));
    free(j);
}

static void unlock_trophy(ppu_context* ctx)
{
    s32 id = (s32)ctx->gpr[5];
    s32 rc = sceNpTrophyUnlockTrophy((s32)ctx->gpr[3], (s32)ctx->gpr[4], id,
                                     (void*)(uintptr_t)(u32)ctx->gpr[6]);
    if (rc == CELL_OK) {
        rl_ach_native((uint32_t)id);
        fprintf(stderr, "[achievements] trophy %d unlocked%s%c", id,
                rl_ach_connected() ? ", sent to RecompLauncher" : "", 10);
    }
    ctx->gpr[3] = (u64)(s64)rc;
}

static void save_auto_load2(ppu_context* ctx)
{
    s32 rc = cellSaveDataAutoLoad2((u32)ctx->gpr[3], (const char*)(uintptr_t)(u32)ctx->gpr[4],
                                   (u32)ctx->gpr[5], (void*)(uintptr_t)(u32)ctx->gpr[6],
                                   (void*)(uintptr_t)(u32)ctx->gpr[7], (void*)(uintptr_t)(u32)ctx->gpr[8],
                                   (u32)ctx->gpr[9], (void*)(uintptr_t)(u32)ctx->gpr[10]);
    if (rc == CELL_OK) rl_ach_save_loaded();
    ctx->gpr[3] = (u64)(s64)rc;
}

void port_achievements_start(void)
{
    char version[32] = "";
    char* v = read_file("version.txt", NULL);
    if (v) {
        size_t i = 0;
        for (; v[i] && v[i] != '\r' && v[i] != '\n' && i + 1 < sizeof version; ++i) version[i] = v[i];
        version[i] = 0;
        free(v);
    }
    rl_ach_set_info("", version);
    int n = define_trophies();
    int connected = rl_ach_init("ps3", NULL, NULL);
    if (connected) send_earlier();
    ps3_hle_register_ctx(NID_sceNpTrophyUnlockTrophy, "sceNpTrophyUnlockTrophy", unlock_trophy);
    ps3_hle_register_ctx(NID_cellSaveDataAutoLoad2, "cellSaveDataAutoLoad2", save_auto_load2);
    fprintf(stderr, "[achievements] %d trophies; %s%c", n,
            connected ? "connected to RecompLauncher" : "not started from RecompLauncher (off)", 10);
}

/* For the CHD_DEBUG console's "trophy <id>": the game's unlock path, from
 * the host. ps3recomp's trophy context 1 / handle 1 is the game's. */
int port_achievements_test_unlock(unsigned id)
{
    s32 rc = sceNpTrophyUnlockTrophy(1, 1, (s32)id, NULL);
    if (rc == CELL_OK) {
        rl_ach_native(id);
        fprintf(stderr, "[achievements] trophy %u unlocked (debug console)%s%c", id,
                rl_ach_connected() ? ", sent to RecompLauncher" : "", 10);
    }
    return rc;
}

/* Once per presented frame (main.cpp): the launcher's messages, and the
 * frame count the Ranked rules use. */
void port_achievements_frame(void)
{
    rl_ach_frame();
}
