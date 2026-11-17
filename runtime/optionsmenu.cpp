// The GRAPHICS row of the game's own Options screen (14 Nov).
// The main Options screen (data/fe/options.big, an Apt movie) has ten rows; the third link row was "Profile Manager" (Xbox profiles - no use on PC).
// Data side (cloud fe/, installed by _archive\staging\optmenu\apply_optmenu.py): the row's label in stloc.en is now "Graphics", and its click handler in the
// movie's script is cut down to the one native call it started with (CmdProfile.IsPrimaryController) - the profile screen / warning popup it led to are gone.
// Program side (here): that native call, made while the Options screen is up and right after an A press, opens the graphics settings panel.
#include "guest.h"
#include <atomic>
void Log(const char* fmt, ...);
bool NbashInMatch(); void NbashGfxMenuOpen(); uint64_t NbashMsSinceAccept();

namespace { std::atomic<bool> g_optionsUp{false}; }

extern "C" PPC_FUNC(__imp__sub_821AE7F0);      // GetOnscreenHelp: read by the Options screen (and the pause menu) when it sets itself up
PPC_FUNC(sub_821AE7F0)
{
    if (!NbashInMatch()) { if (!g_optionsUp.exchange(true)) Log("[options] the game's Options screen opened\n"); }
    __imp__sub_821AE7F0(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_821B5BD8);      // SetOptions: called when the Options screen is left
PPC_FUNC(sub_821B5BD8)
{
    if (g_optionsUp.exchange(false)) Log("[options] the game's Options screen closed\n");
    __imp__sub_821B5BD8(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_821B0300);      // CmdProfile.IsPrimaryController
PPC_FUNC(sub_821B0300)
{
    __imp__sub_821B0300(ctx, base);
    static std::atomic<int> n{0};
    bool up = g_optionsUp.load(); uint64_t ms = NbashMsSinceAccept();
    if (n++ < 40) Log("[options] IsPrimaryController called (Options screen %s, %llu ms after an accept press)\n", up ? "up" : "not up", (unsigned long long)ms);
    if (up && ms < 500) NbashGfxMenuOpen();
}
