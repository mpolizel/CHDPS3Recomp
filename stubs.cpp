/*
 * ps3recomp game project -- game-specific hooks and overrides.
 *
 * main.cpp is the runner and is game-agnostic. This file is where a port puts
 * everything that is not: the PRX loading hook it must answer, overrides for
 * firmware functions the HLE library gets wrong for this title, and patches
 * over recompiled code.
 *
 * It starts out as the empty version of all of that, which is what a game with
 * no lifted PRX and no overrides needs.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

/* ---------------------------------------------------------------------------
 * PRX loading
 *
 * main.cpp calls this once, after the lifted function table is registered and
 * vm_base is live, before the game runs. A title that lifts a real system PRX
 * -- libsre, which is cellSpurs and cellSync -- loads its image into guest RAM
 * here and registers its exports. Most ports never need to: the HLE library
 * answers those modules instead.
 * -----------------------------------------------------------------------*/

extern "C" void port_resc_register(void);   /* src/port_resc.c */
extern "C" void port_prx_register(void);    /* src/port_prx.c */
extern "C" void port_debug_start(void);     /* src/port_debug.c */
extern "C" void port_licence_register(void); /* src/port_licence.c */
extern "C" void port_settings_start(void);   /* src/port_settings.c */
extern "C" void port_achievements_start(void); /* src/port_achievements.c */
extern "C" void rsx_d3d12_alias_texture_to_rt(uint32_t tex_off, uint32_t rt_off);

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

static void set_env_default(const char* name, const char* value)
{
    if (getenv(name)) return;
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 0);
#endif
}

static bool file_exists(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/* /dev_flash is the PS3 firmware, which the user installs through RPCS3
 * (File > Install Firmware). Unless PS3_DEV_FLASH says otherwise, look for an
 * installed copy next to the executable, one level up from it (build/), and
 * under the working directory, in RPCS3's folder or on its own. */
static void find_dev_flash(void)
{
    if (getenv("PS3_DEV_FLASH")) return;

    char exe_dir[1024] = ".";
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, exe_dir, sizeof exe_dir);
    if (n == 0 || n >= sizeof exe_dir) strcpy(exe_dir, ".");
    for (char* p = exe_dir; *p; p++) if (*p == '\\') *p = '/';
    if (char* slash = strrchr(exe_dir, '/')) *slash = 0;
#endif
    const char* bases[] = { exe_dir, nullptr, "." };
    char parent[1024];
    snprintf(parent, sizeof parent, "%s/..", exe_dir);
    bases[1] = parent;
    const char* subdirs[] = { "tools/rpcs3/dev_flash", "rpcs3/dev_flash", "dev_flash" };

    for (const char* base : bases)
        for (const char* sub : subdirs) {
            char root[1200], probe[1300];
            snprintf(root, sizeof root, "%s/%s", base, sub);
            snprintf(probe, sizeof probe, "%s/sys/external/flashATRAC.pic", root);
            if (file_exists(probe)) {
                set_env_default("PS3_DEV_FLASH", root);
                fprintf(stderr, "[port] PS3 firmware found: %s\n", root);
                return;
            }
        }
    fprintf(stderr, "[port] PS3 firmware not found: there will be no sound. Install it with "
                    "RPCS3 (File > Install Firmware) in tools/rpcs3, or set PS3_DEV_FLASH "
                    "to its dev_flash folder.\n");
}

extern "C" void ps3_load_prx_modules(void)
{
    /* Runs after the HLE tables are registered and before the game starts,
     * so context handlers registered here take precedence. */
    port_resc_register();
    port_prx_register();
    port_debug_start();
    port_licence_register();
    port_settings_start();
    port_achievements_start();

    /* The menus render their UI layer into the RT at 0x02AF9800, then the
     * game's own code copies it into one of two buffers and the final pass
     * samples that copy. The RT only exists on the host GPU, so the copy moved
     * zeros: sample the RT. 0x0443C000 is used on the title/main menu,
     * 0x03C53000 from the Local Play lobby on. */
    rsx_d3d12_alias_texture_to_rt(0x0443C000u, 0x02AF9800u);
    rsx_d3d12_alias_texture_to_rt(0x03C53000u, 0x02AF9800u);

    /* The game keeps its command-buffer write pointer in a register across
     * cellRescSetConvertAndFlip, so the runtime must not rewind its context
     * to `begin` when it queues the flip near the end of the ring (see
     * gcm_flip_into_fifo). Read lazily, at the first flip. */
    set_env_default("PS3RECOMP_FLIP_NO_WRAP", "1");

    /* Music is ATRAC3, decoded on the SPU by the firmware's flashATRAC.pic,
     * which MultiStream reads from /dev_flash and relocates into local store
     * at run time. No lift can exist for it, so the interpreter runs it. */
    set_env_default("SPU_INTERP_UNLIFTED", "1");
    find_dev_flash();
}

/* ---------------------------------------------------------------------------
 * Example: Override a specific NID
 *
 * The module system resolves function imports by NID.  To replace a default
 * HLE stub with custom logic, register your function at init time.
 * -----------------------------------------------------------------------*/

/*
 * Example: override cellGcmSetFlipMode (NID 0xA53D12AE) to force vsync.
 *
 *   static void my_cellGcmSetFlipMode(u32 mode)
 *   {
 *       printf("[hook] cellGcmSetFlipMode: forcing VSYNC (was %u)\n", mode);
 *       // Force vsync regardless of what the game requested
 *       cellGcmSetFlipMode(CELL_GCM_DISPLAY_VSYNC);
 *   }
 *
 * Then in your init code:
 *
 *   ps3::modules::override_nid(0xA53D12AE,
 *       reinterpret_cast<void*>(my_cellGcmSetFlipMode));
 */

/* ---------------------------------------------------------------------------
 * Example: Hook into module loading
 *
 * You can intercept cellSysmoduleLoadModule to perform custom actions
 * when a particular module is loaded.
 * -----------------------------------------------------------------------*/

/*
 *   static s32 my_loadmodule_hook(u16 id)
 *   {
 *       printf("[hook] Module 0x%04X loaded\n", id);
 *
 *       if (id == CELL_SYSMODULE_AUDIO) {
 *           // Perform custom audio backend initialization
 *           init_my_audio_backend();
 *       }
 *
 *       // Call the original implementation
 *       return cellSysmoduleLoadModule(id);
 *   }
 */

/* ---------------------------------------------------------------------------
 * Game-specific patches
 *
 * Sometimes you need to patch recompiled code at specific addresses.
 * Use the patch system to modify behavior without editing generated code.
 * -----------------------------------------------------------------------*/

/*
 *   // Skip an integrity check at guest address 0x00812FA0
 *   ps3::patches::nop_range(0x00812FA0, 0x00812FB0);
 *
 *   // Replace a function call with a direct return
 *   ps3::patches::force_return(0x009A1000, 0);  // return 0
 */
