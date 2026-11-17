// Graphics settings of the port (14 Nov): screen mode, frame rate (30 / 60 / 120), colour look. Shown in the GRAPHICS page of the port's own options menu
// (statscreen.cpp), remembered in nbash_graphics.txt next to the program.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <string>
void Log(const char* fmt, ...);
namespace render { extern std::atomic<int> g_colorPreset; }
void NbashApplyFullscreen(bool on);      // window.cpp

namespace
{
    std::atomic<int> g_fps{0}, g_full{0};      // g_fps 0 = AUTO (14 Nov 20:45, the new default): follows the display's refresh rate
    std::atomic<int> g_autoFps{60}, g_displayHz{0};
    std::atomic<bool> g_loaded{false};
    const int kFps[3] = { 30, 60, 120 };
    void Save()
    {
        if (FILE* f = fopen("nbash_graphics.txt", "w")) { fprintf(f, "fullscreen %d\nfps %d\ncolour %d\n", g_full.load(), g_fps.load(), render::g_colorPreset.load()); fclose(f); }
    }
}
void NbashGfxLoad()
{
    if (g_loaded.exchange(true)) return;
    if (FILE* f = fopen("nbash_graphics.txt", "r"))
    {
        char key[32]; int v;
        while (fscanf(f, "%31s %d", key, &v) == 2)
        {
            if (!strcmp(key, "fullscreen")) g_full = v ? 1 : 0;
            if (!strcmp(key, "fps") && (v == 0 || v == 30 || v == 60 || v == 120)) g_fps = v;
            if (!strcmp(key, "colour") && v >= 0 && v <= 2) render::g_colorPreset = v;
        }
        fclose(f);
    }
    if (const char* e = getenv("NBASH_FPS")) { int v = atoi(e); if (v == 0 || v == 30 || v == 60 || v == 120) g_fps = v; }
    Log("[graphics] fullscreen %d, frame rate %d, colour look %d\n", g_full.load(), g_fps.load(), render::g_colorPreset.load());
}
int NbashFps() { NbashGfxLoad(); const int f = g_fps.load(); return f ? f : g_autoFps.load(); }
// AUTO: the display's refresh rate decides - a 120 Hz (or 240 Hz) screen runs the game at 120 frames a second, a 60 Hz screen at 60.  Other rates
// (75, 144, 165 ...) run at 60 and window.cpp switches such a screen to a 60 / 120 Hz mode while the game runs (then this picks 120 on a 120 Hz mode).
// Called by window.cpp every timer tick with the current refresh of the screen the window is on.
void NbashGfxNoteHz(int hz)
{
    auto fits = [&](int f) { for (int n = 1; n <= 2; n++) if (std::abs(hz - n * f) <= 1) return true; return false; };
    const int a = hz >= 118 && fits(120) ? 120 : 60;
    if (hz != g_displayHz.exchange(hz) || a != g_autoFps.load()) { const int was = g_autoFps.exchange(a); if (was != a || g_fps.load() == 0) Log("[graphics] display %d Hz -> automatic frame rate %d%s\n", hz, a, g_fps.load() ? " (not used: a fixed rate is chosen)" : ""); }
}
int NbashFullscreenWanted() { NbashGfxLoad(); return g_full.load(); }
void NbashFullscreenNote(bool on) { g_full = on ? 1 : 0; Save(); }      // Alt+Enter changed it
extern const int kNbashGfxRows = 3;
const char* NbashGfxName(int row) { static const char* n[3] = { "Screen", "Frame rate", "Colours" }; return n[row]; }
const char* NbashGfxValue(int row)
{
    static const char* col[3] = { "Original", "Natural", "Vivid" };
    switch (row)
    {
    case 0: return g_full ? "Full screen" : "Window";
    case 1: { static char b[32]; if (g_fps == 0) { snprintf(b, sizeof b, "Auto (%d)", g_autoFps.load()); return b; } return g_fps == 30 ? "30" : g_fps == 120 ? "120" : "60"; }
    default: { int c = render::g_colorPreset.load(); return col[c < 0 || c > 2 ? 1 : c]; }
    }
}
const char* NbashGfxHint(int row)
{
    switch (row)
    {
    case 0: return "Full screen fills the display the window is on (also Alt + Enter)";
    case 1: return "Frames a second. Auto follows the display: 120 on a 120 Hz screen, 60 on a 60 Hz screen";
    default: return "The colour look of the picture (also F8)";
    }
}
void NbashGfxStep(int row, int d)
{
    NbashGfxLoad();
    if (row == 0) { g_full = g_full ? 0 : 1; NbashApplyFullscreen(g_full != 0); }
    if (row == 1) { static const int k4[4] = { 0, 30, 60, 120 }; int i = g_fps == 0 ? 0 : g_fps == 30 ? 1 : g_fps == 120 ? 3 : 2; i = ((i + d) % 4 + 4) % 4; g_fps = k4[i]; Log("[graphics] frame rate -> %s\n", g_fps.load() ? std::to_string(g_fps.load()).c_str() : "auto"); }
    if (row == 2) render::g_colorPreset = ((render::g_colorPreset.load() + d) % 3 + 3) % 3;
    Save();
}
