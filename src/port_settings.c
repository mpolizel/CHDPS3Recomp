/*
 * port_settings.c - the player's settings (config.ini), applied to the game.
 *
 * config.ini sits in the install folder; the launcher's Settings category
 * writes it. Read before the renderer starts:
 *   [Display] Resolution=1920x1080|1280x720   the image the game is presented at
 *             VSync=1|0
 * Applied live while the game runs (the file is watched):
 *   [Display] Mode=windowed|fullscreen        (borderless fullscreen)
 *             Aspect=keep|stretch             fullscreen on a screen of another shape:
 *                                             black bars, or the picture stretched
 *             WindowSize=1280x720|1600x900|1920x1080|2560x1440
 *   [Audio]   Volume=0..100                   the game's own Windows audio session
 *   [Interface] ShowFps=0|1                   in the window title
 *               EscQuits=0|1                  Esc closes the game (ps3recomp's default)
 *
 * In the game: F11 or Alt+Enter switches fullscreen, F1 opens the launcher's
 * settings over the game. The game window is subclassed for that, and for
 * its title (the game's name, not ps3recomp's frame statistics).
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define COBJMACROS
#include <windows.h>
#include <intrin.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#ifdef _MSC_VER
#pragma comment(lib, "ole32.lib")
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_FILE ".\\config.ini"
static const char kTitle[] = "Castlevania: Harmony of Despair";

#define WM_PORT_APPLY   (WM_APP + 0x51)   /* re-read config.ini on the window's thread */
#define WM_LAUNCHER_SETTINGS (WM_APP + 2) /* CHDLauncher: show the settings over the game */

typedef struct {
    int fullscreen, stretch, win_w, win_h, volume, show_fps, esc_quits;
} LiveSettings;

static LiveSettings s_cur = {-1, -1, 0, 0, -1, -1, -1};
static unsigned s_img_w = 1280, s_img_h = 720;     /* the presented image, for its shape */
static HWND s_backdrop = NULL;                       /* black behind a fullscreen picture */
static int s_backdrop_wanted = 0;
static HWND s_game = NULL;
static WNDPROC s_orig_proc = NULL;
static RECT s_windowed_rect;
static int s_have_windowed_rect = 0;

static int ini_int(const char* sec, const char* key, int def)
{
    return (int)GetPrivateProfileIntA(sec, key, def, CONFIG_FILE);
}

static void ini_size(const char* sec, const char* key, int dw, int dh, int* w, int* h)
{
    char buf[64];
    GetPrivateProfileStringA(sec, key, "", buf, sizeof buf, CONFIG_FILE);
    int a = 0, b = 0;
    if (sscanf(buf, "%dx%d", &a, &b) == 2 && a >= 640 && b >= 360 && a <= 7680 && b <= 4320) { *w = a; *h = b; }
    else { *w = dw; *h = dh; }
}

static LiveSettings read_live(void)
{
    LiveSettings s;
    char mode[32];
    GetPrivateProfileStringA("Display", "Mode", "windowed", mode, sizeof mode, CONFIG_FILE);
    s.fullscreen = _stricmp(mode, "fullscreen") == 0;
    char aspect[32];
    GetPrivateProfileStringA("Display", "Aspect", "keep", aspect, sizeof aspect, CONFIG_FILE);
    s.stretch = _stricmp(aspect, "stretch") == 0;
    ini_size("Display", "WindowSize", 1280, 720, &s.win_w, &s.win_h);
    s.volume = ini_int("Audio", "Volume", 100);
    if (s.volume < 0) s.volume = 0;
    if (s.volume > 100) s.volume = 100;
    s.show_fps = ini_int("Interface", "ShowFps", 0) != 0;
    s.esc_quits = ini_int("Interface", "EscQuits", 0) != 0;
    return s;
}

/* Called from main() before the renderer starts. */
/* A short summary at the top of run.log: the PC and the settings, the first
 * things a problem report needs. */
typedef LONG (WINAPI *RtlGetVersion_fn)(OSVERSIONINFOW*);

static void log_header(int res_w, int res_h)
{
    char ver[32] = "?";
    FILE* vf = fopen("version.txt", "r");
    if (vf) {
        if (fgets(ver, sizeof ver, vf)) ver[strcspn(ver, "\r\n")] = 0;
        fclose(vf);
    }
    OSVERSIONINFOW os = {sizeof os};
    RtlGetVersion_fn rtl = (RtlGetVersion_fn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (rtl) rtl(&os);
    int cpu[4];
    char brand[49] = {0};
    __cpuid(cpu, 0x80000000);
    if ((unsigned)cpu[0] >= 0x80000004u)
        for (int i = 0; i < 3; i++) { __cpuid(cpu, 0x80000002 + i); memcpy(brand + 16 * i, cpu, 16); }
    const char* b = brand;
    while (*b == ' ') b++;
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    MEMORYSTATUSEX mem = {sizeof mem};
    GlobalMemoryStatusEx(&mem);
    DISPLAY_DEVICEA dd = {sizeof dd};
    char gpu[128] = "?";
    for (DWORD i = 0; EnumDisplayDevicesA(NULL, i, &dd, 0); i++)
        if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) { strncpy(gpu, dd.DeviceString, sizeof gpu - 1); break; }
    const LiveSettings s = read_live();
    fprintf(stderr,
            "=== CHDPS3Recomp %s ===\n"
            "[system] Windows %lu.%lu build %lu; CPU %s (%lu threads); RAM %.1f GB; primary display on %s, %dx%d\n"
            "[system] settings: resolution %dx%d, %s, picture %s, window %dx%d, vsync %s, volume %d, fps %s, esc quits %s\n",
            ver, os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber, *b ? b : "?",
            si.dwNumberOfProcessors, mem.ullTotalPhys / 1073741824.0, gpu,
            GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
            res_w, res_h, s.fullscreen ? "fullscreen" : "windowed", s.stretch ? "stretched" : "kept",
            s.win_w, s.win_h, ini_int("Display", "VSync", 1) ? "on" : "off", s.volume,
            s.show_fps ? "on" : "off", s.esc_quits ? "on" : "off");
}

void port_settings_early(unsigned* out_w, unsigned* out_h)
{
    int w, h;
    /* The game renders 1920x1080 internally; presenting it at that size is
     * the sharp picture, 1280x720 the lighter one. */
    ini_size("Display", "Resolution", 1920, 1080, &w, &h);
    *out_w = (unsigned)w;
    *out_h = (unsigned)h;
    s_img_w = (unsigned)w;
    s_img_h = (unsigned)h;
    if (!ini_int("Display", "VSync", 1) && !getenv("D3D12_VSYNC")) _putenv_s("D3D12_VSYNC", "0");
    /* The settings window is another process: while it is in front, the
     * controller must not drive the game. */
    if (!getenv("PAD_FOCUS_ONLY")) _putenv_s("PAD_FOCUS_ONLY", "1");
    if (!getenv("PS3_TITLE")) _putenv_s("PS3_TITLE", kTitle);
    /* The backend takes a texture the size of the display for "the frame so
     * far" (a guess other titles need). This game samples none such: at 1080p
     * its 1920x1080 title art matched the display and was drawn black. */
    if (!getenv("SCREEN_AS_TEX")) _putenv_s("SCREEN_AS_TEX", "0");
    log_header(w, h);
}

/* ---- audio: the volume of this process's session, as in Windows' mixer ---- */

static const GUID kCLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID kIID_IMMDeviceEnumerator  = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID kIID_IAudioSessionManager = {0xBFA971F1, 0x4D5E, 0x40BB, {0x93, 0x5E, 0x96, 0x70, 0x39, 0xBF, 0xBE, 0xE4}};

static int set_volume(int percent)
{
    int ok = 0;
    IMMDeviceEnumerator* en = NULL;
    IMMDevice* dev = NULL;
    IAudioSessionManager* mgr = NULL;
    ISimpleAudioVolume* vol = NULL;
    if (FAILED(CoCreateInstance(&kCLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &kIID_IMMDeviceEnumerator, (void**)&en)))
        return 0;
    if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev)) &&
        SUCCEEDED(IMMDevice_Activate(dev, &kIID_IAudioSessionManager, CLSCTX_ALL, NULL, (void**)&mgr)) &&
        SUCCEEDED(IAudioSessionManager_GetSimpleAudioVolume(mgr, NULL, FALSE, &vol))) {
        ok = SUCCEEDED(ISimpleAudioVolume_SetMasterVolume(vol, percent / 100.0f, NULL));
    }
    if (vol) ISimpleAudioVolume_Release(vol);
    if (mgr) IAudioSessionManager_Release(mgr);
    if (dev) IMMDevice_Release(dev);
    IMMDeviceEnumerator_Release(en);
    return ok;
}

/* ---- window: borderless fullscreen or a window of the chosen size ---- */

/* Borderless fullscreen on a screen whose shape differs from the picture's:
 * the swap chain can only stretch to its window, so the window keeps the
 * picture's shape, centred, over a black window that covers the screen. */
static HWND backdrop(void)
{
    if (!s_backdrop) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = L"CHDBackdrop";
        RegisterClassW(&wc);
        s_backdrop = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"",
                                     WS_POPUP, 0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);
    }
    return s_backdrop;
}

static void keep_backdrop_below(void)
{
    if (s_backdrop && IsWindowVisible(s_backdrop))
        SetWindowPos(s_backdrop, s_game, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void apply_window(const LiveSettings* s)
{
    HMONITOR mon = MonitorFromWindow(s_game, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(mon, &mi);
    LONG style = GetWindowLongW(s_game, GWL_STYLE);
    if (s->fullscreen) {
        if (!(style & WS_POPUP)) { GetWindowRect(s_game, &s_windowed_rect); s_have_windowed_rect = 1; }
        SetWindowLongW(s_game, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP | WS_VISIBLE);
        int mw = mi.rcMonitor.right - mi.rcMonitor.left, mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
        int w = mw, h = mh;
        if (!s->stretch) {
            if ((long long)mw * s_img_h > (long long)mh * s_img_w) w = (int)((long long)mh * s_img_w / s_img_h);
            else h = (int)((long long)mw * s_img_h / s_img_w);
        }
        int x = mi.rcMonitor.left + (mw - w) / 2, y = mi.rcMonitor.top + (mh - h) / 2;
        SetWindowPos(s_game, HWND_TOP, x, y, w, h, SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        s_backdrop_wanted = (w != mw || h != mh);
        if (s_backdrop_wanted) {
            SetWindowPos(backdrop(), s_game, mi.rcMonitor.left, mi.rcMonitor.top, mw, mh,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else if (s_backdrop) {
            ShowWindow(s_backdrop, SW_HIDE);
        }
    } else {
        s_backdrop_wanted = 0;
        if (s_backdrop) ShowWindow(s_backdrop, SW_HIDE);
        SetWindowLongW(s_game, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        RECT r = {0, 0, s->win_w, s->win_h};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        int ww = r.right - r.left, wh = r.bottom - r.top;
        int aw = mi.rcWork.right - mi.rcWork.left, ah = mi.rcWork.bottom - mi.rcWork.top;
        if (ww > aw || wh > ah) {                  /* too big for this screen: fit it */
            double k = (double)aw / ww < (double)ah / wh ? (double)aw / ww : (double)ah / wh;
            ww = (int)(ww * k); wh = (int)(wh * k);
        }
        int x = mi.rcWork.left + (aw - ww) / 2, y = mi.rcWork.top + (ah - wh) / 2;
        if (s_have_windowed_rect && s_cur.fullscreen == 1 && s_cur.win_w == s->win_w && s_cur.win_h == s->win_h) {
            x = s_windowed_rect.left; y = s_windowed_rect.top;   /* back where it was */
        }
        SetWindowPos(s_game, HWND_NOTOPMOST, x, y, ww, wh, SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    }
}

static void apply(void)
{
    LiveSettings s = read_live();
    if (s.fullscreen != s_cur.fullscreen || (s.fullscreen && s.stretch != s_cur.stretch) ||
        (!s.fullscreen && (s.win_w != s_cur.win_w || s.win_h != s_cur.win_h)))
        apply_window(&s);
    if (s.show_fps != s_cur.show_fps) SetWindowTextA(s_game, kTitle);   /* rewritten below */
    int old_volume = s_cur.volume;
    s_cur = s;
    s_cur.volume = old_volume;   /* set by the watcher, which retries until the session exists */
}

static void toggle_fullscreen(void)
{
    LiveSettings s = read_live();
    WritePrivateProfileStringA("Display", "Mode", s.fullscreen ? "windowed" : "fullscreen", CONFIG_FILE);
    apply();
}

static void open_settings(void)
{
    HWND l = FindWindowW(L"CHDLauncher", NULL);
    if (l) {
        /* The game is in front; Windows lets another process come forward
         * only with its leave. */
        DWORD pid = 0;
        GetWindowThreadProcessId(l, &pid);
        AllowSetForegroundWindow(pid);
        PostMessageW(l, WM_LAUNCHER_SETTINGS, 0, 0);
        return;
    }
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    char cmd[] = "CHDLauncher.exe --settings";
    if (CreateProcessA(".\\CHDLauncher.exe", cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

static LRESULT CALLBACK game_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_SETTEXT: {
        /* ps3recomp writes "<title> | FPS: 59.94 | draws: N | WxH" once a second. */
        const char* in = (const char*)lp;
        char out[160];
        const char* fps = in ? strstr(in, "FPS: ") : NULL;
        if (s_cur.show_fps == 1 && fps)
            snprintf(out, sizeof out, "%s  |  %d FPS", kTitle, (int)(atof(fps + 5) + 0.5));
        else
            snprintf(out, sizeof out, "%s", kTitle);
        return CallWindowProcA(s_orig_proc, h, m, wp, (LPARAM)out);
    }
    case WM_KEYDOWN:
        if (wp == VK_F11) { toggle_fullscreen(); return 0; }
        if (wp == VK_F1) { open_settings(); return 0; }
        if (wp == VK_ESCAPE && s_cur.esc_quits != 1) return 0;   /* no accidental quit */
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) { toggle_fullscreen(); return 0; }   /* Alt+Enter */
        break;
    case WM_PORT_APPLY:
        apply();
        return 0;
    case WM_WINDOWPOSCHANGED:
        keep_backdrop_below();
        break;
    case WM_SIZE:
        if (s_backdrop && s_backdrop_wanted) {      /* follow minimise and restore */
            if (wp == SIZE_MINIMIZED) ShowWindow(s_backdrop, SW_HIDE);
            else if (!IsWindowVisible(s_backdrop)) { ShowWindow(s_backdrop, SW_SHOWNOACTIVATE); keep_backdrop_below(); }
        }
        break;
    case WM_DESTROY:
        if (s_backdrop) DestroyWindow(s_backdrop);
        break;
    }
    return CallWindowProcA(s_orig_proc, h, m, wp, lp);
}

typedef struct { DWORD pid; HWND found; } FindCtx;

static BOOL CALLBACK find_game(HWND h, LPARAM lp)
{
    FindCtx* c = (FindCtx*)lp;
    DWORD pid = 0;
    char cls[64];
    GetWindowThreadProcessId(h, &pid);
    if (pid == c->pid && GetClassNameA(h, cls, sizeof cls) && strcmp(cls, "ps3recomp_d3d12") == 0) {
        c->found = h;
        return FALSE;
    }
    return TRUE;
}

static DWORD WINAPI settings_thread(LPVOID unused)
{
    (void)unused;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    FindCtx fc = {GetCurrentProcessId(), NULL};
    for (int i = 0; i < 600 && !fc.found; ++i) {
        EnumWindows(find_game, (LPARAM)&fc);
        if (!fc.found) Sleep(100);
    }
    if (!fc.found) return 0;
    s_game = fc.found;
    s_orig_proc = (WNDPROC)SetWindowLongPtrA(s_game, GWLP_WNDPROC, (LONG_PTR)game_proc);
    PostMessageA(s_game, WM_PORT_APPLY, 0, 0);

    FILETIME last = {0, 0};
    int volume_applied = -1;
    DWORD volume_retry = 0;
    while (IsWindow(s_game)) {
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExA(CONFIG_FILE, GetFileExInfoStandard, &a) &&
            CompareFileTime(&a.ftLastWriteTime, &last) != 0) {
            last = a.ftLastWriteTime;
            PostMessageA(s_game, WM_PORT_APPLY, 0, 0);
            volume_applied = -1;
        }
        int want = read_live().volume;
        if (want != volume_applied && GetTickCount() >= volume_retry) {
            if (set_volume(want)) {
                volume_applied = want;
                s_cur.volume = want;
                fprintf(stderr, "[settings] volume %d%%%c", want, 10);
            }
            else volume_retry = GetTickCount() + 1000;   /* the audio session may not exist yet */
        }
        Sleep(300);
    }
    CoUninitialize();
    return 0;
}

void port_settings_start(void)
{
    HANDLE t = CreateThread(NULL, 0, settings_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

/* Slowdowns in the log. Called on each present with the game's flip count
 * (the presents themselves can repeat a flip, so they are not counted). The
 * PS3 game runs at 60 frames a second: a second with fewer than 55 flips, or
 * with a gap over 50 ms between two (a hitch), gets one line, so a smooth
 * game writes nothing and a report says when it slowed down and how much.
 * PORT_PERFLOG=1 logs every second. */
void port_perf_frame(unsigned flips, unsigned vblanks)
{
    static LARGE_INTEGER freq, start, win, last;
    static unsigned win_flips, last_flips, win_vbl;
    static double worst;
    static int all = -1;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (all < 0) {
        all = getenv("PORT_PERFLOG") ? 1 : 0;
        QueryPerformanceFrequency(&freq);
        start = win = last = now;
        win_flips = last_flips = flips;
        win_vbl = vblanks;
        return;
    }
    if (flips != last_flips) {
        const double gap = (double)(now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart;
        if (gap > worst) worst = gap;
        last = now;
        last_flips = flips;
    }
    const double span = (double)(now.QuadPart - win.QuadPart) / freq.QuadPart;
    if (span >= 1.0) {
        const double fps = (flips - win_flips) / span;
        extern void port_perf_beat_stats(double*, double*, double*, double*);
        double pr, prm, dr, drm;
        port_perf_beat_stats(&pr, &prm, &dr, &drm);
        if (all || fps < 55.0 || worst > 50.0)
            fprintf(stderr, "[perf] t=%.0fs fps=%.1f worst=%.0fms vblank=%.1f present=%.0fms(max %.0f) drain=%.0fms(max %.0f)\n",
                    (double)(now.QuadPart - start.QuadPart) / freq.QuadPart, fps, worst,
                    (vblanks - win_vbl) / span, pr, prm, dr, drm);
        win = now;
        win_flips = flips;
        win_vbl = vblanks;
        worst = 0.0;
    }
}

#else
void port_perf_frame(unsigned flips, unsigned vblanks) { (void)flips; (void)vblanks; }
void port_settings_early(unsigned* w, unsigned* h) { *w = 1280; *h = 720; }
void port_settings_start(void) {}
#endif
