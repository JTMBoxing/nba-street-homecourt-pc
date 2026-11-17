// Which player the cursor is on in the game's "Select Team & Players" screen (select_team_players.big, Apt) - 15 Nov, user: the ratings screen
// should only open there, on the player you are hovering.
// The screen's script asks CmdPlayNow.GetPlayersPerTeamList (sub_821B2C50) for a team's players each time a team is shown; the list it builds
// (members player1..playerN = player ids, added through sub_821A3C80) is caught here. Hovering itself runs only in the script, so the cursor is
// followed from player 1's presses, the same way the script moves it: A on the team card -> choosing players, cursor on player 1; left / right
// move it (wrapping round the list); B goes back to the team card; B on the team card leaves the screen. LaunchGame or the main menu's
// Get/SetCurrentMenuItem also end it.
#include "guest.h"
#include <atomic>
#include <mutex>
#include <vector>
void Log(const char* fmt, ...);

namespace
{
    std::mutex g_m;
    std::vector<uint32_t> g_list, g_building; bool g_capturing = false;
    bool g_active = false, g_choosing = false; int g_idx = 0, g_picks = 0;      // g_picks: players picked on this card (B un-picks them one by one)
    void End(const char* why) { std::lock_guard lk(g_m); if (g_active) Log("[select] screen left (%s)\n", why); g_active = false; g_choosing = false; }
}

extern "C" PPC_FUNC(__imp__sub_821A3C80);
PPC_FUNC(sub_821A3C80)          // adds one member to an Apt object; r5 -> value (first word = int)
{
    if (g_capturing && ctx.r5.u32) g_building.push_back(R32(ctx.r5.u32));
    __imp__sub_821A3C80(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_821B2C50);
PPC_FUNC(sub_821B2C50)          // GetPlayersPerTeamList
{
    { std::lock_guard lk(g_m); g_building.clear(); g_capturing = true; }
    __imp__sub_821B2C50(ctx, base);
    std::lock_guard lk(g_m); g_capturing = false;
    if (!g_building.empty()) g_building.pop_back();          // the last member is the player count
    g_list = g_building; g_active = true; g_choosing = false; g_idx = 0; g_picks = 0;
    Log("[select] team list: %zu players\n", g_list.size());
}
extern "C" PPC_FUNC(__imp__sub_821B42C8);
PPC_FUNC(sub_821B42C8) { End("game launched"); __imp__sub_821B42C8(ctx, base); }      // LaunchGame
extern "C" PPC_FUNC(__imp__sub_821A82E0);
PPC_FUNC(sub_821A82E0) { End("menu"); __imp__sub_821A82E0(ctx, base); }               // SetCurrentMenuItem
extern "C" PPC_FUNC(__imp__sub_821A8398);
PPC_FUNC(sub_821A8398) { End("menu"); __imp__sub_821A8398(ctx, base); }               // GetCurrentMenuItem

// statscreen.cpp: every fresh press of player 1 in the menus (XInput bits)
void NbashSelectInput(uint16_t pressed)
{
    std::lock_guard lk(g_m);
    if (!g_active) return;
    int n = int(g_list.size());
    if (!g_choosing)
    {
        if (pressed & 0x1000) { g_choosing = true; g_idx = 0; g_picks = 0; Log("[select] choosing players\n"); }
        else if (pressed & 0x2000) { g_active = false; Log("[select] screen left (back)\n"); }
        return;
    }
    // 15 Nov 19:26 (user "the LB for the stats is not working now"): B while players are picked UN-PICKS the last one (the screen stays on the players);
    // only with nothing picked does B go back to the team card.  Before, the first B was taken as "back" and LB then did nothing.
    if (pressed & 0x2000)
    {
        if (g_picks > 0) { g_picks--; if (n > 0) g_idx = (g_idx + n - 1) % n; Log("[select] un-picked (picks %d, cursor %d)\n", g_picks, g_idx); }
        else { g_choosing = false; Log("[select] back to the team card\n"); }
        return;
    }
    if (n > 0 && (pressed & 0x1000)) { g_idx = (g_idx + 1) % n; g_picks++; Log("[select] picked (picks %d, cursor %d)\n", g_picks, g_idx); }          // A picks the player and the screen moves the cursor on to the next one (seen under Wine)
    if (n > 0 && (pressed & 0x0004)) g_idx = (g_idx + n - 1) % n;
    if (n > 0 && (pressed & 0x0008)) g_idx = (g_idx + 1) % n;
}
// the hovered player's id, or -1 when not on that screen choosing players; list = the team's players in screen order
int NbashSelectHovered(std::vector<uint32_t>* list, int* idx)
{
    std::lock_guard lk(g_m);
    if (!g_active || !g_choosing || g_list.empty()) return -1;
    if (list) *list = g_list; if (idx) *idx = g_idx;
    return int(g_list[size_t(g_idx) % g_list.size()]);
}
