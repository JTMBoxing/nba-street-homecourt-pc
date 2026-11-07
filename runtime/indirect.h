// Force-included into the recompiled game code: checked indirect calls (function pointers,
// virtual calls). A bad target is reported instead of crashing with no information.
#pragma once
#include <cstdint>
struct PPCContext;
void PPCCallIndirect(PPCContext& ctx, uint8_t* base, uint32_t target);
#define PPC_CALL_INDIRECT_FUNC(x) PPCCallIndirect(ctx, base, uint32_t(x))
uint64_t XeTimebase();   // guest time base (mftb): 50 MHz, like the console

#ifdef NBASH_FIRSTCALL
// Diagnostic build: every game function gets a call counter (and its first call is logged), so the
// code that runs only after a given button press can be listed. See actiontrace.cpp.
unsigned NbashFuncIndex(const char* name);
extern unsigned* g_nbashCalls;
// read watch: loads from up to 4 guest ranges are reported with the reading function's name
extern uint32_t g_watchLo, g_watchSpan, g_watchLo2, g_watchSpan2;
void NbashWatchHit(uint32_t addr, const char* fn);
static inline uint32_t NbashWatch_(uint32_t a, const char* fn)
{
    if (__builtin_expect((a - g_watchLo < g_watchSpan) | (a - g_watchLo2 < g_watchSpan2), 0)) NbashWatchHit(a, fn);
    return a;
}
#define PPC_LOAD_U8(x) *(volatile uint8_t*)(base + NbashWatch_((uint32_t)(x), __func__))
#define PPC_LOAD_U16(x) __builtin_bswap16(*(volatile uint16_t*)(base + NbashWatch_((uint32_t)(x), __func__)))
#define PPC_LOAD_U32(x) __builtin_bswap32(*(volatile uint32_t*)(base + NbashWatch_((uint32_t)(x), __func__)))
#define PPC_LOAD_U64(x) __builtin_bswap64(*(volatile uint64_t*)(base + NbashWatch_((uint32_t)(x), __func__)))
#define PPC_FUNC_PROLOGUE() do { static const unsigned nbashIdx_ = NbashFuncIndex(__func__); g_nbashCalls[nbashIdx_]++; \
    __builtin_assume(((size_t)base & 0x1F) == 0); } while (0)
#endif
