// NBA Street Homecourt PC port - entry point
// Usage: nbash [path\to\default.xex]   (game files: folder in NBASH_GAME_DIR, default ".\game")
#include "guest.h"
#include <csignal>
#include <exception>
#ifdef _WIN32
#include <windows.h>
#endif
void WindowStart();
static void OnCrashTail();

static void OnCrash(int sig)
{
    Log("\n[CRASH] signal %d", sig);
    OnCrashTail();
}
#ifndef _WIN32
static void OnCrashInfo(int sig, siginfo_t* info, void*)
{
    uintptr_t a = (uintptr_t)info->si_addr, b = (uintptr_t)g_base;
    Log("\n[CRASH] signal %d at host address %p", sig, info->si_addr);
    if (a >= b && a < b + 0x100000000ull) Log(" = guest address %08llX", (unsigned long long)(a - b));
    else if (a >= b) Log(" = guest base + %llX (past the 4 GB guest space)", (unsigned long long)(a - b));
    OnCrashTail();
}
#else
static LONG WINAPI OnCrashWin(EXCEPTION_POINTERS* e)
{
    uintptr_t a = e->ExceptionRecord->NumberParameters > 1 ? e->ExceptionRecord->ExceptionInformation[1] : 0;
    uintptr_t b = (uintptr_t)g_base;
    Log("\n[CRASH] exception %08lX at %p, data address %p", e->ExceptionRecord->ExceptionCode,
        e->ExceptionRecord->ExceptionAddress, (void*)a);
    if (a >= b && a < b + 0x100000000ull) Log(" = guest address %08llX", (unsigned long long)(a - b));
    MessageBoxA(nullptr, "The game stopped with an error.\nPlease send nbash_log.txt (next to the program) to Claude.",
        "NBA Street Homecourt", MB_ICONERROR);
    OnCrashTail();
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif
static void OnCrashTail()
{
    if (t_thread && t_thread->ctx)
        Log(" in guest thread %u (lr=%08llX, r1=%08X, r3=%08X, last indirect call %08X -> host %p)", t_thread->id,
            (unsigned long long)t_thread->ctx->lr, t_thread->ctx->r1.u32, t_thread->ctx->r3.u32, t_thread->lastIndirect,
            (void*)LookupFunction(t_thread->lastIndirect));
    Log("\n");
    PrintImportSummary();
    _Exit(1);
}

void ProfRegisterThread(uint32_t guestId);
#ifdef _WIN32
// Laptops with two graphics cards: ask the NVIDIA and AMD drivers to run us on the separate, faster one.
extern "C" __declspec(dllexport) unsigned long NvOptimusEnablement = 1;
extern "C" __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
#endif
int main(int argc, char** argv)
{
#ifdef _WIN32
    {
        // run from the program's folder and write the log there
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (wchar_t* slash = wcsrchr(exe, L'\\')) { *slash = 0; SetCurrentDirectoryW(exe); }
        // keep the previous two runs' logs too (nbash_log_prev.txt, nbash_log_prev2.txt)
        MoveFileExA("nbash_log_prev.txt", "nbash_log_prev2.txt", MOVEFILE_REPLACE_EXISTING);
        MoveFileExA("nbash_log.txt", "nbash_log_prev.txt", MOVEFILE_REPLACE_EXISTING);
        freopen("nbash_log.txt", "w", stdout);
        {
            const char* vars[] = { "NBASH_RES_SCALE", "NBASH_MSAA", "NBASH_ANISO", "NBASH_NOMIPS", "NBASH_RSTICK", "NBASH_RS_HOLDMS", "NBASH_RS_BUMPERMS", "NBASH_GPU" };
            for (const char* v : vars) if (getenv(v)) printf("[setting] %s=%s\n", v, getenv(v));
        }
        timeBeginPeriod(1);   // 1 ms sleep accuracy for 60 Hz timing
    }
#endif
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const char* xex = argc > 1 ? argv[1] : "default.xex";
    if (argc <= 1)
        if (FILE* f = fopen(xex, "rb")) fclose(f); else xex = "game/default.xex";

    MemoryInit();
    KernelInit();
    Log("NBA Street Homecourt PC runtime - loading %s\n", xex);
    if (!LoadXex(xex))
        return 1;

#ifndef _WIN32
    struct sigaction sa{}; sa.sa_sigaction = OnCrashInfo; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    signal(SIGTERM, [](int) { PrintImportSummary(); _Exit(0); });   // timeout/kill: show what was called
#else
    SetUnhandledExceptionFilter(OnCrashWin);
#endif
    signal(SIGILL, OnCrash);
    signal(SIGFPE, OnCrash);
#ifdef SIGTRAP
    signal(SIGTRAP, OnCrash);
#endif
#ifdef SIGBUS
    signal(SIGBUS, OnCrash);
#endif

    WindowStart();

    // main guest thread
    GuestThread* main = CreateGuestThreadState(g_module.defaultStackSize);
    t_thread = main;
    ProfRegisterThread(main->id);
    uint32_t stackSize = g_module.defaultStackSize < 0x100000 ? 0x100000 : g_module.defaultStackSize;
    uint32_t stack = AllocVirtual(stackSize, 0x1000);
    auto* ctx = new PPCContext();
    InitContext(*ctx, main, stack + stackSize);

    Log("[boot] jumping to entry point %08X\n", g_module.entryPoint);
    try
    {
        GuestCall(*ctx, g_module.entryPoint);
        Log("[boot] entry point returned (r3=%08X)\n", ctx->r3.u32);
    }
    catch (const std::exception& e)
    {
        Log("[boot] main thread stopped: %s\n", e.what());
    }
    PrintImportSummary();
    return 0;
}
