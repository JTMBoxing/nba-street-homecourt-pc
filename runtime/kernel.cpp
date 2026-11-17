// Xbox 360 kernel / XAM functions that NBA Street Homecourt needs, implemented on the host.
// Anything not implemented here falls back to the logging stubs in stubs.cpp.
#include "guest.h"
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <filesystem>
#include <thread>
#include <unordered_map>
#include <vector>
#include <algorithm>
#ifdef _WIN32
#define strcasecmp _stricmp
#define gmtime_r(t, r) gmtime_s(r, t)
#else
#include <strings.h>
#endif

namespace fs = std::filesystem;

int g_verbose = 0;
uint32_t g_timeStampBundle = 0;
thread_local GuestThread* t_thread = nullptr;

// ============================================================ logging
std::mutex g_allThreadsLock;
std::vector<GuestThread*> g_allThreads;

static std::mutex g_logLock;
void Log(const char* fmt, ...)
{
    std::lock_guard lock(g_logLock);
    va_list ap; va_start(ap, fmt); vfprintf(stdout, fmt, ap); va_end(ap);
    fflush(stdout);
}

struct ImportStats { std::atomic<uint64_t> calls{0}; bool unimplemented = false; };
static std::mutex g_statsLock;
static std::unordered_map<std::string, ImportStats*> g_stats;

static ImportStats& Stats(const char* name)
{
    std::lock_guard lock(g_statsLock);
    auto& s = g_stats[name];
    if (!s) s = new ImportStats();
    return *s;
}

extern thread_local const char* t_lastImport;
void TraceImport(const char* name, PPCContext& ctx)
{
    t_lastImport = name;
    if (t_thread) t_thread->lastImport = name;
    auto& s = Stats(name);
    uint64_t n = ++s.calls;
    if (n == 1)
        Log("[first] %s(%08X, %08X, %08X, %08X) thread %u lr %08llX\n", name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, t_thread ? t_thread->id : 0, (unsigned long long)ctx.lr);
    if (g_verbose && n <= 3)
        Log("[call] %s(%08X, %08X, %08X, %08X) thread %u\n", name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, t_thread ? t_thread->id : 0);
}

void Unimplemented(const char* name, PPCContext& ctx)
{
    if (t_thread) t_thread->lastImport = name;
    auto& s = Stats(name);
    s.unimplemented = true;
    if (++s.calls <= 2)
        Log("[stub] %s(%08X, %08X, %08X, %08X) - not implemented, returning 0\n", name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}

void PrintThreadStatus()
{
    std::lock_guard lock(g_allThreadsLock);
    for (auto* t : g_allThreads)
        if (t->ctx)
        {
            Log("    thread %2u: last system call %-28s lr=%08llX r1=%08X r3=%08X r29=%08X r30=%08X r31=%08X\n", t->id, t->lastImport ? t->lastImport : "-", (unsigned long long)t->ctx->lr, t->ctx->r1.u32, t->ctx->r3.u32, t->ctx->r29.u32, t->ctx->r30.u32, t->ctx->r31.u32);
            // call chain from the guest stack: each frame stores the caller's r1 at 0(r1),
            // and the function saved its return address at -8 from that
            char chain[512]; int n = 0;
            uint32_t frame = t->ctx->r1.u32;
            for (int depth = 0; depth < 20 && frame >= 0x40000000 && frame < 0x7F000000 && n < 480; depth++)
            {
                uint32_t back = R32(frame);
                if (back <= frame || back - frame > 0x100000) break;
                uint32_t ret = R32(back - 8);
                if (ret >= PPC_CODE_BASE && ret < PPC_CODE_BASE + PPC_CODE_SIZE) n += snprintf(chain + n, sizeof(chain) - n, " %08X", ret);
                frame = back;
            }
            chain[n] = 0;
            if (n) Log("               calls:%s\n", chain);
        }
}

#ifndef _WIN32
// Linux test builds: save game memory (heap, image data, physical) when the game stalls,
// same format as the Windows F11 dump: [u32 guest address][4096 bytes] per non-zero page.
void DumpGuestMemory()
{
    FILE* o = fopen("memdump.bin", "wb");
    if (!o) return;
    static const uint8_t zero[4096] = {};
    const struct { uint32_t lo, hi; } ranges[] = { { 0x40000000, 0x60000000 }, { 0x82000000, 0x90000000 }, { 0xA0000000, 0xC0000000 } };
    uint32_t pages = 0;
    for (auto r : ranges)
        for (uint64_t a = r.lo; a < r.hi; a += 4096)
        {
            const uint8_t* src = G2H(uint32_t(a));
            if (!memcmp(src, zero, 4096)) continue;
            uint32_t ga = uint32_t(a);
            fwrite(&ga, 4, 1, o); fwrite(src, 1, 4096, o); pages++;
        }
    fclose(o);
    Log("[debug] saved %u pages of game memory to memdump.bin\n", pages);
}
#endif

void PrintImportSummary()
{
    std::lock_guard lock(g_statsLock);
    std::vector<std::pair<std::string, ImportStats*>> v(g_stats.begin(), g_stats.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second->calls > b.second->calls; });
    Log("\n==== system functions called (%zu different) ====\n", v.size());
    for (auto& [n, s] : v)
        Log("  %-45s %10llu%s\n", n.c_str(), (unsigned long long)s->calls.load(), s->unimplemented ? "   (stub)" : "");
}

#define TRACE() TRACE_IMPORT()

// ============================================================ threads & guest calls
static std::atomic<uint32_t> g_nextThreadId{1};

thread_local const char* t_lastImport = "";

GuestThread* CreateGuestThreadState(uint32_t stackSize)
{
    auto* t = new GuestThread();
    { std::lock_guard lock(g_allThreadsLock); g_allThreads.push_back(t); }
    t->pcr = AllocSystem(0xB00, 0x1000);
    t->tls = AllocSystem(0x100 + 0x1000, 0x10);          // 64 TLS slots + XEX TLS data
    uint32_t teb = AllocSystem(0x400, 0x10);
    t->object = AllocSystem(0x40, 0x10);                  // dispatcher header (type 6 = thread)
    W8(t->object, 6);
    t->id = g_nextThreadId++;
    W32(t->pcr + 0x00, t->tls);                           // TLS pointer
    W32(t->pcr + 0x100, teb);                             // current thread
    W8(t->pcr + 0x10C, 0);                                // processor number
    W32(teb + 0x14C, t->id);                              // thread id
    (void)stackSize;
    return t;
}

void InitContext(PPCContext& ctx, GuestThread* t, uint32_t stackTop)
{
    memset(&ctx, 0, sizeof(ctx));
    ctx.msr = 0x200A000;
    ctx.r1.u64 = stackTop - 0x100;
    ctx.r13.u64 = t->pcr;
    ctx.fpscr.loadFromHost();
    t->ctx = &ctx;
}

void GuestCall(PPCContext& ctx, uint32_t address)
{
    PPCFunc* fn = LookupFunction(address);
    if (!fn)
    {
        Log("[error] call to unknown guest function %08X\n", address);
        return;
    }
    fn(ctx, g_base);
}

void PPCCallIndirect(PPCContext& ctx, uint8_t* base, uint32_t target)
{
    if (t_thread) t_thread->lastIndirect = target;
    PPCFunc* fn = LookupFunction(target);
    if (!fn)
    {
        static std::atomic<int> reported{0};
        if (reported.load(std::memory_order_relaxed) < 20 && reported++ < 20)
            Log("[error] indirect call to %08X (not a known function), lr=%08llX, thread %u - skipped\n",
                target, (unsigned long long)ctx.lr, t_thread ? t_thread->id : 0);
        ctx.r3.u64 = 0;
        return;
    }
    fn(ctx, base);
}

// ============================================================ dispatcher objects (events, semaphores, threads)
// Kernel objects live in guest memory (the game initializes many of them inline), so waits
// read the Xbox DISPATCHER_HEADER directly: byte 0 = type, +4 = signal state.
static std::mutex g_waitLock;
static std::condition_variable g_waitCv;

enum : uint8_t { kNotificationEvent = 0, kSynchronizationEvent = 1, kSemaphore = 5, kThread = 6, kNotificationTimer = 8, kSynchronizationTimer = 9 };

static bool TryAcquire(uint32_t obj)   // call with g_waitLock held
{
    uint8_t type = R8(obj);
    int32_t state = (int32_t)R32(obj + 4);
    if (state <= 0) return false;
    if (type == kSynchronizationEvent || type == kSynchronizationTimer) W32(obj + 4, 0);
    else if (type == kSemaphore) W32(obj + 4, state - 1);
    return true;
}

static void Signal(uint32_t obj, int32_t value)
{
    { std::lock_guard lock(g_waitLock); W32(obj + 4, value); }
    g_waitCv.notify_all();
}

// timeout: guest pointer to a 64-bit 100ns value (negative = relative), 0 = wait forever
static std::chrono::steady_clock::time_point Deadline(uint32_t timeoutPtr, bool& infinite)
{
    infinite = timeoutPtr == 0;
    if (infinite) return {};
    int64_t t = (int64_t)R64(timeoutPtr);
    auto now = std::chrono::steady_clock::now();
    if (t < 0) return now + std::chrono::microseconds(-t / 10);
    return now;   // absolute times: treat as "now" (rare)
}

static uint32_t WaitMany(const std::vector<uint32_t>& objs, bool waitAll, uint32_t timeoutPtr)
{
    bool infinite; auto deadline = Deadline(timeoutPtr, infinite);
    std::unique_lock lock(g_waitLock);
    for (;;)
    {
        if (waitAll)
        {
            bool all = true;
            for (auto o : objs) if ((int32_t)R32(o + 4) <= 0) { all = false; break; }
            if (all) { for (auto o : objs) TryAcquire(o); return STATUS_SUCCESS; }
        }
        else
        {
            for (size_t i = 0; i < objs.size(); i++)
                if (TryAcquire(objs[i])) return STATUS_SUCCESS + uint32_t(i);
        }
        if (infinite) g_waitCv.wait_for(lock, std::chrono::milliseconds(50));
        else if (g_waitCv.wait_until(lock, deadline) == std::cv_status::timeout)
        {
            // one last check
            if (!waitAll) for (size_t i = 0; i < objs.size(); i++) if (TryAcquire(objs[i])) return uint32_t(i);
            return STATUS_TIMEOUT;
        }
    }
}

// ---- handles: Nt* objects get a guest dispatcher header too; the handle maps to it
static std::mutex g_handleLock;
static std::unordered_map<uint32_t, uint32_t> g_handles;    // handle -> guest object
static uint32_t g_nextHandle = 0xF8000004;

static uint32_t NewHandle(uint32_t object)
{
    std::lock_guard lock(g_handleLock);
    uint32_t h = g_nextHandle; g_nextHandle += 4;
    g_handles[h] = object;
    return h;
}

static uint32_t HandleObject(uint32_t handle)
{
    if (handle == 0xFFFFFFFE) return t_thread ? t_thread->object : 0;  // current thread pseudo-handle
    std::lock_guard lock(g_handleLock);
    auto it = g_handles.find(handle);
    if (it != g_handles.end()) return it->second;
    return handle;   // the game sometimes passes object pointers directly
}

// ============================================================ threads
struct ThreadStart { uint32_t xapiStartup, start, context, stackSize; GuestThread* t; std::atomic<bool> suspended; };

void ProfRegisterThread(uint32_t guestId);
static void ThreadMain(ThreadStart* s)
{
    while (s->suspended) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    t_thread = s->t;
    ProfRegisterThread(s->t->id);
    uint32_t stack = AllocVirtual(s->stackSize, 0x1000);
    auto* ctx = new PPCContext();
    InitContext(*ctx, s->t, stack + s->stackSize);
    Log("[thread] %u started at %08X (ctx %08X)\n", s->t->id, s->start, s->context);
    try
    {
        if (s->xapiStartup)
        {
            ctx->r3.u64 = s->start;
            ctx->r4.u64 = s->context;
            GuestCall(*ctx, s->xapiStartup);
        }
        else
        {
            ctx->r3.u64 = s->context;
            GuestCall(*ctx, s->start);
        }
    }
    catch (const std::exception&) {}
    Log("[thread] %u finished\n", s->t->id);
    Signal(s->t->object, 1);
}

static std::unordered_map<uint32_t, ThreadStart*> g_threads;   // thread object -> start info
static uint32_t FirstCpu(uint32_t mask);

IMPORT(ExCreateThread)
{
    TRACE();
    uint32_t handlePtr = ARG(3).u32, stackSize = ARG(4).u32, idPtr = ARG(5).u32;
    uint32_t xapi = ARG(6).u32, start = ARG(7).u32, context = ARG(8).u32, flags = ARG(9).u32;
    auto* t = CreateGuestThreadState(stackSize);
    auto* s = new ThreadStart{ xapi, start, context, stackSize ? stackSize : g_module.defaultStackSize, t, {} };
    s->suspended = (flags & 1) != 0;
    { std::lock_guard lock(g_handleLock); g_threads[t->object] = s; }
    t->handle = NewHandle(t->object);
    if (handlePtr) W32(handlePtr, t->handle);
    if (idPtr) W32(idPtr, t->id);
    if (uint32_t mask = flags >> 24) W8(t->pcr + 0x10C, uint8_t(FirstCpu(mask)));   // creation affinity
    std::thread(ThreadMain, s).detach();
    RETURN(STATUS_SUCCESS);
}

static void Resume(uint32_t obj)
{
    std::lock_guard lock(g_handleLock);
    auto it = g_threads.find(obj);
    if (it != g_threads.end()) it->second->suspended = false;
}

IMPORT(NtResumeThread)  { TRACE(); Resume(HandleObject(ARG(3).u32)); if (ARG(4).u32) W32(ARG(4).u32, 1); RETURN(0); }
IMPORT(KeResumeThread)  { TRACE(); Resume(ARG(3).u32); RETURN(1); }
IMPORT(NtSuspendThread) { TRACE(); RETURN(0); }
IMPORT(ExTerminateThread) { TRACE(); Log("[thread] ExTerminateThread(%d)\n", ARG(3).s32); if (t_thread) Signal(t_thread->object, 1); throw std::runtime_error("thread exit"); }
// Processor affinity: the game pins worker threads to hardware threads and identifies
// them by the PCR processor number (audio mixer workers meet at a per-CPU barrier).
static uint32_t FirstCpu(uint32_t mask) { for (uint32_t i = 0; i < 6; i++) if (mask & (1u << i)) return i; return 0; }
IMPORT(KeSetAffinityThread)
{
    TRACE();
    uint32_t obj = ARG(3).u32, mask = ARG(4).u32, prevPtr = ARG(5).u32;
    GuestThread* t = nullptr;
    {
        std::lock_guard lock(g_handleLock);
        auto it = g_threads.find(obj);
        if (it != g_threads.end()) t = it->second->t;
    }
    if (!t && t_thread && t_thread->object == obj) t = t_thread;
    if (prevPtr) W32(prevPtr, t ? (1u << R8(t->pcr + 0x10C)) : 1);
    if (t && mask) W8(t->pcr + 0x10C, uint8_t(FirstCpu(mask)));
    RETURN(0);
}
IMPORT(KeSetBasePriorityThread) { TRACE(); RETURN(0); }
IMPORT(KeGetCurrentProcessType) { TRACE(); RETURN(1); }    // title process
IMPORT(KeDelayExecutionThread)
{
    TRACE();
    int64_t t = ARG(5).u32 ? (int64_t)R64(ARG(5).u32) : 0;
    if (t < 0) std::this_thread::sleep_for(std::chrono::microseconds(-t / 10));
    else std::this_thread::yield();
    RETURN(STATUS_SUCCESS);
}
IMPORT(NtYieldExecution) { TRACE(); std::this_thread::yield(); RETURN(0); }

// ============================================================ TLS
static std::mutex g_tlsLock;
static uint64_t g_tlsUsed = 0;
IMPORT(KeTlsAlloc)
{
    TRACE();
    std::lock_guard lock(g_tlsLock);
    for (uint32_t i = 0; i < 64; i++)
        if (!(g_tlsUsed & (1ull << i))) { g_tlsUsed |= 1ull << i; RETURN(i); }
    RETURN(0xFFFFFFFF);
}
IMPORT(KeTlsFree)     { TRACE(); std::lock_guard lock(g_tlsLock); g_tlsUsed &= ~(1ull << (ARG(3).u32 & 63)); RETURN(1); }
IMPORT(KeTlsGetValue) { TRACE(); RETURN(R32(R32(ctx.r13.u32) + (ARG(3).u32 & 63) * 4)); }
IMPORT(KeTlsSetValue) { TRACE(); W32(R32(ctx.r13.u32) + (ARG(3).u32 & 63) * 4, ARG(4).u32); RETURN(1); }

// ============================================================ events / semaphores / waits
IMPORT(KeSetEvent)   { TRACE(); uint32_t o = ARG(3).u32; int32_t prev = (int32_t)R32(o + 4); Signal(o, 1); RETURN(prev); }
IMPORT(KeResetEvent) { TRACE(); uint32_t o = ARG(3).u32; int32_t prev = (int32_t)R32(o + 4); Signal(o, 0); RETURN(prev); }
IMPORT(KeInitializeSemaphore)
{
    TRACE();
    uint32_t o = ARG(3).u32;
    memset(G2H(o), 0, 0x14);
    W8(o, kSemaphore); W32(o + 4, ARG(4).u32); W32(o + 8, o + 8); W32(o + 12, o + 8); W32(o + 16, ARG(5).u32);
    RETURN(0);
}
IMPORT(KeReleaseSemaphore)
{
    TRACE();
    uint32_t o = ARG(3).u32; int32_t adj = ARG(5).s32;
    int32_t prev;
    { std::lock_guard lock(g_waitLock); prev = (int32_t)R32(o + 4); W32(o + 4, prev + adj); }
    g_waitCv.notify_all();
    RETURN(prev);
}
IMPORT(KeWaitForSingleObject) { TRACE(); RETURN(WaitMany({ ARG(3).u32 }, false, ARG(7).u32)); }
IMPORT(KeWaitForMultipleObjects)
{
    TRACE();
    std::vector<uint32_t> objs;
    for (uint32_t i = 0; i < ARG(3).u32; i++) objs.push_back(R32(ARG(4).u32 + i * 4));
    RETURN(WaitMany(objs, ARG(5).u32 == 0 /* WaitAll */, ARG(9).u32));
}

static uint32_t NewDispatcher(uint8_t type, int32_t state)
{
    uint32_t o = AllocSystem(0x20, 0x10);
    W8(o, type); W32(o + 4, state); W32(o + 8, o + 8); W32(o + 12, o + 8);
    return o;
}

IMPORT(NtCreateEvent)
{
    TRACE();
    // (handle*, objattr*, eventType: 0 notification / 1 synchronization, initialState)
    uint32_t o = NewDispatcher(ARG(5).u32 ? kSynchronizationEvent : kNotificationEvent, ARG(6).u32 ? 1 : 0);
    W32(ARG(3).u32, NewHandle(o));
    RETURN(STATUS_SUCCESS);
}
IMPORT(NtSetEvent)   { TRACE(); uint32_t o = HandleObject(ARG(3).u32); if (ARG(4).u32) W32(ARG(4).u32, R32(o + 4)); Signal(o, 1); RETURN(0); }
IMPORT(NtClearEvent) { TRACE(); Signal(HandleObject(ARG(3).u32), 0); RETURN(0); }
IMPORT(NtCreateSemaphore)
{
    TRACE();
    uint32_t o = NewDispatcher(kSemaphore, ARG(5).s32);
    W32(o + 16, ARG(6).u32);
    W32(ARG(3).u32, NewHandle(o));
    RETURN(STATUS_SUCCESS);
}
IMPORT(NtReleaseSemaphore)
{
    TRACE();
    uint32_t o = HandleObject(ARG(3).u32);
    int32_t prev;
    { std::lock_guard lock(g_waitLock); prev = (int32_t)R32(o + 4); W32(o + 4, prev + ARG(4).s32); }
    g_waitCv.notify_all();
    if (ARG(5).u32) W32(ARG(5).u32, prev);
    RETURN(0);
}
IMPORT(NtWaitForSingleObjectEx) { TRACE(); RETURN(WaitMany({ HandleObject(ARG(3).u32) }, false, ARG(6).u32)); }
IMPORT(NtCreateTimer)  { TRACE(); W32(ARG(3).u32, NewHandle(NewDispatcher(ARG(5).u32 ? kSynchronizationTimer : kNotificationTimer, 0))); RETURN(0); }
IMPORT(NtSetTimerEx)   { TRACE(); Signal(HandleObject(ARG(3).u32), 1); RETURN(0); }   // TODO: real timer
IMPORT(NtCancelTimer)  { TRACE(); RETURN(0); }
IMPORT(NtClose)        { TRACE(); RETURN(0); }
IMPORT(NtDuplicateObject) { TRACE(); W32(ARG(4).u32, ARG(3).u32); RETURN(0); }
IMPORT(ObReferenceObjectByHandle) { TRACE(); if (ARG(5).u32) W32(ARG(5).u32, HandleObject(ARG(3).u32)); RETURN(0); }
IMPORT(ObDereferenceObject) { TRACE(); RETURN(0); }

// ============================================================ critical sections & spin locks
// RTL_CRITICAL_SECTION (28 bytes in guest memory): header (16), LockCount (+16),
// RecursionCount (+20), OwningThread (+24). Each one is paired with a host lock that follows
// the Xbox kernel's rules rather than a host mutex's: RtlLeaveCriticalSection does not check
// the owner, so a game may leave a section on another thread than entered it. (With a host
// recursive mutex that unlock silently failed and the section stayed locked forever - one of
// the random hangs.)
namespace
{
    struct GuestCS { std::mutex m; std::condition_variable cv; uint32_t owner = 0; int32_t recursion = 0; int32_t waiters = 0; };
    std::mutex g_csLock;
    std::unordered_map<uint32_t, GuestCS*> g_cs;
    GuestCS* CS(uint32_t a)
    {
        std::lock_guard lock(g_csLock);
        auto& c = g_cs[a];
        if (!c) c = new GuestCS();
        return c;
    }
    uint32_t CurrentThreadObject() { return t_thread ? t_thread->object : 0xFFFFFFFF; }
    void MirrorCS(uint32_t a, const GuestCS* c)      // keep the guest-visible fields sensible
    {
        W32(a + 16, uint32_t(c->owner ? c->waiters + c->recursion - 1 : -1));
        W32(a + 20, uint32_t(c->recursion));
        W32(a + 24, c->owner);
    }
}
IMPORT(RtlInitializeCriticalSection)
{
    TRACE();
    uint32_t a = ARG(3).u32;
    memset(G2H(a), 0, 28); W32(a + 16, 0xFFFFFFFF);
    GuestCS* c = CS(a);
    std::lock_guard lock(c->m);
    c->owner = 0; c->recursion = 0;
    RETURN(0);
}
IMPORT(RtlEnterCriticalSection)
{
    TRACE();
    uint32_t a = ARG(3).u32, me = CurrentThreadObject();
    GuestCS* c = CS(a);
    std::unique_lock lock(c->m);
    if (c->owner && c->owner != me)
    {
        c->waiters++;
        c->cv.wait(lock, [&] { return c->owner == 0; });
        c->waiters--;
    }
    c->owner = me; c->recursion++;
    MirrorCS(a, c);
    RETURN(0);
}
IMPORT(RtlLeaveCriticalSection)
{
    TRACE();
    uint32_t a = ARG(3).u32;
    GuestCS* c = CS(a);
    std::lock_guard lock(c->m);
    if (c->recursion > 0 && --c->recursion == 0)
    {
        c->owner = 0;
        c->cv.notify_one();
    }
    MirrorCS(a, c);
    RETURN(0);
}
IMPORT(RtlTryEnterCriticalSection)
{
    TRACE();
    uint32_t a = ARG(3).u32, me = CurrentThreadObject();
    GuestCS* c = CS(a);
    std::lock_guard lock(c->m);
    if (c->owner && c->owner != me) RETURN(0);
    c->owner = me; c->recursion++;
    MirrorCS(a, c);
    RETURN(1);
}

// Spin locks are a 32-bit word in guest memory; each one is locked on its own (owner = thread id).
static void SpinAcquire(uint32_t lock)
{
    uint32_t me = t_thread ? t_thread->id : 0xFFFF;
    std::atomic_ref<uint32_t> word(*(uint32_t*)G2H(lock));
    uint32_t expected = 0;
    uint32_t tag = __builtin_bswap32(me);
    for (int spins = 0; !word.compare_exchange_weak(expected, tag); spins++)
    {
        if (expected == tag) return;                 // already ours (re-entry)
        expected = 0;
        if (spins > 64) std::this_thread::yield();
    }
}
static void SpinRelease(uint32_t lock) { std::atomic_ref<uint32_t>(*(uint32_t*)G2H(lock)).store(0); }
IMPORT(KfAcquireSpinLock)               { TRACE(); SpinAcquire(ARG(3).u32); RETURN(0); }
IMPORT(KfReleaseSpinLock)               { TRACE(); SpinRelease(ARG(3).u32); RETURN(0); }
IMPORT(KeAcquireSpinLockAtRaisedIrql)   { TRACE(); SpinAcquire(ARG(3).u32); RETURN(0); }
IMPORT(KeReleaseSpinLockFromRaisedIrql) { TRACE(); SpinRelease(ARG(3).u32); RETURN(0); }
IMPORT(KeRaiseIrqlToDpcLevel) { TRACE(); RETURN(0); }
IMPORT(KfLowerIrql)           { TRACE(); RETURN(0); }
IMPORT(KeEnterCriticalRegion) { TRACE(); RETURN(0); }
IMPORT(KeLeaveCriticalRegion) { TRACE(); RETURN(0); }
IMPORT(KeLockL2)   { TRACE(); RETURN(0); }
IMPORT(KeUnlockL2) { TRACE(); RETURN(0); }

// SLIST_HEADER (8 bytes, big-endian): next (+0), depth (+4), sequence (+6). The game pushes
// with its own lock-free code (ldarx/stdcx. on the whole header), so popping must be a
// 64-bit compare-and-swap too (layout and algorithm as in Xenia).
IMPORT(InterlockedPopEntrySList)
{
    TRACE();
    uint32_t head = ARG(3).u32;
    std::atomic_ref<uint64_t> hdr(*(uint64_t*)G2H(head));
    for (;;)
    {
        uint64_t oldRaw = hdr.load();
        uint64_t oldBe = __builtin_bswap64(oldRaw);          // next:32 | depth:16 | sequence:16
        uint32_t first = uint32_t(oldBe >> 32);
        if (!first) RETURN(0);
        uint16_t depth = uint16_t(oldBe >> 16), seq = uint16_t(oldBe);
        uint64_t newBe = (uint64_t(R32(first)) << 32) | (uint64_t(uint16_t(depth - 1)) << 16) | seq;
        if (hdr.compare_exchange_weak(oldRaw, __builtin_bswap64(newBe))) RETURN(first);
    }
}

// ============================================================ memory
IMPORT(NtAllocateVirtualMemory)
{
    TRACE();
    // (BaseAddress*, RegionSize*, AllocationType, Protect, Unknown)
    uint32_t basePtr = ARG(3).u32, sizePtr = ARG(4).u32;
    uint32_t want = R32(basePtr), size = R32(sizePtr);
    if (want && VirtualRegionSize(want)) RETURN(STATUS_SUCCESS);   // commit inside an existing reservation
    uint32_t a = AllocVirtual(size);
    if (!a) RETURN(STATUS_NO_MEMORY);
    W32(basePtr, a);
    W32(sizePtr, (size + 0xFFF) & ~0xFFFu);
    RETURN(STATUS_SUCCESS);
}
IMPORT(NtFreeVirtualMemory) { TRACE(); RETURN(STATUS_SUCCESS); }   // TODO: reuse freed ranges
IMPORT(NtQueryVirtualMemory)
{
    TRACE();
    uint32_t addr = ARG(3).u32, info = ARG(4).u32;
    uint32_t size = VirtualRegionSize(addr);
    memset(G2H(info), 0, 28);
    W32(info + 0, addr & ~0xFFFu); W32(info + 4, addr & ~0xFFFu); W32(info + 8, 4);
    W32(info + 12, size ? size : 0x10000); W32(info + 16, 0x1000 /*MEM_COMMIT*/); W32(info + 20, 4); W32(info + 24, 0x20000);
    RETURN(STATUS_SUCCESS);
}
IMPORT(MmAllocatePhysicalMemoryEx)
{
    TRACE();
    // (flags, size, protect, minAddress, maxAddress, alignment)
    uint32_t align = ARG(8).u32 ? ARG(8).u32 : 0x1000;
    RETURN(AllocPhysical(ARG(4).u32, align));
}
IMPORT(MmFreePhysicalMemory)  { TRACE(); RETURN(0); }
IMPORT(MmGetPhysicalAddress)  { TRACE(); uint32_t a = ARG(3).u32; RETURN((a & 0x1FFFFFFF) + (a >= 0xE0000000 ? 0x1000 : 0)); }
IMPORT(MmQueryAddressProtect) { TRACE(); RETURN(4); }
IMPORT(MmSetAddressProtect)   { TRACE(); RETURN(0); }
IMPORT(RtlFillMemoryUlong)    { TRACE(); for (uint32_t i = 0; i + 4 <= ARG(4).u32; i += 4) W32(ARG(3).u32 + i, ARG(5).u32); RETURN(0); }
IMPORT(RtlCompareMemoryUlong)
{
    TRACE();
    uint32_t i = 0;
    for (; i + 4 <= ARG(4).u32; i += 4) if (R32(ARG(3).u32 + i) != ARG(5).u32) break;
    RETURN(i);
}
IMPORT(XamAlloc) { TRACE(); uint32_t a = AllocVirtual(ARG(4).u32, 0x10); W32(ARG(5).u32, a); RETURN(a ? 0 : 0x8007000E); }
IMPORT(XamFree)  { TRACE(); RETURN(0); }

// ============================================================ time
static uint64_t FileTimeNow()
{
    using namespace std::chrono;
    auto us = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
    return uint64_t(us) * 10 + 116444736000000000ull;   // 1601 epoch, 100ns units
}
IMPORT(KeQuerySystemTime) { TRACE(); W64(ARG(3).u32, FileTimeNow()); RETURN(0); }
IMPORT(KeQueryPerformanceFrequency) { TRACE(); RETURN(50000000); }

// The console's time base (read by the mftb instruction, e.g. in QueryPerformanceCounter)
// ticks at 50 MHz. The recompiler used to emit the host's rdtsc (~3-4 GHz) instead, so every
// game-side time measurement was wildly off. Starts at "10 minutes uptime" like the bundle.
uint64_t XeTimebase()
{
    static const auto start = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    return 600ull * 50000000ull + uint64_t(ns) / 20;
}
IMPORT(RtlTimeToTimeFields)
{
    TRACE();
    time_t secs = time_t((R64(ARG(3).u32) - 116444736000000000ull) / 10000000ull);
    tm t{}; gmtime_r(&secs, &t);
    uint32_t f = ARG(4).u32;
    W16(f + 0, t.tm_year + 1900); W16(f + 2, t.tm_mon + 1); W16(f + 4, t.tm_mday); W16(f + 6, t.tm_hour);
    W16(f + 8, t.tm_min); W16(f + 10, t.tm_sec); W16(f + 12, 0); W16(f + 14, t.tm_wday);
    RETURN(0);
}
IMPORT(RtlTimeFieldsToTime) { TRACE(); W64(ARG(4).u32, FileTimeNow()); RETURN(1); }

static void TimeStampThread()
{
    auto start = std::chrono::steady_clock::now();
    for (;;)
    {
        if (g_timeStampBundle)
        {
            // a console has been up for a while when a game starts; a tick count near 0 makes
            // "now + 0" deadlines look like 0, which the game's waits treat as "forever"
            auto ms = 600000 + std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            W64(g_timeStampBundle + 0, uint64_t(ms) * 10000);
            W64(g_timeStampBundle + 8, FileTimeNow());
            W32(g_timeStampBundle + 16, uint32_t(ms));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// ============================================================ strings / misc rtl
IMPORT(RtlInitAnsiString)
{
    TRACE();
    uint32_t s = ARG(3).u32, src = ARG(4).u32;
    uint16_t len = src ? (uint16_t)strlen((char*)G2H(src)) : 0;
    W16(s, len); W16(s + 2, src ? len + 1 : 0); W32(s + 4, src);
    RETURN(0);
}
IMPORT(RtlMultiByteToUnicodeN)
{
    TRACE();
    uint32_t dst = ARG(3).u32, max = ARG(4).u32, out = ARG(5).u32, src = ARG(6).u32, len = ARG(7).u32;
    uint32_t n = std::min(max / 2, len);
    for (uint32_t i = 0; i < n; i++) W16(dst + i * 2, R8(src + i));
    if (out) W32(out, n * 2);
    RETURN(0);
}
IMPORT(RtlUnicodeToMultiByteN)
{
    TRACE();
    uint32_t dst = ARG(3).u32, max = ARG(4).u32, out = ARG(5).u32, src = ARG(6).u32, len = ARG(7).u32;
    uint32_t n = std::min(max, len / 2);
    for (uint32_t i = 0; i < n; i++) { uint16_t c = R16(src + i * 2); W8(dst + i, c < 0x100 ? c : '?'); }
    if (out) W32(out, n);
    RETURN(0);
}
IMPORT(RtlNtStatusToDosError)
{
    TRACE();
    uint32_t s = ARG(3).u32;
    switch (s)
    {
    case 0: RETURN(0);
    case STATUS_NO_SUCH_FILE: case STATUS_OBJECT_NAME_NOT_FOUND: RETURN(2);
    case STATUS_OBJECT_PATH_NOT_FOUND: RETURN(3);
    case STATUS_END_OF_FILE: RETURN(38);
    case STATUS_NO_MORE_FILES: RETURN(18);
    default: RETURN((s & 0xFFFF0000) == 0x80070000 ? (s & 0xFFFF) : 317);
    }
}
// printf-style formatting with arguments pulled from guest registers / va_list slots
template<class NextArg>
static std::string FormatGuest(const char* f, NextArg next)
{
    std::string out;
    char tmp[512];
    while (*f)
    {
        if (*f != '%') { out += *f++; continue; }
        const char* start = f++;
        if (*f == '%') { out += '%'; f++; continue; }
        while (*f && strchr("-+ #0", *f)) f++;
        if (*f == '*') { f++; next(); } else while (*f >= '0' && *f <= '9') f++;
        if (*f == '.') { f++; if (*f == '*') { f++; next(); } else while (*f >= '0' && *f <= '9') f++; }
        bool wide = false, ll = false;
        while (*f && strchr("hlLqIwjzt", *f)) { if (*f == 'l' && f[1] == 'l') { ll = true; f++; } else if (*f == 'l' || *f == 'w') wide = true; else if (*f == 'I' && f[1] == '6' && f[2] == '4') { ll = true; f += 2; } f++; }
        char conv = *f ? *f++ : 0;
        std::string spec(start, f - start);
        // strip length modifiers for the host printf, then add our own
        std::string clean;
        for (char c : spec) if (!strchr("hlLqIwjzt", c) && !(c == '6' && clean.size() && clean.back() == 'I')) clean += c;
        for (size_t p; (p = clean.find("64")) != std::string::npos && p > 0 && !isdigit((unsigned char)clean[p - 1]); ) clean.erase(p, 2);
        uint64_t v = next();
        switch (conv)
        {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
            if (ll) { clean.insert(clean.size() - 1, "ll"); snprintf(tmp, sizeof tmp, clean.c_str(), (long long)v); }
            else snprintf(tmp, sizeof tmp, clean.c_str(), (int)(uint32_t)v);
            out += tmp; break;
        case 'p': snprintf(tmp, sizeof tmp, "%08X", (uint32_t)v); out += tmp; break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
        { double d; memcpy(&d, &v, 8); snprintf(tmp, sizeof tmp, clean.c_str(), d); out += tmp; break; }
        case 's': case 'S':
        {
            std::string s;
            uint32_t a = (uint32_t)v;
            if (!a) s = "(null)";
            else if (wide || conv == 'S') for (uint32_t i = 0; R16(a + i * 2); i++) s += (char)R16(a + i * 2);
            else s = (const char*)G2H(a);
            clean.back() = 's';
            snprintf(tmp, sizeof tmp, clean.c_str(), s.c_str()); out += tmp; break;
        }
        default: out += spec; break;
        }
    }
    return out;
}

IMPORT(_vsnprintf)
{
    TRACE();
    uint32_t buf = ARG(3).u32, count = ARG(4).u32, fmt = ARG(5).u32, va = ARG(6).u32;
    uint32_t i = 0;
    std::string s = FormatGuest((const char*)G2H(fmt), [&] { return R64(va + 8 * i++); });
    if (count) { uint32_t n = std::min<uint32_t>(count, uint32_t(s.size())); memcpy(G2H(buf), s.data(), n); if (n < count) W8(buf + n, 0); }
    RETURN(s.size() < count ? s.size() : uint32_t(-1));
}
IMPORT(sprintf)
{
    TRACE();
    uint32_t buf = ARG(3).u32, fmt = ARG(4).u32;
    uint64_t regs[6] = { ARG(5).u64, ARG(6).u64, ARG(7).u64, ARG(8).u64, ARG(9).u64, ARG(10).u64 };
    uint32_t i = 0;
    std::string s = FormatGuest((const char*)G2H(fmt), [&] { uint64_t v = i < 6 ? regs[i] : R64(ctx.r1.u32 + 0x50 + 8 * i); i++; return v; });
    memcpy(G2H(buf), s.c_str(), s.size() + 1);
    RETURN(s.size());
}
IMPORT(__C_specific_handler) { TRACE(); RETURN(1); }   // ExceptionContinueSearch

IMPORT(DbgPrint) { TRACE(); Log("[game] %s", (const char*)G2H(ARG(3).u32)); RETURN(0); }
IMPORT(KeBugCheck)   { TRACE(); Log("[FATAL] KeBugCheck(%08X)\n", ARG(3).u32); PrintImportSummary(); exit(2); }
IMPORT(KeBugCheckEx) { TRACE(); Log("[FATAL] KeBugCheckEx(%08X, %08X, %08X, %08X)\n", ARG(3).u32, ARG(4).u32, ARG(5).u32, ARG(6).u32); PrintImportSummary(); exit(2); }
IMPORT(RtlRaiseException) { TRACE(); Log("[FATAL] RtlRaiseException code %08X\n", R32(ARG(3).u32)); PrintImportSummary(); exit(3); }
IMPORT(HalReturnToFirmware) { TRACE(); Log("[exit] HalReturnToFirmware(%u)\n", ARG(3).u32); PrintImportSummary(); exit(0); }
IMPORT(XamLoaderTerminateTitle) { TRACE(); Log("[exit] XamLoaderTerminateTitle\n"); PrintImportSummary(); exit(0); }

// ============================================================ XEX / module
IMPORT(RtlImageXexHeaderField)
{
    TRACE();
    uint32_t hdr = ARG(3).u32, key = ARG(4).u32;
    uint32_t count = R32(hdr + 20);
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t e = hdr + 24 + i * 8;
        if (R32(e) != key) continue;
        uint32_t value = R32(e + 4);
        if ((key & 0xFF) == 0) RETURN(value);          // value stored inline
        if ((key & 0xFF) == 1) RETURN(e + 4);          // pointer to the inline value
        RETURN(hdr + value);                           // pointer to the data
    }
    RETURN(0);
}
IMPORT(XexGetModuleHandle) { TRACE(); W32(ARG(4).u32, g_module.moduleHandle); RETURN(STATUS_SUCCESS); }
IMPORT(XexGetProcedureAddress) { TRACE(); Log("[xex] GetProcedureAddress ordinal %u - not found\n", ARG(4).u32); W32(ARG(5).u32, 0); RETURN(0xC0000139); }
IMPORT(XexCheckExecutablePrivilege) { TRACE(); RETURN(0); }
IMPORT(ExRegisterTitleTerminateNotification) { TRACE(); RETURN(0); }
IMPORT(ExGetXConfigSetting)
{
    TRACE();
    // (category, setting, buffer, bufferSize, requiredSize*)
    uint32_t cat = ARG(3).u32, set = ARG(4).u32, buf = ARG(5).u32, size = ARG(6).u32, req = ARG(7).u32;
    if (buf && size) memset(G2H(buf), 0, size);
    uint32_t value = 0, len = 4;
    if (cat == 3 && set == 9) value = 1;           // language: English
    else if (cat == 3 && set == 10) value = 0x00FF00FF; // game region: all regions
    else if (cat == 3 && set == 0x0A) value = 0x000000FF;
    if (buf && size >= 4) W32(buf, value);
    if (req) W16(req, (uint16_t)len);
    if (g_verbose) Log("[config] ExGetXConfigSetting(%u, %u)\n", cat, set);
    RETURN(STATUS_SUCCESS);
}
IMPORT(XGetLanguage)   { TRACE(); RETURN(1); }       // English
IMPORT(XGetGameRegion) { TRACE(); RETURN(0x00FF); }  // NTSC-U
IMPORT(XGetAVPack)     { TRACE(); RETURN(8); }       // HDMI
IMPORT(XGetVideoMode)
{
    TRACE();
    uint32_t m = ARG(3).u32;
    memset(G2H(m), 0, 0x30);
    W32(m + 0, 1280); W32(m + 4, 720); W32(m + 8, 1 /*interlaced? 0*/); W32(m + 8, 0);
    W32(m + 12, 1);       // widescreen
    W32(m + 16, 1);       // HD
    W32(m + 20, 0x42700000); // 60.0f refresh
    W32(m + 24, 1);       // video standard NTSC
    RETURN(0);
}
IMPORT(XamGetSystemVersion) { TRACE(); RETURN(0x20004929); }
IMPORT(XamGetExecutionId)   { TRACE(); uint32_t a = AllocSystem(0x20); W32(a + 12, 0x454107F7); W32(ARG(3).u32, a); RETURN(0); }

// ============================================================ input / users / UI (no Xbox Live)
IMPORT(XamUserGetSigninState)   { TRACE(); RETURN(ARG(3).u32 == 0 ? 1 : 0); }   // player 1 signed in locally
IMPORT(XamUserGetXUID)          { TRACE(); if (ARG(3).u32 == 0) { W64(ARG(4).u32, 0xE000000000000001ull); RETURN(0); } RETURN(0x525); }
IMPORT(XamUserGetName)
{
    TRACE();
    if (ARG(3).u32 != 0) RETURN(0x525);
    const char* n = "Player";
    uint32_t buf = ARG(4).u32, len = ARG(5).u32;
    if (len) { strncpy((char*)G2H(buf), n, len); W8(buf + len - 1, 0); }
    RETURN(0);
}
IMPORT(XamUserCheckPrivilege) { TRACE(); W32(ARG(5).u32, 1); RETURN(0); }

// ============================================================ files
// Game paths (game:\..., d:\..., \Device\Cdrom0\...) map to the folder the disc was extracted to.
uint8_t* NbashG2H(uint32_t a) { return G2H(a); }
std::string GameRoot()
{
    const char* env = getenv("NBASH_GAME_DIR");
    return env ? env : "game";
}

// Save-game containers mounted by XamContentCreate ("save:" -> a host folder), see content.cpp
static std::mutex g_rootLock;
static std::unordered_map<std::string, std::string> g_contentRoots;
void SetContentRoot(const std::string& name, const std::string& hostDir)
{
    std::string n; for (char c : name) if (c != ':') n += char(tolower((unsigned char)c));
    std::lock_guard lock(g_rootLock);
    if (hostDir.empty()) g_contentRoots.erase(n); else g_contentRoots[n] = hostDir;
}

static std::string MapPath(std::string p)
{
    for (auto& c : p) if (c == '\\') c = '/';
    // right-stick control layout: on-court button prompts that match it (dunk prompt shows X)
    {
        static const bool rstick = !(getenv("NBASH_RSTICK") && !strcmp(getenv("NBASH_RSTICK"), "off"));
        std::string low; for (char ch : p) low += char(tolower((unsigned char)ch));
        const std::string tail = "data/fe/indicators.big";
        if (rstick && low.size() >= tail.size() && low.compare(low.size() - tail.size(), tail.size(), tail) == 0)
            p = p.substr(0, p.size() - 4) + "_rstick.big";
    }
    // your music built into the game: the song list (songs.bin) with your songs in EA's slots
    {
        std::string low; for (char ch : p) low += char(tolower((unsigned char)ch));
        const std::string tail = "data/database/songs.bin";
        if (low.size() >= tail.size() && low.compare(low.size() - tail.size(), tail.size(), tail) == 0)
        {
            bool MusicReplacesGameMusic(); bool MusicWriteSongsBin(const std::string&, const std::string&);
            static int state = 0;   // 0 not built, 1 built, 2 not used
            static std::mutex m; std::lock_guard lk(m);
            const std::string orig = GameRoot() + "/data/database/songs.bin", custom = GameRoot() + "/data/database/songs_custom.bin";
            if (state == 0) state = MusicReplacesGameMusic() && MusicWriteSongsBin(orig, custom) ? 1 : 2;
            if (state == 1) p = p.substr(0, p.size() - 4) + "_custom.bin";
        }
    }
    {
        auto c = p.find(':');
        if (c != std::string::npos && c > 0)
        {
            std::string n; for (char ch : p.substr(0, c)) n += char(tolower((unsigned char)ch));
            std::lock_guard lock(g_rootLock);
            auto it = g_contentRoots.find(n);
            if (it != g_contentRoots.end())
            {
                std::string rest = p.substr(c + 1);
                while (!rest.empty() && rest[0] == '/') rest.erase(0, 1);
                return (fs::path(it->second) / rest).string();
            }
        }
    }
    auto colon = p.find(":/");
    if (colon != std::string::npos) p = p.substr(colon + 2);
    else if (p.rfind("/Device/", 0) == 0) { auto s = p.find('/', 8); p = s == std::string::npos ? "" : p.substr(s + 1); }
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    // case-insensitive lookup, one component at a time
    fs::path cur = GameRoot();
    size_t start = 0;
    while (start <= p.size())
    {
        size_t end = p.find('/', start);
        std::string part = p.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!part.empty() && part != ".")
        {
            fs::path next = cur / part;
            if (!fs::exists(next) && fs::is_directory(cur))
            {
                for (auto& e : fs::directory_iterator(cur))
                    if (strcasecmp(e.path().filename().string().c_str(), part.c_str()) == 0) { next = e.path(); break; }
            }
            cur = next;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return cur.string();
}

// One FILE* per guest handle. Several game threads can use the same handle at once (the
// streaming reader and the loader), so every seek+read/write runs under the file's own lock;
// otherwise one thread's seek moves the other's read and a movie stream gets the wrong bytes.
struct ArchiveOverlay;   // archivemod.cpp: mod files appended to a BIG archive
std::string ArchiveModIndex(const std::string& hostPath);
const ArchiveOverlay* ArchiveModOverlay(const std::string& hostPath);
size_t ArchiveModRead(const ArchiveOverlay* ov, FILE* f, uint64_t pos, uint8_t* dst, size_t len);
struct OpenFile { FILE* f = nullptr; std::string path; bool dir = false; uint64_t size = 0; std::mutex m; uint32_t reader = 0; bool sharedLogged = false; const ArchiveOverlay* overlay = nullptr; uint64_t overlaySize = 0; };
static void FileSeek(FILE* f, int64_t o)
{
#ifdef _WIN32
    _fseeki64(f, o, SEEK_SET);
#else
    fseeko(f, o, SEEK_SET);
#endif
}
static std::mutex g_fileLock;
static std::unordered_map<uint32_t, OpenFile*> g_files;

static std::string ObjectName(uint32_t objAttr)
{
    uint32_t str = R32(objAttr + 4);
    if (!str) return {};
    return std::string((const char*)G2H(R32(str + 4)), R16(str));
}

// The two EA logo movies at startup sometimes freeze (see PROJECT_NOTES); they are skipped
// unless NBASH_PLAYINTROS is set. The game simply moves on when a movie file is missing.
static bool SkippedMovie(const std::string& name)
{
    static const bool play = getenv("NBASH_PLAYINTROS") != nullptr;
    if (play) return false;
    std::string n; for (char c : name) n += char(tolower((unsigned char)c));
    return n.find("ea_hd_ntsc.") != std::string::npos || n.find("eas_big_ntsc.") != std::string::npos;
}

static uint32_t OpenCommon(uint32_t handlePtr, uint32_t objAttr, uint32_t iosb, bool write)
{
    std::string name = ObjectName(objAttr);
    if (SkippedMovie(name))
    {
        if (iosb) { W32(iosb, STATUS_OBJECT_NAME_NOT_FOUND); W32(iosb + 4, 0); }
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    std::string host = MapPath(name);
    if (!write) host = ArchiveModIndex(host);           // X.bh -> X_mod.bh when game/mods/X has files
    auto* of = new OpenFile();
    of->path = host;
    if (fs::is_directory(host)) of->dir = true;
    else
    {
        of->f = fopen(host.c_str(), write ? "r+b" : "rb");
        if (!of->f && write) { fs::create_directories(fs::path(host).parent_path()); of->f = fopen(host.c_str(), "w+b"); }
        if (!of->f)
        {
            Log("[file] NOT FOUND: %s\n", name.c_str());
            delete of;
            if (iosb) { W32(iosb, STATUS_OBJECT_NAME_NOT_FOUND); W32(iosb + 4, 0); }
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        fseek(of->f, 0, SEEK_END); of->size = ftell(of->f); fseek(of->f, 0, SEEK_SET);
        if (!write)
            if ((of->overlay = ArchiveModOverlay(host)))
            {
                extern uint64_t ArchiveOverlayTotal(const ArchiveOverlay*);
                of->size = ArchiveOverlayTotal(of->overlay);
            }
    }
    Log("[file] open %s%s (thread %u)\n", name.c_str(), of->dir ? " (folder)" : "", t_thread ? t_thread->id : 0);
    uint32_t h = NewHandle(0);
    { std::lock_guard lock(g_fileLock); g_files[h] = of; }
    W32(handlePtr, h);
    if (iosb) { W32(iosb, 0); W32(iosb + 4, 1); }
    return STATUS_SUCCESS;
}

static OpenFile* FileFor(uint32_t h) { std::lock_guard lock(g_fileLock); auto it = g_files.find(h); return it == g_files.end() ? nullptr : it->second; }

IMPORT(NtCreateFile)
{
    TRACE();
    bool write = (ARG(4).u32 & 0x40000000) != 0;
    uint32_t disposition = ARG(10).u32;               // 0 supersede, 2 create, 4 overwrite, 5 overwrite-if: start empty
    uint32_t options = R32(ctx.r1.u32 + 0x54);        // 9th argument (on the stack)
    if (options & 1)                                  // FILE_DIRECTORY_FILE
    {
        std::string host = MapPath(ObjectName(ARG(5).u32));
        if (disposition != 1 && !fs::exists(host)) { std::error_code ec; fs::create_directories(host, ec); }
    }
    else if (write && (disposition == 0 || disposition == 2 || disposition == 4 || disposition == 5))
    {
        std::string host = MapPath(ObjectName(ARG(5).u32));
        if (disposition != 4 || fs::exists(host))
        {
            std::error_code ec; fs::create_directories(fs::path(host).parent_path(), ec);
            if (FILE* f = fopen(host.c_str(), "wb")) fclose(f);
        }
    }
    RETURN(OpenCommon(ARG(3).u32, ARG(5).u32, ARG(6).u32, write));
}
IMPORT(NtOpenFile)   { TRACE(); RETURN(OpenCommon(ARG(3).u32, ARG(5).u32, ARG(6).u32, false)); }
IMPORT(NtReadFile)
{
    TRACE();
    // (handle, event, apc, apcContext, iosb, buffer, length, byteOffset*)
    OpenFile* of = FileFor(ARG(3).u32);
    uint32_t iosb = ARG(7).u32, buf = ARG(8).u32, len = ARG(9).u32, off = ARG(10).u32;
    { static std::atomic<int> logged{0}; if ((ARG(5).u32 || ARG(4).u32) && logged++ < 5) Log("[file] read with event %08X apc %08X ctx %08X\n", ARG(4).u32, ARG(5).u32, ARG(6).u32); }
    if (!of || !of->f) RETURN(STATUS_INVALID_HANDLE);
    size_t n;
    {
        std::lock_guard fl(of->m);
        uint32_t me = t_thread ? t_thread->id : 0;
        if (of->reader && of->reader != me && !of->sharedLogged)
        { of->sharedLogged = true; Log("[file] %s read by threads %u and %u\n", of->path.c_str(), of->reader, me); }
        of->reader = me;
        if (off) { int64_t o = (int64_t)R64(off); if (o >= 0) FileSeek(of->f, o); }
        if (of->overlay)
        {
#ifdef _WIN32
            uint64_t pos = uint64_t(_ftelli64(of->f));
#else
            uint64_t pos = uint64_t(ftello(of->f));
#endif
            n = ArchiveModRead(of->overlay, of->f, pos, G2H(buf), len);
            FileSeek(of->f, int64_t(pos + n));
        }
        else
        {
#ifdef _WIN32
            uint64_t start = uint64_t(_ftelli64(of->f));
#else
            uint64_t start = uint64_t(ftello(of->f));
#endif
            n = fread(G2H(buf), 1, len, of->f);
            void NbashShoesOnRead(const std::string&, uint64_t, uint32_t, uint8_t*, size_t);
            NbashShoesOnRead(of->path, start, buf, G2H(buf), n);      // shoe picker (shoes.cpp)
        }
        if (of->path.find("movies") != std::string::npos)
        {
            static std::atomic<int> movieReads{0};
            if (movieReads++ < 400)
                Log("[movie-read] thread %u off %lld len %u got %u event %08X apc %08X t=%llu\n", me,
                    off ? (long long)(int64_t)R64(off) : -1LL, len, unsigned(n), ARG(4).u32, ARG(5).u32,
                    (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() % 1000000);
        }
    }
    uint32_t st = (n == 0 && len) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    if (iosb) { W32(iosb, st); W32(iosb + 4, uint32_t(n)); }
    if (ARG(4).u32) Signal(HandleObject(ARG(4).u32), 1);
    RETURN(st);
}
IMPORT(NtWriteFile)
{
    TRACE();
    OpenFile* of = FileFor(ARG(3).u32);
    uint32_t iosb = ARG(7).u32, buf = ARG(8).u32, len = ARG(9).u32, off = ARG(10).u32;
    if (!of || !of->f) RETURN(STATUS_INVALID_HANDLE);
    size_t n;
    {
        std::lock_guard fl(of->m);
        if (off) { int64_t o = (int64_t)R64(off); if (o >= 0) FileSeek(of->f, o); }
        n = fwrite(G2H(buf), 1, len, of->f);
    }
    if (iosb) { W32(iosb, 0); W32(iosb + 4, uint32_t(n)); }
    RETURN(0);
}
IMPORT(NtQueryInformationFile)
{
    TRACE();
    // (handle, iosb, info, length, class)
    OpenFile* of = FileFor(ARG(3).u32);
    uint32_t iosb = ARG(4).u32, info = ARG(5).u32, cls = ARG(7).u32;
    if (!of) RETURN(STATUS_INVALID_HANDLE);
    memset(G2H(info), 0, ARG(6).u32);
    switch (cls)
    {
    case 5:  W64(info, of->size); W64(info + 8, of->size); W32(info + 16, 1); W8(info + 21, of->dir); break;   // standard
    case 14: { std::lock_guard fl(of->m); W64(info, of->f ? ftell(of->f) : 0); } break;                                                      // position
    case 34: W64(info + 32, of->size); W64(info + 40, of->size); W32(info + 48, of->dir ? 0x10 : 0x80); break; // network open
    default: Log("[file] QueryInformationFile class %u\n", cls); break;
    }
    if (iosb) { W32(iosb, 0); W32(iosb + 4, ARG(6).u32); }
    RETURN(0);
}
IMPORT(NtSetInformationFile)
{
    TRACE();
    OpenFile* of = FileFor(ARG(3).u32);
    if (of && of->f && ARG(7).u32 == 14) { std::lock_guard fl(of->m); FileSeek(of->f, (int64_t)R64(ARG(5).u32)); }
    RETURN(0);
}
IMPORT(NtQueryVolumeInformationFile) { TRACE(); memset(G2H(ARG(5).u32), 0, ARG(6).u32); RETURN(0); }
IMPORT(NtQueryDirectoryFile) { TRACE(); RETURN(STATUS_NO_MORE_FILES); }
IMPORT(NtFlushBuffersFile)   { TRACE(); RETURN(0); }
static std::string AnsiString(uint32_t s) { return s ? std::string((const char*)G2H(R32(s + 4)), R16(s)) : std::string(); }
IMPORT(ObCreateSymbolicLink) { TRACE(); Log("[file] link %s -> %s\n", AnsiString(ARG(3).u32).c_str(), AnsiString(ARG(4).u32).c_str()); RETURN(0); }
IMPORT(ObDeleteSymbolicLink) { TRACE(); RETURN(0); }

// ============================================================ video (Vd*) - display queries; the GPU itself is gpu.cpp
static void FillVideoMode(uint32_t m)
{
    memset(G2H(m), 0, 0x30);
    W32(m + 0, 1280); W32(m + 4, 720);   // display width/height
    W32(m + 8, 0);                       // progressive
    W32(m + 12, 1);                      // widescreen
    W32(m + 16, 1);                      // HD
    W32(m + 20, 0x42700000);             // 60.0f refresh rate
    W32(m + 24, 1);                      // NTSC
    W32(m + 28, 0x4A);                   // pixel rate
    W32(m + 32, 0x10000 | 8);            // AV pack: HDMI
}
IMPORT(VdQueryVideoMode)  { TRACE(); FillVideoMode(ARG(3).u32); RETURN(0); }
IMPORT(VdQueryVideoFlags) { TRACE(); RETURN(3); }   // widescreen | 1280-wide
IMPORT(VdGetCurrentDisplayGamma) { TRACE(); W32(ARG(3).u32, 2); W32(ARG(4).u32, 0x400E38E4 /* 2.2222f */); RETURN(0); }
IMPORT(VdGetCurrentDisplayInformation)
{
    TRACE();
    uint32_t d = ARG(3).u32;
    memset(G2H(d), 0, 0x58);
    W16(d + 0, 1280); W16(d + 2, 720);
    W16(d + 0x40, 1280); W16(d + 0x42, 720);
    W32(d + 0x48, 0x42700000);
    RETURN(0);
}
IMPORT(VdIsHSIOTrainingSucceeded)   { TRACE(); RETURN(1); }
IMPORT(VdRetrainEDRAM)              { TRACE(); RETURN(0); }
IMPORT(VdRetrainEDRAMWorker)        { TRACE(); RETURN(0); }
IMPORT(VdPersistDisplay)            { TRACE(); if (ARG(4).u32) W32(ARG(4).u32, 0); RETURN(0); }
IMPORT(VdEnableDisableClockGating)  { TRACE(); RETURN(0); }
IMPORT(VdSetDisplayMode)            { TRACE(); RETURN(0); }
IMPORT(VdShutdownEngines)           { TRACE(); RETURN(0); }
IMPORT(VdCallGraphicsNotificationRoutines) { TRACE(); RETURN(0); }
IMPORT(KiApcNormalRoutineNop)       { TRACE(); RETURN(0); }

// ============================================================ init
void KernelInit()
{
    if (getenv("NBASH_VERBOSE")) g_verbose = 1;
    if (FILE* f = fopen("nbash_verbose.txt", "r")) { g_verbose = 1; fclose(f); }
    std::thread(TimeStampThread).detach();
}

// ============================================================ network (offline console, as in Xenia)
// WSA events are ordinary manual-reset events.
IMPORT(NetDll_WSACreateEvent) { TRACE(); RETURN(NewHandle(NewDispatcher(kNotificationEvent, 0))); }
IMPORT(NetDll_WSACloseEvent)  { TRACE(); RETURN(1); }
IMPORT(NetDll_WSASetEvent)    { TRACE(); Signal(HandleObject(ARG(3).u32), 1); RETURN(1); }
IMPORT(NetDll_WSAResetEvent)  { TRACE(); Signal(HandleObject(ARG(3).u32), 0); RETURN(1); }
// (count, events*, waitAll, timeout ms, alertable)
IMPORT(NetDll_WSAWaitForMultipleEvents)
{
    TRACE();
    uint32_t n = std::min<uint32_t>(ARG(3).u32, 64), timeoutMs = ARG(6).u32;
    std::vector<uint32_t> objs;
    for (uint32_t i = 0; i < n; i++) objs.push_back(HandleObject(R32(ARG(4).u32 + i * 4)));
    uint32_t t = 0;
    if (timeoutMs != 0xFFFFFFFF)
    {
        // a relative 100 ns timeout, stored in scratch guest memory owned by this thread
        static thread_local uint32_t scratch = 0;
        if (!scratch) scratch = AllocSystem(8, 8);
        W64(scratch, uint64_t(-int64_t(timeoutMs) * 10000));
        t = scratch;
    }
    uint32_t r = WaitMany(objs, ARG(5).u32 != 0, t);
    RETURN(r == STATUS_TIMEOUT ? 0x102 : r);            // WSA_WAIT_TIMEOUT / WSA_WAIT_EVENT_0 + i
}
// XNADDR: ina, inaOnline, wPortOnline, abEnet[6], abOnline[20]
IMPORT(NetDll_XNetGetTitleXnAddr)
{
    TRACE();
    uint32_t a = ARG(4).u32;
    if (a)
    {
        memset(G2H(a), 0, 36);
        W32(a, 0x7F000001);                               // loopback
        memset(G2H(a + 10), 0xCC, 6);                     // a non-zero MAC (some titles need one)
    }
    RETURN(4);                                            // XNET_GET_XNADDR_STATIC
}
// (caller, host, event handle, XNDNS**): lookups fail at once and signal the event
IMPORT(NetDll_XNetDnsLookup)
{
    TRACE();
    if (uint32_t pdns = ARG(6).u32)
    {
        uint32_t dns = AllocSystem(0x40, 8);
        W32(dns, 1);                                      // status: non-zero = error
        W32(pdns, dns);
    }
    if (ARG(5).u32) Signal(HandleObject(ARG(5).u32), 1);
    RETURN(0);
}
IMPORT(NetDll_XNetDnsRelease) { TRACE(); RETURN(0); }

// ============================================================ notifications (as in Xenia)
// A listener is an event (signaled while notifications are queued) plus a queue. Games
// expect a few notifications at startup: UI inactive + player 1 signed in (system), Live
// disconnected (live), and "title controls the music" (music player).
struct NotifyListener { uint32_t object; uint64_t mask; std::vector<std::pair<uint32_t, uint32_t>> queue; };
static std::mutex g_notifyLock;
static std::unordered_map<uint32_t, NotifyListener> g_listeners;   // handle -> listener
static bool g_notifiedSystem = false, g_notifiedLive = false, g_notifiedXmp = false;

IMPORT(XamNotifyCreateListener)
{
    TRACE();
    uint64_t mask = ARG(3).u64;
    uint32_t obj = NewDispatcher(kNotificationEvent, 0);
    uint32_t h = NewHandle(obj);
    std::lock_guard lock(g_notifyLock);
    auto& l = g_listeners[h];
    l.object = obj; l.mask = mask;
    if (!g_notifiedSystem && (mask & 0x1)) { g_notifiedSystem = true; l.queue.push_back({ 0x09, 0 }); l.queue.push_back({ 0x0A, 1 }); }
    else if ((mask & 0x1) && getenv("NBASH_SIGNINALL")) l.queue.push_back({ 0x0A, 1 });
    if (!g_notifiedLive && (mask & 0x2)) { g_notifiedLive = true; l.queue.push_back({ 0x02000001, 0x001510F1 }); l.queue.push_back({ 0x02000003, 0 }); }
    if (!g_notifiedXmp && (mask & 0x20)) { g_notifiedXmp = true; l.queue.push_back({ 0x0A000003, 1 }); }
    if (!l.queue.empty()) Signal(obj, 1);
    Log("[notify] listener %08X mask %llX, %zu startup notifications\n", h, (unsigned long long)mask, l.queue.size());
    RETURN(h);
}

// Send a notification to every listener whose mask covers its area (system = bit 0, live = bit 1, ...)
void BroadcastNotification(uint32_t id, uint32_t param)
{
    uint32_t area = (id >> 25) & 0x3F;
    std::lock_guard lock(g_notifyLock);
    for (auto& [h, l] : g_listeners)
        if (l.mask & (1ull << area)) { l.queue.push_back({ id, param }); Signal(l.object, 1); }
}

// XOVERLAPPED: result (+0), length (+4), context (+8), event (+12), completion routine (+16),
// completion context (+20), extended error (+24)
void CompleteOverlapped(uint32_t ov, uint32_t result, uint32_t extendedError, uint32_t length)
{
    if (!ov) return;
    W32(ov + 0, result); W32(ov + 4, length); W32(ov + 24, extendedError);
    if (uint32_t ev = R32(ov + 12)) if (uint32_t o = HandleObject(ev)) Signal(o, 1);
    if (R32(ov + 16)) Log("[overlapped] completion routine %08X not called (not supported)\n", R32(ov + 16));
}
uint32_t NewPlainHandle() { return NewHandle(0); }

// XNotifyGetNext(handle, matchId, id*, param*) -> 1 if a notification was returned
IMPORT(XNotifyGetNext)
{
    TRACE();
    uint32_t h = ARG(3).u32, match = ARG(4).u32, idPtr = ARG(5).u32, paramPtr = ARG(6).u32;
    if (paramPtr) W32(paramPtr, 0);
    if (!idPtr) RETURN(0);
    W32(idPtr, 0);
    std::lock_guard lock(g_notifyLock);
    auto it = g_listeners.find(h);
    if (it == g_listeners.end()) RETURN(0);
    auto& q = it->second.queue;
    for (size_t i = 0; i < q.size(); i++)
    {
        if (match && q[i].first != match) continue;
        W32(idPtr, q[i].first);
        if (paramPtr) W32(paramPtr, q[i].second);
        q.erase(q.begin() + i);
        if (q.empty()) Signal(it->second.object, 0);
        RETURN(1);
    }
    RETURN(0);
}
