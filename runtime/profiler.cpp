// Sampling profiler (Windows): every ~1 ms each registered game thread is briefly suspended and
// its host call stack recorded. Every 15 s the most common stacks per thread are written to
// nbash_profile.txt as addresses relative to the exe's image base; they are turned into
// function names offline with the build's symbol table (tools: llvm-nm / llvm-symbolizer).
// Off by default (it pauses every game thread 1000 times a second); NBASH_PROFILE=1 turns it on.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>

namespace
{
    struct ProfThread { HANDLE h; uint32_t id; uint64_t samples = 0; std::map<std::array<uint64_t, 12>, uint32_t> stacks; };
    std::mutex g_lock;
    std::vector<ProfThread*> g_threads;
    uint64_t g_imageBase = 0, g_imageEnd = 0;

    void Sample(ProfThread& t)
    {
        if (SuspendThread(t.h) == DWORD(-1)) return;
        CONTEXT c{};
        c.ContextFlags = CONTEXT_FULL;
        std::array<uint64_t, 12> st{};
        if (GetThreadContext(t.h, &c))
        {
            for (int i = 0; i < 12 && c.Rip; i++)
            {
                st[i] = (c.Rip >= g_imageBase && c.Rip < g_imageEnd) ? c.Rip - g_imageBase : (c.Rip | 0x8000000000000000ull);
                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
                if (!fe)
                {
                    // leaf function: return address is on top of the stack
                    if (!c.Rsp) break;
                    c.Rip = *(DWORD64*)c.Rsp;
                    c.Rsp += 8;
                }
                else
                {
                    PVOID handlerData = nullptr; DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fe, &c, &handlerData, &establisher, nullptr);
                }
            }
        }
        ResumeThread(t.h);
        t.samples++;
        t.stacks[st]++;
    }

    void Write()
    {
        FILE* f = fopen("nbash_profile.txt", "w");
        if (!f) return;
        fprintf(f, "# image base %llx; frames relative to it; 8000... = outside the exe (system DLL)\n", (unsigned long long)g_imageBase);
        std::lock_guard<std::mutex> lk(g_lock);
        for (auto* t : g_threads)
        {
            fprintf(f, "thread %u samples %llu\n", t->id, (unsigned long long)t->samples);
            std::vector<std::pair<uint32_t, const std::array<uint64_t, 12>*>> v;
            for (auto& [k, n] : t->stacks) v.push_back({ n, &k });
            std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
            for (size_t i = 0; i < v.size() && i < 300; i++)
            {
                fprintf(f, "%u", v[i].first);
                for (uint64_t a : *v[i].second) if (a) fprintf(f, " %llx", (unsigned long long)a);
                fprintf(f, "\n");
            }
            t->stacks.clear();
            t->samples = 0;
        }
        fclose(f);
    }

    void ProfilerThread()
    {
        auto last = std::chrono::steady_clock::now();
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            {
                std::lock_guard<std::mutex> lk(g_lock);
                for (auto* t : g_threads) Sample(*t);
            }
            if (std::chrono::steady_clock::now() - last > std::chrono::seconds(15))
            {
                Write();
                last = std::chrono::steady_clock::now();
            }
        }
    }
}

void ProfRegisterThread(uint32_t guestId)
{
    if (!getenv("NBASH_PROFILE")) return;
    static std::atomic<bool> started{false};
    if (!started.exchange(true))
    {
        HMODULE m = GetModuleHandleA(nullptr);
        g_imageBase = uint64_t(m);
        auto* dos = (IMAGE_DOS_HEADER*)m;
        auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)m + dos->e_lfanew);
        g_imageEnd = g_imageBase + nt->OptionalHeader.SizeOfImage;
        std::thread(ProfilerThread).detach();
    }
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    if (!h) return;
    auto* t = new ProfThread{ h, guestId };
    std::lock_guard<std::mutex> lk(g_lock);
    g_threads.push_back(t);
}
#else
void ProfRegisterThread(uint32_t) {}
#endif
