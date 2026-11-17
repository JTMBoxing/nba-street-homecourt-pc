// Diagnostics for the spin-move work: records the game's action requests (sub_82266E60:
// r3 = controller, r4 = action number; #87 = ActionGomove, the 1-on-1 "Go" spin dribble) so we
// can see whether the human player's moves go through it, and which controller is ours.
// Every 10 s: a summary per controller; plus each request of a 1-on-1 action (80-105) as it happens.
// Turned on with NBASH_ACTIONTRACE=1 (on by default in the right-stick launcher while we investigate).
#include "guest.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <vector>
#include <string>
#include <mutex>
#include <thread>

void Log(const char* fmt, ...);
uint64_t NbashMs();   // milliseconds since start (input.cpp)

namespace
{
    std::mutex g_lock;
    std::map<uint32_t, std::map<uint32_t, uint32_t>> g_counts;   // controller -> action -> count
    int g_lines = 0;
    bool Enabled() { static const bool on = getenv("NBASH_ACTIONTRACE") != nullptr; return on; }
    void Reporter()
    {
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            std::lock_guard lk(g_lock);
            for (auto& [c, m] : g_counts)
            {
                char line[2048]; int n = snprintf(line, sizeof line, "[actions] controller %08X:", c);
                for (auto& [a, k] : m) if (n < 1900) n += snprintf(line + n, sizeof line - n, " #%u x%u", a, k);
                Log("%s\n", line);
            }
            g_counts.clear();
        }
    }
}

extern "C" PPC_FUNC(__imp__sub_82266E60);
PPC_FUNC(sub_82266E60)
{
    if (Enabled())
    {
        static std::atomic<bool> started{false};
        if (!started.exchange(true)) std::thread(Reporter).detach();
        uint32_t c = ctx.r3.u32, a = ctx.r4.u32;
        std::lock_guard lk(g_lock);
        g_counts[c][a]++;
        if (a >= 80 && a <= 105 && g_lines < 600)
        {
            g_lines++;
            Log("[action] t=%llu ms controller %08X requests #%u (r5=%08X r6=%08X f1=%.3f)\n", (unsigned long long)NbashMs(), c, a, ctx.r5.u32, ctx.r6.u32, ctx.f1.f64);
        }
    }
    __imp__sub_82266E60(ctx, base);
}

// the event handler that queues #83 then #87 (Gomove)
extern "C" PPC_FUNC(__imp__sub_82264688);
PPC_FUNC(sub_82264688)
{
    if (Enabled())
    {
        std::lock_guard lk(g_lock);
        if (g_lines < 600) { g_lines++; Log("[action] t=%llu ms Go-move event handler, controller %08X\n", (unsigned long long)NbashMs(), ctx.r3.u32); }
    }
    __imp__sub_82264688(ctx, base);
}

// ---- offense / defense: the game's "GameplayOffense", "GameplayDefense" and
// "GameplayBallCarrier" states of the human side (vtables near 0x820A9088). Their two functions are
// entering and leaving the state (confirmed in play: BallCarrier <-> Offense on passes, Defense
// after turnovers); they are hooked to know what the human side is doing; the input
// code uses NbashGameplayState() for the defense button layout. Logged with NBASH_ACTIONTRACE.
std::atomic<int> g_gameplayState{0};      // 0 unknown, 1 offense, 2 defense, 3 ball carrier
int NbashGameplayState() { return g_gameplayState.load(); }
namespace
{
    std::atomic<uint32_t> g_stateCalls[6];
    const char* kStateNames[6] = { "Offense.a", "Offense.b", "Defense.a", "Defense.b", "BallCarrier.a", "BallCarrier.b" };
    // .a = entering a state, .b = leaving it. The layout follows the states that are active:
    // ball carrier or offense -> offense; only defense -> defense; nothing active -> keep the last.
    std::atomic<bool> g_active[3];
    void StateCall(int i, uint32_t obj)
    {
        g_active[i / 2] = (i % 2) == 0;
        if (g_active[0] || g_active[2]) g_gameplayState = g_active[2] ? 3 : 1;
        else if (g_active[1]) g_gameplayState = 2;
        {   // state changes always go to the log (a few per possession)
            static std::atomic<int> n{0};
            if (n++ < 3000) Log("[state] t=%llu %s (object %08X) -> layout %s\n", (unsigned long long)NbashMs(), kStateNames[i], obj, g_gameplayState == 2 ? "defense" : "offense");
        }
        if (!Enabled()) return;
        static std::atomic<bool> started{false};
        if (!started.exchange(true))
            std::thread([] { for (;;) { std::this_thread::sleep_for(std::chrono::seconds(10));
                char line[256]; int n = snprintf(line, sizeof line, "[states] calls in 10 s:");
                for (int k = 0; k < 6; k++) n += snprintf(line + n, sizeof line - n, " %s %u", kStateNames[k], g_stateCalls[k].exchange(0));
                Log("%s\n", line); } }).detach();
        g_stateCalls[i]++;
        static std::atomic<int> lines{0};
        static std::atomic<int> last{-1};
        (void)last; (void)lines;
    }
}
#define STATE_HOOK(addr, i) \
    extern "C" PPC_FUNC(__imp__sub_##addr); \
    PPC_FUNC(sub_##addr) { StateCall(i, ctx.r3.u32); __imp__sub_##addr(ctx, base); }
STATE_HOOK(8210C710, 0)
STATE_HOOK(8210C930, 1)
STATE_HOOK(8210D058, 2)
STATE_HOOK(8210D5C8, 3)
STATE_HOOK(8210DCD8, 4)
STATE_HOOK(8210E420, 5)


// ---- the game's own debug messages (its print function sub_8218B718 is empty in the shipped
// game). Channel 5 / level 2 carries gameplay messages such as "Starting dunk", "Doing layup",
// "Shoot down" -- logged as [game] with the time and the player's trigger/shoot buttons, to
// learn how the game picks a dunk or a layup. On with NBASH_ACTIONTRACE.
extern uint8_t* g_base;
uint32_t NbashPadButtons();   // input.cpp: player 1's buttons and right trigger, as last given to the game
extern "C" PPC_FUNC(__imp__sub_8218B718);
PPC_FUNC(sub_8218B718)
{
    // always on: the game's replay messages ("start replay", "end replay cut to ...",
    // "useless replay - no cut moment", ...), to see why replays end early
    {
        // two call forms: (channel, level, fmt, args...) or (fmt, args...)
        bool direct = ctx.r3.u32 >= 0x82000000 && ctx.r3.u32 < 0x82800000;
        uint32_t fmt = direct ? ctx.r3.u32 : ctx.r5.u32;
        if (fmt >= 0x82000000 && fmt < 0x82800000)
        {
            char msg[120]; int n = 0;
            for (; n < 116; n++) { char c = char(g_base[fmt + n]); if (!c) break; msg[n] = (c == '\n') ? ' ' : c; }
            msg[n] = 0;
            static std::atomic<int> lines{0};
            int a = direct ? int(ctx.r4.u32) : int(ctx.r6.u32), b = direct ? int(ctx.r5.u32) : int(ctx.r7.u32), c = direct ? int(ctx.r6.u32) : int(ctx.r8.u32);
            if ((strstr(msg, "eplay") || strstr(msg, "fade") || strstr(msg, "Fade")) && lines++ < 400)
                Log("[replay] t=%llu \"%s\" args %d %d %d f1=%.3f pad %08X\n", (unsigned long long)NbashMs(),
                    msg, a, b, c, ctx.f1.f64, NbashPadButtons());
        }
    }
    if (Enabled() && ctx.r3.u32 == 5 && ctx.r4.u32 == 2)
    {
        uint32_t fmt = ctx.r5.u32;
        if (fmt >= 0x82000000 && fmt < 0x82800000)
        {
            char msg[96]; int n = 0;
            for (; n < 90; n++) { char c = char(g_base[fmt + n]); if (!c) break; msg[n] = (c == '\n') ? ' ' : c; }
            msg[n] = 0;
            static std::atomic<int> lines{0};
            if (lines++ < 3000)
            {
                uint32_t pb = NbashPadButtons();
                Log("[game] t=%llu ms \"%s\" (r6=%d r7=%d) pad: shoot %s, RT %s\n", (unsigned long long)NbashMs(), msg, int(ctx.r6.u32), int(ctx.r7.u32),
                    (pb & 0x2000) ? "down" : "up", (pb >> 16) > 100 ? "held" : "off");
            }
        }
    }
    __imp__sub_8218B718(ctx, base);
}

// ---- signal probe (F7): finds where the game keeps player 1's control values. With your player
// standing still holding the ball, press F7 and leave the controller alone for ~15 s: the probe
// presses the right trigger, then the left bumper, then the right bumper on its own (on/off, 4
// times each) and lists the memory words that switch exactly in step ([probe] lines in the log).
void GuestUsedRanges(uint32_t out[3][2]);
extern uint8_t* g_base;
std::atomic<int> g_probeRT{-1};            // forced right trigger (0-255) or -1
std::atomic<uint32_t> g_probeButtons{0};   // forced buttons
std::atomic<bool> g_probeActive{false};
namespace
{
    struct PCand { uint32_t addr, off, on; };
    void Snapshot(std::vector<uint32_t>& out, uint32_t r[3][2])
    {
        out.clear();
        for (int k = 0; k < 3; k++)
            for (uint32_t a = r[k][0]; a + 4 <= r[k][1]; a += 4) out.push_back(*(uint32_t*)(g_base + a));
    }
    void ProbeOne(const char* name, int rt, uint32_t buttons)
    {
        uint32_t r[3][2]; GuestUsedRanges(r);
        auto set = [&](bool on) { g_probeRT = on ? rt : (rt >= 0 ? 0 : -1); g_probeButtons = on ? buttons : 0; };
        auto settle = [] { std::this_thread::sleep_for(std::chrono::milliseconds(450)); };
        std::vector<uint32_t> offSnap, cur;
        set(false); settle(); Snapshot(offSnap, r);
        set(true); settle(); Snapshot(cur, r);
        std::vector<PCand> c;
        size_t i = 0;
        for (int k = 0; k < 3; k++)
            for (uint32_t a = r[k][0]; a + 4 <= r[k][1]; a += 4, i++)
                if (i < offSnap.size() && i < cur.size() && offSnap[i] != cur[i]) c.push_back({ a, offSnap[i], cur[i] });
        offSnap.clear(); offSnap.shrink_to_fit();
        for (int round = 0; round < 3; round++)
        {
            for (bool on : { false, true })
            {
                set(on); settle();
                size_t keep = 0;
                for (auto& x : c) { uint32_t v = *(uint32_t*)(g_base + x.addr); if (v == (on ? x.on : x.off)) c[keep++] = x; }
                c.resize(keep);
            }
        }
        set(false);
        Log("[probe] %s: %zu words switch in step\n", name, c.size());
        for (size_t j = 0; j < c.size() && j < 40; j++)
        {
            float fo, fn; memcpy(&fo, &c[j].off, 4); memcpy(&fn, &c[j].on, 4);
            uint32_t bo = __builtin_bswap32(c[j].off), bn = __builtin_bswap32(c[j].on); float go, gn; memcpy(&go, &bo, 4); memcpy(&gn, &bn, 4);
            Log("[probe]   %08X off=%08X on=%08X (big-endian float %g -> %g)\n", c[j].addr, bo, bn, go, gn);
        }
    }
}
void StartSignalProbe()
{
    bool expected = false;
    if (!g_probeActive.compare_exchange_strong(expected, true)) return;
    std::thread([] {
        Log("[probe] started -- leave the controller alone\n");
        ProbeOne("right trigger", 255, 0);
        ProbeOne("left bumper", -1, 0x0100);
        ProbeOne("right bumper", -1, 0x0200);
        g_probeRT = -1; g_probeButtons = 0;
        Log("[probe] finished\n");
        g_probeActive = false;
    }).detach();
}

// ---- call counters (diagnostic build, NBASH_FIRSTCALL). Every 250 ms the counters are copied
// into a ring (8 s). On a button press (NbashPressEvent), 1.5 s later the functions that ran in
// those 1.5 s but not at all in the 3 s before the press are logged as [after <button>] lines.
constexpr unsigned kMaxFuncs = 65536;
static unsigned g_callsStore[kMaxFuncs];
unsigned* g_nbashCalls = g_callsStore;
static const char* g_funcNames[kMaxFuncs];
static std::atomic<unsigned> g_funcCount{0};
unsigned NbashFuncIndex(const char* name)
{
    unsigned i = g_funcCount++;
    if (i >= kMaxFuncs) return kMaxFuncs - 1;
    g_funcNames[i] = name;
    static std::atomic<int> n{0};
    if (n++ < 60000) Log("[first] t=%llu %s\n", (unsigned long long)NbashMs(), name);
    return i;
}
namespace
{
    constexpr int kRing = 32;   // 32 x 250 ms
    std::vector<unsigned> g_ring[kRing];
    uint64_t g_ringTime[kRing];
    std::atomic<int> g_ringPos{0};
    std::mutex g_ringLock;
    struct Pending { uint64_t t; std::string what; };
    std::vector<Pending> g_pending;
    int Find(uint64_t t)   // newest snapshot at or before t
    {
        int best = -1; uint64_t bt = 0;
        for (int k = 0; k < kRing; k++) if (!g_ring[k].empty() && g_ringTime[k] <= t && g_ringTime[k] >= bt) { best = k; bt = g_ringTime[k]; }
        return best;
    }
    void Ticker()
    {
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            std::lock_guard lk(g_ringLock);
            int p = g_ringPos++ % kRing;
            unsigned n = std::min<unsigned>(g_funcCount.load(), kMaxFuncs);
            g_ring[p].assign(g_callsStore, g_callsStore + n);
            g_ringTime[p] = NbashMs();
            uint64_t now = g_ringTime[p];
            for (size_t i = 0; i < g_pending.size();)
            {
                auto& e = g_pending[i];
                if (now < e.t + 1500) { i++; continue; }
                int a = Find(e.t), b = Find(e.t - 3000);
                if (a >= 0 && b >= 0 && a != b)
                {
                    auto& A = g_ring[a]; auto& B = g_ring[b]; auto& C = g_ring[p];
                    int listed = 0;
                    for (unsigned f = 0; f < C.size(); f++)
                    {
                        unsigned before = (f < A.size() ? A[f] : 0) - (f < B.size() ? B[f] : 0);
                        unsigned after = C[f] - (f < A.size() ? A[f] : 0);
                        if (after && !before && listed++ < 120)
                            Log("[after %s @%llu] %s x%u\n", e.what.c_str(), (unsigned long long)e.t, g_funcNames[f] ? g_funcNames[f] : "?", after);
                    }
                    Log("[after %s @%llu] %d functions\n", e.what.c_str(), (unsigned long long)e.t, listed);
                }
                g_pending.erase(g_pending.begin() + i);
            }
        }
    }
}
void NbashPressEvent(const char* what)
{
    static std::atomic<bool> started{false};
    if (!started.exchange(true)) std::thread(Ticker).detach();
    std::lock_guard lk(g_ringLock);
    if (g_pending.size() < 16) g_pending.push_back({ NbashMs(), what });
}

// ---- read watch (diagnostic build): NBASH_WATCH="addr:len,addr:len" (hex), default = the
// controller-state arrays found by the F7 probe. Each new (function, address) pair that reads
// there is logged once as [watch]; every 10 s a count summary.
uint32_t g_watchLo = 0xFFFFFFFF, g_watchSpan = 0, g_watchLo2 = 0xFFFFFFFF, g_watchSpan2 = 0;
namespace
{
    struct WRange { uint32_t lo, len; };
    std::vector<WRange> g_wranges;
    std::mutex g_wlock;
    std::map<std::pair<const char*, uint32_t>, uint32_t> g_whits;
    struct WatchInit
    {
        WatchInit()
        {
            const char* e = getenv("NBASH_WATCH");
            std::string spec = e ? e : "";
            size_t p = 0;
            while (p < spec.size())
            {
                size_t c = spec.find(',', p); if (c == std::string::npos) c = spec.size();
                std::string item = spec.substr(p, c - p);
                size_t k = item.find(':');
                if (k != std::string::npos) g_wranges.push_back({ uint32_t(strtoul(item.substr(0, k).c_str(), nullptr, 16)), uint32_t(strtoul(item.substr(k + 1).c_str(), nullptr, 16)) });
                p = c + 1;
            }
            uint32_t lo = 0xFFFFFFFF, hi = 0;
            for (auto& r : g_wranges) { lo = std::min(lo, r.lo); hi = std::max(hi, r.lo + r.len); }
            if (hi > lo) { g_watchLo = lo; g_watchSpan = hi - lo; }
        }
    } g_watchInit;
}
void NbashWatchHit(uint32_t addr, const char* fn)
{
    int ri = -1;
    for (size_t i = 0; i < g_wranges.size(); i++) if (addr - g_wranges[i].lo < g_wranges[i].len) { ri = int(i); break; }
    if (ri < 0 && addr - g_watchLo2 < g_watchSpan2) { std::lock_guard lk(g_wlock); auto& n = g_whits[{ fn, addr }];
        if (n++ == 0) Log("[watch2] t=%llu %s reads %08X (+0x%X)\n", (unsigned long long)NbashMs(), fn, addr, addr - g_watchLo2); return; }
    if (ri < 0) return;
    std::lock_guard lk(g_wlock);
    auto& n = g_whits[{ fn, addr }];
    if (n++ == 0)
        Log("[watch] t=%llu %s reads %08X (range %d +0x%X)\n", (unsigned long long)NbashMs(), fn, addr, ri, addr - g_wranges[ri].lo);
}

#ifdef NBASH_FIRSTCALL
// watch player 1's pad controller (button bits at +52 .. +56) and its player's record field +7060
extern "C" PPC_FUNC(__imp__sub_8213E820);
PPC_FUNC(sub_8213E820)
{
    uint32_t ctrl = ctx.r3.u32;
    static bool armed = false;
    if (!armed && !getenv("NBASH_WATCH"))
    {
        int32_t pad = int32_t(__builtin_bswap32(*(uint32_t*)(g_base + ctrl + 24)));
        uint32_t player = __builtin_bswap32(*(uint32_t*)(g_base + ctrl + 12));
        if (pad == 0 && player)
        {
            armed = true;
            g_wranges.push_back({ ctrl + 28, 28 });
            g_watchLo = ctrl + 28; g_watchSpan = 28;        // sticks, trigger, button bits
            g_watchLo2 = player + 7060; g_watchSpan2 = 4;   // value the pad writes into the player record
            Log("[watch] pad 1 controller %08X (buttons at %08X), player record %08X\n", ctrl, ctrl + 52, player);
        }
    }
    __imp__sub_8213E820(ctx, base);
}
#endif

#ifdef NBASH_FIRSTCALL
// who asks for a pad state copy (sub_8213EBB8: copies controller+28..+56 to r4): each new caller
// (return address) is logged with the time and whether it is player 1's controller; every 10 s,
// counts per caller.
extern "C" PPC_FUNC(__imp__sub_8213EBB8);
PPC_FUNC(sub_8213EBB8)
{
    static std::mutex m; static std::map<uint32_t, uint32_t> callers; static std::atomic<bool> rep{false};
    uint32_t lr = uint32_t(ctx.lr), ctrl = ctx.r3.u32;
    int pad = int32_t(__builtin_bswap32(*(uint32_t*)(g_base + ctrl + 24)));
    {
        std::lock_guard lk(m);
        if (callers[lr]++ == 0) Log("[padcopy] t=%llu new caller %08X (controller %08X, pad %d)\n", (unsigned long long)NbashMs(), lr, ctrl, pad);
    }
    if (!rep.exchange(true))
        std::thread([] { for (;;) { std::this_thread::sleep_for(std::chrono::seconds(10)); std::lock_guard lk(m);
            char line[1024]; int n = snprintf(line, sizeof line, "[padcopy] calls in 10 s:");
            for (auto& [a, c] : callers) if (n < 1000) n += snprintf(line + n, sizeof line - n, " %08X x%u", a, c);
            Log("%s\n", line); for (auto& [a, c] : callers) c = 1; } }).detach();
    __imp__sub_8213EBB8(ctx, base);
}
#endif

#ifdef NBASH_FIRSTCALL
// ---- pad rules -> signals. The engine's pad rule runner (sub_823E8298) looks the signal up with
// sub_823E5DC8(table, key) and then sets it to r7 (still in r7 during the lookup). Log every change
// of a signal's value as [sig] with the key; and log names looked up through sub_823E6130 so keys
// can be matched to names ([signame]).
std::mutex g_keyNameLock; std::map<uint32_t, std::string> g_keyNames;   // signal key -> name
extern "C" PPC_FUNC(__imp__sub_823E5DC8);
PPC_FUNC(sub_823E5DC8)
{
    uint32_t key = ctx.r4.u32, lr = uint32_t(ctx.lr), val = ctx.r7.u32, table = ctx.r3.u32;
    __imp__sub_823E5DC8(ctx, base);
    if (lr != 0x823E85F0)
    {
        // other callers: log dunk/shot related lookups (who, which signal) once per (caller, key)
        std::string nm;
        { std::lock_guard lk(g_keyNameLock); auto it = g_keyNames.find(key); if (it != g_keyNames.end()) nm = it->second; }
        if (!nm.empty() && (nm.find("Dunk") != std::string::npos || nm.find("Gate") != std::string::npos || nm.find("Shot") != std::string::npos || nm.find("Layup") != std::string::npos))
        {
            static std::mutex m; static std::set<std::pair<uint32_t, uint32_t>> seen;
            std::lock_guard lk(m);
            if (seen.insert({ lr, key }).second)
                Log("[simsig] t=%llu %s looked up by %08X (table %08X, r5=%08X r6=%08X r7=%08X f1=%g)\n", (unsigned long long)NbashMs(), nm.c_str(), lr, table, ctx.r5.u32, ctx.r6.u32, val, ctx.f1.f64);
        }
    }
    if (lr == 0x823E85F0)
    {
        static std::mutex m; static std::map<std::pair<uint32_t, uint32_t>, uint32_t> last;
        std::lock_guard lk(m);
        auto it = last.find({ table, key });
        if (it == last.end() || it->second != val)
        {
            bool first = it == last.end();
            last[{ table, key }] = val;
            static std::atomic<int> lines{0};
            if (lines++ < 20000) Log("[sig] t=%llu table %08X key %08X -> %u%s\n", (unsigned long long)NbashMs(), table, key, val, first ? " (first)" : "");
        }
    }
}
extern "C" PPC_FUNC(__imp__sub_823E6130);
PPC_FUNC(sub_823E6130)
{
    uint32_t name = ctx.r4.u32;
    __imp__sub_823E6130(ctx, base);
    char s[64] = {}; for (int i = 0; i < 63; i++) { char c = char(g_base[name + i]); if (!c) break; s[i] = c; }
    static std::atomic<int> lines{0};
    if (lines++ < 3000)
    {
        uint32_t r = ctx.r3.u32, k0 = 0, k1 = 0, k2 = 0;
        if (r >= 0x40000000 && r < 0xC0000000) { k0 = __builtin_bswap32(*(uint32_t*)(g_base + r)); k1 = __builtin_bswap32(*(uint32_t*)(g_base + r + 4)); k2 = __builtin_bswap32(*(uint32_t*)(g_base + r + 8)); }
        Log("[signame] %s -> %08X (words %08X %08X %08X)\n", s, r, k0, k1, k2);
        std::lock_guard lk(g_keyNameLock); g_keyNames[k2] = s;
    }
}
#endif

// ---- replay recorder diagnostics (always on, rate-limited): each recorded frame is committed to
// a 28 MB ring of up to 2048 frames by sub_823B5700(ring, ?, size, frame); replays seek with
// sub_821F2BB8(this, tick) which clamps to the oldest recorded frame.
extern "C" PPC_FUNC(__imp__sub_823B5700);
PPC_FUNC(sub_823B5700)
{
    static std::atomic<uint64_t> n{0}, bytes{0}, maxSize{0};
    uint32_t size = ctx.r5.u32, frame = ctx.r6.u32, ring = ctx.r3.u32;
    uint64_t k = ++n; bytes += size; if (size > maxSize) maxSize = size;
    __imp__sub_823B5700(ctx, base);
    if (k <= 5 || k % 600 == 0)
    {
        int32_t head = int32_t(R32(ring + 16)), tail = int32_t(R32(ring + 20));
        uint32_t oldest = head >= 0 ? R32(ring + (head + 2) * 12) : 0, newest = tail >= 0 ? R32(ring + (tail + 2) * 12) : 0;
        Log("[recorder] frame %u size %u (avg %llu, max %llu over %llu frames) ring holds frames %u..%u (head %d tail %d)\n",
            frame, size, (unsigned long long)(bytes / k), (unsigned long long)maxSize.load(), (unsigned long long)k, oldest, newest, head, tail);
    }
}
extern "C" PPC_FUNC(__imp__sub_821F2BB8);
PPC_FUNC(sub_821F2BB8)
{
    uint32_t want = ctx.r4.u32, self = ctx.r3.u32;
    __imp__sub_821F2BB8(ctx, base);
    uint32_t obj = R32(self + 84);
    static std::atomic<int> lines{0};
    if (lines++ < 200) Log("[replay] seek to tick %u -> got %u\n", want, R32(obj + 8));
}
// replay director update (sub_822561F0): while a replay runs, log its state every frame
extern "C" PPC_FUNC(__imp__sub_822561F0);
PPC_FUNC(sub_822561F0)
{
    uint32_t self = ctx.r3.u32;
    __imp__sub_822561F0(ctx, base);
    static std::atomic<int> lines{0};
    uint32_t rep = R32(self + 20), st = R32(self + 16);
    int mode = st ? int32_t(R32(st + 24)) : -9;
    uint32_t g = R32(0x827218B0), clk = g ? R32(g + 84) : 0;
    float frac = 0; if (clk) { uint32_t b = R32(clk + 12); memcpy(&frac, &b, 4); }
    double tnow = clk ? double(R32(clk + 8)) + frac : -1;
    static int lastMode = -100;
    bool active = rep && (mode != 0 || R8(self + 959) || int32_t(R32(self + 208)) > 0);
    if ((active || mode != lastMode) && lines++ < 1500)
        Log("[replay] t=%llu mode %d time %.2f target %d start %d fade %d cnt192 %d n196 %d f956 %u f957 %u f959 %u\n",
            (unsigned long long)NbashMs(), mode, tnow, int32_t(R32(self + 216)), int32_t(R32(self + 712)), int32_t(R32(self + 208)),
            int32_t(R32(self + 192)), int32_t(R32(self + 196)), R8(self + 956), R8(self + 957), R8(self + 959));
    lastMode = mode;
}
// replay clock advance (sub_82274268): logs its timer (dt, speed, scale) - rate limited
extern "C" PPC_FUNC(__imp__sub_82274268);
PPC_FUNC(sub_82274268)
{
    uint32_t c = ctx.r3.u32;
    uint64_t before = R64(c + 8);
    // match-start inbound fast-forward (drivecam.cpp): the game clock's own speed factor (c+24, 1.0 normally) is raised
    {
        extern std::atomic<uint64_t> g_nbashFastUntil; extern float g_nbashFastSpeed;
        static bool on = false; uint64_t until = g_nbashFastUntil.load();
        bool want = until && NbashMs() < until;
        auto WFl = [](uint32_t a, float f) { uint32_t v; memcpy(&v, &f, 4); W32(a, v); };
        if (want) { WFl(c + 24, g_nbashFastSpeed); on = true; }
        else if (on) { WFl(c + 24, 1.0f); on = false; }
        // slower jump shot for chosen players (shotswap.cpp): the same speed factor, below 1
        extern float NbashShotSlowSpeed(); static bool slowOn = false;
        if (!want) { float sp = NbashShotSlowSpeed(); if (sp < 0.999f) { WFl(c + 24, sp); slowOn = true; } else if (slowOn) { WFl(c + 24, 1.0f); slowOn = false; } }
    }
    { extern void NbashShotSwapTick(); NbashShotSwapTick(); }       // per-player jump shots (shotswap.cpp)
    __imp__sub_82274268(ctx, base);
    static std::atomic<int> n{0};
    int k = n++;
    if (k < 40 || (k % 300 == 0 && k < 30000))
    {
        auto F = [](uint32_t a) { uint32_t b = R32(a); float f; memcpy(&f, &b, 4); return f; };
        uint32_t t = c + 32;
        Log("[clock] t=%llu tick %u+%.3f -> %u+%.3f dt %.4f speed %.4f | timer mode %d start %llu acc %llu scale %g mult %d\n",
            (unsigned long long)NbashMs(), uint32_t(before >> 32), [&] { uint32_t b = uint32_t(before); float f; memcpy(&f, &b, 4); return f; }(),
            R32(c + 8), F(c + 12), F(c + 16), F(c + 24), int32_t(R32(t + 8)), (unsigned long long)R64(t + 16), (unsigned long long)R64(t + 24), F(t + 32),
            int32_t(R32(0x82557778)));
    }
}

// ---- your music replaces the game's soundtrack -------------------------------------------
bool MusicReplacesGameMusic();                                              // music.cpp
bool MusicCurrentInfo(std::string& title, std::string& artist, std::string& album);

// sub_82269250 applies the options' volumes to the game's sound buses each update; with your
// music on, the menu (+24) and in-game (+28) music volumes are passed as 0 (your saved settings
// are put back right after, so the options menu still shows them).
extern "C" PPC_FUNC(__imp__sub_82269250);
PPC_FUNC(sub_82269250)
{
    uint32_t opt = 0;
    if (MusicReplacesGameMusic())
        if (uint32_t g = R32(0x82721898)) opt = R32(g + 4);
    if (!opt) { __imp__sub_82269250(ctx, base); return; }
    uint32_t fe = R32(opt + 24), ingame = R32(opt + 28);
    W32(opt + 24, 0); W32(opt + 28, 0);
    __imp__sub_82269250(ctx, base);
    W32(opt + 24, fe); W32(opt + 28, ingame);
    static bool logged = false;
    if (!logged) { logged = true; Log("[music] game soundtrack muted (menu music %u, in-game music %u kept in your options)\n", fe, ingame); }
}

// sub_822DFB10(eventName, ?, ?, argc, a, b, c) calls a UI function; for
// "_level0.gChyron.screen.ShowSong" (the song popup) the three texts come from a songs.bin
// record (title +0, artist +64, album +128): show your current song instead.
extern "C" PPC_FUNC(__imp__sub_822DFB10);
PPC_FUNC(sub_822DFB10)
{
    bool MusicIsOurTitle(const std::string&);
    if (ctx.r3.u32 == 0x82088910 && MusicReplacesGameMusic() && !MusicIsOurTitle(std::string(reinterpret_cast<const char*>(G2H(ctx.r7.u32))).substr(0, 63)))
    {
        static uint32_t rec = 0;
        if (!rec) rec = AllocSystem(288, 16);
        std::string t, a, al;
        if (MusicCurrentInfo(t, a, al))
        {
            memset(G2H(rec), 0, 288);
            auto put = [&](uint32_t off, const std::string& s) { size_t n = std::min<size_t>(s.size(), 63); memcpy(G2H(rec + off), s.data(), n); };
            put(0, t); put(64, a); put(128, al);
            ctx.r7.u64 = rec; ctx.r9.u64 = rec + 64; ctx.r8.u64 = rec + 128;
            Log("[music] song popup: %s - %s\n", a.c_str(), t.c_str());
        }
    }
    __imp__sub_822DFB10(ctx, base);
}

// sub_8214CD50(this, songIndex, ...) starts song slot N of the game's song list (options+48:
// 320-byte entries, the songs.bin record at +32). Play your song in that slot.
void MusicRequestTitle(const std::string& title);
extern "C" PPC_FUNC(__imp__sub_8214CD50);
PPC_FUNC(sub_8214CD50)
{
    uint32_t self = ctx.r3.u32;
    __imp__sub_8214CD50(ctx, base);
    if (!MusicReplacesGameMusic()) return;
    static uint32_t last = 0;
    uint32_t rec = R32(self + 1156);
    if (!rec || rec == last) return;
    last = rec;
    std::string title(reinterpret_cast<const char*>(G2H(rec)));
    MusicRequestTitle(title.substr(0, 63));
}
