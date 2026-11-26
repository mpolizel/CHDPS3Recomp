/* Save folder name: lock it to the game's real title id.
 *
 * The game names its save folder "<TITLE_ID>__CASTLEVANIA-HOD-SAVE", with the
 * title id it reads from cellGameGetParamString. The runtime answered with its
 * placeholder "BLES00000" because this port never gave it the real one, so
 * saves landed in BLES00000__CASTLEVANIA-HOD-SAVE. A later runtime that reads
 * the id from PARAM.SFO by itself (its own test harness already does) would
 * switch to NPUB30505 and the player's saves would seem to vanish.
 *
 * So the port sets the id itself, NPUB30505 as on a real PS3 (a save copied
 * from a PS3 or RPCS3 has that name too), before the guest runs, and moves any
 * save made under the old name. Nothing is overwritten: if both names exist,
 * both are left as they are and the log says so. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define PORT_TITLE_ID "NPUB30505"
#define OLD_TITLE_ID  "BLES00000"

void cellGame_set_title_id(const char* title_id);   /* libs/system/cellGame.c */

#ifdef _WIN32
/* The save's PARAM.SFO names its own folder (SAVEDATA_DIRECTORY). The two ids
 * have the same length, so the name is swapped in place. */
static void fix_sfo(const char* dir)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s/PARAM.SFO", dir);
    FILE* f = fopen(path, "r+b");
    if (!f) return;
    unsigned char buf[16384];
    size_t n = fread(buf, 1, sizeof buf, f);
    size_t len = strlen(OLD_TITLE_ID "__");
    int changed = 0;
    for (size_t i = 0; i + len <= n; i++) {
        if (memcmp(buf + i, OLD_TITLE_ID "__", len) == 0) {
            memcpy(buf + i, PORT_TITLE_ID, strlen(PORT_TITLE_ID));
            changed = 1;
        }
    }
    if (changed) {
        fseek(f, 0, SEEK_SET);
        fwrite(buf, 1, n, f);
    }
    fclose(f);
}

static void migrate_user(const char* savedata)
{
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s/" OLD_TITLE_ID "__*", savedata);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        char from[MAX_PATH], to[MAX_PATH];
        snprintf(from, sizeof from, "%s/%s", savedata, fd.cFileName);
        snprintf(to, sizeof to, "%s/" PORT_TITLE_ID "%s", savedata,
                 fd.cFileName + strlen(OLD_TITLE_ID));
        if (GetFileAttributesA(to) != INVALID_FILE_ATTRIBUTES) {
            printf("[save] both %s and %s exist; left as they are\n", from, to);
            continue;
        }
        if (MoveFileA(from, to)) {
            fix_sfo(to);
            printf("[save] moved %s -> %s\n", from, to);
        } else {
            printf("[save] could not move %s (error %lu)\n", from, GetLastError());
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void migrate_saves(void)
{
    /* <gamedata>/dev_hdd0/home/<user>/savedata. gamedata is relative to the
     * install root (main.cpp makes it the current directory) unless
     * $PS3_GAMEDATA_ROOT moves it, as in cellSaveData.c. */
    const char* root = getenv("PS3_GAMEDATA_ROOT");
    if (!root || !*root) root = "gamedata";
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s/dev_hdd0/home/*", root);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        char savedata[MAX_PATH];
        snprintf(savedata, sizeof savedata, "%s/dev_hdd0/home/%s/savedata", root, fd.cFileName);
        migrate_user(savedata);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
#endif

void port_save_early(void)
{
    cellGame_set_title_id(PORT_TITLE_ID);
#ifdef _WIN32
    migrate_saves();
#endif
}
