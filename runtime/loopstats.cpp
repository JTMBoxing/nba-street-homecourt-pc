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

#define HOOK(addr, idx) \
    extern "C" PPC_FUNC(__imp__sub_##addr); \
    PPC_FUNC(sub_##addr) { Count(idx); __imp__sub_##addr(ctx, base); }

// Main-loop pacing: sub_821EE528 runs once per pass of the game's main loop (menus and
// matches alike) and the simulation advances one step per pass, so the console's 60 Hz display
// is what sets the game speed. Each pass starts 1/60 s after the previous one; a late pass does
// not wait, and after a long stall the schedule restarts. NBASH_NOLOOPSYNC=1 disables.
extern "C" PPC_FUNC(__imp__sub_821EE528);
PPC_FUNC(sub_821EE528)
{
    Count(0);
    static const bool sync = getenv("NBASH_NOLOOPSYNC") == nullptr;
    if (sync)
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
    __imp__sub_821EE528(ctx, base);
}
HOOK(821EE798, 1)
HOOK(821EB5B8, 2)
HOOK(821EB1F0, 3)
HOOK(82274DB8, 4)
HOOK(821CC710, 5)
HOOK(821EAF08, 6)
