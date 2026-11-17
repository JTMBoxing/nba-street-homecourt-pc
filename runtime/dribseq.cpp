// Per-player LENGTH of the plain dribble moves (14 Nov).
// The moves the trick button plays (Seq_X_STAND_* ...) are EA::Ant::SequenceAnimation assets of 0.15-0.3 s.  A real crossover (2K14 clip written into
// the move's own clip by the cloud tool drib/xdrib.py) needs 0.5-0.6 s.  The sequence stores, 8 bytes before a float 1.0 (the clip's playback rate),
// the shorts (L, 0, L, 4) and, 96 bytes before those, the float L = its length in 1/120 s; tags are float triples (at, length, at).
// For a player listed in game/mods/shots/stretch.txt ("<character hash> <sequence tag start> <factor>") the lengths are multiplied and the rate
// divided while he has the ball; stock values for everybody else.  The loaded sequences are found by their name string.
//   game/mods/shots/seqs.txt (cloud drib/seqtab.py):  <tag> <name> <rel>:<kind>:<stock> ...   rel = bytes from the name, kind f = float, h = u16, r = rate
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "guest.h"
#ifdef _WIN32
#include <windows.h>
#endif
void Log(const char* fmt, ...);
uint64_t NbashMs();
std::string GameRoot();
namespace
{
    struct Field { int rel; char kind; float stock; };
    struct Seq { std::string tag, name; std::vector<Field> f; std::vector<uint32_t> at; };
    struct Rule { uint32_t hash; std::string prefix; float k; };
    std::vector<Seq> g_seqs; std::vector<Rule> g_rules; std::atomic<int> g_state{0}; uint32_t g_applied = 0xFFFFFFFF;
    float RdF(uint32_t a) { uint32_t v = (uint32_t(g_base[a]) << 24) | (uint32_t(g_base[a + 1]) << 16) | (uint32_t(g_base[a + 2]) << 8) | g_base[a + 3]; float f; memcpy(&f, &v, 4); return f; }
    void WrF(uint32_t a, float f) { uint32_t v; memcpy(&v, &f, 4); g_base[a] = uint8_t(v >> 24); g_base[a + 1] = uint8_t(v >> 16); g_base[a + 2] = uint8_t(v >> 8); g_base[a + 3] = uint8_t(v); }
    uint16_t RdH(uint32_t a) { return uint16_t((g_base[a] << 8) | g_base[a + 1]); }
    void WrH(uint32_t a, uint16_t v) { g_base[a] = uint8_t(v >> 8); g_base[a + 1] = uint8_t(v); }
    bool Stock(const Seq& s, uint32_t a)
    {
        for (auto& f : s.f) { uint32_t p = uint32_t(int64_t(a) + f.rel); if (f.kind == 'h' ? RdH(p) != uint16_t(f.stock) : RdF(p) != f.stock) return false; }
        return true;
    }
    void Finder()
    {
        if (FILE* f = fopen((GameRoot() + "/mods/shots/seqs.txt").c_str(), "r"))
        {
            char line[2048];
            while (fgets(line, sizeof line, f))
            {
                if (line[0] == '#') continue;
                char tag[64], name[128]; int n = 0; if (sscanf(line, "%63s %127s%n", tag, name, &n) != 2) continue;
                Seq s; s.tag = tag; s.name = name; const char* p = line + n; int rel; char kind; float st; int used;
                while (sscanf(p, " %d:%c:%f%n", &rel, &kind, &st, &used) == 3) { s.f.push_back({ rel, kind, st }); p += used; }
                if (!s.f.empty()) g_seqs.push_back(s);
            }
            fclose(f);
        }
        if (FILE* f = fopen((GameRoot() + "/mods/shots/stretch.txt").c_str(), "r"))
        {
            char line[256], pre[64]; unsigned h; float k;
            while (fgets(line, sizeof line, f)) if (line[0] != '#' && sscanf(line, "%x %63s %f", &h, pre, &k) == 3 && k >= 1.0f && k <= 4.0f) g_rules.push_back({ h, pre, k });
            fclose(f);
        }
        if (g_seqs.empty() || g_rules.empty()) { g_state = 3; return; }
        Log("[dribseq] %d dribble sequences, %d stretch rules\n", int(g_seqs.size()), int(g_rules.size()));
#ifdef _WIN32
        for (int attempt = 0; attempt < 40; attempt++)
        {
            for (uint64_t a = 0x40000000ull; a < 0xFFFF0000ull;)
            {
                MEMORY_BASIC_INFORMATION mi;
                if (!VirtualQuery(g_base + a, &mi, sizeof mi)) break;
                uint64_t e = uint64_t((uint8_t*)mi.BaseAddress + mi.RegionSize - g_base); if (e > 0xFFFF0000ull) e = 0xFFFF0000ull;
                if (mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && !(a >= 0x82000000ull && a < 0x82800000ull) && e - a > 8192)
                {
                    static std::vector<uint8_t> buf; const size_t CH = 8u << 20, TAIL = 4096; buf.resize(CH);      // copied in pieces (see shotswap.cpp)
                    for (uint64_t c0 = a; c0 + 2 * TAIL < e; c0 += CH - 2 * TAIL)
                    {
                        size_t len = size_t(e - c0 < CH ? e - c0 : CH); SIZE_T got = 0;
                        if (!ReadProcessMemory(GetCurrentProcess(), g_base + c0, buf.data(), len, &got) || got != len) continue;
                        const uint8_t* m = buf.data(); size_t n = len - TAIL;
                        for (size_t k = TAIL; k < n; k += 4)
                        {
                            if (m[k] != 'S' || m[k + 1] != 'e' || m[k + 2] != 'q' || m[k + 3] != '_') continue;
                            for (auto& s : g_seqs)
                                if (!memcmp(m + k, s.name.c_str(), s.name.size() + 1))
                                {
                                    uint32_t at = uint32_t(c0 + k); bool have = false; for (uint32_t x : s.at) if (x == at) have = true;
                                    if (!have && Stock(s, at)) { s.at.push_back(at); Log("[dribseq] %s found at %08X (copy %d)\n", s.tag.c_str(), at, int(s.at.size())); }
                                }
                        }
                    }
                }
                if (e <= a) break;
                a = e;
            }
            int missing = 0; for (auto& s : g_seqs) if (s.at.empty()) missing++;
            if (missing < int(g_seqs.size())) g_state = 2;
            if (!missing) { Log("[dribseq] all sequences found\n"); return; }
            Sleep(4000);
        }
        Log("[dribseq] some sequences were not found - those moves keep their stock length\n");
#endif
    }
}
// shotswap.cpp calls this whenever the clips are exchanged: hash = the ball handler's character (0 = an original player)
void NbashDribSeqApply(uint32_t hash)
{
    int st = g_state.load();
    if (st == 0) { int z = 0; if (g_state.compare_exchange_strong(z, 1)) std::thread(Finder).detach(); return; }
    if (st != 2 || hash == g_applied) return;
    int n = 0;
    for (auto& s : g_seqs)
    {
        float k = 1.0f; for (auto& r : g_rules) if (r.hash == hash && !s.tag.compare(0, r.prefix.size(), r.prefix)) k = r.k;
        for (uint32_t at : s.at)
            for (auto& f : s.f)
            {
                uint32_t p = uint32_t(int64_t(at) + f.rel);
                if (f.kind == 'h') WrH(p, uint16_t(f.stock * k + 0.5f)); else if (f.kind == 'r') WrF(p, f.stock / k); else WrF(p, f.stock * k);
            }
        if (k != 1.0f) n++;
    }
    static int said = 0; if (said++ < 40) Log("[dribseq] ball handler %08X: %d dribble sequences stretched\n", hash, n);
    g_applied = hash;
}
