// Drive camera + two gameplay rules, in code (Nov 10-11). All OFF unless the environment variables are set.
//
//  NBASH_DRIVECAM=1   gameplay camera sits behind the player the USER controls and looks at the hoop the offence attacks;
//                     it swings round (flips) when possession changes.  NBASH_DC_BACK / _HT / _AHEAD (metres) tune it.
//                     How: the key-frame camera's per-frame function sub_82164D20(this, .., r7 = result) leaves eye at
//                     result+0 and a second vector at result+16 (look-at point, or look - eye: detected at run time).
//                     The user's player = the AI player object last given cAssignUser (class id 32) through the slot
//                     setter sub_820F6F48(table, slot, id, player); a player's court position is the vec at player+0x110D0.
//  NBASH_INBOUND=baseline   every inbound is taken from behind the baseline under the basket: the team's inbound type
//                     (team+0x114A4: 1 = behind the baseline, the normal one after a basket; 0 / 2 = side line, e.g. at the start) is forced to 1 when the inbound strategies (27/28) start.
//  NBASH_NOPRESS=1    computer defenders do not press full court: while the user's team has the ball, every move target
//                     of a computer player (sub_820EDBE8(player, player, x, z)) is kept in the half they defend.
//  NBASH_DRIVECAM=log  only writes what it sees to nbash_log.txt ([drivecam] lines).
#include "guest.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include <vector>
#include <thread>
#include "indirect.h"
#include <array>
#include <cmath>
#include <cstdlib>
#include <string>

std::atomic<uint64_t> g_nbashFastUntil{0};   // while NbashMs() < this: game clock speed = g_nbashFastSpeed (actiontrace.cpp) and the main loop is not held to 60 Hz (loopstats.cpp)
float g_nbashFastSpeed = 5.0f;
void Log(const char* fmt, ...);
// camera option picked in the in-game camera menu (statscreen.cpp): 0 Drive, 1 Drive High, 2 Drive Close, 3 Classic (the game's own)
std::atomic<int> g_nbashCam{-1};
extern const int kNbashCamCount; const int kNbashCamCount = 6;
const char* NbashCamName(int i) { static const char* n[] = { "DRIVE", "DRIVE HIGH", "DRIVE CLOSE", "CLASSIC", "SIDELINE", "SPECTATOR" }; return n[i < 0 || i >= kNbashCamCount ? 0 : i]; }
const char* NbashCamHint(int i) { static const char* n[] = { "Behind the ball handler, flips with possession", "Same, higher and further back", "Same, lower and closer", "The game's original camera", "The game's sideline camera", "The game's spectator camera" }; return n[i < 0 || i >= kNbashCamCount ? 0 : i]; }
int NbashCamGet()
{
    int v = g_nbashCam.load();
    if (v < 0) { v = 0; if (FILE* f = fopen("nbash_camera.txt", "r")) { int k = 0; if (fscanf(f, "%d", &k) == 1 && k >= 0 && k < kNbashCamCount) v = k; fclose(f); } g_nbashCam = v; Log("[drivecam] camera option: %s\n", NbashCamName(v)); }
    return v;
}
void NbashCamSet(int v)
{
    v = ((v % kNbashCamCount) + kNbashCamCount) % kNbashCamCount; g_nbashCam = v;
    if (FILE* f = fopen("nbash_camera.txt", "w")) { fprintf(f, "%d\n", v); fclose(f); }
    Log("[drivecam] camera option -> %s\n", NbashCamName(v));
}
bool NbashCamAvailable() { const char* v = getenv("NBASH_DRIVECAM"); return v && *v && *v != '0'; }
uint64_t NbashMs();
int NbashHumanOnDefense();
uint64_t NbashShotPressMs(); uint32_t NbashShotHolderHash(); uint32_t NbashShotHoldMs();
static std::atomic<uint64_t> g_trickMs{0}; static const char* g_trickName = "";
// ms since the last right-stick SIGNATURE dribble flick (gameplay.cpp: the ball-pin hooks), -1 if the last gesture was something else
int NbashSigDribbleMs() { uint64_t t = g_trickMs.load(); if (!t || strncmp(g_trickName, "signature", 9)) return -1; uint64_t n = NbashMs(); return n >= t && n - t < 5000 ? int(n - t) : -1; }
int NbashAnyTrickMs(const char** name) { uint64_t t = g_trickMs.load(); if (!t) return -1; if (name) *name = g_trickName; uint64_t n = NbashMs(); return n >= t && n - t < 5000 ? int(n - t) : -1; }
namespace render { extern std::atomic<int> g_shotBurstFrames; }
void NbashTrickMark(const char* name)
{
    g_trickName = name; g_trickMs = NbashMs();
    // 14 Nov: with game\mods\shots\record_tricks.txt present the first 8 dribble gestures of a session are saved as frame strips (capture\shotN_FF.bmp,
    // 26 frames each) - to SEE what a converted dribble move really looks like in the game.  It halves the frame rate while it records.
    static const bool on = [] { FILE* f = fopen((GameRoot() + "/mods/shots/record_tricks.txt").c_str(), "r"); if (f) fclose(f); return f != nullptr; }();
    static int rec = 0; if (on && rec < 8 && render::g_shotBurstFrames.load() == 0) { rec++; render::g_shotBurstFrames = 52; Log("[trick] recording gesture %d (%s)\n", rec, name); }
}

namespace
{
    const uint32_t kPos = 0x110D0, kBrain = 0x119B0, kTeam = 0x110C0, kInboundType = 0x114A4;   // kTeam: team object (same for the 3 team mates); kBrain: per-player AI data
    float Env(const char* n, float d) { const char* v = getenv(n); return v && *v ? float(atof(v)) : d; }
    int CamMode() { static const int m = [] { const char* v = getenv("NBASH_DRIVECAM"); return !v || !*v || *v == '0' ? 0 : !strcmp(v, "log") ? 1 : 2; }(); return m; }
    bool Baseline() { static const bool b = [] { const char* v = getenv("NBASH_INBOUND"); return v && !strcmp(v, "baseline"); }(); return b; }
    bool NoPress() { static const bool b = [] { const char* v = getenv("NBASH_NOPRESS"); return v && *v == '1'; }(); return b; }
    bool Any() { return CamMode() || Baseline() || NoPress(); }
    bool Guest(uint32_t a) { return a >= 0x40000000 && a < 0xF0000000; }
    float RF(uint32_t a) { uint32_t v = R32(a); float f; memcpy(&f, &v, 4); return f; }
    void WF(uint32_t a, float f) { uint32_t v; memcpy(&v, &f, 4); W32(a, v); }

    std::atomic<uint32_t> g_user{0};       // AI player object the user controls
    std::atomic<uint64_t> g_userSeen{0};
    std::atomic<uint32_t> g_carrier{0}, g_receiver{0};   // player holding the ball (cGameplayBallCarrier, id 121 in slot 7) / player a pass is going to (cAssignReceivePass 45, cAssignReceiveInbound 47)
    std::atomic<uint64_t> g_carrierLost{0}, g_receiverAt{0};
    std::atomic<uint32_t> g_inbounder{0};  // player currently given cAssignInboundBall (id 46): the camera shows him while he inbounds
    // which way the user's team attacks along x (+1 / -1): voted from where the user stands when possession changes
    std::atomic<int> g_votes{0};
    int g_lastDef = -1; uint64_t g_lastFlip = 0;
    float g_yaw = 0; bool g_yawInit = false; uint64_t g_lastCam = 0;
    int g_logged = 0;
    std::atomic<uint64_t> g_ffArm{0};      // match-start inbound: the walk to the baseline is fast-forwarded
    std::atomic<uint64_t> g_tpArm{0};      // match-start inbound: the inbounder is put on his spot instead of walking there
    std::atomic<uint64_t> g_msUntil{0};    // match-start window
    std::atomic<uint64_t> g_ovUntil{0};    // until then the match-start inbounder's spot is moved beside the basket support
    std::atomic<bool> g_tpHave{false}; float g_tpX = 0, g_tpZ = 0;   // his spot (world x, z), from his own "go to" call
    std::atomic<uint32_t> g_chaser{0}; std::atomic<uint64_t> g_chaserAt{0};   // player sent after a loose ball / stealing it
    std::atomic<uint32_t> g_players[8];
    std::atomic<uint32_t> g_dead{0}; std::atomic<uint64_t> g_deadAt{0};   // player sent to pick up the dead ball after a basket (id 44)
    std::atomic<bool> g_dirLocked{false};
    std::atomic<bool> g_practice{false};      // practice mode (players run cGameplayPractice 124 / cGameplayPracticeBallCarrier 125): the camera keeps facing one rim
    void Register(uint32_t pl) { for (auto& a : g_players) { uint32_t v = a.load(); if (v == pl) return; if (!v) { a = pl; return; } } }
    bool Mapped(uint32_t a, uint32_t len)
    {
#ifdef _WIN32
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(g_base + a, &mi, sizeof mi)) return false;
        if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) return false;
        return (uint8_t*)mi.BaseAddress + mi.RegionSize >= g_base + a + len;
#else
        return false;
#endif
    }      // set when the match-start inbound begins (NBASH_INBOUND_SKIPWALK, on with NBASH_INBOUND=baseline)

    void RelayWatch(PPCContext& ctx, uint8_t* base, uint64_t now);
    void CamMenuFrame();
    int AttackSign() { static int last = 1; int v = g_votes.load(); if (v) last = v < 0 ? -1 : 1; return last; }

    // court-local position of a player (the camera works in court coordinates: hoops at x = +-10, court centre at 0).
    // court object = *(*(*(player+0x119B0)+24)+144); the game's own sub_82270C68(out, court, in) converts world -> court.
    bool CourtPos(PPCContext& ctx, uint8_t* base, uint32_t player, float& x, float& y, float& z);


    void TrackPossession(float ux)
    {
        int d = NbashHumanOnDefense();
        if (d < 0) return;
        if (!g_dirLocked.load() && g_lastDef >= 0 && d != g_lastDef && std::fabs(ux) > 3.5f)
        {
            // just got the ball: the user is near the basket they defend.  just lost it: near the one they attack.
            int own = (d == 0) ? (ux > 0 ? 1 : -1) : (ux > 0 ? -1 : 1);
            int v = g_votes.load() - own; g_votes = v > 6 ? 6 : v < -6 ? -6 : v;
            if (g_logged < 300) { Log("[drivecam] possession -> %s, user x %.1f, attack-direction votes %d\n", d ? "DEFENSE" : "OFFENSE", ux, g_votes.load()); g_logged++; }
        }
        g_lastDef = d;
    }
}

extern "C" PPC_FUNC(__imp__sub_82270C68);   // world -> court (out, court, in; "in" is changed)
extern "C" PPC_FUNC(__imp__sub_82270C08);   // court -> world (out, court, in)
namespace
{
    uint32_t CourtOf(uint32_t player)
    {
        uint32_t team = R32(player + kBrain); if (!Guest(team)) return 0;
        uint32_t t2 = R32(team + 24); if (!Guest(t2)) return 0;
        uint32_t court = R32(t2 + 144); return Guest(court) ? court : 0;
    }
    // converts v (x, y, z) between world and court coordinates with the game's own functions
    bool Convert(PPCContext& ctx, uint8_t* base, uint32_t court, bool toCourt, float& x, float& y, float& z)
    {
        static uint32_t scratch = 0;
        if (!scratch) scratch = AllocSystem(64, 16);
        if (!court) return false;
        WF(scratch, x); WF(scratch + 4, y); WF(scratch + 8, z); WF(scratch + 12, 1.0f);
        memset(g_base + scratch + 16, 0, 16);
        PPCContext saved = ctx;
        ctx.r3.u64 = scratch + 16; ctx.r4.u64 = court; ctx.r5.u64 = scratch;
        if (toCourt) __imp__sub_82270C68(ctx, base); else __imp__sub_82270C08(ctx, base);
        ctx = saved;
        x = RF(scratch + 16); y = RF(scratch + 20); z = RF(scratch + 24);
        return std::isfinite(x) && std::isfinite(z);
    }
    bool CourtPos(PPCContext& ctx, uint8_t* base, uint32_t player, float& x, float& y, float& z)
    {
        x = RF(player + kPos); y = RF(player + kPos + 4); z = RF(player + kPos + 8);
        return Convert(ctx, base, CourtOf(player), true, x, y, z) && std::fabs(x) < 40 && std::fabs(z) < 30;
    }
}

// the player object that has the ball, or had it last (*lost = the ball has left him); 0 when the tracking is off
uint32_t NbashCarrier(bool* lost) { if (lost) *lost = g_carrierLost.load() != 0; return g_carrier.load(); }
// 16 Nov (new controller layout): does the user's player hold the ball, and how far (m) is the nearest opponent from him?
// Worked out every camera frame below; -1 = not known (drive camera off, or before the first frame).
static std::atomic<int> g_userHasBall{-1}; static std::atomic<float> g_oppDist{-1.0f}; static std::atomic<uint64_t> g_oppAt{0};
int NbashUserHasBall() { return NbashMs() - g_oppAt.load() < 500 ? g_userHasBall.load() : -1; }
float NbashOppDist() { return NbashMs() - g_oppAt.load() < 500 ? g_oppDist.load() : -1.0f; }

// ---------------------------------------------------------------- the pause menu's own "Camera Type" option
// The game: settings object S = *(*(0x82721898)+4), camera index at S+4 (0 Default, 1 Sideline, 2 Spectator - clamped to
// 0..2 where the camera is chosen, sub_82255A04..).  The menu's script calls GetGameCameraIndex (sub_821AE850) when it opens
// and SetGameCameraIndex (sub_821AED30) on left / right, and shows the locale strings 563 "DEFAULT", 557 "SIDELINE",
// 556 "SPECTATOR" (stloc.en, 3689 strings, kept in memory as loaded: 'LOCL', size, 0, count, offsets[], UTF-16 text).
// Here: each left / right step of the menu moves through OUR six camera options instead, the three locale strings are
// re-pointed to one buffer holding the current option's name, and S+4 is kept on the game camera that option needs.
extern "C" PPC_FUNC(__imp__sub_821AE850);
extern "C" PPC_FUNC(__imp__sub_821AED30);
namespace
{
    std::atomic<uint32_t> g_locl{0}; std::atomic<int> g_loclBE{0}; std::atomic<int> g_loclState{3};   // 0 not searched, 1 searching, 2 found, 3 not found
    int g_uiIndex = 0;
    uint32_t Settings() { uint32_t a = R32(0x82721898); if (!Guest(a)) return 0; uint32_t s = R32(a + 4); return Guest(s) ? s : 0; }
    int EngineIndex(int opt) { return opt == 4 ? 1 : opt == 5 ? 2 : 0; }
    uint32_t RdLoc(uint32_t a, bool be) { uint32_t v; memcpy(&v, g_base + a, 4); return be ? __builtin_bswap32(v) : v; }
    bool LoclText(uint32_t L, bool be, uint32_t idx, const char* want)
    {
        uint32_t o = L + RdLoc(L + 16 + idx * 4, be);
        for (size_t i = 0;; i++) { uint16_t c; memcpy(&c, g_base + o + i * 2, 2); if (be) c = uint16_t(c << 8 | c >> 8); if (c != uint16_t(want[i])) return false; if (!want[i]) return true; }
    }
    // The text is not kept the way the file has it (test: no 'LOCL' table in memory), so the three strings themselves are
    // looked for (UTF-16, either byte order: "SPECTATOR" directly followed by "SIDELINE", and "SFX" followed by "DEFAULT"
    // a little further on), then every pointer to them.  Pointers found -> they are re-pointed to our buffer (any length);
    // none found -> the text is overwritten in place with short names that fit.
    struct LabelSlot { uint32_t text = 0; int cap = 0; std::vector<uint32_t> ptrBE, ptrLE; };
    LabelSlot g_slots[3]; bool g_textBE = true;
    bool Wide(uint32_t a, bool be, const char* want, bool withNul = true)
    {
        for (size_t i = 0;; i++) { if (!want[i] && !withNul) return true; uint16_t c; memcpy(&c, g_base + a + i * 2, 2); if (be) c = uint16_t(c << 8 | c >> 8); if (c != uint16_t(want[i])) return false; if (!want[i]) return true; }
    }
    void FindLocl()
    {
#ifdef _WIN32
        uint64_t t0 = NbashMs(); bool found = false;
        struct Region { uint64_t a, e; }; std::vector<Region> regs;
        for (uint64_t a = 0x40000000ull; a < 0xFFFF0000ull;)
        {
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery(g_base + a, &mi, sizeof mi)) break;
            uint64_t e = uint64_t((uint8_t*)mi.BaseAddress + mi.RegionSize - g_base); if (e > 0xFFFF0000ull) e = 0xFFFF0000ull;
            if (mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && !(a >= 0x82000000ull && a < 0x82800000ull)) regs.push_back({ a, e });
            if (e <= a) break;
            a = e;
        }
        for (auto& r : regs)
        {
            if (found) break;
            const uint8_t* m = g_base + r.a; size_t n = size_t(r.e - r.a);
            for (size_t k = 0; k + 600 < n && !found; k += 2)
            {
                // 'S' 'P' in either byte order
                bool be = m[k] == 0 && m[k + 1] == 'S' && m[k + 2] == 0 && m[k + 3] == 'P', le = m[k] == 'S' && m[k + 1] == 0 && m[k + 2] == 'P' && m[k + 3] == 0;
                if (!be && !le) continue;
                uint32_t A = uint32_t(r.a + k);
                if (!Wide(A, be, "SPECTATOR") || !Wide(A + 20, be, "SIDELINE")) continue;
                uint32_t D = 0;
                for (uint32_t o = 38; o < 560; o += 2) if (Wide(A + o, be, "SFX") && Wide(A + o + 8, be, "DEFAULT")) { D = A + o + 8; break; }
                if (!D) continue;
                g_textBE = be; g_slots[0] = { A, 9 }; g_slots[1] = { A + 20, 8 }; g_slots[2] = { D, 7 }; found = true;
                Log("[camera menu] pause-menu camera names found at %08X / %08X / %08X (%s-endian text), %llu ms\n", A, A + 20, D, be ? "big" : "little", (unsigned long long)(NbashMs() - t0));
            }
        }
        if (found)
        {
            uint32_t want[3] = { g_slots[0].text, g_slots[1].text, g_slots[2].text }, wantSw[3];
            for (int i = 0; i < 3; i++) wantSw[i] = __builtin_bswap32(want[i]);
            for (auto& r : regs)
            {
                const uint32_t* w = (const uint32_t*)(g_base + r.a); size_t n = size_t((r.e - r.a) / 4);
                for (size_t k = 0; k < n; k++)
                {
                    uint32_t v = w[k];
                    for (int i = 0; i < 3; i++)
                    {
                        if (v == wantSw[i] && g_slots[i].ptrBE.size() < 16) g_slots[i].ptrBE.push_back(uint32_t(r.a + k * 4));     // stored big-endian (the game's own pointers)
                        else if (v == want[i] && g_slots[i].ptrLE.size() < 16) g_slots[i].ptrLE.push_back(uint32_t(r.a + k * 4));
                    }
                }
            }
            for (int i = 0; i < 3; i++)
            {
                Log("[camera menu]   name %d: %d pointers (big-endian), %d (little-endian)\n", i, int(g_slots[i].ptrBE.size()), int(g_slots[i].ptrLE.size()));
                for (uint32_t a : g_slots[i].ptrBE) Log("[camera menu]     pointer at %08X\n", a);
            }
            Log("[camera menu] search finished in %llu ms\n", (unsigned long long)(NbashMs() - t0));
            g_loclState = 2; return;
        }
        Log("[camera menu] the pause menu's camera names were not found in memory (%llu ms) - it keeps saying DEFAULT / SIDELINE / SPECTATOR\n", (unsigned long long)(NbashMs() - t0));
#endif
        g_loclState = 3;
    }
    // the three camera names of the pause menu all show the current option's name
    void LabelSync()
    {
        if (g_loclState.load() != 2) return;
        static uint32_t buf = 0; if (!buf) buf = AllocSystem(96, 16);
        int opt = NbashCamGet(); const char* name = NbashCamName(opt);
        auto put = [&](uint32_t at, const char* t, size_t cap) { for (size_t i = 0; i <= cap; i++) { char ch = i < cap ? t[i] : 0; uint16_t c = uint16_t(uint8_t(ch)); if (g_textBE) c = uint16_t(c << 8 | c >> 8); memcpy(g_base + at + i * 2, &c, 2); if (!ch) break; } };
        put(buf, name, 40);
        static const char* shortName[] = { "DRIVE", "DRV HI", "DRV LOW", "CLASSIC", "SIDE", "CROWD" };
        static const char* orig[] = { "SPECTATOR", "SIDELINE", "DEFAULT" };
        for (int i = 0; i < 3; i++)
        {
            LabelSlot& sl = g_slots[i];
            if (!sl.ptrBE.empty())
            {
                uint32_t be = __builtin_bswap32(buf);
                for (uint32_t a : sl.ptrBE) { uint32_t cur; memcpy(&cur, g_base + a, 4); if (cur == __builtin_bswap32(sl.text) || cur == be) memcpy(g_base + a, &be, 4); }
            }
            else put(sl.text, strlen(name) <= size_t(sl.cap) ? name : shortName[opt], size_t(sl.cap));   // in place: must fit the old text's room
            (void)orig;
        }
    }
    // every frame of a match: start the search for the text table once, keep the game's own index on the camera we need
    void CamMenuFrame()
    {
        int st = 0;
        if (g_loclState.compare_exchange_strong(st, 1)) std::thread(FindLocl).detach();
        static int synced = -2; int opt = NbashCamGet();
        if (g_loclState.load() == 2 && synced != opt) { LabelSync(); synced = opt; }
        uint32_t S = Settings();
        if (S && int32_t(R32(S + 4)) != EngineIndex(opt)) W32(S + 4, uint32_t(EngineIndex(opt)));
    }
}
PPC_FUNC(sub_821AE850)     // GetGameCameraIndex: the menu gets the position it had (0..2); the game keeps its own value
{
    uint32_t S = Settings();
    if (!NbashCamAvailable() || !S) { __imp__sub_821AE850(ctx, base); return; }
    LabelSync();
    uint32_t keep = R32(S + 4); W32(S + 4, uint32_t(g_uiIndex));
    __imp__sub_821AE850(ctx, base);
    W32(S + 4, keep);
}
PPC_FUNC(sub_821AED30)     // SetGameCameraIndex: one step right / left in the menu = next / previous of our options
{
    __imp__sub_821AED30(ctx, base);
    uint32_t S = Settings();
    if (!NbashCamAvailable() || !S) return;
    int v = int32_t(R32(S + 4)); if (v < 0) v = 0; if (v > 2) v = 2;
    int d = ((v - g_uiIndex) % 3 + 3) % 3, opt = NbashCamGet();
    if (d == 1) NbashCamSet(opt + 1); else if (d == 2) NbashCamSet(opt - 1);
    Log("[camera menu] pause menu: index %d -> %d, camera option %s\n", g_uiIndex, v, NbashCamName(NbashCamGet()));
    g_uiIndex = v;
    LabelSync();
    W32(S + 4, uint32_t(EngineIndex(NbashCamGet())));
}

// The menus' text lookup: sub_8216D560(loc, key, 0) upper-cases the key ("~KEY" without the ~), hashes it and returns the text
// (UTF-16 as in stloc.en).  Menu text fields that hold "~KEY" are translated when they are drawn.  The Camera Type row uses
// OPT_SIDELINE ("DEFAULT", index 0), OPT_FAR ("SIDELINE", 1) and OPT_END_TO_END ("SPECTATOR", 2): for these the name of
// OUR current camera option is returned instead, so the row always shows the camera that is really on.
extern "C" PPC_FUNC(__imp__sub_8216D560);
PPC_FUNC(sub_8216D560)
{
    uint32_t key = ctx.r4.u32; int r = -1;
    if (NbashCamAvailable() && Guest(key))
    {
        char k[20]; int i = 0; for (; i < 19; i++) { char c = char(g_base[key + i]); if (c >= 'a' && c <= 'z') c = char(c - 32); k[i] = c; if (!c) break; } k[19] = 0;
        r = !strcmp(k, "OPT_SIDELINE") ? 0 : !strcmp(k, "OPT_FAR") ? 1 : !strcmp(k, "OPT_END_TO_END") ? 2 : -1;
    }
    __imp__sub_8216D560(ctx, base);
    if (r < 0) return;
    uint32_t t = ctx.r3.u32; if (!Guest(t)) return;
    static const char* orig[] = { "DEFAULT", "SIDELINE", "SPECTATOR" };
    bool le = g_base[t] == uint8_t(orig[r][0]) && g_base[t + 1] == 0, be = g_base[t] == 0 && g_base[t + 1] == uint8_t(orig[r][0]);
    static int said = 0;
    if (!le && !be) { if (said < 3) { said++; Log("[camera menu] text of camera name %d is not in a known form (%02X %02X %02X %02X) - left alone\n", r, g_base[t], g_base[t + 1], g_base[t + 2], g_base[t + 3]); } return; }
    int d = ((r - g_uiIndex) % 3 + 3) % 3, opt = NbashCamGet();                      // asked for the neighbour's name just before the step is made
    if (d == 1) opt = (opt + 1) % kNbashCamCount; else if (d == 2) opt = (opt + kNbashCamCount - 1) % kNbashCamCount;
    static uint32_t buf[kNbashCamCount][2] = {};
    uint32_t& b = buf[opt][le ? 0 : 1];
    if (!b)
    {
        b = AllocSystem(64, 16); const char* name = NbashCamName(opt);
        for (size_t i = 0; i < 31; i++) { uint8_t ch = uint8_t(name[i]); g_base[b + i * 2 + (le ? 0 : 1)] = ch; g_base[b + i * 2 + (le ? 1 : 0)] = 0; if (!ch) break; }
    }
    if (said < 3) { said++; Log("[camera menu] camera name %d shown as %s (%s text)\n", r, NbashCamName(opt), le ? "little-endian" : "big-endian"); }
    ctx.r3.u32 = b;
}

// ---------------------------------------------------------------- relay inbound after a basket (NBASH_RELAY=1)
// The game's own inbound: sub_82133110(strategy, team) picks the ball holder, or else the team mate closest to the ball;
// sub_82133240(strategy, team, P) hands out the roles: P = holder -> cGameplayBallCarrier (slot 7, 121) + cAssignInboundBall
// (slot 2, 46: walks behind the baseline, then passes in); P without the ball -> cAssignPickupBall (43); P's partner
// *(P+0x11984) -> cAssignReceiveInbound (47); the rest cAssignAttack (33); human players cAssignUser (32).
// An AI pass (sub_8211A068): passer+0x11984 = receiver, team+0x114AC = receiver, action slot 3 := cActionPass (67),
// receiver's slot 7 := cGameplayOffense (119).  Slots: 2 assign, 3 action, 4 move, 7 gameplay.
// Relay: while P goes for the loose ball, another computer-controlled team mate (I) is sent behind the baseline
// (assign NULL 31, move cMoveToInbound 101).  When P has the ball he passes it to I; the game then makes I, the new holder,
// the inbounder by itself.  Anything unexpected (5 s without progress) -> the game's own roles are handed out again.
extern "C" PPC_FUNC(__imp__sub_82133240);
extern "C" PPC_FUNC(__imp__sub_820EFA10);
PPC_FUNC(sub_820F6F48);   // (no extern "C": the hooks override the C++-named recompiled functions; with it the hook was dead in test 15)
namespace
{
    bool RelayOn() { static const bool b = [] { const char* v = getenv("NBASH_RELAY"); return v && *v == '1'; }(); return b; }
    struct RelayState { bool active = false; uint32_t team = 0, strat = 0, P = 0, I = 0; int phase = 0; uint64_t t0 = 0, tPhase = 0, tRetry = 0; } g_relay;
    struct TeamStrat { uint32_t team = 0, id = 0; } g_teamStrat[4];
    int g_relayLog = 0;
    void NoteStrategy(uint32_t team, uint32_t id) { for (auto& t : g_teamStrat) if (t.team == team || !t.team) { t.team = team; t.id = id; return; } }
    uint32_t StrategyOf(uint32_t team) { for (auto& t : g_teamStrat) if (t.team == team) return t.id; return 0; }
    void SetSlot(PPCContext& ctx, uint8_t* base, uint32_t pl, uint32_t slot, uint32_t id)
    {
        uint32_t brain = R32(pl + kBrain); if (!Guest(brain)) return;
        PPCContext saved = ctx;
        ctx.r3.u64 = brain + 0x13D50; ctx.r4.u64 = slot; ctx.r5.u64 = id; ctx.r6.u64 = pl;
        sub_820F6F48(ctx, base);
        ctx = saved;
    }
    uint32_t Holder(PPCContext& ctx, uint8_t* base, uint32_t team)
    {
        PPCContext saved = ctx; ctx.r3.u64 = team; __imp__sub_820EFA10(ctx, base); uint32_t h = ctx.r3.u32; ctx = saved; return Guest(h) ? h : 0;
    }
    bool Human(uint32_t pl) { uint32_t brain = R32(pl + kBrain); return Guest(brain) && int32_t(R32(brain + 684)) != -1; }
    void SendBehindBaseline(PPCContext& ctx, uint8_t* base, uint32_t I) { SetSlot(ctx, base, I, 2, 31); SetSlot(ctx, base, I, 3, 66); SetSlot(ctx, base, I, 4, 101); }
    void RelayCancel(PPCContext& ctx, uint8_t* base, const char* why)
    {
        RelayState r = g_relay; g_relay.active = false;
        if (g_relayLog++ < 200) Log("[relay] cancelled (%s) - the game's own inbound takes over\n", why);
        if (StrategyOf(r.team) != 27 || !Guest(r.strat)) return;
        uint32_t h = Holder(ctx, base, r.team);
        PPCContext saved = ctx; ctx.r3.u64 = r.strat; ctx.r4.u64 = r.team; ctx.r5.u64 = h ? h : r.P; __imp__sub_82133240(ctx, base); ctx = saved;
    }
    // the fetcher has the ball: he stops and throws it to the inbounder, who keeps standing behind the baseline
    void RelayPass(PPCContext& ctx, uint8_t* base, uint64_t now)
    {
        RelayState& r = g_relay;
        SendBehindBaseline(ctx, base, r.I);
        SetSlot(ctx, base, r.P, 2, 31);
        W32(r.P + 0x11984, r.I); W32(r.team + 0x114AC, r.I);
        SetSlot(ctx, base, r.P, 4, 90);
        SetSlot(ctx, base, r.P, 3, 67);
        SetSlot(ctx, base, r.I, 7, 119);
        if (r.phase != 2) { r.phase = 2; r.tPhase = now; }
        if (g_relayLog++ < 200) Log("[relay] %08X has the ball -> passes to the inbounder %08X\n", r.P, r.I);
    }
    void RelayWatch(PPCContext& ctx, uint8_t* base, uint64_t now)
    {
        RelayState& r = g_relay; if (!r.active) return;
        if (StrategyOf(r.team) != 27) { r.active = false; if (g_relayLog++ < 200) Log("[relay] over (inbound finished)\n"); return; }
        uint32_t h = Holder(ctx, base, r.team);
        if (r.phase == 1 && h == r.P) RelayPass(ctx, base, now);
        else if (r.phase == 2 && h == r.P && now - r.tPhase > 700 && now - r.tRetry > 700) { r.tRetry = now; RelayPass(ctx, base, now); }   // still holding it: order the pass again
        else if (h == r.I) { r.active = false; if (g_relayLog++ < 200) Log("[relay] inbounder %08X has the ball after %llu ms\n", r.I, (unsigned long long)(now - r.t0)); }
        else if (r.phase == 1 && now - r.t0 > 7000) RelayCancel(ctx, base, "nobody picked the ball up in 7 s");
        else if (r.phase == 2 && now - r.tPhase > 2200) RelayCancel(ctx, base, "the pass to the inbounder did not arrive in 2.2 s");
    }
}
PPC_FUNC(sub_82133240)
{
    uint32_t strat = ctx.r3.u32, team = ctx.r4.u32, P = ctx.r5.u32;
    __imp__sub_82133240(ctx, base);
    if (!RelayOn() || !Guest(team) || !Guest(P)) return;
    if (R32(P + kTeam) != team) { static int n = 0; if (n++ < 3) Log("[relay] team object mismatch: roles for team %08X, player %08X belongs to %08X - relay not used\n", team, P, R32(P + kTeam)); return; }
    { static int n = 0; if (n++ < 40) Log("[relay] roles handed out: team %08X, first player %08X, holder %08X, strategy id %u\n", team, P, Holder(ctx, base, team), StrategyOf(team)); }
    uint64_t now = NbashMs();
    if (g_msUntil.load() && now < g_msUntil.load()) return;                 // not at the start of the match
    RelayState& r = g_relay;
    uint32_t h = Holder(ctx, base, team);
    if (r.active && r.team == team)
    {
        r.strat = strat;
        if (h == r.I) { r.active = false; if (g_relayLog++ < 200) Log("[relay] inbounder %08X has the ball\n", r.I); return; }
        if (!h && P == r.P && r.phase == 1) { SendBehindBaseline(ctx, base, r.I); return; }       // roles were handed out again: keep ours
        if (h == r.P) { RelayPass(ctx, base, now); return; }      // the game has just made the fetcher the inbounder (46): our orders go on top
        r.active = false;
        if (g_relayLog++ < 200) Log("[relay] dropped (holder %08X, fetcher now %08X)\n", h, P);
    }
    if (h || Human(P)) return;        // somebody already has it, or you fetch it yourself: the normal inbound
    // the inbounder: a computer-controlled team mate, the one nearest to the baseline on the ball's side
    float bx = RF(P + 0x11270), by = RF(P + 0x11274), bz = RF(P + 0x11278);
    if (!Convert(ctx, base, CourtOf(P), true, bx, by, bz) || std::fabs(bx) < 7.0f) return;         // only for a ball near a baseline (after a basket)
    uint32_t n = R32(team + 0x10F00), best = 0; float bestV = -99;
    for (uint32_t i = 0; i < n && i < 5; i++)
    {
        uint32_t pl = R32(team + 0x10F04 + i * 4); float x, y, z;
        if (!Guest(pl) || pl == P || Human(pl) || !CourtPos(ctx, base, pl, x, y, z)) continue;
        float v = (bx > 0 ? x : -x); if (v > bestV) { bestV = v; best = pl; }
    }
    if (!best) return;
    r = RelayState{}; r.active = true; r.team = team; r.strat = strat; r.P = P; r.I = best; r.phase = 1; r.t0 = now;
    SendBehindBaseline(ctx, base, best);
    if (g_relayLog++ < 200) Log("[relay] ball loose at court x %.1f: %08X fetches it, %08X goes behind the baseline to inbound\n", bx, P, best);
}

// ---------------------------------------------------------------- user-controlled player
extern "C" PPC_FUNC(__imp__sub_820F6F48);
PPC_FUNC(sub_820F6F48)
{
    if (Any() && ctx.r4.u32 == 2 && ctx.r5.u32 == 32 && Guest(ctx.r6.u32))
    {
        if (g_user.exchange(ctx.r6.u32) != ctx.r6.u32 && g_logged < 300) { Log("[drivecam] user player = %08X\n", ctx.r6.u32); g_logged++; }
        g_userSeen = NbashMs();
    }
    if (Any() && ctx.r4.u32 == 2 && Guest(ctx.r6.u32))
    {
        if (ctx.r5.u32 == 46) { if (g_inbounder.exchange(ctx.r6.u32) != ctx.r6.u32 && g_logged < 300) { Log("[drivecam] inbounder = %08X\n", ctx.r6.u32); g_logged++; } }
        else if (g_inbounder.load() == ctx.r6.u32) g_inbounder = 0;
    }
    if (Any() && Guest(ctx.r6.u32))
    {
        uint32_t pl = ctx.r6.u32, id = ctx.r5.u32;
        if (ctx.r4.u32 == 7)
        {
            if (id == 124 || id == 125) { if (!g_practice.exchange(true)) Log("[drivecam] practice mode: the camera keeps facing the rim (no flip)\n"); }
            else if (id == 123) { if (g_practice.exchange(false)) Log("[drivecam] practice mode ended (inbound play)\n"); }   // practice players also get the normal offense / defense / carrier ids, only an inbound means a real game
            if (id == 121 || id == 125) { g_carrier = pl; g_carrierLost = 0; g_receiver = 0; }
            else if (g_carrier.load() == pl && !g_carrierLost.load()) g_carrierLost = NbashMs();
        }
        else if (ctx.r4.u32 == 2 && (id == 45 || id == 47)) { g_receiver = pl; g_receiverAt = NbashMs(); }
        Register(pl);
        // cAssignPickupBall 43, cAssignPickupDeadBall 44, cActionDiveForBall 72, cActionStealAttacker 79, cMoveToBall 97, cMoveToPickUpBall 110
        if (id == 44) { g_dead = pl; g_deadAt = NbashMs(); }
        if (ctx.r4.u32 == 2 && id == 46) g_dead = 0;
        if (ctx.r4.u32 == 2 && (id == 46 || id == 47 || id == 48) && g_practice.exchange(false)) Log("[drivecam] practice mode ended (inbound)\n");
        if (id == 43 || id == 44 || id == 72 || id == 79 || id == 97 || id == 110) { g_chaser = pl; g_chaserAt = NbashMs(); }
    }
    __imp__sub_820F6F48(ctx, base);
}

// ---------------------------------------------------------------- camera
extern "C" PPC_FUNC(__imp__sub_82164D20);
PPC_FUNC(sub_82164D20)
{
    uint32_t out = ctx.r7.u32, camSelf = ctx.r3.u32, camIface = ctx.r4.u32;
    __imp__sub_82164D20(ctx, base);
    if (!CamMode() || !Guest(out)) return;
    uint32_t u = g_user.load();
    if (!Guest(u)) return;
    float ux, uy, uz;
    if (!CourtPos(ctx, base, u, ux, uy, uz)) return;
    TrackPossession(ux);
    {   // 16 Nov: ball + nearest opponent for the controller layout (input.cpp)
        uint32_t car = g_carrier.load(); g_userHasBall = (car == u && !g_carrierLost.load()) ? 1 : 0;
        float mn = -1.0f; uint32_t myTeam = R32(u + kTeam);
        for (auto& a : g_players) { uint32_t pl = a.load(); float qx, qy, qz; if (pl != u && Guest(pl) && R32(pl + kTeam) != myTeam && CourtPos(ctx, base, pl, qx, qy, qz)) { float d = std::hypot(qx - ux, qz - uz); if (mn < 0 || d < mn) mn = d; } }
        g_oppDist = mn; g_oppAt = NbashMs();
    }

    float ex = RF(out), ey = RF(out + 4), ez = RF(out + 8), vx = RF(out + 16), vy = RF(out + 20), vz = RF(out + 24);
    uint64_t now = NbashMs();
    RelayWatch(ctx, base, now);
    static uint64_t lastLog = 0;
    if (g_logged < 300 && now - lastLog > 2500)
    {
        lastLog = now; g_logged++;
        Log("[drivecam] t=%llu game eye (%.2f %.2f %.2f) v2 (%.2f %.2f %.2f) s32..48 %.2f %.2f %.2f %.2f %.2f | user %08X pos (%.2f %.2f %.2f) defense %d attack %+d yaw %.2f\n",
            (unsigned long long)now, ex, ey, ez, vx, vy, vz, RF(out + 32), RF(out + 36), RF(out + 40), RF(out + 44), RF(out + 48), u, ux, uy, uz, NbashHumanOnDefense(), AttackSign(), g_yaw);
    }
    if (CamMode() != 2) return;
    int camOpt = NbashCamGet();
    static int lastOpt = -1; bool optChanged = camOpt != lastOpt; lastOpt = camOpt;

    static const float BACK0 = Env("NBASH_DC_BACK", 9.0f), HT0 = Env("NBASH_DC_HT", 4.0f), AHEAD0 = Env("NBASH_DC_AHEAD", 4.0f), LOOKH0 = Env("NBASH_DC_LOOKH", 1.2f);
    const float BACK = camOpt == 1 ? 11.5f : camOpt == 2 ? 7.0f : BACK0, HT = camOpt == 1 ? 6.5f : camOpt == 2 ? 3.2f : HT0;
    const float AHEAD = camOpt == 1 ? 3.0f : AHEAD0, LOOKH = camOpt == 1 ? 1.0f : LOOKH0;
    static const float TURN = Env("NBASH_DC_TURN", 1.0f);     // seconds for the half-circle when possession changes
    static const float FOLLOW = Env("NBASH_DC_FOLLOW", 3.0f); // how quickly the camera catches up with the followed player (1/s)
    // direction the camera looks along the court: towards the hoop the offence attacks.  The engine's offence/defence
    // signal flickers around rebounds and loose balls, so a change only counts once it has lasted 0.45 s.
    int def = NbashHumanOnDefense();
    static int stable = -1, pending = -1; static uint64_t pendingSince = 0;
    if (def >= 0 && !Guest(g_carrier.load()))
    {
        if (stable < 0) stable = def;
        if (def != stable) { if (pending != def) { pending = def; pendingSince = now; } else if (now - pendingSince > 450) { stable = def; pending = -1; } }
        else pending = -1;
    }
    // who to follow: the player with the ball; while a pass is in the air, the player it is going to (the eased follow makes
    // the camera travel with the ball); the user's own player only when nobody is known to have it
    bool loose = false, teamKnown = true; uint32_t follow = 0;
    bool shot = false;   // the ball has left the carrier and nobody is receiving a pass: a shot, lay-up or dunk
    {
        uint32_t f = g_carrier.load(), rcv = g_receiver.load(); uint64_t lost = g_carrierLost.load();
        bool pass = lost && Guest(rcv) && now - g_receiverAt.load() < 2500 && g_receiverAt.load() + 400 >= lost;
        if (pass) f = rcv;
        loose = lost && !pass;
        uint32_t ch = g_chaser.load(); uint64_t chAt = g_chaserAt.load();
        bool chase = loose && Guest(ch) && chAt + 300 >= lost && now - chAt < 3500;
        if (chase) { f = ch; teamKnown = false; }
        shot = loose && !chase && now - lost < 3000;
        follow = f;
        float fx, fy, fz;
        if (Guest(f) && CourtPos(ctx, base, f, fx, fy, fz))
        {
            ux = fx; uz = fz;
            int d2 = (R32(f + kTeam) == R32(u + kTeam)) ? 0 : 1;      // ball with the user's team = offence
            if (!teamKnown) d2 = stable < 0 ? d2 : stable;            // nobody has the ball yet: the camera keeps its direction
            if (stable < 0) stable = d2;
            static int pend2 = -1; static uint64_t since2 = 0;
            if (d2 != stable) { if (pend2 != d2) { pend2 = d2; since2 = now; } else if (now - since2 > 300) { stable = d2; pend2 = -1; } } else pend2 = -1;
        }
    }
    // the ball: the game's own camera asks its subject interface (r4; virtual +8: (out, this, subject, kind)) for three
    // vectors each frame (kinds 15, 16, 17).  Which of them is the ball, and whether it is in court or world coordinates,
    // is worked out while playing: the ball is always with the carrier while somebody has it, and away from everybody
    // at some point of a pass or a shot.
    float ballUp = 0;
    static float ballX = 0, ballY = 0, ballZ = 0; static bool ballOk = false;
    static uint64_t scoredAt = 0; static float scoreSide = 0; static bool scoreInbSeen = false;
    {
        static uint32_t scr = 0; if (!scr) scr = AllocSystem(64, 16);
        struct Cand { int nr = 0, tot = 0; float away = 0; };
        static Cand cand[3][2]; static int bk = -1, bm = -1;
        float v[3][2][3] = {}; bool ok[3][2] = {};
        uint32_t subj = Guest(camSelf) ? R32(camSelf + 0x125D0) : 0, vt = Guest(camIface) ? R32(camIface) : 0, fn = Guest(vt) ? R32(vt + 8) : 0;
        uint32_t court = CourtOf(u);
        if (fn >= 0x82000000 && fn < 0x82600000)
            for (int k = 0; k < 3; k++)
            {
                memset(g_base + scr, 0, 32);
                PPCContext saved = ctx;
                ctx.r3.u64 = scr; ctx.r4.u64 = camIface; ctx.r5.u64 = subj; ctx.r6.u64 = 15 + k;
                PPC_CALL_INDIRECT_FUNC(fn);
                uint32_t r = ctx.r3.u32; ctx = saved;
                if (!Guest(r)) r = scr;
                float x = RF(r), y = RF(r + 4), z = RF(r + 8);
                if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
                v[k][0][0] = x; v[k][0][1] = y; v[k][0][2] = z; ok[k][0] = std::fabs(x) < 30 && std::fabs(z) < 25 && y > -1 && y < 12;
                float cx2 = x, cy2 = y, cz2 = z;
                if (Convert(ctx, base, court, true, cx2, cy2, cz2)) { v[k][1][0] = cx2; v[k][1][1] = cy2; v[k][1][2] = cz2; ok[k][1] = std::fabs(cx2) < 30 && std::fabs(cz2) < 25 && cy2 > -1 && cy2 < 12; }
            }
        uint32_t car = g_carrier.load(); uint64_t lost = g_carrierLost.load(); float px, py, pz;
        if (bk < 0)
        {
            if (Guest(car) && !lost && CourtPos(ctx, base, car, px, py, pz))
            {
                for (int k = 0; k < 3; k++) for (int m = 0; m < 2; m++)
                { Cand& c = cand[k][m]; c.tot++; if (ok[k][m] && std::hypot(v[k][m][0] - px, v[k][m][2] - pz) < 1.6f) c.nr++; }
            }
            else if (lost && now - lost < 2500)
            {
                for (int k = 0; k < 3; k++) for (int m = 0; m < 2; m++)
                {
                    if (!ok[k][m]) continue;
                    float mn = 99;
                    for (auto& a : g_players) { uint32_t pl = a.load(); if (Guest(pl) && CourtPos(ctx, base, pl, px, py, pz)) { float d = std::hypot(v[k][m][0] - px, v[k][m][2] - pz); if (d < mn) mn = d; } }
                    if (mn < 90 && mn > cand[k][m].away) cand[k][m].away = mn;
                }
            }
            for (int k = 0; k < 3 && bk < 0; k++) for (int m = 0; m < 2; m++)
            {
                Cand& c = cand[k][m];
                if (c.tot >= 60 && c.nr > c.tot * 0.9f && c.away > 2.0f)
                { bk = k; bm = m; Log("[drivecam] BALL found: subject vector kind %d in %s coordinates (with the carrier %d of %d frames, up to %.1f m from every player when loose)\n", 15 + k, m ? "world" : "court", c.nr, c.tot, c.away); break; }
            }
        }
        {   // 13 Nov 06:00: FLIGHT LOG - after every shoot press by the ball handler: the real in-game height of his hips (subject vector 15) and of the ball (16),
            // every frame for 1.9 s.  No pictures, no cost; this is what the clips are checked against (take-off, top, landing, when the ball comes up).
            static uint64_t cur = 0, seen = 0; static int n = 0, flights = 0; static char buf[2000]; static int len = 0; static float x0 = 0, z0 = 0; static uint32_t hh = 0;
            uint64_t pr = NbashShotPressMs();
            if (pr && pr != seen && now >= pr && now - pr < 250) { seen = pr; if (flights < 120 && Guest(car) && !lost && car == u) { cur = pr; n = 0; len = 0; flights++; x0 = v[0][0][0]; z0 = v[0][0][2]; hh = NbashShotHolderHash(); Log("[flight] %d: shoot press at t=%llu, character %08X (0 = an original player); columns = ms after the press : hip height / ball height / hips x / hips z from the start (m); he stood at %.2f %.2f\n", flights, (unsigned long long)pr, hh, x0, z0); } }
            if (cur)
            {
                if (now - cur <= 1900 && n < 130) { n++; len += snprintf(buf + len, sizeof buf - size_t(len), " %d:%.3f/%.2f/%.3f/%.3f", int(now - cur), v[0][0][1], v[1][0][1], v[0][0][0] - x0, v[0][0][2] - z0); }
                if (len > 1700 || now - cur > 1900 || n >= 130) { if (len) Log("[flight] %d:%s\n", flights, buf); len = 0; buf[0] = 0; }
                if (now - cur > 1900 || n >= 130) { Log("[flight] %d: end (button held %u ms)\n", flights, NbashShotHoldMs()); cur = 0; }
            }
        {   // 14 Nov: TRICK LOG - after every right-stick dribble gesture of the ball handler: hip height / ball height / hips x,z every frame for 1.3 s.
            // Compared offline with the game's trick clips (cloud drib/tricklib.json) this tells WHICH clip the gesture played.
            static uint64_t tcur = 0, tseen = 0; static int tn = 0, tricks = 0; static char tb[2000]; static int tl = 0; static float tx0 = 0, tz0 = 0;
            uint64_t pr = g_trickMs.load();
            if (pr && pr != tseen && now >= pr && now - pr < 250) { tseen = pr; if (tricks < 150 && Guest(car) && !lost && car == u) { tcur = pr; tn = 0; tl = 0; tricks++; tx0 = v[0][0][0]; tz0 = v[0][0][2]; Log("[trick] %d: %s at t=%llu, character %08X; columns = ms after : hip height / ball height / ball x / ball z from his start / hips x / hips z (m)\n", tricks, g_trickName, (unsigned long long)pr, NbashShotHolderHash()); } }
            { extern void NbashSignalTrace(int); static uint64_t sg = 0; if (tcur) sg = tcur; NbashSignalTrace(sg && now - sg <= 1600 ? int(now - sg) : -1); }
            { extern void NbashBallPinOverride(int, bool); uint64_t tm = g_trickMs.load(); bool sgn = !strncmp(g_trickName, "signature", 9); NbashBallPinOverride(tm && now >= tm && now - tm <= 1700 ? int(now - tm) : -1, sgn); }
            if (tcur)
            {
                if (now - tcur <= 1700 && tn < 110) { tn++; tl += snprintf(tb + tl, sizeof tb - size_t(tl), " %d:%.3f/%.2f/%.2f/%.2f/%.2f/%.2f", int(now - tcur), v[0][0][1], v[1][0][1], v[1][0][0] - tx0, v[1][0][2] - tz0, v[0][0][0] - tx0, v[0][0][2] - tz0); }
                if (tl > 1700 || now - tcur > 1700 || tn >= 110) { if (tl) Log("[trick] %d:%s\n", tricks, tb); tl = 0; tb[0] = 0; }
                if (now - tcur > 1700 || tn >= 110) tcur = 0;
            }
        }
        }
        static uint64_t lastB = 0;
        if (now - lastB > 2500 && g_logged < 300)
        {
            lastB = now;
            Log("[drivecam] subject %08X kinds 15/16/17: (%.2f %.2f %.2f) (%.2f %.2f %.2f) (%.2f %.2f %.2f) | follow point (%.2f %.2f) carrier %08X lost %d | near/tot/away court %d/%d/%.1f %d/%d/%.1f %d/%d/%.1f world %d/%d/%.1f %d/%d/%.1f %d/%d/%.1f\n",
                subj, v[0][0][0], v[0][0][1], v[0][0][2], v[1][0][0], v[1][0][1], v[1][0][2], v[2][0][0], v[2][0][1], v[2][0][2], ux, uz, car, lost ? 1 : 0,
                cand[0][0].nr, cand[0][0].tot, cand[0][0].away, cand[1][0].nr, cand[1][0].tot, cand[1][0].away, cand[2][0].nr, cand[2][0].tot, cand[2][0].away,
                cand[0][1].nr, cand[0][1].tot, cand[0][1].away, cand[1][1].nr, cand[1][1].tot, cand[1][1].away, cand[2][1].nr, cand[2][1].tot, cand[2][1].away);
        }
        // once the ball is known: whenever nobody holds it (pass, shot, lay-up, dunk, steal, loose ball) the camera follows the ball itself
        // a made basket: the loose ball drops through the ring (court x = +-10.2 .. 10.5, z = 0, 3.05 m high)
        {
            bool okb = bk >= 0 && ok[bk][bm];
            float bx = okb ? v[bk][bm][0] : 0, by = okb ? v[bk][bm][1] : 0, bz = okb ? v[bk][bm][2] : 0;
            if (okb && ballOk && lost && std::fabs(bz) < 0.32f && std::fabs(std::fabs(bx) - 10.3f) < 0.4f && by < ballY && ballY >= 2.75f && by < 2.95f && by > 2.0f && now - scoredAt > 3000)
            {
                if (!g_practice.load()) { scoredAt = now; scoreSide = bx > 0 ? 1.0f : -1.0f; scoreInbSeen = false; }   // practice: no behind-the-baseline view after a made shot
                if (g_logged < 300)
                {
                    g_logged++; char line[400]; int n = snprintf(line, sizeof line, "[drivecam] BASKET at court x %.1f; players (distance to the ball):", bx);
                    for (auto& a : g_players) { uint32_t pl = a.load(); float qx, qy, qz; if (Guest(pl) && CourtPos(ctx, base, pl, qx, qy, qz)) n += snprintf(line + n, sizeof line - n, " %08X%s %.1f", pl, R32(pl + kTeam) == R32(u + kTeam) ? "u" : "c", std::hypot(qx - bx, qz - bz)); }
                    Log("%s\n", line);
                }
            }
            ballOk = okb; if (okb) { ballX = bx; ballY = by; ballZ = bz; }
        }
        static float upS = 0;
        bool useBall = bk >= 0 && ok[bk][bm] && lost;
        if (useBall)
        {
            ux = v[bk][bm][0]; uz = v[bk][bm][2]; shot = false;
            if (uz > 8.5f) uz = 8.5f; if (uz < -8.5f) uz = -8.5f; if (ux > 13.0f) ux = 13.0f; if (ux < -13.0f) ux = -13.0f;
        }
        float wantUp = useBall ? (v[bk][bm][1] - 1.5f) * 0.55f : 0.0f; if (wantUp < 0) wantUp = 0; if (wantUp > 1.8f) wantUp = 1.8f;
        upS += (wantUp - upS) * 0.08f; ballUp = upS;
    }
    float target = (((stable == 1) ? -AttackSign() : AttackSign()) > 0) ? 0.0f : 3.14159265f;
    {   // practice mode only: no flip - always look at the rim on the half of the court that is being used
        static int rim = 0; static uint64_t otherSince = 0;
        if (!g_practice.load()) rim = 0;
        else
        {
            int side = ux > 0.5f ? 1 : ux < -0.5f ? -1 : 0; uint64_t t = NbashMs();
            if (!rim) rim = side ? side : 1;
            if (side == -rim && std::fabs(ux) > 3.0f) { if (!otherSince) otherSince = t; else if (t - otherSince > 4000) { rim = side; otherSince = 0; } }   // play really moved to the other basket
            else otherSince = 0;
            target = rim > 0 ? 0.0f : 3.14159265f;
        }
    }
    // shot / lay-up / dunk: the camera keeps its distance (no zoom) and just travels with the ball to the hoop
    float lookUp = 0;
    {
        static float shotW = 0;
        float hx = std::cos(target) * 10.2f;
        bool on = shot && std::fabs(hx - ux) < 9.5f;
        float dt0 = 0.016f; shotW += ((on ? 1.0f : 0.0f) - shotW) * (1.0f - std::exp(-(on ? 2.5f : 5.0f) * dt0));
        ux += (hx - ux) * 0.8f * shotW; uz += (0.0f - uz) * 0.8f * shotW; lookUp = 1.3f * shotW;
    }
    // inbound: a view from the corner behind the baseline, so the inbounder and the court he passes into are both in the picture
    uint32_t inb = g_inbounder.load(); float ix, iy, iz; bool inbView = false;
    if (!g_practice.load() && Guest(inb) && CourtPos(ctx, base, inb, ix, iy, iz) && std::fabs(ix) > 6.0f)
    {
        ux = ix; uz = iz; target = ix < 0 ? 0.0f : 3.14159265f; inbView = true;
        // a team inbounds behind the baseline it defends, so this settles which way each team attacks
        if (std::fabs(ix) > 9.5f)
        {
            int a = ((R32(inb + kTeam) == R32(u + kTeam)) == (ix < 0)) ? 6 : -6;
            g_dirLocked = true;
            if (g_votes.exchange(a) != a && g_logged < 300) { Log("[drivecam] attack direction of the user's team = %+d (inbounder %08X at court x %.1f)\n", a > 0 ? 1 : -1, inb, ix); g_logged++; }
        }
    }
    // after a basket the camera already goes behind that baseline while the ball is being fetched: one move instead of two
    bool realInb = inbView;
    // after a made basket the camera sits behind that baseline from the moment the ball is fetched until it is passed in,
    // and looks at whoever has the ball (or at the ball itself while it is loose or thrown to the inbounder)
    bool scoreView = false;
    if (scoredAt)
    {
        if (realInb) scoreInbSeen = true;
        uint32_t car = g_carrier.load(); bool held = Guest(car) && !g_carrierLost.load(); float qx = 0, qy = 0, qz = 0;
        bool heldPos = held && CourtPos(ctx, base, car, qx, qy, qz);
        bool over = g_practice.load() || now - scoredAt > 12000 || (scoreInbSeen && !realInb) || (!scoreInbSeen && now - scoredAt > 2500 && heldPos && qx * scoreSide < 6.0f);
        if (over) { if (g_logged < 300) { Log("[drivecam] basket view ended after %llu ms (%s)\n", (unsigned long long)(now - scoredAt), scoreInbSeen ? "passed in" : "play went on"); g_logged++; } scoredAt = 0; }
        else if (!realInb)
        {
            if (heldPos) { ix = qx; iz = qz; } else if (ballOk) { ix = ballX; iz = ballZ; } else { ix = scoreSide * 10.5f; iz = 0; }
            if (ix * scoreSide < 4.0f) ix = scoreSide * 4.0f;
            ux = ix; uz = iz; target = scoreSide < 0 ? 0.0f : 3.14159265f; inbView = true; scoreView = true;
        }
        else scoreView = true;
    }
    if (!inbView)
    {
        uint32_t dp = g_dead.load();
        if (Guest(dp) && now - g_deadAt.load() < 6000 && CourtPos(ctx, base, dp, ix, iy, iz) && std::fabs(ix) > 7.5f)
        { ux = ix; uz = iz; target = ix < 0 ? 0.0f : 3.14159265f; inbView = true; }
    }
    static uint32_t lastInb = 0; static float lix = 0, liz = 0;
    if (realInb) { lastInb = inb; lix = ix; liz = iz; }
    else if (lastInb) { if (g_logged < 300) { Log("[drivecam] inbound passed in from court x %.1f z %.1f\n", lix, liz); g_logged++; } lastInb = 0; }
    // match start: the inbounder's walk from the side line to the baseline is fast-forwarded (game clock x5) until he stands still there
    if (uint64_t arm = g_ffArm.load())
    {
        static float px = 0, pz = 0; static int still = 0; static bool seen = false; static int frames = 0;
        bool done = now - arm > 5000; frames++;
        if (realInb)
        {
            seen = true;
            float m = std::fabs(ix - px) + std::fabs(iz - pz); px = ix; pz = iz;
            still = (m < 0.01f && std::fabs(ix) > 9.5f) ? still + 1 : 0;
            if (still > 5) done = true;
        }
        else if (seen && !Guest(inb)) done = true;
        if (done) { Log("[drivecam] match-start inbound: fast-forward ended after %llu ms, %d frames (inbounder court x %.1f z %.1f)\n", (unsigned long long)(now - arm), frames, px, pz); g_ffArm = 0; g_nbashFastUntil = 0; seen = false; still = 0; frames = 0; }
        else g_nbashFastUntil = now + 150;
    }
    // match start: the inbounder is PLACED on his spot behind the basket.  His position is searched in the whole game
    // memory as single numbers (world x, world z, court z) while he stands still; once he has taken his first step only the
    // addresses that followed him are kept (constants drop out), and those are set to the spot for half a second.
    if (uint64_t arm = g_tpArm.load())
    {
        static std::vector<uint32_t> LX, LZ, LC; static int phase = 0, pokes = 0, waited = 0; static float x0, z0, c0; static bool fx = false, fz = false;
        bool end = now - arm > 9000;
        float cxw = 0, czw = 0, ccx = 0, ccy = 0, ccz = 0;
        bool have = !end && Guest(inb) && g_tpHave.load();
        if (have) { cxw = RF(inb + kPos); czw = RF(inb + kPos + 8); ccx = cxw; ccy = RF(inb + kPos + 4); ccz = czw; have = Convert(ctx, base, CourtOf(inb), true, ccx, ccy, ccz); }
        if (have)
        {
            float tx = g_tpX, tz = g_tpZ, tcx = tx, tcy = 0, tcz = tz; Convert(ctx, base, CourtOf(inb), true, tcx, tcy, tcz);
            float dist = std::hypot(cxw - tx, czw - tz);
            if (phase == 0)
            {
                LX.clear(); LZ.clear(); LC.clear(); x0 = cxw; z0 = czw; c0 = ccz; fx = fz = false; pokes = 0; waited = 0;
                uint64_t scanned = 0; uint64_t t0 = NbashMs();
#ifdef _WIN32
                for (uint64_t a = 0x90000000ull; a < 0xD0000000ull;)
                {
                    MEMORY_BASIC_INFORMATION mi;
                    if (!VirtualQuery(g_base + a, &mi, sizeof mi)) break;
                    uint64_t e = uint64_t((uint8_t*)mi.BaseAddress + mi.RegionSize - g_base); if (e > 0xD0000000ull) e = 0xD0000000ull;
                    if (mi.State == MEM_COMMIT && (mi.Protect & PAGE_READWRITE) && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && !(a >= 0x82000000ull && a < 0x82800000ull))
                    {
                        scanned += e - a;
                        const uint32_t* w = (const uint32_t*)(g_base + (a & ~3ull)); size_t n = size_t((e - (a & ~3ull)) / 4);
                        for (size_t k = 0; k < n; k++)
                        {
                            uint32_t raw = w[k]; if (!raw) continue;
                            uint32_t v = __builtin_bswap32(raw); float f; memcpy(&f, &v, 4);
                            if (std::fabs(f - cxw) < 1e-4f) { if (LX.size() < 20000) LX.push_back(uint32_t((a & ~3ull) + k * 4)); }
                            else if (std::fabs(f - czw) < 1e-4f) { if (LZ.size() < 20000) LZ.push_back(uint32_t((a & ~3ull) + k * 4)); }
                            else if (std::fabs(f - ccz) < 1e-4f) { if (LC.size() < 20000) LC.push_back(uint32_t((a & ~3ull) + k * 4)); }
                        }
                    }
                    if (e <= a) break;
                    a = e;
                }
#endif
                Log("[drivecam] placing: inbounder %08X world (%.4f, %.4f) court (%.4f, %.4f); spot world (%.2f, %.2f) court (%.2f, %.2f), %.1f m away; searched %llu MB in %llu ms: %d / %d / %d numbers equal world x / world z / court z\n",
                    inb, cxw, czw, ccx, ccz, tx, tz, tcx, tcz, dist, (unsigned long long)(scanned >> 20), (unsigned long long)(NbashMs() - t0), int(LX.size()), int(LZ.size()), int(LC.size()));
                phase = 1;
            }
            else if (phase == 1)
            {
                // keep what moved with him
                waited++;
                auto filter = [&](std::vector<uint32_t>& v, float was, float is, const char* name)
                {
                    std::vector<uint32_t> keep;
                    for (uint32_t a : v) { float f = RF(a); if (std::fabs(f - is) < 0.06f && std::fabs(f - was) > 0.05f) keep.push_back(a); }
                    v.swap(keep);
                    Log("[drivecam] placing: %d numbers follow his %s (%.3f -> %.3f)\n", int(v.size()), name, was, is);
                    for (size_t k = 0; k < v.size() && k < 40; k++) { if (v[k] - inb < 0x140C0) Log("[drivecam]     player+%05X\n", v[k] - inb); else Log("[drivecam]     %08X\n", v[k]); }
                };
                if (!fx && std::fabs(cxw - x0) > 0.12f) { filter(LX, x0, cxw, "world x"); fx = true; }
                static bool fc = false; if (waited == 1) fc = false;
                if (!fc && std::fabs(ccz - c0) > 0.12f) { filter(LC, c0, ccz, "court z"); fc = true; }
                if (waited > 45 && !fc) LC.clear();
                if (!fz && std::fabs(czw - z0) > 0.12f) { filter(LZ, z0, czw, "world z"); fz = true; }
                if ((fx && fz && fc) || waited > 45)
                {
                    if (!fx) LX.clear(); if (!fz) LZ.clear(); if (!fc) LC.clear();
                    phase = 2;
                }
            }
            if (phase == 2)
            {
                if (pokes < 30 && LX.size() + LZ.size() + LC.size() < 3000)
                {
                    for (uint32_t a : LX) WF(a, tx);
                    for (uint32_t a : LZ) WF(a, tz);
                    for (uint32_t a : LC) WF(a, tcz);
                    if (pokes < 8 || pokes == 29) Log("[drivecam]   placing %d: inbounder %.2f m from his spot (court %.2f, %.2f)\n", pokes, dist, ccx, ccz);
                    pokes++;
                }
                else { end = true; Log("[drivecam] placing finished: inbounder %.2f m from his spot -> %s\n", dist, dist < 1.0f ? "PLACED" : "not placed"); }
            }
        }
        if (end) { g_tpArm = 0; g_tpHave = false; phase = 0; LX.clear(); LZ.clear(); LC.clear(); }
    }

    float dt = (g_lastCam && now - g_lastCam < 200) ? float(now - g_lastCam) / 1000.0f : 0.016f; g_lastCam = now;
    static float sx = 0, sz = 0, side = 1.0f;
    if (!g_yawInit) { g_yaw = target; g_yawInit = true; sx = ux; sz = uz; }
    // a real rotation: the camera goes round the player at a steady rate (eased at both ends), always on the same side
    {
        float d = target - g_yaw, step = (3.14159265f / TURN) * dt;
        float ease = std::fabs(d) < 0.5f ? 0.35f + 1.3f * std::fabs(d) : 1.0f;
        step *= ease;
        if (std::fabs(d) <= step) g_yaw = target; else g_yaw += d > 0 ? step : -step;
    }
    float kp = 1.0f - std::exp(-FOLLOW * dt);
    sx += (ux - sx) * kp; sz += (uz - sz) * kp;
    float dx = std::cos(g_yaw), dz = std::sin(g_yaw);
    // zoom: the camera backs away (and rises a little) just enough to keep the ball handler's team mates in the picture
    // (an alley-oop runner, a cutter); while the ball is loose - a steal, a rebound - it keeps everybody in the picture
    static float back = 9.0f; if (optChanged) back = BACK;
    {
        static const float ZMAX = Env("NBASH_DC_ZOOMMAX", 14.5f), ZLOOSE = Env("NBASH_DC_ZOOMLOOSE", 17.0f);
        float need = BACK; uint32_t ft = Guest(follow) ? R32(follow + kTeam) : 0;
        if (!inbView)
            for (auto& a : g_players)
            {
                uint32_t pl = a.load(); float px, py, pz;
                if (!Guest(pl) || pl == follow) continue;
                if (!loose && R32(pl + kTeam) != ft) continue;
                if (!CourtPos(ctx, base, pl, px, py, pz)) continue;
                float rx = px - sx, rz = pz - sz, along = rx * dx + rz * dz, lat = std::fabs(-rx * dz + rz * dx);
                if (along < -7.0f) continue;                       // far behind the play: not worth zooming out for
                float n = lat / 0.72f - along; if (-along + 3.0f > n) n = -along + 3.0f;
                if (n > need) need = n;
            }
        float mx = loose ? ZLOOSE : ZMAX; if (need > mx) need = mx;
        back += (need - back) * (1.0f - std::exp(-(need > back ? 2.2f : 1.0f) * dt));
    }
    float nex = sx - dx * back, nez = sz * 0.6f - dz * back, ney = HT + 0.35f * (back - BACK);
    // never look through a backboard: behind a baseline the camera moves to the side of the hoop, a little higher, and not far out
    if (nex > 14.5f) nex = 14.5f; if (nex < -14.5f) nex = -14.5f;
    float over = std::fabs(nex) - 10.0f;
    if (sz > 1.5f) side = 1.0f; else if (sz < -1.5f) side = -1.0f;        // keeps its side until the player is clearly on the other one
    if (over > 0)
    {
        float w = over > 1.5f ? 1.0f : over / 1.5f;
        float zs = side * (std::fabs(nez) > 3.4f ? std::fabs(nez) : 3.4f);
        nez += (zs - nez) * w; ney += 0.8f * w;
    }
    if (nez > 9.0f) nez = 9.0f; if (nez < -9.0f) nez = -9.0f;
    float lx = sx + dx * AHEAD, ly = LOOKH + lookUp + ballUp, lz = sz * 0.85f + dz * AHEAD;
    if (inbView)
    {
        // centred behind the baseline, well back and high, looking up the court: the inbounder and everybody he can pass to
        float so = ix > 0 ? 1.0f : -1.0f, ax = 11.8f;
        static const float ID = Env("NBASH_DC_INB_BACK", 7.0f), IH = Env("NBASH_DC_INB_HT", 6.8f), IC = Env("NBASH_DC_INB_CENTER", 0.5f);
        static const float IF = Env("NBASH_DC_INB_FOCUS", 0.65f);     // 1 = looks straight at the player with the ball, 0 = at the court in front of him
        nex = so * (ax + ID); ney = IH; nez = iz * IC;
        if (nex > 21.0f) nex = 21.0f; if (nex < -21.0f) nex = -21.0f;
        lx = so * 5.0f + (ix - so * 5.0f) * IF; ly = 1.0f; lz = iz * (IC + (1.0f - IC) * IF);
        (void)scoreView;
        // the orbit carries on from this view when the ball is passed in
        g_yaw = std::atan2(lz - nez, lx - nex);
        while (target - g_yaw > 3.14159265f) g_yaw += 6.2831853f;
        while (g_yaw - target > 3.14159265f) g_yaw -= 6.2831853f;
    }
    // the final camera position itself is eased too, so nothing ever jumps
    static float cx = 0, cy = 0, cz = 0, clx = 0, cly = 0, clz = 0; static bool cInit = false;
    if (!cInit) { cx = nex; cy = ney; cz = nez; clx = lx; cly = ly; clz = lz; cInit = true; }
    // going into and out of the inbound view is one slow glide; the normal follow tightens up again over two seconds
    static bool wasInb = false; static uint64_t modeAt = 0;
    if (inbView != wasInb) { wasInb = inbView; modeAt = now; }
    float ramp = float(now - modeAt) / 2000.0f; if (ramp > 1) ramp = 1;
    float kc = 1.0f - std::exp(-(1.6f + 4.4f * ramp * ramp) * dt);
    cx += (nex - cx) * kc; cy += (ney - cy) * kc; cz += (nez - cz) * kc; clx += (lx - clx) * kc; cly += (ly - cly) * kc; clz += (lz - clz) * kc;
    nex = cx; ney = cy; nez = cz; lx = clx; ly = cly; lz = clz;

    // the game's second vector: a look-at point, or (look - eye) (possibly unit length)?
    float gdx = vx, gdy = vy, gdz = vz, len = std::sqrt(gdx * gdx + gdy * gdy + gdz * gdz);
    static int kind = -1;   // 0 absolute point, 1 look - eye, 2 unit direction
    if (kind < 0 && len > 0.01f)
    {
        float ay = ey + vy;                               // if v2 is (look - eye), eye.y + v2.y is a look height (0..3 m) and v2.y is negative
        kind = (std::fabs(len - 1.0f) < 0.02f) ? 2 : (vy < -0.3f && ay > -0.5f && ay < 3.5f) ? 1 : 0;
        Log("[drivecam] result vector 2 is %s (eye %.2f %.2f %.2f, v2 %.2f %.2f %.2f)\n", kind == 2 ? "a unit direction" : kind == 1 ? "look - eye" : "a look-at point", ex, ey, ez, vx, vy, vz);
    }
    if (kind < 0) return;
    CamMenuFrame();
    if (camOpt >= 3) return;      // Classic / Sideline / Spectator: everything above keeps running (so switching back is seamless) but the game's own camera is left alone
    WF(out, nex); WF(out + 4, ney); WF(out + 8, nez);
    // same lens all the time: the game widens it and tilts the picture when the gamebreaker meter is full (+32 = field of view, +36 = roll)
    { static const float FOVV = Env("NBASH_DC_FOV", 0.54f); if (FOVV > 0) { WF(out + 32, FOVV); WF(out + 36, 0.0f); } }
    float ox = lx - nex, oy = ly - ney, oz = lz - nez;
    if (kind == 0) { WF(out + 16, lx); WF(out + 20, ly); WF(out + 24, lz); }
    else
    {
        if (kind == 2) { float l = std::sqrt(ox * ox + oy * oy + oz * oz); if (l > 0.001f) { ox /= l; oy /= l; oz /= l; } }
        WF(out + 16, ox); WF(out + 20, oy); WF(out + 24, oz);
    }
}

// ---------------------------------------------------------------- inbound from the baseline
// sub_820F6FF0(?, strategy id, team): 27 = cStrategyInboundOffense, 28 = cStrategyInboundDefense
extern "C" PPC_FUNC(__imp__sub_820F6FF0);
PPC_FUNC(sub_820F6FF0)
{
    if (Guest(ctx.r5.u32)) NoteStrategy(ctx.r5.u32, ctx.r4.u32);
    if (Baseline() && (ctx.r4.u32 == 27 || ctx.r4.u32 == 28) && Guest(ctx.r5.u32))
    {
        uint32_t t = R32(ctx.r5.u32 + kInboundType);
        static const bool skip = !getenv("NBASH_INBOUND_SKIPWALK") || *getenv("NBASH_INBOUND_SKIPWALK") != '0';
        static const bool ff = getenv("NBASH_INBOUND_FF") != nullptr;   // fast-forward of the walk: only on request
        if (ff && t == 0 && skip && CamMode() == 2 && !g_ffArm.load()) { g_nbashFastSpeed = Env("NBASH_INBOUND_FF", 5.0f); g_ffArm = NbashMs(); }
        if (t == 0) g_msUntil = NbashMs() + 9000;                        // match start: the inbound spot is moved behind the basket (sub_82102000 hook)
        static const bool tp = !getenv("NBASH_INBOUND_TP") || *getenv("NBASH_INBOUND_TP") != '0';
        if (tp && t == 0 && skip && CamMode() == 2 && !g_tpArm.load()) { g_tpHave = false; g_tpArm = NbashMs(); }
        if (t != 1) { W32(ctx.r5.u32 + kInboundType, 1); if (g_logged < 300) { Log("[drivecam] inbound type %u (side line) -> 1 (behind the baseline), team %08X strategy %u\n", t, ctx.r5.u32, ctx.r4.u32); g_logged++; } }
    }
    __imp__sub_820F6FF0(ctx, base);
}

// ---------------------------------------------------------------- match-start inbound spot behind the basket
// sub_82102000(?, player): the "behind the baseline" spot of cMoveToInbound.  It keeps the player's own court z (only
// staying 1.5 m clear of the basket support), so from the side line at half court he ends up in the corner.  During the
// match-start window his AI position is, for the length of this call only, replaced by one beside the basket: the spot
// then comes out 1.5 m from the basket support.  The spot is left in player+0x110E0 by the function.
extern "C" PPC_FUNC(__imp__sub_82102000);
PPC_FUNC(sub_82102000)
{
    uint32_t p = ctx.r4.u32; uint64_t until = g_msUntil.load();
    if (!(Baseline() && until && NbashMs() < until && Guest(p))) { __imp__sub_82102000(ctx, base); return; }
    uint32_t court = CourtOf(p);
    uint32_t o0 = R32(p + kPos), o1 = R32(p + kPos + 4), o2 = R32(p + kPos + 8);
    float x = RF(p + kPos), y = RF(p + kPos + 4), z = RF(p + kPos + 8), cz0 = 0;
    bool ok = Convert(ctx, base, court, true, x, y, z);
    if (ok) { cz0 = z; z = Env("NBASH_INBOUND_SIDE", 1.0f) * 0.4f; ok = Convert(ctx, base, court, false, x, y, z); }
    if (ok) { WF(p + kPos, x); WF(p + kPos + 8, z); }
    __imp__sub_82102000(ctx, base);
    W32(p + kPos, o0); W32(p + kPos + 4, o1); W32(p + kPos + 8, o2);
    if (ok)
    {
        float tx = RF(p + 0x110E0), ty = RF(p + 0x110E4), tz = RF(p + 0x110E8), sx = tx, sy = ty, sz = tz;
        Convert(ctx, base, court, true, sx, sy, sz);
        if (!g_tpHave.load()) Log("[drivecam] match-start inbound spot for %08X: court (%.2f, %.2f) instead of z %.2f (world %.2f, %.2f)\n", p, sx, sz, cz0, tx, tz);
        g_tpX = tx; g_tpZ = tz; g_tpHave = true;
    }
}

// ---------------------------------------------------------------- no full-court press
// sub_820EDBE8(player, player, f1 = x, f2 = z, ..): "walk / run to this spot" used by every cMoveTo* class
extern "C" PPC_FUNC(__imp__sub_820EDBE8);
PPC_FUNC(sub_820EDBE8)
{
    if (g_ovUntil.load() && NbashMs() < g_ovUntil.load() && Guest(ctx.r4.u32) && ctx.r4.u32 == g_inbounder.load())
    {
        // match start: his spot is moved from the corner to beside the basket support (court z = 1.2), as after a made basket
        float x = float(ctx.f1.f64), y = 0, z = float(ctx.f2.f64); uint32_t court = CourtOf(ctx.r4.u32);
        static const float UZ = Env("NBASH_INBOUND_Z", 1.2f);
        if (Convert(ctx, base, court, true, x, y, z) && std::fabs(x) > 10.0f)
        {
            float oz = z; z = UZ;
            if (Convert(ctx, base, court, false, x, y, z))
            {
                if (!g_tpHave.load()) Log("[drivecam] match-start inbound spot: court z %.1f -> %.1f (world %.2f, %.2f)\n", oz, UZ, x, z);
                ctx.f1.f64 = double(x); ctx.f2.f64 = double(z);
                g_tpX = x; g_tpZ = z; g_tpHave = true;
            }
        }
    }
    if (NoPress() && g_votes.load() != 0 && NbashHumanOnDefense() == 0)
    {
        uint32_t p = ctx.r4.u32, u = g_user.load();
        if (Guest(p) && Guest(u) && R32(p + kTeam) != R32(u + kTeam))
        {
            float a = float(AttackSign()), keep = Env("NBASH_NOPRESS_LINE", 1.5f);
            float x = float(ctx.f1.f64), y = 0, z = float(ctx.f2.f64); uint32_t court = CourtOf(p);
            if (Convert(ctx, base, court, true, x, y, z) && a * x < keep)      // target is in world coordinates; the half-court line is court x = 0
            {
                float ox = x; x = a * keep;
                if (Convert(ctx, base, court, false, x, y, z))
                {
                    static int n = 0; if (n++ < 12) Log("[drivecam] no press: computer player %08X target court x %.1f -> %.1f (world %.1f, %.1f)\n", p, ox, a * keep, x, z);
                    ctx.f1.f64 = double(x); ctx.f2.f64 = double(z);
                }
            }
        }
    }
    __imp__sub_820EDBE8(ctx, base);
}
