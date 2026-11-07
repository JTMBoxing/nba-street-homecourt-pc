// Position probe (NBASH_POSPROBE=1): finds where the game keeps the controlled player's position.
// Each right-stick crossover (made while standing still) triggers four reads of guest memory:
// t0 (just after the flick), t0+300 ms (the crossover has moved the player), then two reads while
// standing idle (t0+1.6 s and t0+1.9 s). A float is kept if it moved by a court-sized step during the
// crossover but stayed put while idle. The first crossover scans all committed guest memory; later
// ones only re-check the survivors. Results go to nbash_posprobe.txt.
#include <windows.h>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include <string>

extern uint8_t* g_base;
void Log(const char* fmt, ...);
uint64_t NbashMs();
uint32_t NbashLatestPadTable();
std::string GameRoot();

namespace
{
    std::atomic<int> g_events{0};
    bool Enabled() { static const bool on = getenv("NBASH_POSPROBE") && getenv("NBASH_POSPROBE")[0] == '1'; return on; }
    bool Readable(uint32_t a)
    {
        MEMORY_BASIC_INFORMATION mi{};
        if (!VirtualQuery(g_base + a, &mi, sizeof(mi))) return false;
        if (mi.State != MEM_COMMIT) return false;
        DWORD p = mi.Protect & 0xFF;
        return p == PAGE_READWRITE || p == PAGE_READONLY || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_WRITECOPY;
    }
    float RF(uint32_t a) { uint32_t v = __builtin_bswap32(*(uint32_t*)(g_base + a)); float f; memcpy(&f, &v, 4); return f; }

    struct Cand { uint32_t addr; float v[4][4]; };   // up to 4 crossovers x 4 reads

    // reads the candidates' values safely (pages can be freed): returns false for unreadable ones
    void ReadAll(std::vector<Cand>& c, int ev, int slot, std::vector<char>& ok)
    {
        uint32_t lastPage = 0xFFFFFFFF; bool lastOk = false;
        for (size_t i = 0; i < c.size(); i++)
        {
            uint32_t pg = c[i].addr & ~0xFFFu;
            if (pg != lastPage) { lastPage = pg; lastOk = Readable(pg); }
            if (!lastOk) { ok[i] = 0; continue; }
            c[i].v[ev][slot] = RF(c[i].addr);
        }
    }

    void Probe()
    {
        std::vector<Cand> cand; int ev = 0;
        for (;;)
        {
            if (g_events.load() <= ev) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
            if (ev == 0)
            {
                // full scan: every committed page in the guest heap range
                uint64_t t = NbashMs();
                for (uint64_t a = 0x40000000; a < 0xC0000000;)
                {
                    MEMORY_BASIC_INFORMATION mi{};
                    if (!VirtualQuery(g_base + a, &mi, sizeof(mi))) { a += 0x10000; continue; }
                    uint64_t start = uint64_t((uint8_t*)mi.BaseAddress - g_base), end = start + mi.RegionSize;
                    DWORD p = mi.Protect & 0xFF;
                    bool rd = mi.State == MEM_COMMIT && (p == PAGE_READWRITE || p == PAGE_EXECUTE_READWRITE);
                    if (rd)
                        for (uint64_t x = std::max<uint64_t>(start, a); x + 4 <= end && x < 0xC0000000; x += 4)
                        {
                            float f = RF(uint32_t(x));
                            if (std::isfinite(f) && std::fabs(f) > 0.01f && std::fabs(f) < 60.0f) { Cand c{}; c.addr = uint32_t(x); c.v[0][0] = f; cand.push_back(c); }
                        }
                    a = std::max(end, a + 0x1000);
                }
                Log("[posprobe] crossover 1: %zu floats scanned in %llu ms\n", cand.size(), (unsigned long long)(NbashMs() - t));
            }
            std::vector<char> ok(cand.size(), 1);
            if (ev > 0) ReadAll(cand, ev, 0, ok);
            std::this_thread::sleep_for(std::chrono::milliseconds(300)); ReadAll(cand, ev, 1, ok);
            std::this_thread::sleep_for(std::chrono::milliseconds(1300)); ReadAll(cand, ev, 2, ok);
            std::this_thread::sleep_for(std::chrono::milliseconds(300)); ReadAll(cand, ev, 3, ok);
            std::vector<Cand> keep;
            for (size_t i = 0; i < cand.size(); i++)
            {
                if (!ok[i]) continue;
                float* v = cand[i].v[ev];
                float moved = std::fabs(v[1] - v[0]), idle = std::fabs(v[3] - v[2]);
                if (moved > 0.03f && moved < 3.0f && idle < 0.002f && std::fabs(v[1]) < 60.0f) keep.push_back(cand[i]);
            }
            cand.swap(keep); ev++;
            Log("[posprobe] crossover %d: %zu candidates left\n", ev, cand.size());
            if (ev >= 4 || cand.size() < 40)
            {
                uint32_t table = NbashLatestPadTable();
                std::string fn = GameRoot() + "\\..\\nbash_posprobe.txt";
                if (FILE* f = fopen(fn.c_str(), "w"))
                {
                    fprintf(f, "signal table %08X, %d crossovers, %zu candidates\n", table, ev, cand.size());
                    for (size_t i = 0; i < cand.size() && i < 3000; i++)
                    {
                        fprintf(f, "%08X (table%+lld)", cand[i].addr, (long long)cand[i].addr - (long long)table);
                        for (int e = 0; e < ev; e++) fprintf(f, " | %.4f %.4f %.4f %.4f", cand[i].v[e][0], cand[i].v[e][1], cand[i].v[e][2], cand[i].v[e][3]);
                        fprintf(f, "\n");
                    }
                    fclose(f);
                }
                Log("[posprobe] done: results in nbash_posprobe.txt\n");
                return;
            }
        }
    }
}

void NbashProbeCrossover()
{
    if (!Enabled()) return;
    static std::atomic<bool> started{false}; if (!started.exchange(true)) { std::thread(Probe).detach(); Log("[posprobe] started\n"); }
    g_events++;
}

// Menu probe (NBASH_MENUPROBE=1): finds where the player-select menu keeps the highlighted player.
// After each menu move (left/right/up/down keystroke, 400 ms to settle) every 32-bit word holding a
// valid player id is read; a word is kept if it changed on every move. Results: nbash_menuprobe.txt
std::vector<int> NbashStatsPlayerIds();
namespace
{
    std::atomic<int> g_menuMoves{0};
    bool MenuProbeOn() { static const bool on = getenv("NBASH_MENUPROBE") && getenv("NBASH_MENUPROBE")[0] == '1'; return on; }
    void MenuProbe()
    {
        std::vector<int> ids = NbashStatsPlayerIds(); std::vector<char> valid(4096, 0);
        for (int id : ids) if (id > 0 && id < 4096) valid[id] = 1;
        struct C { uint32_t a; std::vector<uint16_t> v; }; std::vector<C> cand; int done = 0;
        for (;;)
        {
            if (g_menuMoves.load() <= done) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            done = g_menuMoves.load();
            if (cand.empty() && done <= 1)
            {
                for (uint64_t a = 0x40000000; a < 0xC0000000;)
                {
                    MEMORY_BASIC_INFORMATION mi{};
                    if (!VirtualQuery(g_base + a, &mi, sizeof(mi))) { a += 0x10000; continue; }
                    uint64_t st = uint64_t((uint8_t*)mi.BaseAddress - g_base), en = st + mi.RegionSize; DWORD p = mi.Protect & 0xFF;
                    if (mi.State == MEM_COMMIT && (p == PAGE_READWRITE || p == PAGE_EXECUTE_READWRITE))
                        for (uint64_t x = std::max<uint64_t>(st, a); x + 4 <= en && x < 0xC0000000; x += 4)
                        { uint32_t v = __builtin_bswap32(*(uint32_t*)(g_base + x)); if (v < 4096 && valid[v]) cand.push_back({ uint32_t(x), { uint16_t(v) } }); }
                    a = std::max(en, a + 0x1000);
                }
                Log("[menuprobe] move 1: %zu words hold a player id\n", cand.size());
                continue;
            }
            std::vector<C> keep;
            for (auto& c : cand)
            {
                if (!Readable(c.a & ~0xFFFu)) continue;
                uint32_t v = __builtin_bswap32(*(uint32_t*)(g_base + c.a));
                if (v < 4096 && valid[v] && v != c.v.back()) { c.v.push_back(uint16_t(v)); keep.push_back(c); }
            }
            cand.swap(keep);
            Log("[menuprobe] move %d: %zu candidates\n", done, cand.size());
            if (done >= 8 || cand.size() < 20)
            {
                std::string fn = GameRoot() + "\\..\\nbash_menuprobe.txt";
                if (FILE* f = fopen(fn.c_str(), "w"))
                {
                    for (auto& c : cand) { fprintf(f, "%08X:", c.a); for (auto v : c.v) fprintf(f, " %u", v); fprintf(f, "\n"); }
                    fclose(f);
                }
                Log("[menuprobe] done: %zu candidates written\n", cand.size());
                return;
            }
        }
    }
}
void NbashMenuMove()
{
    if (!MenuProbeOn()) return;
    static std::atomic<bool> started{false}; if (!started.exchange(true)) { std::thread(MenuProbe).detach(); Log("[menuprobe] started\n"); }
    g_menuMoves++;
}
