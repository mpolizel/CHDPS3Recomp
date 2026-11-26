/*
 * port_licence.c - the full-version licence, and the "Trial Mode" label.
 *
 * The PSN release is the trial; the full game is an upgrade whose licence is
 * a .rap file. At boot the game asks sceNpDrmVerifyUpgradeLicense with the
 * unlock's content ID (UP0101-NPUB30505_00-CVHDZDLCZUNLOCKZ). ps3recomp had
 * no handler, and an unresolved import answers CELL_OK, so every copy ran as
 * the full game. As on the console (and in RPCS3), the licence is there when
 * licenses/<content id>.rap is: the installer and the launcher put the
 * player's .rap in that folder.
 *
 * Without it the game runs as the trial, and a small "Trial Mode" label sits
 * over the bottom-left corner of the game window. It is a separate layered
 * window (click-through, owned by the game window so it stays above it and
 * minimises with it), which keeps the renderer untouched.
 */
#include <stdio.h>
#include <string.h>

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include "ppu_context.h"
#include "ppu_memory.h"

extern void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));

#define NID_sceNpDrmVerifyUpgradeLicense   0x2ECD48EDu
#define NID_sceNpDrmVerifyUpgradeLicense2  0xBE0E3EE2u
#define SCE_NP_DRM_ERROR_LICENSE_NOT_FOUND 0x80029521u

static void trial_label_start(void);

static int licence_present(const char* content_id)
{
    char path[256];
    if (!content_id || !*content_id || strlen(content_id) > 64) return 0;
    for (const char* p = content_id; *p; ++p)          /* a file name, nothing else */
        if (*p == '/' || *p == '\\' || *p == ':' || *p == '.') return 0;
    snprintf(path, sizeof path, "licenses/%s.rap", content_id);
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n == 16;
}

static void verify_upgrade_license(ppu_context* ctx)
{
    char cid[72] = {0};
    u32 p = (u32)ctx->gpr[3];
    if (p) {
        const char* s = (const char*)(vm_base + p);
        for (int i = 0; i < 70 && s[i]; ++i) cid[i] = s[i];
    }
    int ok = licence_present(cid);
    static int logged = 0;
    if (!logged++)
        fprintf(stderr, "[licence] %s: %s\n", cid[0] ? cid : "(no content id)",
                ok ? "licence found, full version" : "no licence, trial");
    if (!ok) trial_label_start();
    ctx->gpr[3] = ok ? CELL_OK : (s32)SCE_NP_DRM_ERROR_LICENSE_NOT_FOUND;
}

void port_licence_register(void)
{
    ps3_hle_register_ctx(NID_sceNpDrmVerifyUpgradeLicense, "sceNpDrmVerifyUpgradeLicense", verify_upgrade_license);
    ps3_hle_register_ctx(NID_sceNpDrmVerifyUpgradeLicense2, "sceNpDrmVerifyUpgradeLicense2", verify_upgrade_license);
}

/* ------------------------------------------------------------------ label */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

static const wchar_t kLabel[] = L"Trial Mode";

typedef struct { DWORD pid; HWND found; HWND self; } FindCtx;

static BOOL CALLBACK find_game_window(HWND h, LPARAM lp)
{
    FindCtx* c = (FindCtx*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == c->pid && h != c->self && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == NULL) {
        c->found = h;
        return FALSE;
    }
    return TRUE;
}

/* Premultiplied 32-bit bitmap of the label at the given text height: white
 * text over a translucent dark rounded plate. GDI draws the text white on
 * black, and its brightness becomes the coverage. */
static HBITMAP render_label(HDC screen, int text_h, int* out_w, int* out_h)
{
    HFONT font = CreateFontW(-text_h, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HDC dc = CreateCompatibleDC(screen);
    HGDIOBJ oldf = SelectObject(dc, font);
    SIZE sz;
    GetTextExtentPoint32W(dc, kLabel, (int)wcslen(kLabel), &sz);
    int pad_x = text_h * 7 / 10, pad_y = text_h * 3 / 10;
    int w = sz.cx + 2 * pad_x, h = sz.cy + 2 * pad_y;

    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = NULL;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ oldb = SelectObject(dc, bmp);
    memset(bits, 0, (size_t)w * h * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    TextOutW(dc, pad_x, pad_y, kLabel, (int)wcslen(kLabel));
    GdiFlush();

    int r = h / 2;                      /* plate corner radius */
    unsigned char* px = (unsigned char*)bits;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            unsigned char* q = px + ((size_t)y * w + x) * 4;
            int cov = q[0] > q[1] ? (q[0] > q[2] ? q[0] : q[2]) : (q[1] > q[2] ? q[1] : q[2]);
            /* rounded plate: distance to the nearest corner centre */
            int cx = x < r ? r : (x >= w - r ? w - r - 1 : x);
            int cy = y < r ? r : (y >= h - r ? h - r - 1 : y);
            int dx = x - cx, dy = y - cy;
            int plate = (dx * dx + dy * dy <= r * r) ? 120 : 0;   /* plate opacity */
            int under = plate * (255 - cov) / 255;                 /* plate showing under the text */
            /* BGRA, premultiplied: white text over a dark crimson plate (R 50, G 10, B 30). */
            q[0] = (unsigned char)(cov + 30 * under / 255);
            q[1] = (unsigned char)(cov + 10 * under / 255);
            q[2] = (unsigned char)(cov + 50 * under / 255);
            q[3] = (unsigned char)(cov + under);
        }
    }
    SelectObject(dc, oldb);
    SelectObject(dc, oldf);
    DeleteObject(font);
    DeleteDC(dc);
    *out_w = w;
    *out_h = h;
    return bmp;
}

static DWORD WINAPI label_thread(LPVOID unused)
{
    (void)unused;
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"CHDTrialLabel";
    RegisterClassW(&wc);

    FindCtx fc = {GetCurrentProcessId(), NULL, NULL};
    for (int i = 0; i < 600 && !fc.found; ++i) {      /* the game window may not exist yet */
        EnumWindows(find_game_window, (LPARAM)&fc);
        if (!fc.found) Sleep(100);
    }
    if (!fc.found) return 0;
    HWND game = fc.found;
    HWND label = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                 wc.lpszClassName, kLabel, WS_POPUP, 0, 0, 1, 1, game, NULL, wc.hInstance, NULL);
    if (!label) return 0;

    HDC screen = GetDC(NULL);
    HBITMAP bmp = NULL;
    int bw = 0, bh = 0, made_for = 0, shown = 0;
    POINT last = {-1, -1};
    while (IsWindow(game)) {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        RECT rc;
        if (IsIconic(game) || !IsWindowVisible(game) || !GetClientRect(game, &rc) || rc.bottom < 120) {
            if (shown) { ShowWindow(label, SW_HIDE); shown = 0; }
            Sleep(150);
            continue;
        }
        int text_h = rc.bottom / 34;
        if (text_h < 13) text_h = 13;
        if (!bmp || text_h != made_for) {
            if (bmp) DeleteObject(bmp);
            bmp = render_label(screen, text_h, &bw, &bh);
            made_for = text_h;
            last.x = -1;
        }
        POINT pos = {text_h, rc.bottom - bh - text_h};
        ClientToScreen(game, &pos);
        if (pos.x != last.x || pos.y != last.y || !shown) {
            HDC mem = CreateCompatibleDC(screen);
            HGDIOBJ old = SelectObject(mem, bmp);
            SIZE size = {bw, bh};
            POINT src = {0, 0};
            BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            UpdateLayeredWindow(label, screen, &pos, &size, mem, &src, 0, &bf, ULW_ALPHA);
            SelectObject(mem, old);
            DeleteDC(mem);
            if (!shown) { ShowWindow(label, SW_SHOWNOACTIVATE); shown = 1; }
            last = pos;
        }
        Sleep(50);
    }
    if (bmp) DeleteObject(bmp);
    ReleaseDC(NULL, screen);
    DestroyWindow(label);
    return 0;
}

static void trial_label_start(void)
{
    static LONG started = 0;
    if (InterlockedExchange(&started, 1)) return;
    HANDLE t = CreateThread(NULL, 0, label_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}
#else
static void trial_label_start(void) {}
#endif
