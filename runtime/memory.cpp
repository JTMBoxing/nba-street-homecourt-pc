// Guest memory: one 4 GB block that mirrors the Xbox 360's 32-bit address space.
#include "guest.h"
#include <map>
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

uint8_t* g_base = nullptr;

namespace
{
    std::mutex g_memLock;

    struct Range { uint32_t next, end; const char* name; };
    // Xbox 360 layout: 0x40000000-0x7FFFFFFF virtual (64 KB pages), 0xA0000000+ physical.
    // The game image lives at 0x82000000 and the recompiler's function table right after it.
    Range g_virtual  { 0x40000000, 0x7F000000, "virtual" };
    Range g_physical { 0xA0000000, 0xBFFF0000, "physical" };
    Range g_system   { 0x30000000, 0x3FFF0000, "system" };

    std::map<uint32_t, uint32_t> g_regions;   // base -> size (virtual reservations)

    uint32_t Bump(Range& r, uint32_t size, uint32_t align)
    {
        std::lock_guard lock(g_memLock);
        if (align < 0x10) align = 0x10;
        uint32_t a = (r.next + align - 1) & ~(align - 1);
        if (size == 0 || uint64_t(a) + size > r.end)
        {
            Log("[mem] OUT OF %s MEMORY (asked 0x%X)\n", r.name, size);
            return 0;
        }
        r.next = a + size;
        memset(g_base + a, 0, size);
        return a;
    }
}

// used parts of guest memory (for the possession finder)
void GuestUsedRanges(uint32_t out[3][2])
{
    std::lock_guard lock(g_memLock);
    out[0][0] = 0x40000000; out[0][1] = g_virtual.next;
    out[1][0] = 0xA0000000; out[1][1] = g_physical.next;
    out[2][0] = 0x82000000; out[2][1] = 0x82800000;   // the game image (its global variables)
}

void MemoryInit()
{
    const size_t size = PPC_MEMORY_SIZE;
#ifdef _WIN32
    // 4 GB placeholder, then: 0x00000000-0x9FFFFFFF ordinary memory, and the same 512 MB
    // pagefile section mapped at 0xA0000000, 0xC0000000 and 0xE0000000.
    // (The console's 0xE0000000 view is offset by 4 KB; this game never uses that range.)
    using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
    using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);
    HMODULE kb = LoadLibraryW(L"kernelbase.dll");
    auto pVirtualAlloc2 = (VirtualAlloc2Fn)GetProcAddress(kb, "VirtualAlloc2");
    auto pMapViewOfFile3 = (MapViewOfFile3Fn)GetProcAddress(kb, "MapViewOfFile3");
    if (!pVirtualAlloc2 || !pMapViewOfFile3)
    {
        MessageBoxA(nullptr, "This needs Windows 10 (version 1803) or newer.", "NBA Street Homecourt", MB_ICONERROR);
        exit(1);
    }
    constexpr ULONG kReservePlaceholder = 0x00040000, kReplacePlaceholder = 0x00004000, kPreservePlaceholder = 0x00000002;
    g_base = (uint8_t*)pVirtualAlloc2(nullptr, nullptr, size, MEM_RESERVE | kReservePlaceholder, PAGE_NOACCESS, nullptr, 0);
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 0x20000000, nullptr);
    bool ok = g_base && section;
    for (uint64_t view : { 0xE0000000ull, 0xC0000000ull, 0xA0000000ull })   // split from the end down
    {
        if (!ok) break;
        ok = VirtualFree(g_base + view, 0x20000000, MEM_RELEASE | kPreservePlaceholder)
          && pMapViewOfFile3(section, nullptr, g_base + view, 0, 0x20000000, kReplacePlaceholder, PAGE_READWRITE, nullptr, 0);
    }
    ok = ok && pVirtualAlloc2(nullptr, g_base, 0xA0000000, MEM_RESERVE | MEM_COMMIT | kReplacePlaceholder, PAGE_READWRITE, nullptr, 0);
    if (!ok)
    {
        char msg[200];
        snprintf(msg, sizeof(msg), "Could not set up the 4 GB of console memory (error %lu).\nClose other programs and try again.", GetLastError());
        MessageBoxA(nullptr, msg, "NBA Street Homecourt", MB_ICONERROR);
        exit(1);
    }
    return;
#else
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    g_base = p == MAP_FAILED ? nullptr : (uint8_t*)p;
#endif
    if (!g_base)
    {
        fprintf(stderr, "Could not reserve 4 GB for guest memory.\n");
        exit(1);
    }

    // The 512 MB of physical memory is visible three times, like on the console:
    //   0xA0000000 (64 KB pages), 0xC0000000 (16 MB pages), 0xE0000000 (4 KB pages, offset by 0x1000)
#ifndef _WIN32
    int fd = memfd_create("xenon-physical", 0);
    if (fd < 0 || ftruncate(fd, 0x20000000) != 0) { fprintf(stderr, "Could not create physical memory.\n"); exit(1); }
    bool ok = mmap(g_base + 0xA0000000, 0x20000000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) != MAP_FAILED
           && mmap(g_base + 0xC0000000, 0x20000000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) != MAP_FAILED
           && mmap(g_base + 0xE0000000, 0x20000000 - 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0x1000) != MAP_FAILED;
    if (!ok) { fprintf(stderr, "Could not mirror physical memory.\n"); exit(1); }
#else
    // TODO (Windows build): same mirroring with CreateFileMapping + MapViewOfFile3 placeholders
#endif
}

uint32_t AllocVirtual(uint32_t size, uint32_t align)
{
    size = (size + 0xFFF) & ~0xFFFu;
    uint32_t a = Bump(g_virtual, size, align);
    if (a)
    {
        std::lock_guard lock(g_memLock);
        g_regions[a] = size;
    }
    return a;
}

uint32_t VirtualRegionSize(uint32_t addr)
{
    std::lock_guard lock(g_memLock);
    auto it = g_regions.upper_bound(addr);
    if (it == g_regions.begin()) return 0;
    --it;
    return (addr < it->first + it->second) ? it->second : 0;
}

uint32_t AllocPhysical(uint32_t size, uint32_t align) { return Bump(g_physical, (size + 0xFFF) & ~0xFFFu, align); }
uint32_t AllocSystem(uint32_t size, uint32_t align)   { return Bump(g_system, size, align); }
