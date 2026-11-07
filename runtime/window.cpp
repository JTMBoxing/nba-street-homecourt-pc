// Game window (Windows). Until the renderer exists it shows what the game is doing:
// frame rate, draw calls and controller state, so you can see the game running.
#include "guest.h"
#include "render/renderer.h"
#ifdef _WIN32
#include <windows.h>
#include <thread>
#include <chrono>
#include <atomic>

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
namespace
{
    HWND g_window = nullptr;
    uint64_t g_lastFrames = 0, g_lastDraws = 0;
    double g_fps = 0, g_drawsPerFrame = 0;
    auto g_lastTime = std::chrono::steady_clock::now();
    auto g_startTime = std::chrono::steady_clock::now();

    LRESULT CALLBACK WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_TIMER:
        {
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
        case WM_KEYDOWN:
            if (wp == VK_F12) { g_captureRequest = true; Log("[window] F12: capturing the next frame\n"); }
            if (wp == VK_F11) { DumpGuestMemory(); PrintThreadStatus(); }
            if (wp == VK_F7) StartSignalProbe();
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
