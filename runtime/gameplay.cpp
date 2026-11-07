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
        static const bool dunkProbe = getenv("NBASH_DUNKPROBE") != nullptr;
        bool watch = dunkProbe && (CiFind(s, "dunk") || CiFind(s, "layup") || CiFind(s, "rating") || CiFind(s, "skill") ||
                                   CiFind(s, "overall") || CiFind(s, "player") || CiFind(s, "level") || CiFind(s, "uber") || CiFind(s, "style") || CiFind(s, "shot") || CiFind(s, "score") || CiFind(s, "point") || CiFind(s, "ball"));
        if (watch || !strcmp(s, "SigSIM_CarryBall") || !strcmp(s, "SigSIM_DunkGate_1") || !strcmp(s, "SigSIM_DunkGate_2") ||
            !strcmp(s, "SigSIM_Gamebreaker_Ready") || !strcmp(s, "SigSIM_Gamebreaker_Active") || !strcmp(s, "SigSIM_MasterSkill_Dunk") || !strncmp(s, "SigSIM_Class_", 13) || !strncmp(s, "SigSIM_DunkType_", 16))
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
