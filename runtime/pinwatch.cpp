// 14 Nov: WHO WRITES THE BALL-PIN SIGNALS?  (diagnostic)
// The game pins the ball to a hand during a trick (SigSIM_BallPinLeft / Right go 1 at ~0.8 s) and clearing the signal afterwards changes nothing, so the
// code that writes it is what has to be found.  Hardware write breakpoints (debug registers) on the two signal values in every thread; the exception
// handler logs the address of the writing instruction and the return addresses on the host stack, as offsets in this exe -> looked up in the cloud
// build (llvm-nm nbash.exe: each recompiled game function is a symbol sub_8xxxxxxx).  Armed once, at the first signature dribble.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include "guest.h"
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif
void Log(const char* fmt, ...);
uint64_t NbashMs();
#ifdef _WIN32
namespace
{
    uint8_t* g_w[2] = {}; std::atomic<int> g_hits{0}; uint64_t g_modLo = 0, g_modHi = 0; std::atomic<uint64_t> g_mark{0};
    LONG CALLBACK Veh(EXCEPTION_POINTERS* e)
    {
        if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
        CONTEXT* c = e->ContextRecord; DWORD64 d6 = c->Dr6 & 3; if (!d6) return EXCEPTION_CONTINUE_SEARCH;
        c->Dr6 = 0;
        int n = g_hits.fetch_add(1);
        if (n < 60)
        {
            int which = (d6 & 1) ? 0 : 1; uint32_t v = __builtin_bswap32(*(uint32_t*)g_w[which]); float f; memcpy(&f, &v, 4);
            char buf[400]; int len = 0; const uint64_t* sp = (const uint64_t*)c->Rsp; int found = 0;
            for (int i = 0; i < 400 && found < 14; i++) { uint64_t x = sp[i]; if (x >= g_modLo && x < g_modHi) { len += snprintf(buf + len, sizeof buf - size_t(len), " %llX", (unsigned long long)(x - g_modLo + 0x140000000ull)); found++; } }
            uint64_t m = g_mark.load();
            Log("[pinwatch] BallPin%s = %.0f written at %llX (%lld ms after the flick); stack:%s\n", which ? "Right" : "Left", f, (unsigned long long)(c->Rip - g_modLo + 0x140000000ull), m ? (long long)(NbashMs() - m) : -1ll, buf);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    void Arm()
    {
        HMODULE h = GetModuleHandleA(nullptr); MEMORY_BASIC_INFORMATION mi; g_modLo = uint64_t(h); g_modHi = g_modLo + 0x8000000ull;
        auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)h + ((IMAGE_DOS_HEADER*)h)->e_lfanew); g_modHi = g_modLo + nt->OptionalHeader.SizeOfImage; (void)mi;
        AddVectoredExceptionHandler(1, Veh);
        DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId(); HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); THREADENTRY32 te; te.dwSize = sizeof te; int n = 0;
        if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te))
            do
            {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
                HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID); if (!t) continue;
                if (SuspendThread(t) != DWORD(-1))
                {
                    CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (GetThreadContext(t, &c))
                    {
                        c.Dr0 = DWORD64(g_w[0]); c.Dr1 = DWORD64(g_w[1]); c.Dr6 = 0;
                        c.Dr7 = (c.Dr7 & ~0x00FF000Full) | 0x1 | 0x4 | (0x1ull << 16) | (0x3ull << 18) | (0x1ull << 20) | (0x3ull << 22);      // bp0 + bp1: write, 4 bytes
                        if (SetThreadContext(t, &c)) n++;
                    }
                    ResumeThread(t);
                }
                CloseHandle(t);
            } while (Thread32Next(snap, &te));
        if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
        Log("[pinwatch] armed in %d threads: BallPinLeft value at guest %08X, BallPinRight at %08X; exe base %llX\n", n, uint32_t(g_w[0] - g_base), uint32_t(g_w[1] - g_base), (unsigned long long)g_modLo);
    }
}
#endif
// gameplay.cpp calls this at every signature dribble with the two signal instances (guest addresses); the values are the floats at +4
void NbashPinWatch(uint32_t instL, uint32_t instR)
{
#ifdef _WIN32
    g_mark = NbashMs();
    static std::atomic<bool> armed{false};
    if (armed.exchange(true)) return;
    g_w[0] = g_base + instL + 4; g_w[1] = g_base + instR + 4;
    std::thread(Arm).detach();
#endif
}
