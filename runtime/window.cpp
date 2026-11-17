// Game window (Windows). Until the renderer exists it shows what the game is doing:
// frame rate, draw calls and controller state, so you can see the game running.
#include "guest.h"
#include "render/renderer.h"
#ifdef _WIN32
#include <windows.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <string>
#include <cstdlib>

uint64_t GpuFrameCount();
extern std::atomic<bool> g_rendererActive;
extern std::atomic<bool> g_captureRequest;
uint64_t GpuDrawCount();
void PrintThreadStatus();

// F11: save the game's working memory (heap, stacks, image data) to memdump.bin for offline
// debugging: [u32 guest address][4096 bytes]... for every committed, non-zero page.
static void DumpGuestMemory()
{
    FILE* o = fopen("memdump.bin", "wb");
    if (!o) return;
    static const uint8_t zero[4096] = {};
    uint32_t pages = 0;
    const struct { uint32_t lo, hi; } ranges[] = { { 0x40000000, 0x80000000 }, { 0x82000000, 0x90000000 } };
    for (auto r : ranges)
        for (uint64_t a = r.lo; a < r.hi; )
        {
            MEMORY_BASIC_INFORMATION mi{};
            if (!VirtualQuery(G2H(uint32_t(a)), &mi, sizeof mi)) break;
            uint64_t end = a + ((uint8_t*)mi.BaseAddress + mi.RegionSize - G2H(uint32_t(a)));
            if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                for (uint64_t p = a & ~0xFFFull; p < end && p < r.hi; p += 4096)
                {
                    const uint8_t* src = G2H(uint32_t(p));
                    if (!memcmp(src, zero, 4096)) continue;
                    uint32_t ga = uint32_t(p);
                    fwrite(&ga, 4, 1, o); fwrite(src, 1, 4096, o); pages++;
                }
            a = end > a ? end : a + 4096;
        }
    fclose(o);
    Log("[window] F11: saved %u pages of game memory to memdump.bin\n", pages);
}

void StartSignalProbe();   // actiontrace.cpp
void MusicStart(); void MusicNext(); void MusicPause(); std::wstring MusicNowPlaying(); void MusicLouder(int); int MusicLevelPercent();   // music.cpp
void NbashCamMenuToggle();   // statscreen.cpp
void NbashGfxMenuToggle(); void NbashFullscreenNote(bool on); int NbashFullscreenWanted();
#include <atomic>
static std::atomic<bool> g_fullscreen{false};
int NbashFps(); void NbashGfxNoteHz(int hz);
namespace
{
    HWND g_window = nullptr;
    uint64_t g_lastFrames = 0, g_lastDraws = 0;
    double g_fps = 0, g_drawsPerFrame = 0;
    auto g_lastTime = std::chrono::steady_clock::now();
    auto g_startTime = std::chrono::steady_clock::now();

    // 14 Nov 14:36 ("60 fps doesn't feel like 60 fps"): the display runs at 74.98 Hz and 60 is not a whole part of 75, so frames are shown for 1 or 2
    // refreshes in turn (judder) and the clock cannot follow the vblank.  While the game runs, the display the window is on is switched (temporarily -
    // CDS_FULLSCREEN, Windows puts it back when the game closes) to a refresh rate that IS a whole multiple of the chosen frame rate (same resolution):
    // the highest such rate not above the current one (75 -> 60, 144 -> 120), else the lowest above.  Checked every second (frame rate setting, other
    // screen).  NBASH_MATCHHZ=0 turns it off.
    void MatchRefresh(HWND w)
    {
        static const bool off = getenv("NBASH_MATCHHZ") && getenv("NBASH_MATCHHZ")[0] == '0';
        static int lastFps = 0; static std::wstring lastDev; static int tries = 0;
        if (off || !w) return;
        MONITORINFOEXW mi{}; mi.cbSize = sizeof mi; if (!GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi)) return;
        DEVMODEW cur{}; cur.dmSize = sizeof cur; if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &cur)) return;
        const int hz = int(cur.dmDisplayFrequency); char dev[64]; WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, dev, sizeof dev, nullptr, nullptr);
        NbashGfxNoteHz(hz);      // 14 Nov 20:45: automatic frame rate follows this screen
        const int fps = NbashFps();      // (after the note above: AUTO has just picked its rate for this screen)
        auto fits = [&](int f) { int n = (f + fps / 2) / fps; return n >= 1 && n <= 8 && std::abs(f - n * fps) <= 1; };
        if (fits(hz)) { lastFps = fps; lastDev = mi.szDevice; return; }
        if (fps == lastFps && lastDev == mi.szDevice && tries > 0) return;      // already tried for this screen and rate
        lastFps = fps; lastDev = mi.szDevice; tries++;
        int best = 0, bestAbove = 0; DEVMODEW dm{}; dm.dmSize = sizeof dm;
        for (DWORD i = 0; EnumDisplaySettingsW(mi.szDevice, i, &dm); i++)
        {
            if (dm.dmPelsWidth != cur.dmPelsWidth || dm.dmPelsHeight != cur.dmPelsHeight || dm.dmBitsPerPel != cur.dmBitsPerPel) continue;
            int f = int(dm.dmDisplayFrequency); if (!fits(f)) continue;
            if (f <= hz) { if (f > best) best = f; } else if (!bestAbove || f < bestAbove) bestAbove = f;
        }
        const int pick = best ? best : bestAbove;
        if (!pick) { Log("[display] %s runs at %d Hz; no %dx%d mode at a multiple of %d fps - frames cannot all stay on screen equally long\n", dev, hz, int(cur.dmPelsWidth), int(cur.dmPelsHeight), fps); return; }
        DEVMODEW nd = cur; nd.dmDisplayFrequency = pick; nd.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
        LONG r = ChangeDisplaySettingsExW(mi.szDevice, &nd, nullptr, CDS_FULLSCREEN, nullptr);
        Log("[display] %s: %d Hz -> %d Hz while the game runs (frame rate %d: every frame on screen equally long) - %s\n", dev, hz, pick, fps, r == DISP_CHANGE_SUCCESSFUL ? "done" : "FAILED");
    }

    LRESULT CALLBACK WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_TIMER:
        {
            MatchRefresh(w);
            auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - g_lastTime).count();
            uint64_t f = GpuFrameCount(), d = GpuDrawCount();
            if (dt > 0) g_fps = (f - g_lastFrames) / dt;
            g_drawsPerFrame = f > g_lastFrames ? double(d - g_lastDraws) / double(f - g_lastFrames) : 0;
            g_lastFrames = f; g_lastDraws = d; g_lastTime = now;
            MusicStart();   // your own music (music folder)
            if (g_rendererActive)
            {
                wchar_t title[384];
                std::wstring song = MusicNowPlaying();
                static auto levelShownUntil = std::chrono::steady_clock::time_point{};
                static int lastLevel = -1;
                int level = MusicLevelPercent();
                if (lastLevel >= 0 && level != lastLevel) levelShownUntil = now + std::chrono::seconds(3);
                lastLevel = level;
                wchar_t lv[64] = L"";
                if (now < levelShownUntil) _snwprintf(lv, 64, L"  (music %d%%)", level);
                static auto colorShownUntil = std::chrono::steady_clock::time_point{};
                static int lastColor = render::g_colorPreset;
                if (render::g_colorPreset != lastColor) { lastColor = render::g_colorPreset; colorShownUntil = now + std::chrono::seconds(3); }
                if (now < colorShownUntil) _snwprintf(lv, 64, L"  (colours: %s)", lastColor == 0 ? L"original" : lastColor == 1 ? L"natural" : L"vivid");
                _snwprintf(title, 384, L"NBA Street Homecourt  -  %.0f fps%s%s%s%s", g_fps,
                           render::g_tileCollapse ? L"" : L"  [F9: single-pass drawing OFF]",
                           song.empty() ? L"" : L"  -  \u266A ", song.c_str(), lv);
                SetWindowTextW(w, title);
            }
            else InvalidateRect(w, nullptr, TRUE);
            return 0;
        }
        case WM_PAINT:
        {
            if (g_rendererActive) { ValidateRect(w, nullptr); return 0; }   // Direct3D draws the window
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(w, &ps);
            RECT r; GetClientRect(w, &r);
            HBRUSH bg = CreateSolidBrush(RGB(18, 18, 22));
            FillRect(dc, &r, bg);
            DeleteObject(bg);
            SetBkMode(dc, TRANSPARENT);
            HFONT big = CreateFontW(44, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HFONT small = CreateFontW(24, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HGDIOBJ old = SelectObject(dc, big);
            SetTextColor(dc, RGB(255, 140, 0));
            TextOutW(dc, 40, 40, L"NBA Street Homecourt - PC port (test build)", 43);
            SelectObject(dc, small);
            SetTextColor(dc, RGB(230, 230, 230));
            int secs = int(std::chrono::duration<double>(std::chrono::steady_clock::now() - g_startTime).count());
            wchar_t line[256];
            int y = 120;
            auto put = [&](const wchar_t* s) { TextOutW(dc, 40, y, s, (int)wcslen(s)); y += 34; };
            _snwprintf(line, 256, L"Game running for %d:%02d", secs / 60, secs % 60); put(line);
            _snwprintf(line, 256, L"Frame rate: %.0f fps     Frames: %llu", g_fps, (unsigned long long)GpuFrameCount()); put(line);
            _snwprintf(line, 256, L"Objects drawn per frame: %.0f", g_drawsPerFrame); put(line);
            y += 20;
            SetTextColor(dc, RGB(150, 150, 160));
            put(L"The game itself is running, but pictures and sound are not hooked up yet.");
            put(L"Controller works now (keyboard too: arrows, Enter = Start, Space = A).");
            put(L"Close this window to quit. A log is saved next to the program as nbash_log.txt.");
            SelectObject(dc, old);
            DeleteObject(big); DeleteObject(small);
            EndPaint(w, &ps);
            return 0;
        }
        case WM_APP + 7:      // full screen on / off (graphics menu, Alt + Enter, saved setting): a borderless window over the display it is on
        {
            static WINDOWPLACEMENT prev{ sizeof(WINDOWPLACEMENT) }; static bool fs = false;
            bool want = wp != 0;
            if (want == fs) return 0;
            if (want)
            {
                GetWindowPlacement(w, &prev);
                MONITORINFO mi{}; mi.cbSize = sizeof mi; GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi);
                SetWindowLongPtrW(w, GWL_STYLE, WS_POPUP | WS_VISIBLE);
                SetWindowPos(w, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
            }
            else
            {
                SetWindowLongPtrW(w, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
                SetWindowPlacement(w, &prev);
                SetWindowPos(w, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
            }
            fs = want; g_fullscreen = want; Log("[window] %s\n", want ? "full screen" : "window");
            return 0;
        }
        case WM_SYSKEYDOWN:
            if (wp == VK_RETURN && (lp & (1 << 29))) { bool on = !g_fullscreen.load(); PostMessageW(w, WM_APP + 7, on, 0); NbashFullscreenNote(on); return 0; }
            break;
        case WM_KEYDOWN:
            if (wp == VK_F1) ::NbashGfxMenuToggle();      // graphics options menu
            if (wp == VK_F12) { g_captureRequest = true; Log("[window] F12: capturing the next frame\n"); }
            if (wp == VK_F11) { DumpGuestMemory(); PrintThreadStatus(); }
            if (wp == VK_F7) StartSignalProbe();
            if (wp == VK_F2) ::NbashCamMenuToggle();      // camera options menu
            if (wp == VK_F5) MusicNext();
            if (wp == VK_F6) MusicPause();
            if (wp == VK_F8) render::g_colorPreset = (render::g_colorPreset + 1) % 3;
            if (wp == VK_F3) MusicLouder(-1);
            if (wp == VK_F4) MusicLouder(+1);
            if (wp == VK_F9)
            {
                render::g_tileCollapse = !render::g_tileCollapse;
                Log("[window] F9: single-pass drawing %s\n", render::g_tileCollapse ? "on" : "off");
            }
            break;
        case WM_CLOSE:
            Log("[window] closed by user\n");
            ExitProcess(0);
        }
        return DefWindowProcW(w, msg, wp, lp);
    }

    void WindowThread()
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        wc.lpszClassName = L"NBASHWindow";
        RegisterClassW(&wc);
        RECT r{ 0, 0, 1280, 720 };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        g_window = CreateWindowW(L"NBASHWindow", L"NBA Street Homecourt", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
        SetTimer(g_window, 1, 1000, nullptr);
        if (NbashFullscreenWanted()) PostMessageW(g_window, WM_APP + 7, 1, 0);      // the saved setting
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

extern "C" HWND NbashWindow() { return g_window; }
void* WindowHandle() { return g_window; }
std::atomic<bool> g_rendererActive{false};

void WindowStart()
{
    std::thread(WindowThread).detach();
}
#else
void WindowStart() {}
void* WindowHandle() { return nullptr; }
#endif

#ifdef _WIN32
void NbashApplyFullscreen(bool on) { if (HWND w = (HWND)WindowHandle()) PostMessageW(w, WM_APP + 7, on, 0); }
#else
void NbashApplyFullscreen(bool) {}
#endif
