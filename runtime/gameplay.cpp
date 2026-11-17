// Gameplay changes made at the animation engine's pad signals (EA ANT).
//
// Pressing shoot turns on SigPAD_Shoot_Press/_Hold AND SigPAD_Dunk_Hold together (and on release
// SigPAD_Dunk_Release, SigCAL_UberDunk_Request); near the hoop the engine dunks, and how long the
// dunk press lasts picks the level: a very quick tap (~100 ms) is a layup, longer is a bigger dunk.
// "Dunk only with turbo" (NBASH_TURBODUNK=1): for a shoot press made without player 1's turbo
// (right trigger), if the engine's SigSIM_Dunk signal is on (it is going up for a dunk), the shoot
// button is let go for the game after NBASH_LAYUP_MS (default 70 ms) -- a quick tap, i.e. a layup.
// Jump shots (SigSIM_Dunk off) keep the real press. Turbo held: untouched, normal dunks.
// (Cutting only the dunk signals was not enough: the engine also ends the dunk on the shoot press.)
//
// How: the pad rule runner (sub_823E8298) looks each signal up with sub_823E5DC8(table, key) and
// then sets it to the value in r7; our hook clears r7 for the dunk keys. Keys are the third word
// of the signal asset objects, which sub_823E6130 returns by name.
#include "guest.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <map>
#include <vector>

void Log(const char* fmt, ...);
uint32_t NbashPadButtons();   // input.cpp: player 1 buttons | right trigger << 16
extern uint8_t* g_base;

uint64_t NbashMs();

namespace
{
    bool CiFind(const char* h, const char* n) { for (; *h; h++) { const char* a = h; const char* b = n; while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; } if (!*b) return true; } return false; }
    std::mutex g_lock;
    uint32_t g_keySimDunk = 0;     // key of SigSIM_Dunk
    uint32_t g_keyOnDefense = 0, g_keyOnOffense = 0;   // keys of SigSIM_OnDefense / SigSIM_OnOffense
    std::map<std::string, uint32_t> g_allKeys;        // every signal name -> key (signal trace)
    std::map<std::string, uint32_t> g_keysByName;     // other engine signals we read (see NbashPlayerSignal)
    // signal tables the pad rules have driven recently (the controller can move between players,
    // e.g. in pick-up games): table -> last time seen (ms)
    std::map<uint32_t, uint64_t> g_padTables;
    bool TurboDunk() { static const bool on = !(getenv("NBASH_TURBODUNK") && getenv("NBASH_TURBODUNK")[0] == '0'); return on; }
    uint32_t GR32(uint32_t a) { return __builtin_bswap32(*(uint32_t*)(g_base + a)); }
    // same lookup as the game's sub_823E5DC8: 305 buckets of 8 (key, value) slots
    uint32_t FindSignal(uint32_t table, uint32_t key)
    {
        uint32_t b = (key & 0x7FFFFFFF) % 305;
        for (uint32_t i = 0; i < 8; i++)
            if (GR32(table + 1240 + b * 64 + i * 8) == key) return GR32(table + 1236 + (b * 8 + i) * 8);
        return 0;
    }
}

// Is player 1's character starting/doing a dunk right now? (engine signal SigSIM_Dunk)
bool NbashSimDunkActive()
{
    std::vector<uint32_t> tables; uint32_t key;
    {
        std::lock_guard lk(g_lock); key = g_keySimDunk;
        uint64_t now = NbashMs();
        for (auto& [t, seen] : g_padTables) if (now - seen < 1000) tables.push_back(t);
    }
    if (!key) return false;
    for (uint32_t table : tables)   // any character the pad drives that is going up for a dunk
    {
        uint32_t inst = FindSignal(table, key);
        if (inst < 0x40000000 || inst >= 0xC0000000) continue;
        uint32_t v = GR32(inst + 4); float f; memcpy(&f, &v, 4);
        if (f > 0.5f) return true;
    }
    return false;
}

#ifndef NBASH_FIRSTCALL   // the diagnostic build has its own logging hooks on these
extern "C" PPC_FUNC(__imp__sub_823E6130);
PPC_FUNC(sub_823E6130)
{
    uint32_t name = ctx.r4.u32;
    __imp__sub_823E6130(ctx, base);
    char s[64] = {};
    for (int i = 0; i < 63; i++) { char c = char(g_base[name + i]); if (!c) break; s[i] = c; }
    uint32_t r = ctx.r3.u32;
    if (r < 0x40000000 || r >= 0xC0000000) return;
    uint32_t* slot = !strcmp(s, "SigSIM_Dunk") ? &g_keySimDunk : !strcmp(s, "SigSIM_OnDefense") ? &g_keyOnDefense : !strcmp(s, "SigSIM_OnOffense") ? &g_keyOnOffense : nullptr;
    if (!slot)
    {
        { std::lock_guard lk(g_lock); if (g_allKeys.size() < 600) g_allKeys[s] = GR32(r + 8); }      // 14 Nov: every named signal, for NbashSignalTrace
        static const bool dunkProbe = getenv("NBASH_DUNKPROBE") != nullptr;
        bool watch = dunkProbe && (CiFind(s, "dunk") || CiFind(s, "layup") || CiFind(s, "rating") || CiFind(s, "skill") ||
                                   CiFind(s, "overall") || CiFind(s, "player") || CiFind(s, "level") || CiFind(s, "uber") || CiFind(s, "style") || CiFind(s, "shot") || CiFind(s, "score") || CiFind(s, "point") || CiFind(s, "ball"));
        if (watch || !strcmp(s, "SigSIM_CarryBall") || !strcmp(s, "SigSIM_DunkGate_1") || !strcmp(s, "SigSIM_DunkGate_2") ||
            !strcmp(s, "SigSIM_Gamebreaker_Ready") || !strcmp(s, "SigSIM_Gamebreaker_Active") || !strcmp(s, "SigSIM_MasterSkill_Dunk") || !strncmp(s, "SigSIM_Class_", 13) || !strncmp(s, "SigSIM_DunkType_", 16) || !strncmp(s, "SigSIM_Trick", 12) || !strcmp(s, "SigSIM_MasterSkill_Trick") || !strncmp(s, "SigPAD_Trick", 12))
        { std::lock_guard lk(g_lock); g_keysByName[s] = GR32(r + 8); }
        return;
    }
    std::lock_guard lk(g_lock);
    uint32_t key = GR32(r + 8);
    if (*slot != key) { *slot = key; Log("[signals] %s key %08X\n", s, key); }
}

std::atomic<uint64_t> g_lastPadRules{0};   // when the pad rules last ran (only while playing)
extern "C" PPC_FUNC(__imp__sub_823E5DC8);
PPC_FUNC(sub_823E5DC8)
{
    uint32_t lr = uint32_t(ctx.lr), table = ctx.r3.u32;
    {   // 14 Nov: WHO looks up the ball-pin signals (the code that pins the ball to a hand)?  Each different caller chain is logged once.
        static uint32_t kl = 0, kr = 0; static std::set<uint64_t> seen; uint32_t key = ctx.r4.u32;
        if (!kl) { std::lock_guard lk(g_lock); auto a = g_allKeys.find("SigSIM_BallPinLeft"), b = g_allKeys.find("SigSIM_BallPinRight"); if (a != g_allKeys.end()) kl = a->second; if (b != g_allKeys.end()) kr = b->second; }
        if (kl && (key == kl || key == kr) && seen.size() < 40)
        {
            uint32_t chain[7] = {}; uint32_t fp = ctx.r1.u32;
            for (int i = 0; i < 7 && fp >= 0x40000000 && fp < 0xC0000000; i++) { uint32_t up = GR32(fp); if (up < 0x40000000 || up >= 0xC0000000) break; chain[i] = GR32(up - 8); fp = up; }
            uint64_t h = (uint64_t(lr) << 32) ^ chain[0] ^ (uint64_t(chain[1]) << 13) ^ (uint64_t(chain[2]) << 27);
            if (seen.insert(h).second) Log("[pincall] %s looked up from %08X <- %08X %08X %08X %08X %08X %08X | r5 %08X r6 %08X r7 %08X f1 %.3f t=%llu\n", key == kl ? "BallPinLeft" : "BallPinRight", lr, chain[0], chain[1], chain[2], chain[3], chain[4], chain[5], ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.f1.f64, (unsigned long long)NbashMs());
        }
    }
    __imp__sub_823E5DC8(ctx, base);
    if (lr == 0x823E85F0) g_lastPadRules = NbashMs();
    if (lr == 0x823E85F0)
    {
        std::lock_guard lk(g_lock);
        auto it = g_padTables.find(table);
        if (it == g_padTables.end()) Log("[turbodunk] pad drives signal table %08X\n", table);
        g_padTables[table] = NbashMs();
    }
}
#endif

// Value of an engine signal (by name, from the list captured above) for the player the pad drives
// most recently; -1 if unknown.
float NbashPlayerSignal(const char* name)
{
    uint32_t table = 0, key = 0; uint64_t best = 0;
    {
        std::lock_guard lk(g_lock);
        auto k = g_keysByName.find(name); if (k != g_keysByName.end()) key = k->second;
        for (auto& [t, seen] : g_padTables) if (seen > best) { best = seen; table = t; }
    }
    if (!table || !key) return -1.0f;
    uint32_t inst = FindSignal(table, key);
    if (inst < 0x40000000 || inst >= 0xC0000000) return -1.0f;
    uint32_t v = GR32(inst + 4); float f; memcpy(&f, &v, 4); return f;
}

// Is the player the pad drives (most recently) on defense? -1 = unknown, 0 = no, 1 = yes.
// From the engine's own SigSIM_OnDefense / SigSIM_OnOffense of that player.
int NbashHumanOnDefense()
{
    uint32_t table = 0, kd, ko; uint64_t best = 0;
    {
        std::lock_guard lk(g_lock); kd = g_keyOnDefense; ko = g_keyOnOffense;
        for (auto& [t, seen] : g_padTables) if (seen > best) { best = seen; table = t; }
    }
    if (!table || !kd) return -1;
    auto val = [&](uint32_t key) -> float {
        uint32_t inst = key ? FindSignal(table, key) : 0;
        if (inst < 0x40000000 || inst >= 0xC0000000) return -1.0f;
        uint32_t v = GR32(inst + 4); float f; memcpy(&f, &v, 4); return f; };
    float d = val(kd), o = val(ko);
    if (d > 0.5f) return 1;
    if (o > 0.5f || d == 0.0f) return 0;
    return -1;
}

// In a match, controlling a player? The engine's pad rules run only then (not in menus).
#ifndef NBASH_FIRSTCALL
bool NbashInMatch() { return NbashMs() - g_lastPadRules.load() < 250; }
#else
bool NbashInMatch() { return true; }
#endif

// signal table of the player the pad drives most recently (0 if none) -- used by the position probe
uint32_t NbashLatestPadTable()
{
    uint32_t table = 0; uint64_t best = 0;
    std::lock_guard lk(g_lock);
    for (auto& [t, seen] : g_padTables) if (seen > best) { best = seen; table = t; }
    return table;
}

// dunk probe (NBASH_DUNKPROBE=1): logs every watched engine signal that is non-zero for the controlled player
void NbashDumpSignals(const char* tag)
{
    std::vector<std::pair<std::string, uint32_t>> keys; uint32_t table = 0; uint64_t best = 0;
    {
        std::lock_guard lk(g_lock);
        for (auto& kv : g_keysByName) keys.push_back(kv);
        for (auto& [t, seen] : g_padTables) if (seen > best) { best = seen; table = t; }
    }
    if (!table) return;
    std::string line;
    for (auto& [name, key] : keys)
    {
        uint32_t inst = FindSignal(table, key);
        if (inst < 0x40000000 || inst >= 0xC0000000) continue;
        uint32_t v = GR32(inst + 4); float f; memcpy(&f, &v, 4);
        if (f != 0.0f) { char b[160]; snprintf(b, sizeof b, " %s=%g", name.c_str(), f); line += b; }
    }
    Log("[dunkprobe] t=%llu %s table %08X:%s\n", (unsigned long long)NbashMs(), tag, table, line.c_str());
}

// writes an engine signal of the player the pad drives most recently; returns the previous value (-1 if unknown)
float NbashSetPlayerSignal(const char* name, float value)
{
    uint32_t table = 0, key = 0; uint64_t best = 0;
    {
        std::lock_guard lk(g_lock);
        auto k = g_keysByName.find(name); if (k != g_keysByName.end()) key = k->second;
        for (auto& [t, seen] : g_padTables) if (seen > best) { best = seen; table = t; }
    }
    if (!table || !key) return -1.0f;
    uint32_t inst = FindSignal(table, key);
    if (inst < 0x40000000 || inst >= 0xC0000000) return -1.0f;
    uint32_t old = GR32(inst + 4); float f; memcpy(&f, &old, 4);
    uint32_t nv; memcpy(&nv, &value, 4); *(uint32_t*)(g_base + inst + 4) = __builtin_bswap32(nv);
    return f;
}

// Finds signals the game never looks up by name: scans guest memory for the name string, then for
// objects pointing at it whose key (third word) is present in the controlled player's signal table.
#include <windows.h>
#include <thread>
void NbashResolveSignalsByScan()
{
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    std::thread([] {
        const char* names[] = { "SigSIM_ShotAccuracy", "SigSIM_ShotPower", "SigTRM_ShotInAir", "SigSIM_ReboundSuccess", "SigSIM_Class_SmallMan" };
        uint32_t table = 0; uint64_t best = 0;
        { std::lock_guard lk(g_lock); for (auto& [t, seen] : g_padTables) if (seen > best) { best = seen; table = t; } }
        if (!table) { Log("[sigscan] no player table yet\n"); return; }
        uint64_t t0 = NbashMs();
        struct Region { uint32_t a, n; }; std::vector<Region> regs;
        for (uint64_t a = 0x40000000; a < 0xC0000000;)
        {
            MEMORY_BASIC_INFORMATION mi{};
            if (!VirtualQuery(g_base + a, &mi, sizeof(mi))) { a += 0x10000; continue; }
            uint64_t st = uint64_t((uint8_t*)mi.BaseAddress - g_base), en = st + mi.RegionSize;
            DWORD pr = mi.Protect & 0xFF;
            if (mi.State == MEM_COMMIT && (pr == PAGE_READWRITE || pr == PAGE_EXECUTE_READWRITE || pr == PAGE_READONLY))
                regs.push_back({ uint32_t(std::max<uint64_t>(st, a)), uint32_t(std::min<uint64_t>(en, 0xC0000000) - std::max<uint64_t>(st, a)) });
            a = std::max(en, a + 0x1000);
        }
        for (const char* nm : names)
        {
            size_t L = strlen(nm) + 1; std::vector<uint32_t> strAddrs;
            for (auto& r : regs) { const uint8_t* p = g_base + r.a; for (uint32_t i = 0; i + L <= r.n; i++) if (p[i] == 'S' && !memcmp(p + i, nm, L)) strAddrs.push_back(r.a + i); }
            uint32_t found = 0;
            for (uint32_t sa : strAddrs)
            {
                uint32_t be = __builtin_bswap32(sa);
                for (auto& r : regs)
                {
                    const uint8_t* p = g_base + r.a;
                    for (uint32_t i = 0; i + 4 <= r.n && !found; i += 4)
                        if (*(const uint32_t*)(p + i) == be)
                            for (int back = 0; back <= 32 && !found; back += 4)
                            {
                                uint32_t obj = r.a + i - back; uint32_t key = GR32(obj + 8);
                                uint32_t inst = FindSignal(table, key);
                                if (inst >= 0x40000000 && inst < 0xC0000000) { found = key; std::lock_guard lk(g_lock); g_keysByName[nm] = key; }
                            }
                    if (found) break;
                }
                if (found) break;
            }
            Log("[sigscan] %s: %zu name copies, key %08X\n", nm, strAddrs.size(), found);
        }
        Log("[sigscan] done in %llu ms\n", (unsigned long long)(NbashMs() - t0));
    }).detach();
}

// 14 Nov: SIGNAL TRACE - for 1.6 s after a right-stick dribble gesture (ms = time since it, -1 = idle) every engine signal of the pad's player is read
// each frame and every change is logged: "[sig] <ms> <name> <old> -> <new>".  This is how the game's own ball logic during a trick is found
// (which signal pins the ball to a hand, and when).  First 6 gestures of a session.
void NbashSignalTrace(int ms)
{
    static const bool on = getenv("NBASH_SIGTRACE") != nullptr; if (!on) return;      // (diagnostic; it found SigSIM_BallPinLeft / Right)
    static std::map<uint32_t, float> last; static int runs = 0; static bool active = false; static std::vector<std::pair<std::string, uint32_t>> keys;
    if (ms < 0 || ms > 1600) { if (active) { active = false; Log("[sig] end\n"); } return; }
    uint32_t table = 0; uint64_t best = 0;
    {
        std::lock_guard lk(g_lock);
        for (auto& [t, seen] : g_padTables) if (seen > best) { best = seen; table = t; }
        if (!active) { keys.clear(); for (auto& [n, k] : g_allKeys) keys.push_back({ n, k }); for (auto& [n, k] : g_keysByName) keys.push_back({ n, k }); }
    }
    if (!table) return;
    if (!active) { if (runs >= 6) return; runs++; active = true; last.clear(); Log("[sig] gesture %d: %d signals watched, table %08X\n", runs, int(keys.size()), table); }
    char buf[1800]; int len = 0;
    for (auto& [n, k] : keys)
    {
        uint32_t inst = FindSignal(table, k); if (inst < 0x40000000 || inst >= 0xC0000000) continue;
        uint32_t v = GR32(inst + 4); float f; memcpy(&f, &v, 4);
        auto it = last.find(k);
        if (it == last.end()) { last[k] = f; if (f != 0.0f && len < 1700) len += snprintf(buf + len, sizeof buf - size_t(len), " %s=%.3g", n.c_str(), f); continue; }
        if (it->second != f) { if (len < 1700) len += snprintf(buf + len, sizeof buf - size_t(len), " %s %.3g->%.3g", n.c_str(), it->second, f); it->second = f; }
    }
    if (len) Log("[sig] %d:%s\n", ms, buf);
}

// 14 Nov: THE BALL DURING A SIGNATURE DRIBBLE.  Found with the signal trace: the game pins the ball to a hand with SigSIM_BallPinLeft / SigSIM_BallPinRight.
// In a Y trick: pin of the starting hand on at ~0.07 s, OFF at ~0.38 s (the ball follows the clip), ON AGAIN at ~0.82 s (the ball flew back to the
// starting hand although the clip had it in the other one), and around 1.2 s the pins change to the other hand.
// While one of OUR moves plays (ms = time since the right-stick flick, sig = true) the starting hand's pin is held off from 0.15 s until the game pins
// the other hand (or 1.6 s): the ball then follows the clip's own path all the way, and the hand change at the end finds it already there.
void NbashBallPinOverride(int ms, bool sig)
{
    static int startHand = -1; static bool active = false; static int said = 0, zeroed = 0;
    uint32_t table = 0, kl = 0, kr = 0; uint64_t best = 0;
    {
        std::lock_guard lk(g_lock);
        for (auto& [t, seen] : g_padTables) if (seen > best) { best = seen; table = t; }
        auto a = g_allKeys.find("SigSIM_BallPinLeft"), b = g_allKeys.find("SigSIM_BallPinRight"); if (a != g_allKeys.end()) kl = a->second; if (b != g_allKeys.end()) kr = b->second;
    }
    if (!table || !kl || !kr) return;
    uint32_t il = FindSignal(table, kl), ir = FindSignal(table, kr);
    if (il < 0x40000000 || il >= 0xC0000000 || ir < 0x40000000 || ir >= 0xC0000000) return;
    auto rd = [](uint32_t inst) { uint32_t v = GR32(inst + 4); float f; memcpy(&f, &v, 4); return f; };
    auto wr = [](uint32_t inst, float f) { uint32_t v; memcpy(&v, &f, 4); *(uint32_t*)(g_base + inst + 4) = __builtin_bswap32(v); };
    static int lastMs = 1 << 30; static bool finished = false;
    if (ms >= 0 && ms < lastMs) { finished = false; }      // a new flick
    if (ms >= 0) lastMs = ms;
    float L = rd(il), R = rd(ir);
    if (finished && sig && ms >= 0 && ms <= 1600) return;
    if (!sig || ms < 0 || ms > 1600)
    {
        if (active)
        {
            active = false;      // the move is over (or was cut short): if the game still means the starting hand and no pin is on, give it back
            if (L < 0.5f && R < 0.5f && zeroed) wr(startHand == 0 ? il : ir, 1.0f);
            if (said++ < 40) Log("[ballpin] move over: starting hand %s, its pin was held off in %d frames; pins now L %.0f R %.0f\n", startHand == 0 ? "left" : "right", zeroed, rd(il), rd(ir));
        }
        return;
    }
    if (!active) { active = true; zeroed = 0; startHand = -1; }
    { extern void NbashPinWatch(uint32_t, uint32_t); static int lastArm = 1 << 30; if (getenv("NBASH_PINWATCH") && ms < lastArm) NbashPinWatch(il, ir); lastArm = ms; }      // diagnostic: who writes the pins
    if (startHand < 0) { if (L > 0.5f) startHand = 0; else if (R > 0.5f) startHand = 1; else if (ms > 120) startHand = 0; else return; }
    if (ms < 150) return;
    uint32_t si = startHand == 0 ? il : ir; float other = startHand == 0 ? R : L;
    if (other > 0.5f) { active = false; finished = true; if (said++ < 40) Log("[ballpin] %d ms: the game pinned the other hand - done (starting hand %s, held off in %d frames)\n", ms, startHand == 0 ? "left" : "right", zeroed); return; }
    static const bool ovr = getenv("NBASH_PINOVERRIDE") != nullptr; if (ovr && rd(si) > 0.5f) { wr(si, 0.0f); zeroed++; }      // (writing the value changed nothing - off unless asked for)
}

// 14 Nov: THE REAL BALL PINS.  Found with hardware write breakpoints (pinwatch.cpp): the two pin signals are written by
//   sub_821402F8(player, tag)  = "hold the ball in the RIGHT hand"  (posts message 20 and sets BallPinRight 1 / Left 0)
//   sub_821403D8(player, tag)  = "hold the ball in the LEFT hand"
//   sub_821405A0               = let go (both 0; the ball follows the clip)
// all called from sub_8213BCC8 (the player's tag / state update).  In a Y trick the game calls: hold START hand at ~0.07 s, let go ~0.37 s, hold START
// hand AGAIN at ~0.82 s, let go ~1.2 s, hold the OTHER hand at ~1.39 s.  A crossover from our clips has the ball in the other hand by 0.65 s, so the
// second "hold start hand" threw it back.  During a signature dribble that one call is dropped (0.3 s .. 1.3 s after the flick, starting hand only).
int NbashSigDribbleMs();
namespace { int g_sigStartHand = -1; uint64_t g_sigHandAt = 0; }
namespace { uint32_t g_sigPlayer = 0; std::atomic<bool> g_sigNoSwap{false}; std::atomic<uint64_t> g_sigOverAt{0}; std::atomic<bool> g_sigLive{false}; std::atomic<int> g_lastHand{-1}; }
// which hand the ball handler last had the ball put in by the game (0 left, 1 right, -1 unknown) - input.cpp picks the move's side from it
int NbashBallHand() { return g_lastHand.load(); }
uint64_t NbashSigOverAt() { return g_sigOverAt.load(); }
float NbashSigSpeed();
static bool DropPin(int hand, uint32_t player)      // hand 0 = left, 1 = right
{
    static const bool off = getenv("NBASH_SIGPIN") && getenv("NBASH_SIGPIN")[0] == '0';
    int ms = NbashSigDribbleMs(); static int said = 0;
    extern float NbashSigSpeedNow(); const float k = NbashSigSpeedNow(); ms = int(ms * k);      // (sequence time: the move runs k x faster, see sub_8213BCC8 below)
    if (off || ms < 0 || ms > 1300) return false;
    if (ms < 300) { g_sigPlayer = player; g_sigStartHand = hand; g_sigHandAt = NbashMs(); return false; }
    { extern uint32_t NbashCarrier(bool*); if (player != NbashCarrier(nullptr)) return false; }      // only the ball handler
    // 14 Nov 17:20: every move now ends with the ball in the OTHER hand (single crossovers), so the game's holds of the START hand are turned
    // round while the move runs and for 250 ms after it (the heel-flip slot re-takes the start hand right at its end)
    if (g_sigNoSwap.load()) return false;      // 14 Nov 17:32: HEEL_FLIP = our double crossover ends in the start hand - the game's holds are right
    { extern uint64_t NbashSigOverAt(); const uint64_t ov = NbashSigOverAt();
      static uint64_t usedFor = 0;      // after the move: only ONE such call, within 150 ms (14 Nov 17:18: a shot right after the move glitched)
      if (!g_sigLive.load()) { if (!(ov && NbashMs() - ov < 150 && hand == g_sigStartHand && usedFor != ov)) return false; usedFor = ov; }
      if (hand != g_sigStartHand) return false; }      // 14 Nov 16:33: only while the move's own sequence runs (after it the game's normal holds were turned round = "the ball moves uncontrollably")
    // (every hold call in this window is the game re-taking the ball for the starting hand; the hand-over to the other hand comes at ~1.39 s)
    if (said++ < 60) Log("[ballpin] %d ms after the flick: the game wanted the ball back in the %s hand - held in the other hand instead (the ball stays with the move)\n", ms, hand ? "right" : "left");
    return true;
}
// ---------------------------------------------------------------- 15 Nov: LEFT-HANDED jump shots (user: Brunson "he's left handed")
// game/mods/shots/lefty.txt: character hashes (8 hex digits, one per line).  For those players the jump-shot clips are the mirrored 2K shot
// (left arm shoots) and, from the shoot press until 1.6 s after it, every "hold the ball in the RIGHT hand" call of the game for the shooter is turned
// into "hold it in the LEFT hand"; if the ball is still in his right hand from the dribble, one left-hand hold is made at the press (sub_8213BCC8 hook).
// Logs "[lefty] ...".  Off with NBASH_LEFTY=0.
std::string GameRoot();
namespace { std::atomic<uint32_t> g_holdTag{0}; std::atomic<uint64_t> g_leftyForcedFor{0}; }
static bool LeftyHash(uint32_t h)
{
    static const std::vector<uint32_t> L = [] { std::vector<uint32_t> v; if (getenv("NBASH_LEFTY") && getenv("NBASH_LEFTY")[0] == '0') return v;
        if (FILE* f = fopen((GameRoot() + "/mods/shots/lefty.txt").c_str(), "r")) { char ln[128]; while (fgets(ln, sizeof ln, f)) { unsigned x; if (ln[0] != '#' && sscanf(ln, "%x", &x) == 1) v.push_back(x); } fclose(f); }
        Log("[lefty] %d left-handed shooter(s)\n", int(v.size())); return v; }();
    for (uint32_t x : L) if (x == h) return true; return false;
}
uint32_t NbashShotHolderHash(); uint64_t NbashShotPressMs(); uint64_t NbashMs();
static bool LeftyShotNow()      // the ball handler is a lefty and he pressed shoot less than 1.6 s ago
{
    const uint32_t h = NbashShotHolderHash(); if (!h || !LeftyHash(h)) return false;
    const uint64_t pr = NbashShotPressMs(), now = NbashMs(); return pr && now >= pr && now - pr < 1600;
}
extern "C" PPC_FUNC(__imp__sub_821402F8);
// 14 Nov 15:05: dropping the call outright skipped the game's own "hold the ball" message, and after the first signature move the trick sound never
// came again (the original tricks sound every time).  Now the call is kept but turned round: the ball is held in the OTHER hand (the one that caught it).
extern "C" PPC_FUNC(__imp__sub_821403D8);
PPC_FUNC(sub_821402F8) { extern uint32_t NbashCarrier(bool*); const bool car = ctx.r3.u32 == NbashCarrier(nullptr);
    if (car) g_holdTag = ctx.r4.u32;
    if (car && LeftyShotNow()) { static int n = 0; if (n++ < 60) Log("[lefty] %d ms after the press: right-hand hold -> LEFT hand\n", int(NbashMs() - NbashShotPressMs())); g_lastHand = 0; __imp__sub_821403D8(ctx, base); return; }
    { static int n = 0; int ms = NbashSigDribbleMs(); if (car && ms >= 0 && ms < 2500 && n++ < 600) Log("[holdlog] %d ms: game holds the ball in the RIGHT hand (live %d)\n", ms, int(g_sigLive.load())); }
    if (DropPin(1, ctx.r3.u32)) { if (car) g_lastHand = 0; __imp__sub_821403D8(ctx, base); return; } if (car) g_lastHand = 1; __imp__sub_821402F8(ctx, base); }
extern "C" PPC_FUNC(__imp__sub_821403D8);
PPC_FUNC(sub_821403D8) { extern uint32_t NbashCarrier(bool*); const bool car = ctx.r3.u32 == NbashCarrier(nullptr);
    if (car) g_holdTag = ctx.r4.u32;
    { static int n = 0; int ms = NbashSigDribbleMs(); if (car && ms >= 0 && ms < 2500 && n++ < 600) Log("[holdlog] %d ms: game holds the ball in the LEFT hand (live %d)\n", ms, int(g_sigLive.load())); }
    if (DropPin(0, ctx.r3.u32)) { if (car) g_lastHand = 1; __imp__sub_821402F8(ctx, base); return; } if (car) g_lastHand = 0; __imp__sub_821403D8(ctx, base); }

// ---------------------------------------------------------------- signature dribble: the WHOLE move k x faster (14 Nov: "faster ... Homecourt style")
// The Y-trick slot lasts a fixed 1.58 s of SEQUENCE time (its length is cached at load; the rate only changes the clip speed).  sub_8213BCC8 (the
// player's tag update) asks each of his 8 animation layers for its time (object at player+200+212*i, virtual +44) and keeps it at player+404+212*i.
// During a signature move, in the ball handler's layer that started with the flick, the float field of the layer object that holds that same time is
// found, and every tick its step is multiplied by k  ->  the sequence (move, ball tags, the end of the slot) runs k x faster.
// k from game/mods/shots/sigspeed.txt (one number, 1.0 .. 2.5; 1.0 = off).  Logs "[sigspeed] ...".
std::string GameRoot();
float NbashSigSpeed()
{
    static float k = [] { float v = 1.0f; if (FILE* f = fopen((GameRoot() + "/mods/shots/sigspeed.txt").c_str(), "r")) { if (fscanf(f, "%f", &v) != 1) v = 1.0f; fclose(f); }
                          if (const char* e = getenv("NBASH_SIGSPEED")) v = float(atof(e)); if (!(v >= 1.0f && v <= 2.5f)) v = 1.0f; Log("[sigspeed] signature moves at %.2f x\n", v); return v; }();
    return k;
}
namespace
{
    float GRF(uint32_t a) { uint32_t v = GR32(a); float f; memcpy(&f, &v, 4); return f; }
    void GWF(uint32_t a, float f) { uint32_t v; memcpy(&v, &f, 4); *(uint32_t*)(g_base + a) = __builtin_bswap32(v); }
    struct SigLayer { bool done = false; uint32_t pl = 0; int layer = -1; uint32_t obj = 0, field = 0; float last = 0; int ticks = 0, said = 0; } g_sl;
}

namespace
{
    // name of the sequence a second-loop layer object plays: a pointer (1 or 2 steps away) to a "Seq_..." string
    std::string SeqName(uint32_t obj)
    {
        auto str = [](uint32_t p) -> std::string { if (p < 0x40000000u || p > 0xF0000000u) return ""; const char* c = (const char*)(g_base + p); if (strncmp(c, "Seq_", 4)) return ""; size_t n = strnlen(c, 80); return std::string(c, n); };
        for (int i = 0; i < 48; i++) { uint32_t a = GR32(obj + 4 * i); std::string s = str(a); if (!s.empty()) return s;
            if (a > 0x40000000u && a < 0xF0000000u) for (int j = 0; j < 48; j++) { s = str(GR32(a + 4 * j)); if (!s.empty()) return s; } }
        return "";
    }
}
extern "C" PPC_FUNC(__imp__sub_8213BCC8);
PPC_FUNC(sub_8213BCC8)
{
    // 14 Nov 14:05: the clocks round the clip controller were sped up but the move still handed the ball over at ~1.37 s - they are not the move's
    // clock.  sub_8213BCC8 has a SECOND loop (not looked at before): 8 more layers, object at part+1896+212*i, its time (virtual +44) kept at
    // part+2100+212*i, its TAGS at part+1900 (count) / part+1904.. (pointers) - the tag that hands the ball over is evaluated there, so that is the
    // sequence.  During a signature move: the ball handler's part (contains the carrier); a second-loop layer whose time starts again (drops) in the
    // first 600 ms = the move's sequence -> its time field (found in its object = the value the getter gave) runs k x; the clip controller too.
    extern uint32_t NbashCarrier(bool*);
    extern float NbashSigSpeedNow(); const uint32_t pl = ctx.r3.u32; const float k = NbashSigSpeedNow(); const int ms = NbashSigDribbleMs();
    {   // 15 Nov lefty: at the shoot press, if the ball is still in his RIGHT hand, hold it in the left once (before this part updates)
        const uint32_t car = NbashCarrier(nullptr); const uint64_t pr = NbashShotPressMs();
        if (car && car >= pl && car < pl + 0x14040u && g_lastHand.load() == 1 && g_holdTag.load() && pr && g_leftyForcedFor.load() != pr && LeftyShotNow())
        {
            g_leftyForcedFor = pr; PPCContext c2 = ctx; c2.r3.u64 = car; c2.r4.u64 = g_holdTag.load(); __imp__sub_821403D8(c2, base); g_lastHand = 0;
            static int n = 0; if (n++ < 60) Log("[lefty] %d ms after the press: ball moved to the LEFT hand for the shot\n", int(NbashMs() - pr));
        }
    }
    {   // 14 Nov 18:07 ("the original X moves of EVERY player are sped up since the X-move work"): after any NON-signature stick move, the carrier's
        // sequences (second loop: name, time) and clip controllers (first loop: time) every 3 ticks for 0.7 s -> their real step per tick vs stock
        extern uint32_t NbashCarrier(bool*); extern int NbashAnyTrickMs(const char**);
        const char* tn = ""; const int ams = NbashAnyTrickMs(&tn); static int lines = 0, tick = 0;
        const uint32_t car = NbashCarrier(nullptr);
        if (ams >= 0 && ams < 700 && strncmp(tn, "signature", 9) && car >= pl && car < pl + 0x14040u && lines < 600 && (tick++ % 3) == 0)
        {
            char b2[900]; int w = snprintf(b2, sizeof b2, "[xprobe] %s %d ms:", tn, ams);
            for (int i = 0; i < 8 && w < 820; i++) { uint32_t o2 = GR32(pl + 1896 + 212 * i); if (o2) { std::string nm = SeqName(o2); w += snprintf(b2 + w, sizeof b2 - w, " S%d %s t=%.2f", i, nm.empty() ? "?" : nm.c_str() + 4, GRF(pl + 2100 + 212 * i)); } }
            for (int i = 0; i < 8 && w < 860; i++) { uint32_t o2 = GR32(pl + 200 + 212 * i); if (o2) w += snprintf(b2 + w, sizeof b2 - w, " C%d t=%.2f", i, GRF(o2 + 16)); }
            lines++; Log("%s\n", b2);
        }
    }
    {   // 14 Nov 17:04 (trick sound only when the side changes): the trick signals of the player the pad drives, around every signature move
        static int lastLogged = -1; static int n = 0; extern float NbashPlayerSignal(const char*); extern int NbashAnyTrickMs(const char**);
        const char* tn = ""; const int ams = NbashAnyTrickMs(&tn); const int ms = ams;      // any right-stick / LT trick, not only the signature move
        int slot = ms < 0 ? -1 : ms < 30 ? 0 : (ms >= 300 && ms < 330) ? 1 : (ms >= 700 && ms < 730) ? 2 : (ms >= 1200 && ms < 1230) ? 3 : -1;
        if (slot >= 0 && slot != lastLogged && n < 300)
        {
            lastLogged = slot; n++;
            const char* nm[] = { "SigSIM_TrickGate_1", "SigSIM_TrickGate_2", "SigSIM_TrickType_YLeft", "SigSIM_TrickType_YRight", "SigSIM_TrickType_Behind", "SigSIM_TrickType_Between", "SigSIM_MasterSkill_Trick", "SigPAD_Trick_Release", "SigPAD_Trick_Hold" };
            char b2[500]; int w = snprintf(b2, sizeof b2, "[tricksig] %s %d ms:", tn, ms); for (auto s : nm) w += snprintf(b2 + w, sizeof b2 - w, " %s=%g", s + 7, NbashPlayerSignal(s)); Log("%s\n", b2);
        }
        if (ms < 0 || ms > 1300) lastLogged = -1;
    }
    struct Sp { uint32_t pl = 0, ctrl = 0, seqObj = 0, seqField = 0; int layer = -1, ticks = 0; float last[8] = {}, lastSeq = 0, lastc = 0, acc = 0; bool init = false;
                std::vector<std::pair<uint32_t, float>> cand; std::vector<std::pair<uint32_t, float>> tmr; int tmrPhase = 0; float len = 95.0f; };
    static Sp sp; static int lastMs = 1 << 30; static int said = 0;
    if (ms >= 0 && ms < 2500 && pl)
    {
        if (ms < lastMs - 50) { sp = Sp(); g_sigLive = false; g_sigOverAt = 0; }
        lastMs = ms;
        const uint32_t car = NbashCarrier(nullptr);
        if (!sp.pl && ms < 400 && car >= pl && car < pl + 0x14040u)
        {
            sp.pl = pl; for (int i = 0; i < 8; i++) sp.last[i] = GRF(pl + 2100 + 212 * i);
            if (said++ < 400) { char b2[700]; int w = snprintf(b2, sizeof b2, "[sigspeed] %d ms: part %08X; second-loop layers:", ms, pl);
                for (int i = 0; i < 8 && w < 650; i++) { uint32_t o = GR32(pl + 1896 + 212 * i); if (o) w += snprintf(b2 + w, sizeof b2 - w, " %d:%08X vt %08X get %08X t %.2f n %d", i, o, GR32(o), GR32(GR32(o) + 44), sp.last[i], int(GR32(pl + 1900 + 212 * i))); }
                Log("%s\n", b2); }
        }
        else if (sp.pl == pl)
        {
            if (!sp.seqObj && ms < 700)
                for (int i = 0; i < 8; i++)
                {
                    uint32_t o = GR32(pl + 1896 + 212 * i); float t = GRF(pl + 2100 + 212 * i);
                    std::string nm; bool isY = false;
                    if (o && t + 0.001f < sp.last[i] && t < 20.0f && !sp.seqObj) { nm = SeqName(o); isY = !nm.compare(0, 11, "Seq_TRICK_Y");
                        if (said < 400 && !isY) { said++; Log("[sigspeed] %d ms: layer %d started %s - not a Y trick, left alone\n", ms, i, nm.empty() ? "(no name found)" : nm.c_str()); } }
                    if (isY)      // 14 Nov 15:40: only the Y-trick sequence of the move (before, other moves started in that window were sped up too)
                    {
                        sp.seqObj = o; sp.layer = i; sp.lastSeq = t; g_sigLive = true;
                        // 14 Nov 17:57 (spin froze him): the sequence only ends if its clock steps ONTO its end - at 1.8 x it jumped over it and he stayed in
                        // the trick.  Its length (ticks): kick 95, slap 99 (R) / 96 (L), heel 79 (R) / 82 (L); the last 3 ticks run at normal speed.
                        // 14 Nov 19:55: lengths of every Y-trick sequence (game data, cloud scan of ebo.big); MOVE_FWD / MOVE_BCK added (tricks while moving)
                        auto has = [&](const char* x) { return nm.find(x) != std::string::npos; };
                        if (has("MOVE_FWD")) sp.len = has("HEEL_KICK") ? 110.0f : has("HEEL_FLIP") ? 93.0f : has("SLAP_L") ? 104.0f : 102.0f;
                        else if (has("MOVE_BCK")) sp.len = has("SLAP") ? 88.0f : 105.0f;
                        else sp.len = has("KICK") ? 95.0f : has("SLAP_R") ? 99.0f : has("SLAP") ? 96.0f : has("HEEL_FLIP_R") ? 79.0f : has("HEEL") ? 82.0f : 79.0f;
                        // the tricks that end in the START hand (our hesitation): STD HEEL_FLIP and MOVE_FWD HEEL_KICK - the game's holds are right there
                        g_sigNoSwap = (has("STD") && has("HEEL_FLIP")) || has("HEEL_KICK");
                        { const uint32_t g = GR32(GR32(o) + 44); sp.seqField = g == 0x823C96D0u ? o + 20 : g == 0x823C96C8u ? o + 16 : 0; }      // getters: lfs f1,20(r3) / lfs f1,16(r3)
                        const float fv = sp.seqField ? GRF(sp.seqField) : -1.0f;
                        if (sp.seqField && !(fv >= 0.0f && fv < t + 4.0f)) sp.seqField = 0;      // (the stored copy is one update behind the object)
                        if (said++ < 400) Log("[sigspeed] %d ms: the move's time in its object: %.3f\n", ms, fv);
                        sp.lastSeq = sp.seqField ? fv : t; sp.acc = 0.0f;
                        for (int c = 7; c >= 0 && !sp.ctrl; c--) { uint32_t oc = GR32(pl + 200 + 212 * c); if (oc && GRF(oc + 16) < 12.0f) { sp.ctrl = oc; sp.lastc = GRF(oc + 16); sp.init = true; } }      // the clip controller that started with it
                        if (said++ < 400) Log("[sigspeed] %d ms: %s\n", ms, nm.c_str());
                        if (said++ < 400) Log("[sigspeed] %d ms: the move's sequence = second-loop layer %d, object %08X (vt %08X, getter %08X), time %.3f (was %.3f), field %s+%u, %d tags\n", ms, i, o, GR32(o), GR32(GR32(o) + 44), t, sp.last[i], sp.seqField ? "" : "NOT FOUND ", sp.seqField ? sp.seqField - o : 0, int(GR32(pl + 1900 + 212 * i)));
                    }
                    sp.last[i] = t;
                }
            if (sp.seqObj > 1 && GR32(pl + 1896 + 212 * sp.layer) != sp.seqObj) { if (said++ < 400) Log("[sigspeed] %d ms: the move's sequence is over\n", ms); sp.seqField = 0; sp.ctrl = 0; sp.seqObj = 1; g_sigLive = false; g_sigOverAt = NbashMs();  { extern void NbashSigOverSoon(); NbashSigOverSoon(); } }
            // 14 Nov 16:57 (recorder): after our sped-up sequence ends he stood still ~0.5 s - the game keeps him in the trick state on its OWN
            // timer (seconds, 1/60 per tick; the 14:05 clock finder saw one at part+0x12E70 starting with the move).  When the move's sequence is
            // found: every float in the part between 0 and 0.4 is noted; on the next tick the ones that grew by one tick (1/60 s) are the state's
            // timers -> they run k x until they start again (the state is over) or 2.5 s.
            // 14 Nov 17:12: OFF by default - with it the trick sound stopped completely, and the stand-still was already gone without it
            // (the finder found a timer in 1 of 8 moves).  game\mods\shots\sigtimer.txt turns it back on.
            static const bool timerOn = [] { FILE* f = fopen((GameRoot() + "/mods/shots/sigtimer.txt").c_str(), "r"); if (f) fclose(f); return f != nullptr; }();
            if (timerOn && k > 1.0f && sp.seqObj > 1 && sp.tmrPhase == 0)
            {
                sp.tmrPhase = 1;
                for (uint32_t o = 0; o < 0x14040u; o += 4) { float v = GRF(pl + o); if (v > 0.0f && v < 0.4f) sp.cand.push_back({ pl + o, v }); }
            }
            else if (sp.tmrPhase == 1)
            {
                sp.tmrPhase = 2;
                for (auto& c : sp.cand) { float v = GRF(c.first), d = v - c.second; if (d > 0.0160f && d < 0.0175f && sp.tmr.size() < 8) sp.tmr.push_back({ c.first, v }); }
                if (said++ < 400) { char b2[400]; int w = snprintf(b2, sizeof b2, "[sigspeed] %d ms: %d trick-state timer(s):", ms, int(sp.tmr.size())); for (auto& t : sp.tmr) w += snprintf(b2 + w, sizeof b2 - w, " part+%05X=%.3f", t.first - pl, t.second); Log("%s\n", b2); }
                sp.cand.clear(); sp.cand.shrink_to_fit();
            }
            else if (sp.tmrPhase == 2)
            {
                bool any = false;
                for (auto& t : sp.tmr)
                {
                    if (!t.first) continue;
                    float v = GRF(t.first), d = v - t.second;
                    if (d < 0.0f || d > 0.2f) { if (said++ < 400) Log("[sigspeed] %d ms: timer part+%05X started again (%.3f) - trick state over\n", ms, t.first - pl, v); t.first = 0; continue; }
                    v += d * (k - 1.0f); GWF(t.first, v); t.second = v; any = true;
                }
                if (!any) sp.tmrPhase = 3;
            }
            if (sp.seqField && GR32(pl + 1896 + 212 * sp.layer) == sp.seqObj)
            {
                float t = GRF(sp.seqField), d = t - sp.lastSeq;
                // 14 Nov 19:40: the sequence only ends when its clock lands EXACTLY on the end (KICK 95 ...).  At 1.8x the old speed-up left the clock on
                // fractions (73 -> 87.4 -> 95.4: past the end, never over = the spin / behind-the-back FREEZE).  The extra time is now added in whole
                // ticks (the fraction is carried over), and a clock that is still on a fraction is put back on a whole tick before it nears the end.
                // 14 Nov 20:55 ("the FULL animation"): k may now be < 1 = the slot runs SLOWER so a whole 2K move fits (a tick is held back: whole ticks only,
                // same rule).  The first 16 % of the sequence (the blend in from the dribble, clip frames 0-5 = the move's first pose) never runs slower
                // than normal, so a slow move still starts at once.
                const float kEff = (k < 1.0f && t < sp.len * 0.16f) ? 1.0f : k;
                if (kEff > 1.0f && d > 0.0f && d < 5.0f && t + d * kEff < sp.len - 3.0f) { sp.acc += d * (kEff - 1.0f); const float add = std::floor(sp.acc); sp.acc -= add; if (add > 0.0f) { t += add; GWF(sp.seqField, t); } }
                else if (kEff < 1.0f && d > 0.0f && d < 5.0f) { sp.acc += d * (kEff - 1.0f); if (sp.acc <= -1.0f) { sp.acc += 1.0f; t -= 1.0f; GWF(sp.seqField, t); } }
                else if (t > 0.0f && t < sp.len && std::fabs(t - std::floor(t + 0.5f)) > 0.001f) { t = std::floor(t); GWF(sp.seqField, t); }
                sp.lastSeq = t;
                // the clip: only while it is the one that started with the move, and only if the engine does not already speed it with the sequence
                if (sp.ctrl) { bool still = false; for (int c = 0; c < 8; c++) if (GR32(pl + 200 + 212 * c) == sp.ctrl) still = true;
                    if (!still) sp.ctrl = 0;
                    else { float c = GRF(sp.ctrl + 16), dc = c - sp.lastc; if (kEff != 1.0f && sp.init && dc > 0.0f && dc < 1.3f) { c += dc * (kEff - 1.0f); GWF(sp.ctrl + 16, c); } sp.lastc = c; } }
                if (sp.ticks++ % 8 == 0 && said++ < 400) Log("[sigspeed] %d ms: sequence time %.3f (step %.3f), clip %.3f\n", ms, t, d, sp.ctrl ? GRF(sp.ctrl + 16) : -1.0f);
            }
        }
    }
    __imp__sub_8213BCC8(ctx, base);
}

// ---------------------------------------------------------------- 14 Nov 14:39 ("add the right sounds to the animation"): TAG DUMP
// Sounds in this engine come from tags in the sequences (EA::Ant::PlayerAudioEvent / MiscAudioEvent / TrickEventTag ...).  sub_8213BA80(part, tag,
// f1 time, f2 step, f3) collects a tag whose window the layer's time has just crossed.  This logs every DIFFERENT tag the ball handler's part
// passes through it (first 400): its words, the object at +28 and that object's first words - to find the bounce / trick sound tags.
extern "C" PPC_FUNC(__imp__sub_8213BA80);
PPC_FUNC(sub_8213BA80)
{
    extern uint32_t NbashCarrier(bool*);
    static std::set<uint32_t> seen; static int lines = 0;
    const uint32_t part = ctx.r3.u32, tg = ctx.r4.u32, car = NbashCarrier(nullptr);
    if (tg && lines < 400 && car >= part && car < part + 0x14040u && seen.insert(tg).second)
    {
        lines++; const int ms = NbashSigDribbleMs(); const uint32_t p = GR32(tg + 28);
        char b2[900]; int w = snprintf(b2, sizeof b2, "[tagdump] %s%d ms | t %.2f step %.2f f3 %.2f r8 %u | tag %08X:", ms >= 0 ? "signature +" : "", ms, ctx.f1.f64, ctx.f2.f64, ctx.f3.f64, ctx.r8.u32, tg);
        for (int i = 0; i < 10; i++) w += snprintf(b2 + w, sizeof b2 - w, " %08X", GR32(tg + 4 * i));
        if (p > 0x40000000u && p < 0xF0000000u) { w += snprintf(b2 + w, sizeof b2 - w, " | +28 -> %08X:", p); for (int i = 0; i < 8; i++) w += snprintf(b2 + w, sizeof b2 - w, " %08X", GR32(p + 4 * i)); }
        Log("%s\n", b2);
    }
    __imp__sub_8213BA80(ctx, base);
}

// 14 Nov 17:25: the game letting go of the ball (the clip carries it) - logged with the holds around signature moves
extern "C" PPC_FUNC(__imp__sub_821405A0);
PPC_FUNC(sub_821405A0)
{
    extern uint32_t NbashCarrier(bool*);
    static int n = 0; int ms = NbashSigDribbleMs();
    if (ctx.r3.u32 == NbashCarrier(nullptr) && ms >= 0 && ms < 2500 && n++ < 600) Log("[holdlog] %d ms: game lets go of the ball (the clip carries it)\n", ms);
    __imp__sub_821405A0(ctx, base);
}

// 14 Nov 17:52 ("the new animation is slow too, a little faster"): the extra moves (behind-the-back, spin) run 1.2 x faster again than the crossover
// (sigspeed.txt 1.5 -> 1.8); game\mods\shots\sigspeed_extra.txt can set that factor (1.0 .. 2.0).
float NbashSigSpeedNow()
{
    extern int NbashSigVariant();
    static float extra = [] { float v = 1.2f; if (FILE* f = fopen((GameRoot() + "/mods/shots/sigspeed_extra.txt").c_str(), "r")) { if (fscanf(f, "%f", &v) != 1) v = 1.2f; fclose(f); } if (!(v >= 1.0f && v <= 2.0f)) v = 1.2f; return v; }();
    // 14 Nov 19:45 ("like the shots"): the clips now carry the 2K move at its real speed x a small exaggeration for the slot's own game speed, so
    // every move has its own slot speed: mods/shots/sigspeed_v<variant>.txt (1 crossover, 2 hesitation, 3 behind-the-back, 4 spin) wins when it exists.
    static float per[5] = { -1, -1, -1, -1, -1 }; static bool loaded = false;
    if (!loaded) { loaded = true; for (int v = 1; v <= 4; v++) if (FILE* f = fopen((GameRoot() + "/mods/shots/sigspeed_v" + std::to_string(v) + ".txt").c_str(), "r")) { float x = 0; if (fscanf(f, "%f", &x) == 1 && x >= 0.25f && x <= 2.5f) per[v] = x; fclose(f); Log("[sigspeed] variant %d at %.2f x\n", v, per[v]); } }
    const int var = NbashSigVariant(); if (var >= 1 && var <= 4 && per[var] > 0.0f) return per[var];
    const float k = NbashSigSpeed(); return k > 1.0f && var >= 2 ? k * extra : k;      // 2 double crossover, 3 behind-the-back, 4 spin
}
