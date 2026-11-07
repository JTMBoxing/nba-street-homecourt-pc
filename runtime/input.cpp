// Controller input. On Windows, Xbox controllers are read through XInput (same button layout as
// the Xbox 360), and the keyboard works as a fallback for player 1.
// On Linux (test builds) NBASH_AUTOPRESS=1 taps Start/A every few seconds to move through menus.
#include "guest.h"
#include <vector>
#include <atomic>
#include <mutex>
#include <chrono>
#include <map>
#include <thread>
uint64_t GpuFrameCount();
uint32_t VblankCount();
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#include <xinput.h>
#endif

bool NbashStatsInput(uint16_t pressed, bool inMatch); bool NbashStatsOpen(); bool NbashInMatch(); void NbashMenuMove();
namespace
{
    struct Pad { uint16_t buttons = 0; uint8_t lt = 0, rt = 0; int16_t lx = 0, ly = 0, rx = 0, ry = 0; };

#ifdef _WIN32
    // XInput loaded at run time: 1.4 ships with Windows 10/11 and sees current controllers;
    // older versions are fallbacks.
    using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    using XInputSetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
    XInputGetStateFn pXInputGetState = nullptr;
    XInputSetStateFn pXInputSetState = nullptr;
    void LoadXInput()
    {
        static bool tried = false;
        if (tried) return;
        tried = true;
        for (const char* dll : { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" })
            if (HMODULE m = LoadLibraryA(dll))
            {
                pXInputGetState = (XInputGetStateFn)GetProcAddress(m, "XInputGetState");
                pXInputSetState = (XInputSetStateFn)GetProcAddress(m, "XInputSetState");
                if (pXInputGetState) { Log("[input] using %s\n", dll); return; }
            }
        Log("[input] XInput not available - keyboard only\n");
    }

    bool Key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
    extern "C" HWND NbashWindow();

    // Y-trick modifier for right-stick moves: hold turbo (RT, default) or LT (NBASH_YMOD=lt)
    bool YModIsLT() { static const bool lt = getenv("NBASH_YMOD") && !strcmp(getenv("NBASH_YMOD"), "lt"); return lt; }
    bool YMod(const Pad& p) { return YModIsLT() ? p.lt > 100 : p.rt > 100; }
    bool ReadKeyboard(Pad& p)
    {
        HWND w = NbashWindow();
        if (!w || GetForegroundWindow() != w) return false;     // only while our window is focused
        struct { int vk; uint16_t bit; } map[] = {
            { VK_UP, 0x0001 }, { VK_DOWN, 0x0002 }, { VK_LEFT, 0x0004 }, { VK_RIGHT, 0x0008 },
            { VK_RETURN, 0x0010 }, { VK_BACK, 0x0020 }, { 'Q', 0x0040 }, { 'E', 0x0080 },
            { 'R', 0x0100 }, { 'F', 0x0200 },                          // LB, RB
            { VK_SPACE, 0x1000 }, { 'X', 0x2000 }, { 'Z', 0x4000 }, { 'C', 0x8000 },  // A B X Y
        };
        bool any = false;
        for (auto& m : map) if (Key(m.vk)) { p.buttons |= m.bit; any = true; }
        if (Key('W')) { p.ly = 32767; any = true; }
        if (Key('S')) { p.ly = -32768; any = true; }
        if (Key('A')) { p.lx = -32768; any = true; }
        if (Key('D')) { p.lx = 32767; any = true; }
        if (Key(VK_SHIFT)) { p.rt = 255; any = true; }                  // turbo
        if (Key(VK_CONTROL)) { p.lt = 255; any = true; }
        if (Key(VK_TAB)) { p.buttons |= 0x0100; any = true; }          // Tab = LB (opens the player ratings screen in menus)
        return any;
    }

    bool ReadPad(uint32_t user, Pad& p)
    {
        XINPUT_STATE s{};
        LoadXInput();
        bool connected = pXInputGetState && pXInputGetState(user, &s) == ERROR_SUCCESS;
        static bool seen[4] = {};
        if (connected && !seen[user]) { seen[user] = true; Log("[input] Xbox controller %u connected\n", user + 1); }
        if (connected)
        {
            auto& g = s.Gamepad;
            p = { g.wButtons, g.bLeftTrigger, g.bRightTrigger, g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY };
        }
        if (user == 0)
        {
            Pad k;
            if (ReadKeyboard(k) || !connected)
            {
                if (connected) { p.buttons |= k.buttons; if (k.lx) p.lx = k.lx; if (k.ly) p.ly = k.ly;
                                 if (k.lt) p.lt = k.lt; if (k.rt) p.rt = k.rt; }
                else p = k;
            }
            return true;                                                  // player 1 always present
        }
        return connected;
    }
    void SetVibration(uint32_t user, uint16_t left, uint16_t right)
    {
        XINPUT_VIBRATION v{ left, right };
        LoadXInput();
        if (pXInputSetState) pXInputSetState(user, &v);
    }
#else
    bool ReadPad(uint32_t user, Pad& p)
    {
        if (user != 0) return false;
        static const bool autopress = getenv("NBASH_AUTOPRESS") != nullptr;
        if (autopress)
        {
            using namespace std::chrono;
            static const auto start = steady_clock::now();
            auto ms = duration_cast<milliseconds>(steady_clock::now() - start).count();
            if (ms > 15000 && ms % 3000 < 150) p.buttons = (ms / 3000) % 2 ? 0x0010 : 0x1000;   // Start, A
        }
        return true;
    }
    void SetVibration(uint32_t, uint16_t, uint16_t) {}
#endif
    uint32_t g_packet[4] = {};
    Pad g_last[4];
}

static std::atomic<uint32_t> g_lastPad1{0};
uint32_t NbashPadButtons() { return g_lastPad1.load(); }

uint64_t NbashMs()
{
    static const auto t0 = std::chrono::steady_clock::now();
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
}

// XamInputGetState(user, flags, XINPUT_STATE*)
IMPORT(XamInputGetState)
{
    TRACE_IMPORT();
    uint32_t user = ARG(3).u32 & 0xFF, out = ARG(5).u32;
    static const bool trace = getenv("NBASH_INPUTTRACE") != nullptr;
    if (trace)
    {
        static std::mutex m; static std::map<uint64_t, uint64_t> counts; static auto last = std::chrono::steady_clock::now();
        std::lock_guard lk(m);
        if (counts[(uint64_t(ctx.lr) << 8) | user]++ == 0 && user == 0)
        {
            char chain[512]; int n = 0;
            uint32_t frame = ctx.r1.u32;
            for (int d = 0; d < 20 && n < 480; d++)
            {
                uint32_t back = R32(frame);
                if (back <= frame || back - frame > 0x100000) break;
                n += snprintf(chain + n, sizeof chain - n, " %08X", R32(back - 8));
                frame = back;
            }
            chain[n] = 0;
            Log("[input-trace] chain thread %u:%s\n", t_thread ? t_thread->id : 0, chain);
        }
        if (std::chrono::steady_clock::now() - last > std::chrono::seconds(10))
        {
            last = std::chrono::steady_clock::now();
            for (auto& [k, v] : counts) Log("[input-trace] lr %08llX user %llu: %llu calls\n", (unsigned long long)(k >> 8), (unsigned long long)(k & 0xFF), (unsigned long long)v);
            counts.clear();
        }
    }
    // Pace the game's main loop to the 60 Hz display, as on the console: it reads player 1's
    // controller once per pass, so that read waits for the next vblank. Without this the loop
    // ran ~1500 times a second and menu "hold to repeat" scrolled far too fast.
    // (NBASH_NOINPUTSYNC turns it off.)
    // Main-loop pacing. The game advances its simulation one step per pass of its main loop
    // (sub_821EE528), which on the console is held to the 60 Hz display. Each pass reads player
    // 1's pad from sub_821EB1F0 (and sometimes again from sub_82274DB8), so the read from
    // sub_821EB1F0 waits until 1/60 s after the previous pass. A late pass does not wait, and
    // after a long stall the schedule restarts instead of running catch-up passes.
    // NBASH_NOLOOPSYNC=1 disables it.
    static const bool sync = false;
    uint32_t lr = uint32_t(ctx.lr); (void)lr;
    if (false && sync && user == 0 && lr >= 0x821EB1F0 && lr < 0x821EB5B8)   // moved to loopstats.cpp (sub_821EE528)
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
    Pad p;
    if (user >= 4 || !ReadPad(user, p)) RETURN(0x48F);                // ERROR_DEVICE_NOT_CONNECTED
    // player ratings screen: LB in the menus opens it; while open it takes all of player 1's input
    if (user == 0)
    {
        extern bool NbashStatsInput(uint16_t pressed, bool inMatch); extern bool NbashStatsOpen(); extern bool NbashInMatch();
        static uint16_t prevB = 0;
        uint16_t b = p.buttons;
        if (std::abs(int(p.lx)) > 20000) b |= p.lx < 0 ? 0x0004 : 0x0008;      // left stick works like the d-pad here
        if (std::abs(int(p.ly)) > 20000) b |= p.ly > 0 ? 0x0001 : 0x0002;
        uint16_t pressed = uint16_t(b & ~prevB); prevB = b;
        bool wasOpen = NbashStatsOpen();
        if ((pressed && NbashStatsInput(pressed, NbashInMatch())) || wasOpen || NbashStatsOpen()) p = Pad{};
    }
    // Optional control scheme (NBASH_RSTICK=dribble, "Play (right stick dribble).bat"):
    //  - the X button shoots (it sends B, the game's shoot button); B sets a pick (sends R3)
    //  - dribble moves are on the right stick:
    //      push left or right                        -> crossover (tap X)
    //      push down, then rotate to the left/right  -> spin dribble: the game's "Go" move,
    //                                                   left bumper = spin left, right = right
    //                                                   (Seq_1ON1_GO_SPINDRIB_SOLO_L/_R)
    //      push up, then rotate to the left/right    -> Spinstep (stutter + spin): X held at least
    //                                                   NBASH_RS_HOLDMS ms (default 600)
    // The stick must come back near the middle before the next move. It is not passed to the game.
    {
        static const bool rsDribble = !(getenv("NBASH_RSTICK") && !strcmp(getenv("NBASH_RSTICK"), "off"));   // your custom scheme is the default
        extern bool NbashInMatch();
        if (rsDribble && NbashInMatch())   // menus keep the normal buttons (A forward, B back)
        {
            static const int holdMs = getenv("NBASH_RS_HOLDMS") ? atoi(getenv("NBASH_RS_HOLDMS")) : 600;
            static const int bumperMs = getenv("NBASH_RS_BUMPERMS") ? atoi(getenv("NBASH_RS_BUMPERMS")) : 120;
            // buttons: on offense X shoots (sends B); on defense X is steal (stays X) and Y jumps (sends B)
            extern int NbashGameplayState(); extern int NbashHumanOnDefense();
            int onDef = NbashHumanOnDefense();
            bool defense = onDef >= 0 ? onDef == 1 : NbashGameplayState() == 2;   // the engine's own value for your player
            { static int lastLayout = -1; if (int(defense) != lastLayout) { lastLayout = defense; Log("[layout] t=%llu %s (engine says %s)\n", (unsigned long long)NbashMs(), defense ? "defense: X steal, Y jump" : "offense: X shoot, B pick", onDef < 0 ? "unknown" : onDef ? "on defense" : "on offense"); } }
            // on offense the B button sets a pick (the game's R3 / right stick click)
            if (defense)
            {
                if (p.buttons & 0x8000) { p.buttons &= ~0x8000; p.buttons |= 0x2000; }   // Y -> jump (B)
                // X = steal (mash it); NBASH_STEAL_MS > 0 would repeat it while held
                static const int stealMs = getenv("NBASH_STEAL_MS") ? atoi(getenv("NBASH_STEAL_MS")) : 0;   // 0 = mash (default)
                static uint64_t stealT[4];
                uint64_t now = NbashMs();
                if (p.buttons & 0x4000)
                {
                    if (!stealT[user])
                    {
                        // NBASH_STEALCAPTURE=1: the first 3 steal presses save the frame 0.15 s and
                        // 0.4 s later (like F12), to catch the steal prompt icon
                        static const bool capSteal = getenv("NBASH_STEALCAPTURE") != nullptr;
                        static int caps = 0;
                        if (capSteal && caps < 3)
                        {
                            caps++;
                            std::thread([] {
                                extern std::atomic<bool> g_captureRequest;
                                std::this_thread::sleep_for(std::chrono::milliseconds(150)); g_captureRequest = true;
                                std::this_thread::sleep_for(std::chrono::milliseconds(250)); g_captureRequest = true;
                            }).detach();
                            Log("[capture] steal press %d: saving frames\n", caps);
                        }
                    }
                    if (!stealT[user]) stealT[user] = now;
                    uint64_t held = now - stealT[user];
                    if (stealMs > 0 && held >= uint64_t(stealMs) && (held / (stealMs / 2)) % 2 == 1) p.buttons &= ~0x4000;   // let go half of each period
                }
                else stealT[user] = 0;
            }
            else
            {
                // R3 (clicking the right stick) = Spinstep: X held at least NBASH_RS_HOLDMS, longer while R3 is held
                static std::chrono::steady_clock::time_point r3Until[4];
                static uint16_t r3Tb[4] = { 0x4000, 0x4000, 0x4000, 0x4000 };   // X, or Y with LT held
                bool r3 = (p.buttons & 0x0080) != 0;
                p.buttons &= ~0x0080;
                {
                    auto t = std::chrono::steady_clock::now();
                    static bool r3Was[4];
                    if (r3 && !r3Was[user]) { r3Until[user] = t + std::chrono::milliseconds(holdMs); r3Tb[user] = YMod(p) ? 0x8000 : 0x4000; Log("[move] t=%llu ms pad%u R3 spinstep\n", (unsigned long long)NbashMs(), user + 1); }
                    if (r3 && t + std::chrono::milliseconds(30) > r3Until[user] && r3Was[user]) r3Until[user] = t + std::chrono::milliseconds(30);
                    r3Was[user] = r3;
                }
                if (p.buttons & 0x2000) { p.buttons &= ~0x2000; p.buttons |= 0x0080; }
                if (p.buttons & 0x4000) { p.buttons &= ~0x4000; p.buttons |= 0x2000; }
                // Y = off the heezy. The game does it for a Y tap with the left stick idle (defender in
                // front), so: left stick held idle from the press, Y sent NBASH_HEEZY_PRE ms later (150),
                // stick kept idle NBASH_HEEZY_POST ms more (250).
                {
                    static const int pre = getenv("NBASH_HEEZY_PRE") ? atoi(getenv("NBASH_HEEZY_PRE")) : 150;
                    static const int post = getenv("NBASH_HEEZY_POST") ? atoi(getenv("NBASH_HEEZY_POST")) : 250;
                    static std::chrono::steady_clock::time_point hStart[4]; static bool hActive[4], yWas[4];
                    auto t = std::chrono::steady_clock::now();
                    bool y = (p.buttons & 0x8000) != 0 && !YMod(p);   // (modifier + stick moves send Y themselves)
                    if (y) p.buttons &= ~0x8000;
                    if (y && !yWas[user]) { hStart[user] = t; hActive[user] = true; Log("[move] t=%llu ms pad%u Y: off the heezy\n", (unsigned long long)NbashMs(), user + 1); }
                    yWas[user] = y;
                    if (hActive[user])
                    {
                        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t - hStart[user]).count();
                        if (ms < pre + 120 + post) { p.lx = 0; p.ly = 0; }
                        if (ms >= pre && ms < pre + 120) p.buttons |= 0x8000;   // the Y tap
                        if (ms >= pre + 120 + post) hActive[user] = false;
                    }
                }
                // (after the X -> shoot mapping, so the Spinstep's X stays a trick)
                if (std::chrono::steady_clock::now() < r3Until[user])
                {
                    p.buttons |= r3Tb[user];
                    if (r3Tb[user] == 0x8000 && YModIsLT()) p.lt = 0;   // LT was our modifier
                }
            }
            enum State { Idle, Down, Up, WaitCentre, SideHold };
            // hold LT: the stick moves use the Y trick button instead of X (LT itself is hidden from the
            // game while a stick move runs)
            const uint16_t tb = YMod(p) ? 0x8000 : 0x4000;
            static uint16_t gestureTb[4] = { 0x4000, 0x4000, 0x4000, 0x4000 };
            // held up / down: keep dribbling like mashing X -- a tap every NBASH_DRIBBLE_MS (160)
            static const int dribbleMs = getenv("NBASH_DRIBBLE_MS") ? atoi(getenv("NBASH_DRIBBLE_MS")) : 160;
            static std::chrono::steady_clock::time_point sideT[4], moveUntil[4];
            static int moveDir[4];
            static const int moveDribbleMs = getenv("NBASH_MOVEDRIBBLE_MS") ? atoi(getenv("NBASH_MOVEDRIBBLE_MS")) : 0;   // off: crossovers stay in place
            static State st[4] = {};
            static std::chrono::steady_clock::time_point xUntil[4], bumpUntil[4];
            static uint16_t bump[4];
            static std::chrono::steady_clock::time_point tapFrom[4];   // X of a bumper move starts here
            static std::chrono::steady_clock::time_point noTbUntil[4]; // trick buttons forced up until here
            auto now = std::chrono::steady_clock::now();
            int rx = p.rx, ry = p.ry;                       // ry > 0 = up, < 0 = down
            int64_t mag2 = int64_t(rx) * rx + int64_t(ry) * ry;
            bool centred = mag2 < 12000ll * 12000ll, pushed = mag2 > 16000ll * 16000ll;
            // zones: up / down reach to ~59 degrees from vertical (so a quick quarter-circle that starts on
            // a diagonal still counts as starting down or up); only a clearly sideways flick is a crossover.
            // "rotated" = past 45 degrees towards the side.
            bool down = pushed && ry < 0 && -ry * 10 >= std::abs(rx) * 6, up = pushed && ry > 0 && ry * 10 >= std::abs(rx) * 6;
            bool sideways = pushed && !down && !up;
            bool rotated = std::abs(rx) > 12000 && std::abs(rx) > std::abs(ry);
            if (defense) st[user] = WaitCentre;   // on defense the stick does no dribble moves
            else switch (st[user])
            {
            case Idle:
                if (tb == 0x8000 && pushed)
                {
                    // LT held: simple Y moves -- up = Y, left = LB + Y, right = RB + Y, down = LB + RB + Y
                    bump[user] = up ? 0 : down ? 0x0300 : rx < 0 ? 0x0100 : 0x0200;
                    gestureTb[user] = 0x8000;
                    bumpUntil[user] = now + std::chrono::milliseconds(bump[user] ? 300 : 0);
                    tapFrom[user] = now + std::chrono::milliseconds(bump[user] ? 60 : 0);
                    xUntil[user] = tapFrom[user] + std::chrono::milliseconds(120);
                    noTbUntil[user] = tapFrom[user];
                    st[user] = WaitCentre;
                    Log("[move] t=%llu ms pad%u LT+%s: %sY\n", (unsigned long long)NbashMs(), user + 1, up ? "up" : down ? "down" : rx < 0 ? "left" : "right",
                        up ? "" : down ? "LB + RB + " : rx < 0 ? "LB + " : "RB + ");
                }
                else if (down) { st[user] = Down; sideT[user] = now; }
                else if (up) { st[user] = Up; sideT[user] = now; }
                else if (sideways)
                {
                    p.buttons |= tb; gestureTb[user] = tb; st[user] = WaitCentre;
                    // moving crossover: if the left stick is idle, push it to that side briefly so the
                    // game uses its moving dribble move (NBASH_MOVEDRIBBLE_MS, default 250; 0 = off)
                    int64_t lmag2 = int64_t(p.lx) * p.lx + int64_t(p.ly) * p.ly;
                    if (moveDribbleMs > 0 && lmag2 < 9000ll * 9000ll) { moveUntil[user] = now + std::chrono::milliseconds(moveDribbleMs); moveDir[user] = rx > 0 ? 1 : -1; }
                    Log("[move] t=%llu ms pad%u crossover\n", (unsigned long long)NbashMs(), user + 1);
                    { extern void NbashProbeCrossover(); if (user == 0) NbashProbeCrossover(); }
                }
                break;
            case Up:     // held up: front dribble (X tapped repeatedly); up then rotate = LB + RB + X
                if (centred) st[user] = Idle;
                else if (rotated)
                {
                    bump[user] = 0x0300; gestureTb[user] = tb;
                    bumpUntil[user] = now + std::chrono::milliseconds(300);
                    tapFrom[user] = now + std::chrono::milliseconds(80);    // a clear gap after any dribble press,
                    xUntil[user] = now + std::chrono::milliseconds(200);    // so the game sees a fresh press
                    noTbUntil[user] = tapFrom[user];
                    st[user] = WaitCentre;
                    Log("[move] t=%llu ms pad%u up+rotate: LB + RB + %s\n", (unsigned long long)NbashMs(), user + 1, tb == 0x8000 ? "Y" : "X");
                }
                else if (dribbleMs > 0)
                {
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - sideT[user]).count();
                    if (ms % dribbleMs < dribbleMs / 2) p.buttons |= tb;
                }
                break;
            case Down:   // held down: behind-the-back dribble (RB held, X tapped repeatedly)
                if (centred) st[user] = Idle;
                else if (rotated)
                {
                    // down then rotate: left = LB + X, right = RB + X (bumper first, then the X tap)
                    bump[user] = rx < 0 ? 0x0100 : 0x0200; gestureTb[user] = tb;
                    bumpUntil[user] = now + std::chrono::milliseconds(300);
                    tapFrom[user] = now + std::chrono::milliseconds(80);    // a clear gap after any dribble press,
                    xUntil[user] = now + std::chrono::milliseconds(200);    // so the game sees a fresh press
                    noTbUntil[user] = tapFrom[user];
                    st[user] = WaitCentre;
                    Log("[move] t=%llu ms pad%u down+rotate %s: %s + X\n", (unsigned long long)NbashMs(), user + 1, rx < 0 ? "left" : "right", rx < 0 ? "LB" : "RB");
                }
                else
                {
                    p.buttons |= 0x0200;
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - sideT[user]).count();
                    if (dribbleMs > 0 && ms >= 40 && (ms - 40) % dribbleMs < dribbleMs / 2) p.buttons |= tb;   // RB first, then X
                }
                break;
            case SideHold:
            case WaitCentre:
                if (centred) st[user] = Idle;
                else if (now < xUntil[user] && now >= bumpUntil[user]) xUntil[user] = std::max(xUntil[user], now + std::chrono::milliseconds(30));  // Spinstep: keep holding while pushed
                break;
            }
            if (now < moveUntil[user])
            {
                int64_t lmag2 = int64_t(p.lx) * p.lx + int64_t(p.ly) * p.ly;
                if (lmag2 < 9000ll * 9000ll) { p.lx = int16_t(moveDir[user] * 26000); p.ly = 0; }   // still idle: keep the nudge
                else moveUntil[user] = now;                                                        // player took over
            }
            if (now < noTbUntil[user]) p.buttons &= ~(0x4000 | 0x8000);
            if (now < xUntil[user] && now >= tapFrom[user]) p.buttons |= gestureTb[user];
            if (now < bumpUntil[user] && now >= tapFrom[user]) p.buttons |= bump[user];   // bumper together with the trick button
            p.lt = 0;   // in this layout LT is only the Y-trick switch: the game never sees it (no walking)
            p.rx = 0; p.ry = 0;
        }
    }
    // Analog walk: a light push of the left stick walks (the game's LT walk), a full push runs.
    // NBASH_WALK=0 turns it off; NBASH_WALK_PCT sets the walk/run boundary (% of full tilt, default 70).
    {
        static const bool walkOn = !(getenv("NBASH_WALK") && getenv("NBASH_WALK")[0] == '0');
        static const int walkPct = getenv("NBASH_WALK_PCT") ? atoi(getenv("NBASH_WALK_PCT")) : 70;
        if (walkOn && p.rt <= 100)   // turbo always runs
        {
            int64_t m2 = int64_t(p.lx) * p.lx + int64_t(p.ly) * p.ly;
            int64_t lo = 9000, hi = int64_t(32767) * walkPct / 100;
            static bool walking[4];
            // a little hysteresis so the stride doesn't flicker at the boundary
            int64_t edge = walking[user] ? hi + 1500 : hi;
            walking[user] = m2 > lo * lo && m2 < edge * edge;
            if (walking[user]) p.lt = 255;
        }
    }
    // A quick tap can fall between two of the game's reads (~16 ms apart); keep every newly
    // pressed button down for at least 3 reads so the game always sees it.
    {
        static uint16_t held[4]; static uint8_t left[4][16];
        for (int b = 0; b < 16; b++)
        {
            uint16_t bit = uint16_t(1u << b);
            if ((p.buttons & bit) && !(held[user] & bit)) left[user][b] = 3;
            if (left[user][b]) { left[user][b]--; p.buttons |= bit; }
        }
        held[user] = p.buttons;
    }
    if (user == 0)
    {
        extern std::atomic<bool> g_probeActive; extern std::atomic<int> g_probeRT; extern std::atomic<uint32_t> g_probeButtons;
        if (g_probeActive) { p = Pad{}; if (g_probeRT >= 0) p.rt = uint8_t(g_probeRT.load()); p.buttons = uint16_t(g_probeButtons.load()); }
        uint32_t now1 = p.buttons | (uint32_t(p.rt > 100 ? 1 : 0) << 16);
        static uint32_t prev1 = 0;
#ifdef NBASH_FIRSTCALL
        static const bool trace = true;   // diagnostic build: always record presses
#else
        static const bool trace = getenv("NBASH_ACTIONTRACE") != nullptr;
#endif
        if (trace && now1 != prev1)
        {
            uint32_t ch = now1 ^ prev1;
            const struct { uint32_t bit; const char* name; } names[] = { { 0x2000, "shoot(B)" }, { 0x4000, "X" }, { 0x8000, "Y" }, { 0x1000, "A" },
                { 0x0100, "LB" }, { 0x0200, "RB" }, { 0x10000, "turbo(RT)" } };
            for (auto& n : names) if (ch & n.bit)
            {
                Log("[press] t=%llu %s %s\n", (unsigned long long)NbashMs(), n.name, (now1 & n.bit) ? "down" : "up");
#ifdef NBASH_FIRSTCALL
                extern void NbashPressEvent(const char*);
                if (now1 & n.bit) NbashPressEvent(n.name);
#endif
            }
            prev1 = now1;
        }
        g_lastPad1 = p.buttons | (uint32_t(p.rt) << 16);
    }
    // dunk only with turbo: a dunk started without turbo becomes a layup (quick tap) -- gameplay.cpp
    if (user == 0)
    {
        static const bool turboDunk = !(getenv("NBASH_TURBODUNK") && getenv("NBASH_TURBODUNK")[0] == '0');
        static const int layupMs = getenv("NBASH_LAYUP_MS") ? atoi(getenv("NBASH_LAYUP_MS")) : 70;
        // with turbo: a dunk always stays a dunk -- a quick tap would be the game's layup, so the
        // shoot press is held for the game for at least NBASH_DUNK_MS (default 250 ms)
        static const int dunkMs = getenv("NBASH_DUNK_MS") ? atoi(getenv("NBASH_DUNK_MS")) : 250;
        static uint64_t pressT = 0, holdUntil = 0; static uint64_t g_dunkStyleUntil = 0; static bool master = true;
        static const char* kClass[3] = { "SigSIM_Class_SmallMan", "SigSIM_Class_HighFlyer", "SigSIM_Class_BigMan" };
        static const char* kType[3] = { "SigSIM_DunkType_Rotating", "SigSIM_DunkType_Signature", "SigSIM_DunkType_Windmill" };
        static int anyClass = -1, anyType = 0; static float origClass[3], origType[3]; static uint64_t anyUntil = 0; static uint64_t g_probeAt1 = 0, g_probeAt2 = 0, g_probeAt3 = 0; static bool noTurbo = false, cut = false;
        static uint16_t layupStyle = 0; static bool carrying = false;
        // safety net: if a cut-short press did not start anything (e.g. standing in the paint with the
        // dribble used), the next press is left alone so the player is never stuck
        static uint64_t cutT = 0; static bool cutChecked = true, natural = false; static uint64_t naturalSince = 0;
        static const bool randomLayups = !getenv("NBASH_NORANDOMLAYUP");
        if (turboDunk)
        {
            extern bool NbashSimDunkActive();
            uint64_t now = NbashMs();
            { extern void NbashDumpSignals(const char*);
              if (g_probeAt1 && now >= g_probeAt1) { g_probeAt1 = 0; NbashDumpSignals("+250ms"); }
              if (g_probeAt2 && now >= g_probeAt2) { g_probeAt2 = 0; NbashDumpSignals("+700ms"); g_probeAt3 = now + 800; }
              if (g_probeAt3 && now >= g_probeAt3) { g_probeAt3 = 0; NbashDumpSignals("+1500ms"); } }
            if (!cutChecked && now - cutT >= 250)
            {
                cutChecked = true;
                if (!NbashSimDunkActive()) { natural = true; naturalSince = now; Log("[turbodunk] t=%llu the layup did not start: next shot is left to the game\n", (unsigned long long)now); }
            }
            if (natural && !pressT && now - naturalSince > 3000) natural = false;   // back to layups after 3 s
            if (p.buttons & 0x2000)
            {
                if (!pressT)
                {
                    pressT = now; noTurbo = p.rt <= 100; cut = false; holdUntil = 0;
                    { static const bool dp = getenv("NBASH_DUNKPROBE") != nullptr; extern void NbashDumpSignals(const char*); if (dp) { extern void NbashResolveSignalsByScan(); NbashResolveSignalsByScan(); NbashDumpSignals("press"); g_probeAt1 = now + 250; g_probeAt2 = now + 700; } }
                    // NBASH_STEALCAPTURE also saves frames on the first 4 shoot presses (to catch the
                    // dunk prompt icons, e.g. the "off someone's back" dunk)
                    {
                        static const bool capShoot = getenv("NBASH_STEALCAPTURE") != nullptr;
                        static int shootCaps = 0;
                        if (capShoot && shootCaps < 4)
                        {
                            shootCaps++;
                            std::thread([] {
                                extern std::atomic<bool> g_captureRequest;
                                g_captureRequest = true;
                                std::this_thread::sleep_for(std::chrono::milliseconds(250)); g_captureRequest = true;
                            }).detach();
                            Log("[capture] shoot press %d: saving frames\n", shootCaps);
                        }
                    }
                    // random layup style: the game picks it from the bumpers held (none/LB/RB/both)
                    static uint16_t lastStyle = 0xFFFF;
                    static uint32_t seed = uint32_t(now * 2654435761u) | 1;
                    const uint16_t styles[4] = { 0x0000, 0x0100, 0x0200, 0x0300 };
                    // rating-based moves (NBASH_RATINGMOVES=0 turns off): the game's own "master dunker" flag decides
                    // how many dunk/layup styles a player can use and how big his dunks are
                    static const bool ratingMoves = !(getenv("NBASH_RATINGMOVES") && getenv("NBASH_RATINGMOVES")[0] == '0');
                    extern float NbashPlayerSignal(const char*);
                    master = !ratingMoves || NbashPlayerSignal("SigSIM_MasterSkill_Dunk") > 0.5f;
                    const int nStyles = master ? 4 : 2;            // role players: X and LB+X only
                    uint16_t pick;
                    do { seed = seed * 1664525u + 1013904223u; pick = styles[(seed >> 16) % nStyles]; } while (pick == lastStyle && nStyles > 1);
                    lastStyle = pick; layupStyle = pick;
                    // any-dunk mode for master dunkers (NBASH_ANYDUNK=0 turns off): each dunk uses a random dunk class
                    // (small man / high-flyer / big man) and dunk type; Gamebreaker dunks are left to the game
                    {
                        static const bool anyDunk = !(getenv("NBASH_ANYDUNK") && getenv("NBASH_ANYDUNK")[0] == '0');
                        bool gbNow = NbashPlayerSignal("SigSIM_Gamebreaker_Active") > 0.5f;
                        anyClass = -1;
                        if (anyDunk && master && !noTurbo && !gbNow)
                        {
                            static int lastClass = -1;
                            do { seed = seed * 1664525u + 1013904223u; anyClass = int((seed >> 16) % 3); } while (anyClass == lastClass);
                            lastClass = anyClass; anyUntil = now + 1200;
                            seed = seed * 1664525u + 1013904223u; anyType = int((seed >> 16) % 4);   // 0 none, 1 rotating, 2 signature, 3 windmill
                            for (int c = 0; c < 3; c++) origClass[c] = NbashPlayerSignal(kClass[c]);
                            for (int t = 0; t < 3; t++) origType[t] = NbashPlayerSignal(kType[t]);
                            Log("[anydunk] t=%llu class %s, type %s (player was %s)\n", (unsigned long long)now, kClass[anyClass] + 13,
                                anyType ? kType[anyType - 1] + 16 : "plain", origClass[0] > 0.5f ? "small man" : origClass[1] > 0.5f ? "high-flyer" : origClass[2] > 0.5f ? "big man" : "?");
                        }
                    }
                    Log("[ratingmoves] t=%llu %s: style %s\n", (unsigned long long)now, master ? "master dunker" : "regular player",
                        pick == 0 ? "X" : pick == 0x100 ? "LB+X" : pick == 0x200 ? "RB+X" : "LB+RB+X");
                }
                // holding the ball (dribble used): a 70 ms tap is a pump fake there, so the press is cut later
                static const int layupStandMs = getenv("NBASH_LAYUP_STAND_MS") ? atoi(getenv("NBASH_LAYUP_STAND_MS")) : 200;
                extern float NbashPlayerSignal(const char*);
                if (pressT == now)
                {
                    carrying = NbashPlayerSignal("SigSIM_CarryBall") > 0.5f;
                    bool gb = NbashPlayerSignal("SigSIM_Gamebreaker_Ready") > 0.5f || NbashPlayerSignal("SigSIM_Gamebreaker_Active") > 0.5f;
                    bool again = cutT && now - cutT < 1500;
                    if (carrying || gb || again)
                    {
                        natural = true; naturalSince = now;
                        Log("[turbodunk] t=%llu shot left to the game (%s)\n", (unsigned long long)now, gb ? "gamebreaker" : carrying ? "holding the ball" : "pressed again right after a layup try");
                    }
                }
                uint64_t cutAt = uint64_t(carrying ? layupStandMs : layupMs);
                if (!cut && noTurbo && !natural && now - pressT >= cutAt && now - pressT < 600 && NbashSimDunkActive())
                { cut = true; cutT = now; cutChecked = false;
                  Log("[turbodunk] t=%llu no turbo: dunk -> layup (shoot let go after %llu ms%s), style %s, dunk gates %.0f/%.0f\n", (unsigned long long)now, (unsigned long long)(now - pressT),
                      carrying ? ", holding the ball" : "", layupStyle == 0 ? "none" : layupStyle == 0x100 ? "LB" : layupStyle == 0x200 ? "RB" : "LB+RB",
                      NbashPlayerSignal("SigSIM_DunkGate_1"), NbashPlayerSignal("SigSIM_DunkGate_2")); }
                if (cut) p.buttons &= ~0x2000;
                if (noTurbo && randomLayups && !natural) p.buttons = uint16_t((p.buttons & ~0x0300) | layupStyle);   // random layup style
                // random dunk style too: with turbo the same pick (X / LB+X / RB+X / LB+RB+X) picks the dunk
                static const bool randomDunks = !getenv("NBASH_NORANDOMDUNK");
                if (!noTurbo && randomDunks && !natural)
                {
                    if (pressT == now) g_dunkStyleUntil = now + 900;
                    if (pressT == now) Log("[turbodunk] t=%llu dunk style %s\n", (unsigned long long)now, layupStyle == 0 ? "X" : layupStyle == 0x100 ? "LB+X" : layupStyle == 0x200 ? "RB+X" : "LB+RB+X");
                    p.buttons = uint16_t((p.buttons & ~0x0300) | layupStyle);
                }
            }
            else if (pressT)
            {
                if (natural && !holdUntil) { natural = false; }   // one untouched press, then back to layups
                // released: with turbo during a dunk, keep it held until the minimum
                if (!noTurbo && !holdUntil && now - pressT < uint64_t(dunkMs) && NbashSimDunkActive())
                {
                    holdUntil = pressT + (master ? dunkMs + 200 : dunkMs);   // master dunkers: longer press = bigger dunk
                    Log("[turbodunk] t=%llu turbo: quick tap held to %d ms so it stays a dunk\n", (unsigned long long)now, dunkMs);
                }
                if (holdUntil && now < holdUntil) p.buttons |= 0x2000;
                else { pressT = 0; holdUntil = 0; }
                // keep the random dunk style held through the whole dunk (the game reads the bumpers
                // while the dunk starts, not only on the shoot press)
            }
            // the random dunk style stays held for the whole dunk (0.9 s from the turbo shoot press),
            // even after the shoot button is let go
            if (now < g_dunkStyleUntil) p.buttons = uint16_t((p.buttons & ~0x0300) | layupStyle);
            // hold the chosen dunk class/type on the player while the dunk is picked and played; then restore his own
            {
                extern float NbashSetPlayerSignal(const char*, float);
                static bool applied = false;
                if (anyClass >= 0 && NbashMs() < anyUntil)
                {
                    for (int c = 0; c < 3; c++) NbashSetPlayerSignal(kClass[c], c == anyClass ? 1.0f : 0.0f);
                    for (int t = 0; t < 3; t++) NbashSetPlayerSignal(kType[t], anyType == t + 1 ? 1.0f : 0.0f);
                    applied = true;
                }
                else if (applied)
                {
                    for (int c = 0; c < 3; c++) if (origClass[c] >= 0) NbashSetPlayerSignal(kClass[c], origClass[c]);
                    for (int t = 0; t < 3; t++) if (origType[t] >= 0) NbashSetPlayerSignal(kType[t], origType[t]);
                    applied = false; anyClass = -1;
                }
            }
        }
    }
    if (memcmp(&p, &g_last[user], sizeof(Pad))) { g_last[user] = p; g_packet[user]++; }
    // every 10 s: what the game asked for and what the players pressed (to diagnose "can't move")
    {
        static std::mutex m; std::lock_guard lk(m);
        static uint32_t calls[4], pressed[4], moved[4]; static uint16_t ever[4];
        static auto last = std::chrono::steady_clock::now();
        calls[user]++;
        if (p.buttons) pressed[user]++;
        if (std::abs(int(p.lx)) > 8000 || std::abs(int(p.ly)) > 8000) moved[user]++;
        ever[user] |= p.buttons;
        if (std::chrono::steady_clock::now() - last > std::chrono::seconds(10))
        {
            last = std::chrono::steady_clock::now();
            Log("[input] last 10 s: window %s;", GetForegroundWindow() == NbashWindow() ? "in front" : "NOT in front");
            for (int u = 0; u < 4; u++)
                if (calls[u]) Log(" pad%d: %u reads, buttons held in %u, stick pushed in %u, buttons seen %04X;", u + 1, calls[u], pressed[u], moved[u], ever[u]);
            Log("\n");
            for (int u = 0; u < 4; u++) calls[u] = pressed[u] = moved[u] = ever[u] = 0;
        }
    }
    if (out)
    {
        W32(out + 0, g_packet[user]);
        W16(out + 4, p.buttons);
        W8(out + 6, p.lt); W8(out + 7, p.rt);
        W16(out + 8, uint16_t(p.lx)); W16(out + 10, uint16_t(p.ly));
        W16(out + 12, uint16_t(p.rx)); W16(out + 14, uint16_t(p.ry));
    }
    RETURN(0);
}

// XamInputSetState(user, flags, XINPUT_VIBRATION*)
IMPORT(XamInputSetState)
{
    TRACE_IMPORT();
    uint32_t user = ARG(3).u32 & 0xFF, v = ARG(5).u32;
    if (user >= 4) RETURN(0x48F);
    if (v) SetVibration(user, R16(v), R16(v + 2));
    RETURN(0);
}

// Button presses as keystrokes (menus use these for "press A/START"). Changes in button
// state between calls become KEYDOWN/KEYUP events with the 360's VK_PAD_* codes.
namespace
{
    struct KeyEvent { uint16_t vk, flags; uint8_t user; };
    std::vector<KeyEvent> g_keyQueue;
    uint16_t g_keyButtons[4] = {};
    std::mutex g_keyLock;

    uint16_t PadVk(uint16_t bit)
    {
        switch (bit)
        {
        case 0x1000: return 0x5800;   // A
        case 0x2000: return 0x5801;   // B
        case 0x4000: return 0x5802;   // X
        case 0x8000: return 0x5803;   // Y
        case 0x0200: return 0x5804;   // right shoulder
        case 0x0100: return 0x5805;   // left shoulder
        case 0x0001: return 0x5810;   // dpad up
        case 0x0002: return 0x5811;   // dpad down
        case 0x0004: return 0x5812;   // dpad left
        case 0x0008: return 0x5813;   // dpad right
        case 0x0010: return 0x5814;   // start
        case 0x0020: return 0x5815;   // back
        case 0x0040: return 0x5816;   // left thumb press
        case 0x0080: return 0x5817;   // right thumb press
        default: return 0;
        }
    }

    void PollKeys(uint32_t user)
    {
        Pad p;
        if (!ReadPad(user, p)) return;
        uint16_t b = p.buttons;
        // stick directions as dpad-style keys for menus
        if (p.ly > 16000) b |= 0x0001;
        if (p.ly < -16000) b |= 0x0002;
        if (p.lx < -16000) b |= 0x0004;
        if (p.lx > 16000) b |= 0x0008;
        // player ratings screen: LB opens it from the menus; while open the menus get no keys
        if (user == 0)
        {
            uint16_t pressed = uint16_t(b & ~g_keyButtons[user]);
            bool wasOpen = NbashStatsOpen();
            if ((pressed && NbashStatsInput(pressed, NbashInMatch())) || wasOpen || NbashStatsOpen()) { g_keyButtons[user] = b; return; }
        }
        if (user == 0 && (b & ~g_keyButtons[user] & 0x000F)) NbashMenuMove();
        uint16_t changed = b ^ g_keyButtons[user];
        for (uint16_t bit = 1; bit; bit <<= 1)
            if ((changed & bit) && PadVk(bit))
                g_keyQueue.push_back({ PadVk(bit), uint16_t((b & bit) ? 1 : 2), uint8_t(user) });   // 1 = down, 2 = up
        g_keyButtons[user] = b;
    }
}

// XamInputGetKeystrokeEx(user index*, flags, XINPUT_KEYSTROKE*)
IMPORT(XamInputGetKeystrokeEx)
{
    TRACE_IMPORT();
    uint32_t userPtr = ARG(3).u32, out = ARG(5).u32;
    uint32_t want = userPtr ? R32(userPtr) : 0xFF;
    uint32_t flags = ARG(4).u32;
    if (flags & 0x40000000) want = 0xFF;                 // XUSER_INDEX_ANY
    // flags bit 0 = gamepad keystrokes, bit 1 = keyboard/chatpad text. The game only asks for
    // keyboard text (flags 2); handing it gamepad buttons there made it treat them as typing.
    if (!(flags & 1)) RETURN(0x10D2);                    // ERROR_EMPTY
    std::lock_guard lock(g_keyLock);
    for (uint32_t u = 0; u < 4; u++)
        if (want == 0xFF || want == u) PollKeys(u);
    for (size_t i = 0; i < g_keyQueue.size(); i++)
    {
        KeyEvent k = g_keyQueue[i];
        if (want != 0xFF && k.user != want) continue;
        g_keyQueue.erase(g_keyQueue.begin() + i);
        if (out)
        {
            W16(out + 0, k.vk);       // VirtualKey
            W16(out + 2, 0);          // Unicode
            W16(out + 4, k.flags);    // Flags
            W8(out + 6, k.user);      // UserIndex
            W8(out + 7, 0);           // HidCode
        }
        if (userPtr) W32(userPtr, k.user);
        Log("[input] keystroke %04X %s (player %u)\n", k.vk, k.flags == 1 ? "down" : "up", k.user + 1);
        RETURN(0);
    }
    if (want != 0xFF && want != 0) RETURN(0x48F);   // only player 1 always present
    RETURN(0x10D2);                                  // ERROR_EMPTY: no keystroke waiting
}

// Debug (NBASH_KEYTRACE): watch the game's "is input action N active?" query.
extern "C" PPC_FUNC(__imp__sub_821C7448);
PPC_FUNC(sub_821C7448)
{
    static const bool trace = getenv("NBASH_KEYTRACE") != nullptr;
    uint32_t obj = ctx.r3.u32, key = ctx.r4.u32, lr = uint32_t(ctx.lr);
    __imp__sub_821C7448(ctx, base);
    if (!trace) return;
    static std::mutex m; static std::map<uint64_t, std::pair<uint64_t, uint64_t>> counts;
    static auto last = std::chrono::steady_clock::now();
    std::lock_guard lk(m);
    auto& c = counts[(uint64_t(lr) << 32) | (uint64_t(key) << 16) | (obj & 0xFFFF)];
    c.first++;
    if (ctx.r3.u32 & 0xFF) { c.second++; Log("[keytrace] TRUE key %u obj %08X lr %08X\n", key, obj, lr); }
    if (std::chrono::steady_clock::now() - last > std::chrono::seconds(10))
    {
        last = std::chrono::steady_clock::now();
        for (auto& [k, v] : counts)
            Log("[keytrace] lr %08llX key %llu obj ..%04llX: %llu calls, %llu true\n", (unsigned long long)(k >> 32), (unsigned long long)((k >> 16) & 0xFFFF), (unsigned long long)(k & 0xFFFF), (unsigned long long)v.first, (unsigned long long)v.second);
        counts.clear();
    }
}
