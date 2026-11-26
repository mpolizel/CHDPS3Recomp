/*
 * CHDLauncher - installs, sets up and starts the Castlevania: Harmony of
 * Despair recomp. Windows only.
 *
 * The menu is inspired by the PS3's: categories in a row, their options in a
 * column, over moving waves. Everything here is drawn by this file (Direct2D
 * and DirectWrite, icons from the Segoe MDL2 Assets font that ships with
 * Windows 10/11); no console, game or Sony asset is involved.
 *
 * It contains no game code, so it can be shipped built. The game itself is
 * built on the player's PC by tools/install.py, which this program runs with
 * its window hidden and whose @step/@progress/@error/@done lines it shows.
 *
 * Keyboard: arrows, Enter/Space, Esc/Backspace. Controller (XInput): D-pad or
 * left stick, A, B. Mouse: click, wheel.
 */
#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ------------------------------------------------------------------ paths

static std::wstring g_root;       // install folder (has tools/install.py)
static std::wstring g_ini;        // launcher.ini, the player's choices

static bool file_exists(const std::wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool dir_exists(const std::wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring parent_dir(const std::wstring& p)
{
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(0, s);
}

static std::wstring base_name(const std::wstring& p)
{
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

static void find_root()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring dir = parent_dir(buf);
    // The release puts CHDLauncher.exe at the root; a development build sits
    // one level down (build-launcher/).
    for (int up = 0; up < 3; ++up) {
        if (file_exists(dir + L"\\tools\\install.py")) { g_root = dir; break; }
        dir = parent_dir(dir);
    }
    if (g_root.empty()) g_root = parent_dir(buf);
    g_ini = g_root + L"\\launcher.ini";
}

static std::wstring ini_get(const wchar_t* key)
{
    wchar_t buf[2048] = {};
    GetPrivateProfileStringW(L"Paths", key, L"", buf, 2048, g_ini.c_str());
    return buf;
}

static void ini_set(const wchar_t* key, const std::wstring& v)
{
    WritePrivateProfileStringW(L"Paths", key, v.c_str(), g_ini.c_str());
}

static std::wstring read_text(const std::wstring& p)
{
    std::wstring out;
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return out;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    for (size_t i = 0; i < n; ++i) out += (wchar_t)(unsigned char)buf[i];
    return out;
}

static std::wstring game_exe() { return g_root + L"\\build-mingw\\CHDPS3Recomp.exe"; }

static bool game_installed()
{
    return file_exists(game_exe()) &&
           read_text(g_root + L"\\game\\install_state.json").find(L"\"build\"") != std::wstring::npos;
}

static std::wstring licence_file()
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_root + L"\\licenses\\*.rap").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    FindClose(h);
    return fd.cFileName;
}

static std::wstring version_text()
{
    std::wstring v = read_text(g_root + L"\\version.txt");
    while (!v.empty() && (v.back() == L'\n' || v.back() == L'\r' || v.back() == L' ')) v.pop_back();
    return v.empty() ? L"" : L"v" + v;
}

// ------------------------------------------------------------- installer

struct InstallState {
    std::mutex m;
    bool running = false, finished = false;
    int exit_code = 0;
    int step = 0, steps = 0;
    std::wstring step_title, error;
    double done = 0, total = 0;
    DWORD step_started = 0;
    std::deque<std::wstring> tail;   // last plain output lines
};
static InstallState g_inst;
static HANDLE g_job = nullptr;       // kills the installer and every tool it started
static HWND g_hwnd = nullptr;
#define WM_APP_GAME_EXITED (WM_APP + 1)

static std::wstring widen(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static void install_line(const std::wstring& line)
{
    std::lock_guard<std::mutex> lk(g_inst.m);
    if (line.rfind(L"@step ", 0) == 0) {
        int n = 0, t = 0; wchar_t id[64] = {}; int consumed = 0;
        if (swscanf(line.c_str() + 6, L"%d %d %63ls %n", &n, &t, id, &consumed) >= 3) {
            g_inst.step = n; g_inst.steps = t;
            g_inst.step_title = line.substr(6 + consumed);
            g_inst.done = g_inst.total = 0;
            g_inst.step_started = GetTickCount();
        }
    } else if (line.rfind(L"@progress ", 0) == 0) {
        double d = 0, t = 0;
        if (swscanf(line.c_str() + 10, L"%lf %lf", &d, &t) == 2) { g_inst.done = d; g_inst.total = t; }
    } else if (line.rfind(L"@error ", 0) == 0) {
        g_inst.error = line.substr(7);
    } else if (line == L"@done") {
    } else if (!line.empty() && line[0] != L'@') {
        g_inst.tail.push_back(line);
        while (g_inst.tail.size() > 5) g_inst.tail.pop_front();
    }
}

static std::wstring quote(const std::wstring& s) { return L"\"" + s + L"\""; }

static bool start_install(const std::wstring& pkg, const std::wstring& rap, std::wstring& why)
{
    std::wstring py = g_root + L"\\python\\python.exe";
    if (!file_exists(py)) py = g_root + L"\\.venv\\Scripts\\python.exe";
    if (!file_exists(py)) { why = L"The bundled Python is missing (python\\python.exe). Download the release again."; return false; }

    std::wstring cmd = quote(py) + L" -u " + quote(g_root + L"\\tools\\install.py");
    if (!pkg.empty()) cmd += L" " + quote(L"--pkg=" + pkg);
    if (!rap.empty()) cmd += L" " + quote(L"--rap=" + rap);

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { why = L"Could not start the installer."; return false; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cl(cmd.begin(), cmd.end());
    cl.push_back(0);
    if (!CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, g_root.c_str(), &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        why = L"Could not start the installer.";
        return false;
    }
    CloseHandle(wr);
    if (!g_job) {
        g_job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    {
        std::lock_guard<std::mutex> lk(g_inst.m);
        g_inst.running = true; g_inst.finished = false; g_inst.exit_code = 0;
        g_inst.step = 0; g_inst.steps = 0; g_inst.step_title = L"Starting";
        g_inst.error.clear(); g_inst.tail.clear(); g_inst.done = g_inst.total = 0;
        g_inst.step_started = GetTickCount();
    }
    std::thread([rd, proc = pi.hProcess]() {
        std::string acc;
        char buf[4096];
        DWORD n;
        while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) {
            acc.append(buf, n);
            size_t nl;
            while ((nl = acc.find('\n')) != std::string::npos) {
                std::string l = acc.substr(0, nl);
                acc.erase(0, nl + 1);
                if (!l.empty() && l.back() == '\r') l.pop_back();
                install_line(widen(l));
            }
        }
        CloseHandle(rd);
        WaitForSingleObject(proc, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(proc, &code);
        CloseHandle(proc);
        std::lock_guard<std::mutex> lk(g_inst.m);
        g_inst.running = false; g_inst.finished = true; g_inst.exit_code = (int)code;
    }).detach();
    return true;
}

static void cancel_install()
{
    if (g_job) TerminateJobObject(g_job, 1);
}

// ------------------------------------------------------------- game

static bool start_game(std::wstring& why)
{
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::wstring exe = game_exe();
    std::vector<wchar_t> cl(exe.begin(), exe.end());
    cl.insert(cl.begin(), L'"'); cl.push_back(L'"'); cl.push_back(0);
    if (!CreateProcessW(exe.c_str(), cl.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        g_root.c_str(), &si, &pi)) {
        why = L"The game could not be started. Try Verify installation.";
        return false;
    }
    CloseHandle(pi.hThread);
    std::thread([proc = pi.hProcess]() {
        WaitForSingleObject(proc, INFINITE);
        CloseHandle(proc);
        PostMessageW(g_hwnd, WM_APP_GAME_EXITED, 0, 0);
    }).detach();
    return true;
}

static std::wstring pick_file(HWND owner, const wchar_t* title, const wchar_t* filter_name, const wchar_t* filter)
{
    std::wstring out;
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d))))
        return out;
    COMDLG_FILTERSPEC spec[2] = {{filter_name, filter}, {L"All files", L"*.*"}};
    d->SetFileTypes(2, spec);
    d->SetTitle(title);
    if (SUCCEEDED(d->Show(owner))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = p; CoTaskMemFree(p); }
            it->Release();
        }
    }
    d->Release();
    return out;
}

// ------------------------------------------------------------- menu model

struct Item {
    std::wstring label;
    wchar_t icon;
    std::function<std::wstring()> desc;
    std::function<void()> act;
};
struct Category {
    std::wstring label;
    wchar_t icon;
    std::vector<Item> items;
    int sel = 0;
    float pos = 0;     // animated selection
};

enum class Mode { Menu, Installing, Message, Confirm, Choice };
static Mode g_mode = Mode::Menu;
static std::vector<Category> g_cats;
static int g_cat = 0;
static float g_cat_pos = 0;
static std::wstring g_msg_title, g_msg_text;
static std::function<void()> g_confirm_yes;
static bool g_hidden_for_game = false;
static bool g_overlay = false;            // opened over the running game (F1 in the game)
static bool g_started_for_settings = false;   // started by the game with --settings
#define WM_APP_SHOW_SETTINGS (WM_APP + 2)    // the game's F1 (src/port_settings.c)

// ------------------------------------------------------------- settings

// config.ini in the install folder; src/port_settings.c applies it to the game.
struct Setting {
    const wchar_t* section;
    const wchar_t* key;
    const wchar_t* label;
    wchar_t icon;
    const wchar_t* help;
    std::vector<std::pair<std::wstring, std::wstring>> values;   // stored value, shown label
    const wchar_t* def;
    bool next_start;          // applies the next time the game starts
};
static std::vector<Setting> g_settings;
static int g_choice_setting = 0, g_choice_sel = 0;

static std::wstring cfg_get(const Setting& s)
{
    wchar_t buf[128] = {};
    GetPrivateProfileStringW(s.section, s.key, s.def, buf, 128, (g_root + L"\\config.ini").c_str());
    return buf;
}

static void cfg_set(const Setting& s, const std::wstring& v)
{
    WritePrivateProfileStringW(s.section, s.key, v.c_str(), (g_root + L"\\config.ini").c_str());
}

static int setting_index(const Setting& s)
{
    std::wstring v = cfg_get(s);
    for (size_t i = 0; i < s.values.size(); ++i)
        if (_wcsicmp(s.values[i].first.c_str(), v.c_str()) == 0) return (int)i;
    for (size_t i = 0; i < s.values.size(); ++i)
        if (s.values[i].first == s.def) return (int)i;
    return 0;
}

static HWND game_window() { return FindWindowW(L"ps3recomp_d3d12", nullptr); }

static void init_settings()
{
    std::vector<std::pair<std::wstring, std::wstring>> volume;
    for (int v = 100; v >= 0; v -= 10) volume.push_back({std::to_wstring(v), std::to_wstring(v) + L"%"});
    g_settings = {
        {L"Display", L"Mode", L"Screen", 0xE7F4, L"F11 or Alt+Enter also switches it in the game.",
         {{L"windowed", L"Windowed"}, {L"fullscreen", L"Fullscreen"}}, L"windowed", false},
        {L"Display", L"Aspect", L"Fullscreen picture", 0xE740, L"For screens that aren't 16:9, like ultrawide or 16:10.",
         {{L"keep", L"Keep the game's shape"}, {L"stretch", L"Stretch to fill the screen"}}, L"keep", false},
        {L"Display", L"WindowSize", L"Window size", 0xE7C4, L"The size of the game's window when it isn't fullscreen.",
         {{L"1280x720", L"1280 x 720"}, {L"1600x900", L"1600 x 900"}, {L"1920x1080", L"1920 x 1080"}, {L"2560x1440", L"2560 x 1440"}},
         L"1280x720", false},
        {L"Audio", L"Volume", L"Volume", 0xE767, L"The game's volume (Windows' mixer shows the same).", volume, L"100", false},
        {L"Display", L"VSync", L"VSync", 0xE9F5, L"Off can raise the frame rate, with tearing. Applies the next time the game starts.",
         {{L"1", L"On"}, {L"0", L"Off"}}, L"1", true},
        {L"Interface", L"ShowFps", L"Show FPS", 0xEC4A, L"The frame rate, in the window's title.",
         {{L"0", L"Off"}, {L"1", L"On"}}, L"0", false},
        {L"Interface", L"EscQuits", L"Esc closes the game", 0xE711, L"Off keeps a stray Esc from closing the game.",
         {{L"0", L"No"}, {L"1", L"Yes"}}, L"0", false},
    };
}

static void open_choice(int si)
{
    g_choice_setting = si;
    g_choice_sel = setting_index(g_settings[si]);
    g_mode = Mode::Choice;
}

static void back_to_game();

static void message(const std::wstring& title, const std::wstring& text)
{
    g_msg_title = title; g_msg_text = text; g_mode = Mode::Message;
}

static void confirm(const std::wstring& title, const std::wstring& text, std::function<void()> yes)
{
    g_msg_title = title; g_msg_text = text; g_confirm_yes = std::move(yes); g_mode = Mode::Confirm;
}

static void goto_category(const wchar_t* label)
{
    for (size_t i = 0; i < g_cats.size(); ++i)
        if (g_cats[i].label == label) g_cat = (int)i;
}

static void do_install()
{
    std::wstring pkg = ini_get(L"Package");
    if (!game_installed() && (pkg.empty() || !file_exists(pkg))) {
        message(L"Choose the game first",
                L"Pick your PSN package of Castlevania: Harmony of Despair (USA, NPUB30505) under Game package.");
        return;
    }
    std::wstring why;
    std::wstring rap = ini_get(L"Licence");
    if (!rap.empty() && !file_exists(rap)) rap.clear();
    if (!start_install(file_exists(pkg) ? pkg : L"", rap, why)) { message(L"Installation", why); return; }
    g_mode = Mode::Installing;
}

static void play()
{
    if (!game_installed()) {
        message(L"Not installed yet",
                L"Install the game first: choose your package under Installation, then Install.");
        goto_category(L"Installation");
        return;
    }
    std::wstring why;
    if (!start_game(why)) { message(L"Play", why); return; }
    g_hidden_for_game = true;
    ShowWindow(g_hwnd, SW_HIDE);
}

static void choose_package()
{
    std::wstring p = pick_file(g_hwnd, L"Choose the game's PSN package (.pkg)", L"PS3 package (*.pkg)", L"*.pkg");
    if (!p.empty()) ini_set(L"Package", p);
}

static void add_licence()
{
    std::wstring p = pick_file(g_hwnd, L"Choose your licence (.rap)", L"PS3 licence (*.rap)", L"*.rap");
    if (p.empty()) return;
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) || a.nFileSizeLow != 16 || a.nFileSizeHigh) {
        message(L"Licence", L"That file is not a valid licence: a .rap file is exactly 16 bytes.");
        return;
    }
    ini_set(L"Licence", p);
    CreateDirectoryW((g_root + L"\\licenses").c_str(), nullptr);
    if (CopyFileW(p.c_str(), (g_root + L"\\licenses\\" + base_name(p)).c_str(), FALSE))
        message(L"Licence added", L"The full version's licence is saved with the game. It takes effect the next time the game starts.");
    else
        message(L"Licence", L"The licence could not be copied into the game's folder.");
}

static void open_path(const std::wstring& p)
{
    ShellExecuteW(g_hwnd, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void build_menu()
{
    g_cats.clear();
    g_cats.push_back({L"Game", 0xE7FC, {
        {L"Play", 0xE768, [] {
            return game_installed() ? std::wstring(L"Start Castlevania: Harmony of Despair")
                                    : std::wstring(L"Install the game first");
        }, play},
        {L"Edition", 0xE946, [] {
            std::wstring l = licence_file();
            return l.empty() ? std::wstring(L"Trial: add your .rap licence under Licences for the full game")
                             : L"Full version (licence " + l + L")";
        }, [] { goto_category(L"Licences"); }},
    }});
    g_cats.push_back({L"Licences", 0xE8D7, {
        {L"Add licence", 0xE710, [] {
            return std::wstring(L"Your .rap file for the full game (UP0101-NPUB30505_00-CVHDZDLCZUNLOCKZ.rap)");
        }, add_licence},
        {L"Installed licences", 0xE8A5, [] {
            std::wstring l = licence_file();
            return l.empty() ? std::wstring(L"None yet") : l;
        }, [] { open_path(g_root + L"\\licenses"); }},
    }});
    g_cats.push_back({L"Installation", 0xE7B8, {
        {L"Game package", 0xE8B7, [] {
            std::wstring p = ini_get(L"Package");
            return p.empty() ? std::wstring(L"Choose your PSN package (.pkg)") : base_name(p);
        }, choose_package},
        {L"Install", 0xE896, [] {
            return game_installed() ? std::wstring(L"Installed. Choose to build it again from your package")
                                    : std::wstring(L"Builds the game on this PC: about 15 to 30 minutes, once");
        }, [] {
            if (game_installed())
                confirm(L"Install again?", L"The game is already installed. Build it again from your package?", do_install);
            else
                do_install();
        }},
        {L"Verify installation", 0xE73E, [] {
            return std::wstring(L"Finishes or repairs an interrupted installation");
        }, do_install},
    }});
    {
        Category set{L"Settings", 0xE713, {}};
        if (g_overlay)
            set.items.push_back({L"Back to game", 0xE768, [] {
                return std::wstring(L"Close the settings and return to the game (Esc / B)");
            }, back_to_game});
        for (size_t i = 0; i < g_settings.size(); ++i) {
            set.items.push_back({g_settings[i].label, g_settings[i].icon, [i] {
                const Setting& s = g_settings[i];
                return s.values[setting_index(s)].second + L"   ·   " + s.help;
            }, [i] { open_choice((int)i); }});
        }
        set.items.push_back({L"Controls", 0xE765, [] {
            return std::wstring(L"Keyboard and controller buttons");
        }, [] {
            message(L"Controls",
                    L"Keyboard: arrows move, Z = Cross, X = Circle, A = Square, S = Triangle, Q/W = L1/R1, "
                    L"1/2 = L2/R2, Enter = Start, Tab = Select, 4 = camera zoom.\n"
                    L"An Xbox controller works as the PS3 pad.\n"
                    L"In the game: F1 settings, F11 or Alt+Enter fullscreen.");
        }});
        g_cats.insert(g_cats.begin() + 1, set);
    }
    g_cats.push_back({L"System", 0xE770, {
        {L"Game folder", 0xE838, [] { return g_root; }, [] { open_path(g_root); }},
        {L"Logs", 0xE8A5, [] { return std::wstring(L"The game's and the installer's logs"); },
         [] { CreateDirectoryW((g_root + L"\\out").c_str(), nullptr); open_path(g_root + L"\\out"); }},
        {L"Support the project", 0xEB51, [] { return std::wstring(L"buymeacoffee.com/mpolizel"); },
         [] { open_path(L"https://buymeacoffee.com/mpolizel"); }},
        {L"About", 0xE946, [] {
            return L"Castlevania: Harmony of Despair PS3 Recomp " + version_text() + L". Windows only.";
        }, [] {
            message(L"About", L"Castlevania: Harmony of Despair PS3 Recomp " + version_text() +
                    L"\nA static recompilation built with ps3recomp. No game data or keys are included: "
                    L"the game is built on your PC from your own copy.\nNot affiliated with Konami or Sony.");
        }},
        {L"Quit", 0xE7E8, [] { return std::wstring(L"Close the launcher"); }, [] { PostMessageW(g_hwnd, WM_CLOSE, 0, 0); }},
    }});
    if (!game_installed()) goto_category(L"Installation");
}

// ------------------------------------------------------------- rendering

static ID2D1Factory* g_d2d = nullptr;
static IDWriteFactory* g_dw = nullptr;
static ID2D1HwndRenderTarget* g_rt = nullptr;
static ID2D1SolidColorBrush* g_brush = nullptr;
static ID2D1LinearGradientBrush* g_bg = nullptr;
static IDWriteTextFormat *f_small, *f_cat, *f_item, *f_item_sel, *f_desc, *f_h1, *f_icon_cat, *f_icon_item, *f_mono, *f_body;
static float g_w = 1280, g_h = 720, g_scale = 1;
static DWORD g_t0;

struct HitBox { D2D1_RECT_F r; int cat, item; };
static std::vector<HitBox> g_hits;

static IDWriteTextFormat* fmt(const wchar_t* family, float size, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL)
{
    IDWriteTextFormat* f = nullptr;
    g_dw->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
    return f;
}

static void create_formats()
{
    f_small    = fmt(L"Segoe UI", 15);
    f_cat      = fmt(L"Segoe UI", 18, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    f_item     = fmt(L"Segoe UI", 21);
    f_item_sel = fmt(L"Segoe UI", 25, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    f_desc     = fmt(L"Segoe UI", 15);
    f_h1       = fmt(L"Segoe UI", 26, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    f_body     = fmt(L"Segoe UI", 17);
    f_icon_cat = fmt(L"Segoe MDL2 Assets", 46);
    f_icon_item= fmt(L"Segoe MDL2 Assets", 30);
    f_mono     = fmt(L"Consolas", 13);
    f_icon_cat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    f_cat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    f_icon_item->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    f_icon_item->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f_item->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f_item_sel->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    for (auto* f : {f_desc, f_body, f_mono, f_small}) f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
}

static void create_target()
{
    if (g_rt) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_d2d->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(g_hwnd, D2D1::SizeU(rc.right, rc.bottom)), &g_rt);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &g_brush);
    D2D1_GRADIENT_STOP stops[3] = {
        {0.0f, D2D1::ColorF(0.22f, 0.05f, 0.10f)},
        {0.55f, D2D1::ColorF(0.10f, 0.03f, 0.07f)},
        {1.0f, D2D1::ColorF(0.03f, 0.015f, 0.03f)},
    };
    ID2D1GradientStopCollection* sc = nullptr;
    g_rt->CreateGradientStopCollection(stops, 3, &sc);
    g_rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, 720)), sc, &g_bg);
    sc->Release();
}

static void release_target()
{
    if (g_bg) { g_bg->Release(); g_bg = nullptr; }
    if (g_brush) { g_brush->Release(); g_brush = nullptr; }
    if (g_rt) { g_rt->Release(); g_rt = nullptr; }
}

static void color(float r, float g, float b, float a = 1) { g_brush->SetColor(D2D1::ColorF(r, g, b, a)); }

static void text(const std::wstring& s, IDWriteTextFormat* f, float x, float y, float w, float h)
{
    g_rt->DrawText(s.c_str(), (UINT32)s.size(), f, D2D1::RectF(x, y, x + w, y + h), g_brush);
}

static void draw_waves(float t)
{
    // Three ribbons of summed sines drifting at different speeds.
    for (int k = 0; k < 3; ++k) {
        ID2D1PathGeometry* g = nullptr;
        g_d2d->CreatePathGeometry(&g);
        ID2D1GeometrySink* s = nullptr;
        g->Open(&s);
        float base = 470 + k * 26.0f;
        auto top = [&](float x) {
            return base + 34 * sinf(x * 0.0042f + t * (0.35f + 0.1f * k) + k * 1.3f)
                        + 16 * sinf(x * 0.0110f - t * (0.22f + 0.07f * k) + k);
        };
        auto bottom = [&](float x) {
            return top(x) + 26 + 18 * sinf(x * 0.0065f + t * 0.3f + k * 2.1f);
        };
        s->BeginFigure(D2D1::Point2F(-10, top(-10)), D2D1_FIGURE_BEGIN_FILLED);
        for (float x = 0; x <= 1300; x += 20) s->AddLine(D2D1::Point2F(x, top(x)));
        for (float x = 1300; x >= -10; x -= 20) s->AddLine(D2D1::Point2F(x, bottom(x)));
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
        s->Close();
        s->Release();
        color(1.0f, 0.72f, 0.80f, 0.045f + 0.02f * k);
        g_rt->FillGeometry(g, g_brush);
        g->Release();

        ID2D1PathGeometry* line = nullptr;
        g_d2d->CreatePathGeometry(&line);
        line->Open(&s);
        s->BeginFigure(D2D1::Point2F(-10, top(-10)), D2D1_FIGURE_BEGIN_HOLLOW);
        for (float x = 0; x <= 1300; x += 20) s->AddLine(D2D1::Point2F(x, top(x)));
        s->EndFigure(D2D1_FIGURE_END_OPEN);
        s->Close();
        s->Release();
        color(1.0f, 0.80f, 0.86f, 0.28f - 0.06f * k);
        g_rt->DrawGeometry(line, g_brush, 1.4f);
        line->Release();
    }
}

static void draw_header()
{
    color(1, 1, 1, 0.75f);
    text(L"Castlevania: Harmony of Despair", f_small, 40, 28, 500, 24);
    color(1, 1, 1, 0.45f);
    text(L"PS3 Recomp " + version_text(), f_small, 40, 48, 500, 24);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t clock[64];
    swprintf(clock, 64, L"%02d/%02d  %02d:%02d", st.wDay, st.wMonth, st.wHour, st.wMinute);
    color(1, 1, 1, 0.8f);
    IDWriteTextFormat* f = f_small;
    f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    text(clock, f, 1280 - 40 - 300, 30, 300, 24);
    f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
}

static void draw_menu(float t)
{
    const float cx = 330, row_y = 170, spacing = 160;
    g_hits.clear();
    for (size_t i = 0; i < g_cats.size(); ++i) {
        float x = cx + ((float)i - g_cat_pos) * spacing;
        float d = fabsf((float)i - g_cat_pos);
        float a = d < 1 ? 1.0f - 0.55f * d : 0.45f / (0.6f + d * 0.4f);
        color(1, 1, 1, a);
        text(std::wstring(1, g_cats[i].icon), f_icon_cat, x - 60, row_y, 120, 64);
        if ((int)i == g_cat) {
            color(1, 1, 1, 0.95f);
            text(g_cats[i].label, f_cat, x - 90, row_y + 66, 180, 28);
        }
        g_hits.push_back({D2D1::RectF(x - 60, row_y, x + 60, row_y + 96), (int)i, -1});
    }

    Category& c = g_cats[g_cat];
    const float item_x = cx - 30, anchor = row_y + 130, step = 62;
    for (size_t i = 0; i < c.items.size(); ++i) {
        float rel = (float)i - c.pos;
        float y = rel >= 0 ? anchor + rel * step + (rel > 0.5f ? 34 : rel * 68)
                           : row_y - 74 + rel * step;   // passed items move above the row
        bool sel = (int)i == c.sel;
        float a = sel ? 1.0f : (rel < 0 ? 0.35f : 0.6f);
        if (sel) {
            color(1, 1, 1, 0.07f + 0.03f * sinf(t * 2.2f));
            g_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(item_x - 22, y - 10, item_x + 780, y + 92), 10, 10), g_brush);
        }
        color(1, 1, 1, a);
        text(std::wstring(1, c.items[i].icon), f_icon_item, item_x - 4, y, 64, 52);
        text(c.items[i].label, sel ? f_item_sel : f_item, item_x + 72, y, 700, 52);
        if (sel) {
            color(1.0f, 0.86f, 0.90f, 0.85f);
            text(c.items[i].desc(), f_desc, item_x + 74, y + 48, 690, 40);
        }
        g_hits.push_back({D2D1::RectF(item_x - 22, y - 10, item_x + 780, y + (sel ? 92 : 52)), g_cat, (int)i});
    }

    color(1, 1, 1, 0.5f);
    text(L"← → ↑ ↓  Move      Enter / A  Select      Esc / B  Back", f_small, 40, 720 - 44, 900, 24);
}

static void panel(float x, float y, float w, float h)
{
    color(0.07f, 0.025f, 0.045f, 1.0f);   // opaque: the menu must not show through
    g_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), 14, 14), g_brush);
    color(1.0f, 0.78f, 0.85f, 0.35f);
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), 14, 14), g_brush, 1.2f);
}

static void bar(float x, float y, float w, float frac)
{
    frac = std::clamp(frac, 0.0f, 1.0f);
    color(1, 1, 1, 0.12f);
    g_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + 10), 5, 5), g_brush);
    color(1.0f, 0.78f, 0.86f, 0.95f);
    if (frac > 0) g_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w * frac, y + 10), 5, 5), g_brush);
}

static std::wstring human(double v, bool bytes)
{
    wchar_t b[64];
    if (bytes) swprintf(b, 64, L"%.0f MB", v / 1048576.0);
    else swprintf(b, 64, L"%.0f", v);
    return b;
}

static void draw_installing()
{
    std::lock_guard<std::mutex> lk(g_inst.m);
    float x = 190, y = 150, w = 900, h = 420;
    panel(x, y, w, h);
    color(1, 1, 1);
    text(L"Installing Castlevania: Harmony of Despair", f_h1, x + 40, y + 34, w - 80, 36);
    wchar_t step[256];
    if (g_inst.steps)
        swprintf(step, 256, L"Step %d of %d: %ls", g_inst.step, g_inst.steps, g_inst.step_title.c_str());
    else
        swprintf(step, 256, L"%ls", g_inst.step_title.c_str());
    color(1, 1, 1, 0.9f);
    text(step, f_body, x + 40, y + 92, w - 80, 26);
    float overall = g_inst.steps ? ((g_inst.step - 1) + (g_inst.total > 0 ? (float)(g_inst.done / g_inst.total) : 0)) / g_inst.steps : 0;
    bar(x + 40, y + 128, w - 80, overall);

    if (g_inst.total > 0) {
        bar(x + 40, y + 170, w - 80, (float)(g_inst.done / g_inst.total));
        bool bytes = g_inst.total > 4 * 1048576.0;
        std::wstring detail = human(g_inst.done, bytes) + L" of " + human(g_inst.total, bytes);
        double el = (GetTickCount() - g_inst.step_started) / 1000.0;
        if (g_inst.done > 0 && el > 20) {
            double left = el * (g_inst.total - g_inst.done) / g_inst.done;
            wchar_t eta[64];
            if (left >= 90) swprintf(eta, 64, L"   about %d minutes left", (int)(left / 60 + 0.5));
            else swprintf(eta, 64, L"   under 2 minutes left");
            detail += eta;
        }
        color(1, 1, 1, 0.7f);
        text(detail, f_small, x + 40, y + 188, w - 80, 22);
    }

    color(1, 1, 1, 0.45f);
    float ly = y + 236;
    for (const auto& l : g_inst.tail) {
        std::wstring s = l.size() > 120 ? l.substr(0, 117) + L"..." : l;
        text(s, f_mono, x + 40, ly, w - 80, 18);
        ly += 19;
    }
    color(1, 1, 1, 0.55f);
    text(L"You can keep using your PC. Esc / B stops the installation; it resumes from the same step next time.",
         f_small, x + 40, y + h - 44, w - 80, 24);
}

static void draw_dialog(bool with_choice)
{
    float x = 290, y = 220, w = 700, h = 280;
    color(0, 0, 0, 0.55f);                  // dim the menu behind the dialog
    g_rt->FillRectangle(D2D1::RectF(-2000, -2000, 4000, 4000), g_brush);
    panel(x, y, w, h);
    color(1, 1, 1);
    text(g_msg_title, f_h1, x + 40, y + 34, w - 80, 36);
    color(1, 1, 1, 0.88f);
    text(g_msg_text, f_body, x + 40, y + 88, w - 80, 130);
    color(1.0f, 0.86f, 0.90f, 0.9f);
    text(with_choice ? L"Enter / A  Yes        Esc / B  No" : L"Enter / A  OK", f_small, x + 40, y + h - 46, w - 80, 24);
}

static void draw_choice()
{
    const Setting& s = g_settings[g_choice_setting];
    float n = (float)s.values.size();
    float row = 40, x = 760, w = 440, h = 96 + n * row;
    float y = std::max(60.0f, 360 - h / 2);
    color(0, 0, 0, 0.35f);
    g_rt->FillRectangle(D2D1::RectF(-2000, -2000, 4000, 4000), g_brush);
    panel(x, y, w, h);
    color(1, 1, 1);
    text(s.label, f_cat, x, y + 22, w, 28);       // f_cat is centred
    int current = setting_index(s);
    for (size_t i = 0; i < s.values.size(); ++i) {
        float ry = y + 66 + i * row;
        if ((int)i == g_choice_sel) {
            color(1, 1, 1, 0.12f);
            g_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x + 16, ry - 4, x + w - 16, ry + row - 6), 8, 8), g_brush);
        }
        color(1, 1, 1, (int)i == g_choice_sel ? 1.0f : 0.7f);
        text(s.values[i].second, f_body, x + 56, ry + 4, w - 80, 28);
        if ((int)i == current) {
            color(1.0f, 0.80f, 0.86f, 0.95f);
            text(std::wstring(1, (wchar_t)0xE73E), f_icon_item, x + 14, ry - 6, 44, 44);
        }
    }
}

static void render()
{
    create_target();
    float t = (GetTickCount() - g_t0) / 1000.0f;
    g_rt->BeginDraw();
    D2D1_SIZE_F sz = g_rt->GetSize();
    g_w = sz.width; g_h = sz.height;
    g_scale = std::min(g_w / 1280.0f, g_h / 720.0f);
    float ox = (g_w - 1280 * g_scale) / 2, oy = (g_h - 720 * g_scale) / 2;
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    g_rt->Clear(D2D1::ColorF(0.03f, 0.015f, 0.03f));
    g_rt->SetTransform(D2D1::Matrix3x2F::Scale(g_w / 1280.0f, g_h / 720.0f));
    g_rt->FillRectangle(D2D1::RectF(0, 0, 1280, 720), g_bg);
    draw_waves(t);
    g_rt->SetTransform(D2D1::Matrix3x2F::Scale(g_scale, g_scale) * D2D1::Matrix3x2F::Translation(ox, oy));

    // ease the animated positions towards their targets
    g_cat_pos += (g_cat - g_cat_pos) * 0.2f;
    for (auto& c : g_cats) c.pos += (c.sel - c.pos) * 0.22f;

    draw_header();
    if (g_mode == Mode::Installing) {
        draw_installing();
    } else {
        draw_menu(t);
        if (g_mode == Mode::Message) draw_dialog(false);
        if (g_mode == Mode::Confirm) draw_dialog(true);
        if (g_mode == Mode::Choice) draw_choice();
    }
    if (g_rt->EndDraw() == D2DERR_RECREATE_TARGET) release_target();
}

// ------------------------------------------------------------- input

enum Key { K_LEFT, K_RIGHT, K_UP, K_DOWN, K_OK, K_BACK };

static void on_key(Key k)
{
    if (g_mode == Mode::Message) { if (k == K_OK || k == K_BACK) g_mode = Mode::Menu; return; }
    if (g_mode == Mode::Confirm) {
        if (k == K_OK) { g_mode = Mode::Menu; auto f = g_confirm_yes; if (f) f(); }
        else if (k == K_BACK) g_mode = Mode::Menu;
        return;
    }
    if (g_mode == Mode::Choice) {
        const Setting& s = g_settings[g_choice_setting];
        if (k == K_UP && g_choice_sel > 0) g_choice_sel--;
        else if (k == K_DOWN && g_choice_sel + 1 < (int)s.values.size()) g_choice_sel++;
        else if (k == K_BACK || k == K_LEFT) g_mode = Mode::Menu;
        else if (k == K_OK) {
            bool changed = g_choice_sel != setting_index(s);
            cfg_set(s, s.values[g_choice_sel].first);     // the game picks it up within a second
            g_mode = Mode::Menu;
            if (changed && s.next_start && game_window())
                message(L"Saved", L"This one applies the next time the game starts.");
        }
        return;
    }
    if (g_mode == Mode::Installing) {
        if (k == K_BACK) confirm(L"Stop installing?", L"The installation stops now. Next time it resumes from the step it was on.",
                                 [] { cancel_install(); });
        return;
    }
    Category& c = g_cats[g_cat];
    switch (k) {
    case K_LEFT:  if (g_cat > 0) g_cat--; break;
    case K_RIGHT: if (g_cat + 1 < (int)g_cats.size()) g_cat++; break;
    case K_UP:    if (c.sel > 0) c.sel--; break;
    case K_DOWN:  if (c.sel + 1 < (int)c.items.size()) c.sel++; break;
    case K_OK:    c.items[c.sel].act(); break;
    case K_BACK:
        if (c.sel > 0) c.sel = 0;
        else if (g_overlay) back_to_game();
        break;
    }
}

// F1 in the game: the settings over it. The launcher may be hidden behind a
// running game (started from Play), or not running at all (then the game
// starts it with --settings, and it closes again on the way back).
static void show_settings_overlay()
{
    g_overlay = true;
    build_menu();
    goto_category(L"Settings");
    g_cats[g_cat].sel = 0;
    g_cat_pos = (float)g_cat;
    g_mode = Mode::Menu;
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    RECT gr, lr;
    HWND game = game_window();
    if (game && GetWindowRect(game, &gr) && GetWindowRect(g_hwnd, &lr)) {   // centred on the game
        int lw = lr.right - lr.left, lh = lr.bottom - lr.top;
        SetWindowPos(g_hwnd, HWND_TOPMOST, (gr.left + gr.right - lw) / 2, std::max((int)gr.top, (int)(gr.top + gr.bottom - lh) / 2),
                     0, 0, SWP_NOSIZE);
    } else {
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);   // above a fullscreen game
    }
    SetForegroundWindow(g_hwnd);
}

static void back_to_game()
{
    g_overlay = false;
    SetWindowPos(g_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    HWND game = game_window();
    if (g_started_for_settings) { PostMessageW(g_hwnd, WM_CLOSE, 0, 0); }
    else if (game) ShowWindow(g_hwnd, g_hidden_for_game ? SW_HIDE : SW_MINIMIZE);
    if (game) SetForegroundWindow(game);
    build_menu();
    goto_category(L"Settings");
}

static void poll_install()
{
    bool finished = false; int code = 0; std::wstring err;
    {
        std::lock_guard<std::mutex> lk(g_inst.m);
        if (g_inst.finished) { finished = true; code = g_inst.exit_code; err = g_inst.error; g_inst.finished = false; }
    }
    if (!finished) return;
    if (g_mode == Mode::Confirm || g_mode == Mode::Installing) g_mode = Mode::Menu;
    if (code == 0) {
        goto_category(L"Game");
        g_cats[g_cat].sel = 0;
        message(L"Ready to play", L"The game is installed. Choose Play to start it.");
    } else {
        static const wchar_t* why[] = {
            nullptr, L"Setting up the game failed.", nullptr,
            L"This package isn't the USA PSN release of Castlevania: Harmony of Despair (NPUB30505).",
            L"A download failed (the PS3 firmware from Sony, or the build tools). Check your internet connection and try again.",
            L"Building the game failed.",
            L"The licence file (.rap) isn't valid.",
        };
        std::wstring m = (code > 0 && code < 7 && why[code]) ? why[code] : L"The installation stopped.";
        if (!err.empty()) m += L"\n" + err;
        m += L"\nThe details are in Logs (install.log).";
        message(L"Installation stopped", m);
    }
}

typedef DWORD(WINAPI* XInputGetStateFn)(DWORD, XINPUT_STATE*);
static XInputGetStateFn g_xget = nullptr;

static void poll_pad()
{
    static WORD prev = 0;
    static DWORD repeat_at = 0;
    if (!g_xget) return;
    XINPUT_STATE st{};
    if (g_xget(0, &st) != ERROR_SUCCESS) { prev = 0; return; }
    WORD b = st.Gamepad.wButtons;
    const SHORT dz = 16000;
    if (st.Gamepad.sThumbLX < -dz) b |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (st.Gamepad.sThumbLX > dz) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if (st.Gamepad.sThumbLY > dz) b |= XINPUT_GAMEPAD_DPAD_UP;
    if (st.Gamepad.sThumbLY < -dz) b |= XINPUT_GAMEPAD_DPAD_DOWN;
    WORD pressed = b & ~prev;
    const WORD dirs = XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN;
    DWORD now = GetTickCount();
    if ((b & dirs) && (b & dirs) == (prev & dirs) && now >= repeat_at) { pressed |= b & dirs; repeat_at = now + 140; }
    else if (pressed & dirs) repeat_at = now + 400;
    if (GetForegroundWindow() == g_hwnd) {
        if (pressed & XINPUT_GAMEPAD_DPAD_LEFT) on_key(K_LEFT);
        if (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) on_key(K_RIGHT);
        if (pressed & XINPUT_GAMEPAD_DPAD_UP) on_key(K_UP);
        if (pressed & XINPUT_GAMEPAD_DPAD_DOWN) on_key(K_DOWN);
        if (pressed & XINPUT_GAMEPAD_A) on_key(K_OK);
        if (pressed & XINPUT_GAMEPAD_B) on_key(K_BACK);
    }
    prev = b;
}

static bool to_design(LPARAM lp, float& x, float& y)
{
    float ox = (g_w - 1280 * g_scale) / 2, oy = (g_h - 720 * g_scale) / 2;
    x = ((short)LOWORD(lp) - ox) / g_scale;
    y = ((short)HIWORD(lp) - oy) / g_scale;
    return true;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_SIZE:
        if (g_rt) g_rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        return 0;
    case WM_KEYDOWN:
        switch (wp) {
        case VK_LEFT: on_key(K_LEFT); break;
        case VK_RIGHT: on_key(K_RIGHT); break;
        case VK_UP: on_key(K_UP); break;
        case VK_DOWN: on_key(K_DOWN); break;
        case VK_RETURN: case VK_SPACE: on_key(K_OK); break;
        case VK_ESCAPE: case VK_BACK: on_key(K_BACK); break;
        }
        return 0;
    case WM_MOUSEWHEEL:
        on_key(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? K_UP : K_DOWN);
        return 0;
    case WM_LBUTTONDOWN: {
        if (g_mode == Mode::Message) { g_mode = Mode::Menu; return 0; }
        if (g_mode != Mode::Menu) return 0;
        float x, y;
        to_design(lp, x, y);
        for (const auto& hb : g_hits) {
            if (x < hb.r.left || x > hb.r.right || y < hb.r.top || y > hb.r.bottom) continue;
            if (hb.item < 0) { g_cat = hb.cat; }
            else if (g_cats[g_cat].sel == hb.item) { on_key(K_OK); }
            else { g_cats[g_cat].sel = hb.item; }
            break;
        }
        return 0;
    }
    case WM_APP_SHOW_SETTINGS:
        show_settings_overlay();
        return 0;
    case WM_APP_GAME_EXITED:
        if (g_overlay) { g_overlay = false; build_menu(); }
        SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        g_hidden_for_game = false;
        ShowWindow(h, SW_SHOW);
        SetForegroundWindow(h);
        return 0;
    case WM_CLOSE:
        if (g_mode == Mode::Installing) {
            confirm(L"Stop installing and quit?", L"The installation stops now. Next time it resumes from the step it was on.",
                    [] { cancel_install(); DestroyWindow(g_hwnd); });
            return 0;
        }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdline, int show)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool want_settings = cmdline && wcsstr(cmdline, L"--settings");
    HANDLE single = CreateMutexW(nullptr, TRUE, L"CHDPS3RecompLauncher");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"CHDLauncher", nullptr);
        if (other && want_settings) PostMessageW(other, WM_APP_SHOW_SETTINGS, 0, 0);
        else if (other) { ShowWindow(other, SW_SHOW); SetForegroundWindow(other); }
        return 0;
    }
    find_root();
    init_settings();
    if (HMODULE x = LoadLibraryW(L"xinput1_4.dll")) g_xget = (XInputGetStateFn)GetProcAddress(x, "XInputGetState");

    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw);
    create_formats();
    build_menu();
    g_cat_pos = (float)g_cat;

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, L"APPICON");
    wc.lpszClassName = L"CHDLauncher";
    RegisterClassW(&wc);

    UINT dpi = GetDpiForSystem();
    int ww = MulDiv(1280, dpi, 96), wh = MulDiv(720, dpi, 96);
    RECT r{0, 0, ww, wh};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    g_hwnd = CreateWindowExW(0, L"CHDLauncher", L"Castlevania: Harmony of Despair - PS3 Recomp",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    if (want_settings) {
        g_started_for_settings = true;
        show_settings_overlay();
    } else {
        ShowWindow(g_hwnd, show);
    }
    g_t0 = GetTickCount();

    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto out;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        poll_install();
        if (!IsWindowVisible(g_hwnd) || IsIconic(g_hwnd)) { Sleep(50); continue; }
        poll_pad();
        render();        // EndDraw waits for the display's refresh
    }
out:
    if (g_job) CloseHandle(g_job);  // kill-on-close: no installer is left running
    release_target();
    CloseHandle(single);
    CoUninitialize();
    return 0;
}
