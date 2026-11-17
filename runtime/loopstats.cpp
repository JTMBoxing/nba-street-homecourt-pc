// Counts how often the game's main-loop pieces run (logged every 10 s as [loop]), to work out
// how the game paces its simulation. Each hook just counts and calls the recompiled function.
#include "guest.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>

void Log(const char* fmt, ...);

namespace
{
    std::atomic<uint64_t> g_count[8];
    const char* kNames[8] = { "main 821EE528", "pass 821EE798", "sim 821EB5B8", "padA 821EB1F0", "padB 82274DB8", "821CC710", "821EAF08", "" };
    void Reporter()
    {
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            char line[512]; int n = snprintf(line, sizeof line, "[loop] per second:");
            for (int i = 0; i < 7; i++) n += snprintf(line + n, sizeof line - n, " %s %.1f;", kNames[i], g_count[i].exchange(0) / 10.0);
            Log("%s\n", line);
        }
    }
    void Count(int i)
    {
        static std::atomic<bool> started{false};
        if (!started.exchange(true)) std::thread(Reporter).detach();
        g_count[i]++;
    }
}

// ---- pacing statistics: time between the starts of two main-loop passes, logged every 10 s as [pace]
uint32_t VblankCount(); uint32_t NbashWaitVblank(uint32_t last); int NbashVblankSource(int* hz100); int NbashFps();
namespace
{
    std::atomic<uint32_t> g_hist[64];      // 1 ms bins, 0..63 ms
    void PaceReporter()
    {
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            uint32_t h[64]; uint64_t n = 0; for (int i = 0; i < 64; i++) { h[i] = g_hist[i].exchange(0); n += h[i]; }
            if (n < 100) continue;
            auto pct = [&](double p) { uint64_t acc = 0; for (int i = 0; i < 64; i++) { acc += h[i]; if (acc >= n * p) return i; } return 63; };
            int med = pct(0.5); uint64_t in = 0, lo = 0, hi = 0, vhi = 0; for (int i = 0; i < 64; i++) { if (i >= med - 2 && i <= med + 2) in += h[i]; else if (i < med - 2) lo += h[i]; else hi += h[i]; if (i >= 2 * med - 2) vhi += h[i]; }
            int hz100 = 0; int real = NbashVblankSource(&hz100);
            Log("[pace] pass interval ms: p1 %d p5 %d median %d p95 %d p99 %d | within 2 ms of the median %.1f%%, shorter %.1f%%, longer %.1f%% (a whole frame late: %llu of %llu) | tick = %s\n",
                pct(0.01), pct(0.05), pct(0.5), pct(0.95), pct(0.99), 100.0 * in / n, 100.0 * lo / n, 100.0 * hi / n, (unsigned long long)vhi, (unsigned long long)n,
                real ? "display vblank" : "timer");
            Log("[pace] frame rate setting %d, display %.2f Hz\n", NbashFps(), hz100 / 100.0);
        }
    }
    void PaceSample()
    {
        static std::atomic<bool> started{false};
        if (!started.exchange(true)) std::thread(PaceReporter).detach();
        static auto prev = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(now - prev).count(); prev = now;
        int b = ms < 0 ? 0 : ms > 63 ? 63 : int(ms + 0.5);
        g_hist[b]++;
    }
}

#define HOOK(addr, idx) \
    extern "C" PPC_FUNC(__imp__sub_##addr); \
    PPC_FUNC(sub_##addr) { Count(idx); __imp__sub_##addr(ctx, base); }

// Main-loop pacing: sub_821EE528 runs once per pass of the game's main loop (menus and
// matches alike) and the simulation advances one step per pass, so the console's 60 Hz display
// is what sets the game speed. Each pass starts 1/60 s after the previous one; a late pass does
// not wait, and after a long stall the schedule restarts. NBASH_NOLOOPSYNC=1 disables.
#include <atomic>
extern std::atomic<uint64_t> g_nbashFastUntil;   // drivecam.cpp
uint64_t NbashMs();
extern "C" PPC_FUNC(__imp__sub_821EE528);
PPC_FUNC(sub_821EE528)
{
    Count(0);
    static const bool sync = getenv("NBASH_NOLOOPSYNC") == nullptr;
    static const bool old = getenv("NBASH_OLDPACE") != nullptr;
    if (sync && !(g_nbashFastUntil.load() && NbashMs() < g_nbashFastUntil.load()))
    {
        if (!old)
        {
            // 13 Nov: each pass starts on the display tick (gpu.cpp VblankThread). A pass that is already late (the tick has moved on) does not wait.
            static uint32_t last = VblankCount();
            uint32_t c = VblankCount();
            if (c == last) c = NbashWaitVblank(last);
            last = c;
        }
        else
        {
            static auto next = std::chrono::steady_clock::now();
            constexpr auto period = std::chrono::nanoseconds(16666667);
            next += period;
            auto now = std::chrono::steady_clock::now();
            if (next < now - period) next = now;
            else if (now < next)
            {
                if (next - now > std::chrono::milliseconds(3)) std::this_thread::sleep_until(next - std::chrono::milliseconds(2));
                while (std::chrono::steady_clock::now() < next) std::this_thread::yield();
            }
        }
    }
    PaceSample();
    __imp__sub_821EE528(ctx, base);
}
HOOK(821EE798, 1)
HOOK(821EB5B8, 2)
HOOK(821EB1F0, 3)
HOOK(82274DB8, 4)
HOOK(821CC710, 5)
HOOK(821EAF08, 6)
