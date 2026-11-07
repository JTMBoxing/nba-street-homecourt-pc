// NBA Street Homecourt PC port - runtime shared definitions
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <string>
#include <mutex>

#include "ppc_config.h"
#include "ppc_context.h"

// ---------------------------------------------------------------- guest memory
extern uint8_t* g_base;                       // host address of guest address 0

inline uint8_t* G2H(uint32_t a) { return g_base + a; }
inline uint32_t H2G(const void* p) { return uint32_t((const uint8_t*)p - g_base); }

inline uint8_t  R8 (uint32_t a) { return g_base[a]; }
inline uint16_t R16(uint32_t a) { uint16_t v; memcpy(&v, g_base + a, 2); return __builtin_bswap16(v); }
inline uint32_t R32(uint32_t a) { uint32_t v; memcpy(&v, g_base + a, 4); return __builtin_bswap32(v); }
inline uint64_t R64(uint32_t a) { uint64_t v; memcpy(&v, g_base + a, 8); return __builtin_bswap64(v); }
inline void W8 (uint32_t a, uint8_t v)  { g_base[a] = v; }
inline void W16(uint32_t a, uint16_t v) { v = __builtin_bswap16(v); memcpy(g_base + a, &v, 2); }
inline void W32(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(g_base + a, &v, 4); }
inline void W64(uint32_t a, uint64_t v) { v = __builtin_bswap64(v); memcpy(g_base + a, &v, 8); }

// allocators (memory.cpp)
void     MemoryInit();
uint32_t AllocVirtual(uint32_t size, uint32_t align = 0x10000);   // NtAllocateVirtualMemory range
uint32_t AllocPhysical(uint32_t size, uint32_t align = 0x1000);   // MmAllocatePhysicalMemoryEx range
uint32_t AllocSystem(uint32_t size, uint32_t align = 0x10);       // runtime-owned objects
uint32_t VirtualRegionSize(uint32_t addr);

// ---------------------------------------------------------------- threads
struct GuestThread
{
    uint32_t pcr = 0;          // r13
    uint32_t tls = 0;          // TLS slot array (64 slots)
    uint32_t object = 0;       // guest KTHREAD-like dispatcher object (signaled on exit)
    uint32_t handle = 0;
    uint32_t id = 0;
    PPCContext* ctx = nullptr;
    const char* lastImport = nullptr;
    uint32_t lastIndirect = 0;
};

extern thread_local GuestThread* t_thread;
GuestThread* CreateGuestThreadState(uint32_t stackSize);
void         InitContext(PPCContext& ctx, GuestThread* t, uint32_t stackTop);

// guest -> host function table (kept in host memory: the game uses the memory after its image)
extern PPCFunc** g_funcTable;
inline PPCFunc* LookupFunction(uint32_t a)
{
    if (a < PPC_CODE_BASE || a >= PPC_CODE_BASE + PPC_CODE_SIZE || (a & 3)) return nullptr;
    return g_funcTable[(a - PPC_CODE_BASE) / 4];
}

// call a guest function from host code
void GuestCall(PPCContext& ctx, uint32_t address);

// ---------------------------------------------------------------- logging
extern int g_verbose;
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void TraceImport(const char* name, PPCContext& ctx);   // counts calls, prints the first few
void Unimplemented(const char* name, PPCContext& ctx);
void PrintImportSummary();
void PrintThreadStatus();

// ---------------------------------------------------------------- helpers for import implementations
#define IMPORT(name) PPC_FUNC(__imp__##name)
#define TRACE_IMPORT() TraceImport(__func__ + 7, ctx)   // __func__ is "__imp__Name"
#define ARG(n) ctx.r##n
#define RETURN(v) do { ctx.r3.u64 = uint64_t(v); return; } while (0)

constexpr uint32_t STATUS_SUCCESS              = 0x00000000;
constexpr uint32_t STATUS_TIMEOUT              = 0x00000102;
constexpr uint32_t STATUS_PENDING              = 0x00000103;
constexpr uint32_t STATUS_UNSUCCESSFUL         = 0xC0000001;
constexpr uint32_t STATUS_NOT_IMPLEMENTED      = 0xC0000002;
constexpr uint32_t STATUS_INVALID_HANDLE       = 0xC0000008;
constexpr uint32_t STATUS_INVALID_PARAMETER    = 0xC000000D;
constexpr uint32_t STATUS_NO_SUCH_FILE         = 0xC000000F;
constexpr uint32_t STATUS_END_OF_FILE          = 0xC0000011;
constexpr uint32_t STATUS_NO_MEMORY            = 0xC0000017;
constexpr uint32_t STATUS_OBJECT_NAME_NOT_FOUND= 0xC0000034;
constexpr uint32_t STATUS_OBJECT_PATH_NOT_FOUND= 0xC000003A;
constexpr uint32_t STATUS_NO_MORE_FILES        = 0x80000006;

// xex loader (loader.cpp)
struct LoadedModule
{
    uint32_t entryPoint = 0;
    uint32_t xexHeader = 0;      // guest copy of the XEX header
    uint32_t moduleHandle = 0;   // guest LDR_DATA_TABLE_ENTRY-like block
    uint32_t defaultStackSize = 0x40000;
};
extern LoadedModule g_module;
bool LoadXex(const char* path);

// kernel (kernel.cpp)
extern uint32_t g_timeStampBundle;
void KernelInit();
std::string GameRoot();
