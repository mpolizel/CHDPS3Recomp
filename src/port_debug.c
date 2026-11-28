/*
 * CHD_DEBUG=<file>: a tiny debug console for a running game.
 *
 * Write one command into <file>; the answer is appended to <file>.out and
 * <file> is truncated, ready for the next one. ps3recomp's PS3_DEBUG console
 * lives in its test harness (boot_main.cpp), not in the template's main.cpp,
 * so a port has none. Commands:
 *
 *   mem <hex ea> [len]     hexdump guest memory (len default 64, max 64 KB)
 *   dump <hex ea> <len> <host path>   write guest memory to a host file
 *
 * Reads only. Guest memory is read through vm_base without locking, so a
 * dump of a region being written can tear; that is fine for inspection.
 */
#ifdef _WIN32
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps3emu/ps3types.h"
#include "ppu_memory.h"

static char s_path[1024];

static void debug_answer(const char* cmd)
{
    char out_path[1100];
    snprintf(out_path, sizeof out_path, "%s.out", s_path);
    FILE* o = fopen(out_path, "a");
    if (!o) return;
    unsigned ea = 0, len = 64;
    char host[600];
    unsigned val = 0;
    if (sscanf(cmd, "poke8 %x %x", &ea, &val) == 2) {
        if (vm_base) vm_base[ea] = (unsigned char)val;
        fprintf(o, "> %s\nwrote 0x%02X at 0x%08X\n", cmd, val & 0xFFu, ea);
    } else if (sscanf(cmd, "dump %x %u %599s", &ea, &len, host) == 3) {
        FILE* f = fopen(host, "wb");
        if (f && vm_base) { fwrite(vm_base + ea, 1, len, f); fclose(f);
                            fprintf(o, "> %s\nwrote %u bytes from 0x%08X to %s\n", cmd, len, ea, host); }
        else fprintf(o, "> %s\ncannot write %s\n", cmd, host);
    } else if (strncmp(cmd, "press ", 6) == 0 || strncmp(cmd, "hold ", 5) == 0) {
        /* press <buttons> [ms]: hold controller buttons on port 0 for ms
         * (default 150), e.g. "press cross", "press down 100", "press right+cross 600".
         * Names: up down left right cross circle square triangle l1 r1 l2 r2 l3 r3 start select. */
        static const struct { const char* n; unsigned b; } names[] = {
            {"select",1u<<0},{"l3",1u<<1},{"r3",1u<<2},{"start",1u<<3},{"up",1u<<4},{"right",1u<<5},
            {"down",1u<<6},{"left",1u<<7},{"l2",1u<<8},{"r2",1u<<9},{"l1",1u<<10},{"r1",1u<<11},
            {"triangle",1u<<12},{"circle",1u<<13},{"cross",1u<<14},{"square",1u<<15}};
        char list[200] = {0};
        unsigned ms = 150, mask = 0;
        sscanf(cmd, "%*s %199s %u", list, &ms);
        for (char* t = strtok(list, "+"); t; t = strtok(NULL, "+"))
            for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
                if (strcmp(t, names[i].n) == 0) mask |= names[i].b;
        extern volatile unsigned int g_pad_inject_buttons;   /* libs/input/cellPad.c */
        if (ms > 10000) ms = 10000;
        g_pad_inject_buttons = mask;
#ifdef _WIN32
        Sleep(ms);
#endif
        g_pad_inject_buttons = 0;
        fprintf(o, "> %s\nheld 0x%04X for %u ms\n", cmd, mask, ms);
    } else if (sscanf(cmd, "trophy %u", &val) == 1) {
        /* Unlock a trophy through ps3recomp's sceNpTrophyUnlockTrophy, as the
         * game does, and pass it on like src/port_achievements.c's hook:
         * tests the achievements path without earning the trophy in play. */
        extern int port_achievements_test_unlock(unsigned id);
        int rc = port_achievements_test_unlock(val);
        fprintf(o, "> %s\nsceNpTrophyUnlockTrophy -> 0x%08X\n", cmd, (unsigned)rc);
    } else if (sscanf(cmd, "mem %x %u", &ea, &len) >= 1) {
        if (len > 0x10000u) len = 0x10000u;
        fprintf(o, "> %s\n", cmd);
        for (unsigned i = 0; vm_base && i < len; i += 16) {
            fprintf(o, "%08X:", ea + i);
            for (unsigned j = 0; j < 16 && i + j < len; j++)
                fprintf(o, "%s%02X", (j % 4) ? "" : " ", vm_base[ea + i + j]);
            fputc('\n', o);
        }
    } else {
        fprintf(o, "> %s\nunknown command (mem <ea> [len] | dump <ea> <len> <path>)\n", cmd);
    }
    fclose(o);
}

#ifdef _WIN32
static DWORD WINAPI debug_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        Sleep(200);
        FILE* f = fopen(s_path, "rb");
        if (!f) continue;
        char line[700] = {0};
        size_t n = fread(line, 1, sizeof line - 1, f);
        fclose(f);
        if (!n) continue;
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        FILE* t = fopen(s_path, "wb"); if (t) fclose(t);   /* truncate: consumed */
        debug_answer(line);
    }
    return 0;
}
#endif

void port_debug_start(void)
{
    const char* p = getenv("CHD_DEBUG");
    if (!p || !*p) return;
    snprintf(s_path, sizeof s_path, "%s", p);
#ifdef _WIN32
    CreateThread(NULL, 0, debug_thread, NULL, 0, NULL);
    fprintf(stderr, "[port_debug] console on %s\n", s_path);
#endif
}
