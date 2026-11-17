// Xenos GPU front end (command processor). Consumes the game's PM4 command stream so
// Direct3D's synchronization works: ring buffer, indirect buffers, bin predication, register
// writes, scratch/fence write-back, waits, interrupts and the 60 Hz vblank.
// Packet semantics follow Xenia's command processor (BSD licensed).
// Drawing is not implemented yet - draw packets are only counted.
#include "guest.h"
#include <chrono>
#include <condition_variable>
namespace render { int WaitDisplayVblank(double* hzOut, int fps); }
int NbashFps();
#include <thread>
#include <vector>
#include <map>
#include <mutex>
#include <string>
#include <filesystem>
#ifdef _WIN32
#include <direct.h>
#endif
#ifdef _WIN32
#include "render/renderer.h"
#endif
void* WindowHandle();
uint64_t AudioFramesSubmitted();
extern std::atomic<bool> g_rendererActive;
std::atomic<bool> g_captureRequest{false};   // F12 in the game window

void DumpGuestMemory();
uint64_t AudioFramesWithSound();   // audio.cpp
namespace
{
    constexpr uint32_t kRegisterBase = 0x7FC80000;   // MMIO register window seen by the CPU
    constexpr uint32_t REG_CP_RB_RPTR = 0x01C4, REG_CP_RB_WPTR = 0x01C5;
    constexpr uint32_t REG_SCRATCH_UMSK = 0x01DC, REG_SCRATCH_ADDR = 0x01DD;
    constexpr uint32_t REG_SCRATCH_REG0 = 0x0578;
    constexpr uint32_t REG_COHER_STATUS_HOST = 0x0A31;
    constexpr uint32_t REG_D1MODE_VBLANK_STATUS = 0x1951;
    constexpr uint32_t REG_D1MODE_VIEWPORT_SIZE = 0x1961;

    uint32_t g_ringBase = 0, g_ringDwords = 0, g_rptrWriteBack = 0;
    uint32_t g_rptr = 0, g_wrapBuf = 0;
    uint32_t g_interruptCallback = 0, g_interruptData = 0;
    std::vector<uint32_t> g_regs(0x6000);
    uint64_t g_binMask = 0xFFFFFFFFull, g_binSelect = 0xFFFFFFFFull;
    std::atomic<uint32_t> g_vblankCounter{0};
    std::atomic<uint64_t> g_gpuIdleNs{0};   // time the command processor waited for work
    std::atomic<uint64_t> g_waitNs{0};      // time spent in WAIT_REG_MEM
    std::mutex g_waitStatsLock;
    std::map<uint64_t, std::pair<uint64_t, uint64_t>> g_waitStats;   // what was waited on: ns, count
    std::atomic<uint64_t> g_swaps{0}, g_draws{0}, g_packets{0};
    std::atomic<bool> g_started{false};
    std::atomic<uint64_t> g_drawNs{0}, g_presentNs{0};
    std::atomic<uint64_t> g_opCounts[128];   // time spent in the renderer
    std::atomic<uint32_t> g_frontBuffer{0};

    uint32_t Phys(uint32_t a) { return a < 0x20000000 ? a + 0xA0000000 : a; }   // physical -> our mapping

    uint32_t GpuSwap(uint32_t v, uint32_t endian)
    {
        switch (endian & 3)
        {
        case 1: return ((v & 0x00FF00FF) << 8) | ((v & 0xFF00FF00) >> 8);  // 8-in-16
        case 2: return __builtin_bswap32(v);                               // 8-in-32
        case 3: return (v << 16) | (v >> 16);                              // 16-in-32
        default: return v;
        }
    }
    // the low two address bits select the byte swap, as on the real GPU
    void GpuStore(uint32_t address, uint32_t value)
    {
        uint32_t v = GpuSwap(value, address & 3);
        memcpy(G2H(Phys(address & ~3u)), &v, 4);
    }
    uint32_t GpuLoad(uint32_t address)
    {
        uint32_t v; memcpy(&v, G2H(Phys(address & ~3u)), 4);
        return GpuSwap(v, address & 3);
    }

    // Xenia's MatchValueAndRef: 0 never, 1 <, 2 <=, 3 ==, 4 !=, 5 >=, 6 >, 7 always
    bool Match(uint32_t value, uint32_t ref, uint32_t info)
    {
        uint32_t bits = ((value < ref) << 1) | ((value <= ref) << 2) | ((value == ref) << 3) |
                        ((value != ref) << 4) | ((value >= ref) << 5) | ((value > ref) << 6) | (1u << 7);
        return (bits >> (info & 7)) & 1;
    }

    // ---- interrupts (serialized; one guest context acts as the DPC)
    std::mutex g_interruptLock;
    PPCContext* g_interruptCtx = nullptr;
    GuestThread* g_interruptThread = nullptr;

    void Interrupt(uint32_t source, uint32_t cpu)
    {
        if (!g_interruptCallback) return;
        std::lock_guard lock(g_interruptLock);
        if (!g_interruptCtx)
        {
            g_interruptThread = CreateGuestThreadState(0x10000);
            uint32_t stack = AllocVirtual(0x40000, 0x1000);
            g_interruptCtx = new PPCContext();
            InitContext(*g_interruptCtx, g_interruptThread, stack + 0x40000);
        }
        t_thread = g_interruptThread;
        W8(g_interruptThread->pcr + 0x10C, uint8_t(cpu));   // processor number seen by the handler
        g_interruptCtx->r3.u64 = source;
        g_interruptCtx->r4.u64 = g_interruptData;
        GuestCall(*g_interruptCtx, g_interruptCallback);
    }

    void MakeCoherent() { g_regs[REG_COHER_STATUS_HOST] = 0; }   // caches are always coherent here

    FILE* g_capRegLog = nullptr;     // capture: log of fetch-constant writes
    uint32_t g_curOp = 0;
    void WriteRegister(uint32_t index, uint32_t value)
    {
        if (index >= g_regs.size()) return;
        if (g_capRegLog && index >= 0x4800 && index < 0x4810) fprintf(g_capRegLog, "  op %02X reg %04X = %08X\n", g_curOp, index, value);
        g_regs[index] = value;
        if (index >= REG_SCRATCH_REG0 && index < REG_SCRATCH_REG0 + 8)
        {
            uint32_t i = index - REG_SCRATCH_REG0;
            if ((1u << i) & g_regs[REG_SCRATCH_UMSK])
                W32(Phys(g_regs[REG_SCRATCH_ADDR] + i * 4), value);    // stored big-endian
        }
        else if (index == REG_COHER_STATUS_HOST)
        {
            g_regs[index] |= 0x80000000u;                              // pending until a wait polls it
        }
    }

    void Execute(uint32_t addr, uint32_t dwords, int depth);

    // ---- shader programs currently loaded (0 = vertex, 1 = pixel), copied out of memory
    std::vector<uint32_t> g_shader[2];
    uint64_t g_shaderHash[2] = {};

    // ---- renderer (Windows): created on the GPU thread once the game window exists
#ifdef _WIN32
    bool g_renderReady = false, g_renderFailed = false;
    uint8_t* PhysToHostImpl(uint32_t a) { return G2H(0xA0000000u + (a & 0x1FFFFFFFu)); }
    void RenderLog(const char* t) { Log("%s", t); }
    bool RenderAvailable()
    {
        if (g_renderReady || g_renderFailed) return g_renderReady;
        void* wnd = WindowHandle();
        if (!wnd) return false;
        render::PhysToHost = PhysToHostImpl;
        render::LogFn = RenderLog;
        g_renderReady = render::Init(wnd, 1280, 720);
        g_renderFailed = !g_renderReady;
        g_rendererActive = g_renderReady;
        return g_renderReady;
    }
#endif
    void LoadShader(uint32_t type, uint32_t guestAddr, uint32_t dwords)
    {
        if (type > 1) return;
        auto& v = g_shader[type];
        v.resize(dwords);
        for (uint32_t i = 0; i < dwords; i++) v[i] = R32(guestAddr + i * 4);
        uint64_t h = 1469598103934665603ull;
        for (uint32_t x : v) { h ^= x; h *= 1099511628211ull; }
        g_shaderHash[type] = h;
    }
    uint64_t Hash(const std::vector<uint32_t>& v)
    {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t x : v) { h ^= x; h *= 1099511628211ull; }
        return h;
    }

    // ---- frame capture (NBASH_CAPTURE=first[,count]): registers per draw, shaders, memory
    struct Capture
    {
        uint64_t first = ~0ull, count = 1;
        FILE* draws = nullptr;
        std::string dir;
        uint32_t drawIndex = 0;
        bool active = false;
    } g_cap;

    void CaptureBeginFrame(uint64_t frame)
    {
        if (frame < g_cap.first || frame >= g_cap.first + g_cap.count) { g_cap.active = false; return; }
        g_cap.active = true;
        g_cap.drawIndex = 0;
        g_cap.dir = "capture/frame" + std::to_string(frame);
        std::error_code ec;
        std::filesystem::create_directories(g_cap.dir + "/shaders", ec);
        if (ec) Log("[capture] could not create %s: %s\n", g_cap.dir.c_str(), ec.message().c_str());
        g_cap.draws = fopen((g_cap.dir + "/draws.txt").c_str(), "w");
        g_capRegLog = fopen((g_cap.dir + "/fetchwrites.txt").c_str(), "w");
        Log("[capture] recording frame %llu into %s\n", (unsigned long long)frame, g_cap.dir.c_str());
    }
    void SaveShader(const std::vector<uint32_t>& v, uint64_t h)
    {
        std::string f = g_cap.dir + "/shaders/" + std::to_string(h) + ".bin";
        if (std::filesystem::exists(f)) return;
        FILE* o = fopen(f.c_str(), "wb");
        fwrite(v.data(), 4, v.size(), o);                // host-endian dwords
        fclose(o);
    }
    void CaptureDraw(uint32_t op, uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize)
    {
        if (!g_cap.active) return;
        uint64_t vs = Hash(g_shader[0]), ps = Hash(g_shader[1]);
        SaveShader(g_shader[0], vs);
        SaveShader(g_shader[1], ps);
        char name[64];
        snprintf(name, sizeof(name), "/regs%04u.bin", g_cap.drawIndex);
        FILE* o = fopen((g_cap.dir + name).c_str(), "wb");
        fwrite(g_regs.data(), 4, g_regs.size(), o);
        fclose(o);
        if (g_capRegLog) fprintf(g_capRegLog, "DRAW %u vs=%llu\n", g_cap.drawIndex, (unsigned long long)vs);
        fprintf(g_cap.draws, "%u op=%02X init=%08X dma=%08X size=%08X vs=%llu ps=%llu\n", g_cap.drawIndex, op,
            initiator, dmaBase, dmaSize, (unsigned long long)vs, (unsigned long long)ps);
        // Per-draw snapshots of the memory this draw reads (index buffer + every vertex buffer):
        // the game rewrites these dynamic buffers during the frame, so the end-of-frame physmem
        // alone replays garbage. mem%04u.txt lists "addr bytes hash"; blobs/<hash>.bin holds data.
        snprintf(name, sizeof(name), "/mem%04u.txt", g_cap.drawIndex);
        if (FILE* ml = fopen((g_cap.dir + name).c_str(), "w"))
        {
            std::filesystem::create_directories(g_cap.dir + "/blobs");
            auto snap = [&](uint32_t addr, uint32_t bytes) {
                addr &= 0x1FFFFFFF;
                if (!bytes || bytes > (8u << 20) || uint64_t(addr) + bytes > 0x20000000ull) return;
                const uint8_t* p = G2H(0xA0000000u + addr);
                uint64_t h = 1469598103934665603ull;
                for (uint32_t i = 0; i < bytes; i++) { h ^= p[i]; h *= 1099511628211ull; }
                char bn[64]; snprintf(bn, sizeof bn, "/blobs/%016llx.bin", (unsigned long long)h);
                if (!std::filesystem::exists(g_cap.dir + bn))
                    if (FILE* bf = fopen((g_cap.dir + bn).c_str(), "wb")) { fwrite(p, 1, bytes, bf); fclose(bf); }
                fprintf(ml, "%08X %u %016llx\n", addr, bytes, (unsigned long long)h);
            };
            uint32_t count = initiator >> 16;
            if (dmaBase && ((initiator >> 6) & 3) == 0) snap(dmaBase, count * (((initiator >> 11) & 1) ? 4 : 2));
            for (uint32_t fc = 0; fc < 96; fc++)
            {
                uint32_t d0 = g_regs[0x4800 + fc * 2], d1 = g_regs[0x4800 + fc * 2 + 1];
                if ((d0 & 3) != 3) continue;
                snap(d0 & ~3u, ((d1 >> 2) & 0xFFFFFF) * 4);
            }
            // small textures too (the game rewrites some every frame: fades, colour tables)
            for (uint32_t tf = 0; tf < 32; tf++)
            {
                const uint32_t* t = &g_regs[0x4800 + tf * 6];
                if ((t[0] & 3) != 2) continue;
                uint32_t base = t[1] & 0xFFFFF000, dim = (t[5] >> 9) & 3;
                uint64_t bytes;
                if (dim == 2) bytes = uint64_t((t[2] & 0x7FF) + 1) * (((t[2] >> 11) & 0x7FF) + 1) * ((t[2] >> 22) + 1) * 16;
                else bytes = uint64_t((t[2] & 0x1FFF) + 1) * (((t[2] >> 13) & 0x1FFF) + 1) * 16;
                if (base && bytes <= (512u << 10)) snap(base, uint32_t(bytes));
            }
            fclose(ml);
        }
        g_cap.drawIndex++;
    }
    void CaptureEndFrame()
    {
        if (!g_cap.active) return;
        fclose(g_cap.draws);
        if (g_capRegLog) { fclose(g_capRegLog); g_capRegLog = nullptr; }
        // physical memory, non-zero 4 KB pages only: [u32 page index][4096 bytes]...
        FILE* o = fopen((g_cap.dir + "/physmem.bin").c_str(), "wb");
        static const uint8_t zero[4096] = {};
        for (uint32_t p = 0; p < 0x20000000 / 4096; p++)
        {
            const uint8_t* src = G2H(0xA0000000 + p * 4096);
            if (!memcmp(src, zero, 4096)) continue;
            fwrite(&p, 4, 1, o);
            fwrite(src, 1, 4096, o);
        }
        fclose(o);
        // everything except physmem.bin also goes into one bundle.bin ([u32 name length][name]
        // [u32 size][data]...), so the capture can be copied as two files instead of hundreds
        if (FILE* b = fopen((g_cap.dir + "/bundle.bin").c_str(), "wb"))
        {
            std::error_code ec;
            for (auto& e : std::filesystem::recursive_directory_iterator(g_cap.dir, ec))
            {
                if (!e.is_regular_file()) continue;
                std::string rel = std::filesystem::relative(e.path(), g_cap.dir).generic_string();
                if (rel == "physmem.bin" || rel == "bundle.bin") continue;
                FILE* f = fopen(e.path().string().c_str(), "rb");
                if (!f) continue;
                std::vector<char> data(size_t(e.file_size()));
                size_t got = fread(data.data(), 1, data.size(), f);
                fclose(f);
                uint32_t n = uint32_t(rel.size()), sz = uint32_t(got);
                fwrite(&n, 4, 1, b); fwrite(rel.data(), 1, n, b);
                fwrite(&sz, 4, 1, b); fwrite(data.data(), 1, sz, b);
            }
            fclose(b);
        }
        Log("[capture] %u draws saved\n", g_cap.drawIndex);
        g_cap.active = false;
    }

    uint32_t Type3(uint32_t addr, uint32_t hdr, int depth)
    {
        uint32_t op = (hdr >> 8) & 0x7F, count = ((hdr >> 16) & 0x3FFF) + 1;
        g_curOp = op;
        g_opCounts[op & 0x7F]++;
        uint32_t d = addr + 4;
        auto D = [&](uint32_t i) { return R32(d + i * 4); };

        // predicated packets only run when the selected bin is enabled
        if ((hdr & 1) && !(g_binSelect & g_binMask))
        {
#ifdef _WIN32
            // collapsed tiled rendering: the first piece draws the whole frame, so it also runs
            // the DRAWS predicated for the other pieces. Other predicated packets are those
            // pieces' own setup (window offset, scissors, copy destination...) and stay skipped.
            // (resolves are draws too, in EDRAM copy mode: those belong to their own piece)
            if ((op == 0x22 || op == 0x36 || op == 0x34 || op == 0x35) && (g_regs[0x2208] & 7) != 6 &&
                RenderAvailable() && render::TileCollapseQuery(g_regs.data()) == 1)
                return Type3(addr, hdr & ~1u, depth);
#endif
            return 1 + count;
        }

        switch (op)
        {
        case 0x3F: case 0x37:                                       // INDIRECT_BUFFER(_PFD)
            if (depth < 8) Execute(Phys(D(0)), D(1) & 0xFFFFF, depth + 1);
            break;
        case 0x3C:                                                  // WAIT_REG_MEM
        {
            uint32_t info = D(0), poll = D(1), ref = D(2), mask = D(3);
            auto start = std::chrono::steady_clock::now();
            for (int spins = 0;; spins++)
            {
                uint32_t value;
                if (info & 0x10) value = GpuLoad(poll);
                else
                {
                    if ((poll & 0x7FFF) == REG_COHER_STATUS_HOST) MakeCoherent();
                    value = g_regs[poll & 0x5FFF];
                }
                if (Match(value & mask, ref, info)) break;
                // poll tightly (yield) for 2 ms -- a Windows sleep lasts ~1 ms or more, far longer
                // than most of these waits -- then back off
                if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(2)) std::this_thread::sleep_for(std::chrono::microseconds(100));
                else std::this_thread::yield();
                if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5))
                {
                    static std::atomic<int> warned{0};
                    if (warned++ < 10) Log("[gpu] WAIT_REG_MEM gave up: %s %08X & %08X func %u ref %08X (value %08X)\n",
                        (info & 0x10) ? "memory" : "register", poll, mask, info & 7, ref, value);
                    break;
                }
            }
            {
                uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
                g_waitNs += ns;
                std::lock_guard<std::mutex> lk(g_waitStatsLock);
                auto& w = g_waitStats[((uint64_t)(info & 0x10) << 32) | poll];
                w.first += ns; w.second++;
            }
            break;
        }
        case 0x21:                                                  // REG_RMW
        {
            uint32_t info = D(0), andm = D(1), orm = D(2), r = info & 0x1FFF;
            uint32_t v = g_regs[r];
            v &= (info >> 31) ? g_regs[andm & 0x1FFF] : andm;
            v |= ((info >> 30) & 1) ? g_regs[orm & 0x1FFF] : orm;
            WriteRegister(r, v);
            break;
        }
        case 0x3E:                                                  // REG_TO_MEM
            GpuStore(D(1), g_regs[D(0) & 0x5FFF]);
            break;
        case 0x3D:                                                  // MEM_WRITE
        {
            uint32_t a = D(0);
            for (uint32_t i = 1; i < count; i++, a += 4) GpuStore(a, D(i));
            break;
        }
        case 0x45:                                                  // COND_WRITE
        {
            uint32_t info = D(0), poll = D(1), ref = D(2), mask = D(3), wa = D(4), wd = D(5);
            uint32_t value = (info & 0x10) ? GpuLoad(poll) : g_regs[poll & 0x5FFF];
            if (Match(value & mask, ref, info))
            {
                if (info & 0x100) GpuStore(wa, wd);
                else WriteRegister(wa & 0x5FFF, wd);
            }
            break;
        }
        case 0x58:                                                  // EVENT_WRITE_SHD: initiator, address, value
        {
            uint32_t init = D(0), a = D(1), v = D(2);
            GpuStore(a, (init >> 31) ? g_vblankCounter.load() : v);
            break;
        }
        case 0x5A:                                                  // EVENT_WRITE_EXT: screen extents (8-in-16)
        {
            uint32_t a = Phys(D(1) & ~3u);
            const uint16_t ext[6] = { 0, 8192 >> 3, 0, 8192 >> 3, 0, 1 };
            for (int i = 0; i < 6; i++) W16(a + i * 2, ext[i]);
            break;
        }
        case 0x54:                                                  // INTERRUPT
        {
            uint32_t mask = D(0);
            for (uint32_t n = 0; n < 6; n++)
                if (mask & (1u << n)) Interrupt(1, n);
            break;
        }
        case 0x60: g_binMask   = (g_binMask   & 0xFFFFFFFF00000000ull) | D(0); break;
        case 0x61: g_binMask   = (g_binMask   & 0xFFFFFFFFull) | (uint64_t(D(0)) << 32); break;
        case 0x62: g_binSelect = (g_binSelect & 0xFFFFFFFF00000000ull) | D(0); break;
        case 0x63: g_binSelect = (g_binSelect & 0xFFFFFFFFull) | (uint64_t(D(0)) << 32); break;
        case 0x50: g_binMask   = (uint64_t(D(0)) << 32) | D(1); break;
        case 0x51: g_binSelect = (uint64_t(D(0)) << 32) | D(1); break;
        case 0x64:                                                  // our VdSwap marker
            g_frontBuffer = D(1);
            {
                // the F12 screenshot shows the captured frame itself (not the one before it)
                bool captured = g_cap.active;
                CaptureEndFrame();
#ifdef _WIN32
                if (captured) render::g_screenshotRequest = true;
#endif
            }
#ifdef _WIN32
            if (g_renderReady)
            {
                auto t0 = std::chrono::steady_clock::now();
                render::Present(g_frontBuffer);
                g_presentNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
            }
#endif
            g_swaps++;
            if (g_captureRequest.exchange(false)) { g_cap.first = g_swaps; g_cap.count = 1; }
            CaptureBeginFrame(g_swaps);
            break;
        case 0x2D:                                                  // SET_CONSTANT
        {
            static const uint32_t bases[5] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
            uint32_t ot = D(0), index = ot & 0x7FF, type = (ot >> 16) & 0xFF;
            if (type < 5)
                for (uint32_t i = 1; i < count; i++) WriteRegister(bases[type] + index + i - 1, D(i));
            break;
        }
        case 0x55: case 0x56:                                       // SET_CONSTANT2, SET_SHADER_CONSTANTS
        {
            uint32_t index = D(0) & 0xFFFF;
            for (uint32_t i = 1; i < count; i++) WriteRegister(index + i - 1, D(i));
            break;
        }
        case 0x2F:                                                  // LOAD_ALU_CONSTANT (from memory)
        {
            static const uint32_t bases[5] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
            uint32_t a = Phys(D(0) & 0x1FFFFFFF), ot = D(1), index = ot & 0x7FF, type = (ot >> 16) & 0xFF;
            uint32_t n = D(2) & 0xFFF;
            if (type < 5)
                for (uint32_t i = 0; i < n; i++) WriteRegister(bases[type] + index + i, R32(a + i * 4));
            break;
        }
        case 0x27:                                                  // IM_LOAD: shader from memory
            LoadShader(D(0) & 3, Phys((D(0) & ~3u) & 0x1FFFFFFF), D(1) & 0xFFFF);
            break;
        case 0x2B:                                                  // IM_LOAD_IMMEDIATE: shader in packet
            LoadShader(D(0), d + 8, D(1) & 0xFFFF);
            break;
        case 0x22: case 0x36: case 0x34: case 0x35:                 // draws
        {
            // DRAW_INDX: viz token, initiator, [dma base, dma size]; DRAW_INDX_2: initiator, indices...
            bool hasViz = op == 0x22 || op == 0x34;
            uint32_t init = D(hasViz ? 1 : 0);
            uint32_t dmaBase = 0, dmaSize = 0;
            if ((init & 0x3F) != 0 && ((init >> 6) & 3) == 0 && count >= uint32_t(hasViz ? 4 : 3))
            {
                dmaBase = D(hasViz ? 2 : 1);
                dmaSize = D(hasViz ? 3 : 2);
            }
            CaptureDraw(op, init, dmaBase, dmaSize);
#ifdef _WIN32
            if (RenderAvailable())
            {
                render::DrawCall dc{ g_regs.data(), &g_shader[0], &g_shader[1], g_shaderHash[0], g_shaderHash[1], init, dmaBase, dmaSize };
                auto t0 = std::chrono::steady_clock::now();
                render::Draw(dc);
                g_drawNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
            }
#endif
            g_draws++;
            break;
        }
        default:
            break;                                                  // state/constants/shaders: for the renderer later
        }
        return 1 + count;
    }

    uint32_t Packet(uint32_t addr, int depth)
    {
        uint32_t hdr = R32(addr);
        g_packets++;
        switch (hdr >> 30)
        {
        case 0:
        {
            g_curOp = 0x100;
            uint32_t count = ((hdr >> 16) & 0x3FFF) + 1, base = hdr & 0x7FFF;
            bool one = (hdr >> 15) & 1;
            for (uint32_t i = 0; i < count; i++) WriteRegister(one ? base : base + i, R32(addr + 4 + i * 4));
            return 1 + count;
        }
        case 1:
            WriteRegister(hdr & 0x7FF, R32(addr + 4));
            WriteRegister((hdr >> 11) & 0x7FF, R32(addr + 8));
            return 3;
        case 2:
            return 1;
        default:
            return Type3(addr, hdr, depth);
        }
    }

    void Execute(uint32_t addr, uint32_t dwords, int depth)
    {
        uint32_t i = 0;
        while (i < dwords) i += Packet(addr + i * 4, depth);
    }

    void GpuThread()
    {
        auto idleSince = std::chrono::steady_clock::now();
        for (;;)
        {
            if (g_ringBase && g_ringDwords)
            {
                uint32_t wptr = R32(kRegisterBase + REG_CP_RB_WPTR * 4) % g_ringDwords;
                uint32_t avail = (wptr + g_ringDwords - g_rptr) % g_ringDwords;
                while (avail)
                {
                    uint32_t addr = g_ringBase + g_rptr * 4, hdr = R32(addr);
                    uint32_t size = (hdr >> 30) == 1 ? 3 : (hdr >> 30) == 2 ? 1 : ((hdr >> 16) & 0x3FFF) + 2;
                    if (size > avail) break;                      // rest of the packet not written yet
                    if (g_rptr + size > g_ringDwords)             // packet wraps around the ring end
                    {
                        if (!g_wrapBuf) g_wrapBuf = AllocSystem(0x10000 * 4, 0x100);
                        for (uint32_t i = 0; i < size; i++)
                            W32(g_wrapBuf + i * 4, R32(g_ringBase + ((g_rptr + i) % g_ringDwords) * 4));
                        addr = g_wrapBuf;
                    }
                    Packet(addr, 0);
                    g_rptr = (g_rptr + size) % g_ringDwords;
                    avail -= size;
                    // report progress as we go, so the game sees ring space free up promptly
                    W32(kRegisterBase + REG_CP_RB_RPTR * 4, g_rptr);
                    if (g_rptrWriteBack) W32(g_rptrWriteBack, g_rptr);
                    idleSince = std::chrono::steady_clock::now();
                }
                W32(kRegisterBase + REG_CP_RB_RPTR * 4, g_rptr);
                if (g_rptrWriteBack) W32(g_rptrWriteBack, g_rptr);
            }
            // Idle: keep polling (yielding) for 4 ms after the last packet -- the game often kicks
            // the ring again within microseconds and may be waiting on our progress -- then sleep.
            auto now = std::chrono::steady_clock::now();
            if (now - idleSince < std::chrono::milliseconds(4)) std::this_thread::yield();
            else std::this_thread::sleep_for(std::chrono::microseconds(250));
            g_gpuIdleNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - now).count();
        }
    }

    // 60 Hz display refresh (Xenia's MarkVblank): counter + vblank interrupt on CPU 2.
    // 13 Nov (smooth pacing): ONE clock for everything. The tick follows the real monitor's vblank when it can (render::WaitDisplayVblank), otherwise a
    // precise 60 Hz timer (sleep to 2 ms before, then spin - a plain sleep_until was up to 1-2 ms late). The game's main loop now waits for this tick
    // (loopstats.cpp) instead of running on a second, unrelated 60 Hz timer: the two timers drifted against each other, so frames alternately caught and
    // missed the vblank the game's own GPU wait is tied to (measured: frame times 9 .. 26 ms at "60 fps"). NBASH_OLDPACE=1 = the old behaviour.
    std::mutex g_vbMutex; std::condition_variable g_vbCv;
    std::atomic<int> g_vbReal{0}; std::atomic<int> g_vbHz100{0};
    void VblankThread()
    {
        static const bool old = getenv("NBASH_OLDPACE") != nullptr;
        auto next = std::chrono::steady_clock::now();
        for (;;)
        {
            bool real = false; int fps = old ? 60 : NbashFps();
#ifdef _WIN32
            if (!old) { double hz = 0; real = render::WaitDisplayVblank(&hz, fps) == 1; g_vbHz100 = int(hz * 100); }
#endif
            g_vbReal = real;
            if (real) next = std::chrono::steady_clock::now();
            else
            {
                next += std::chrono::nanoseconds(1000000000ll / fps);
                auto now = std::chrono::steady_clock::now();
                if (next < now - std::chrono::milliseconds(50)) next = now;
                if (old) std::this_thread::sleep_until(next);
                else
                {
                    if (next - now > std::chrono::milliseconds(3)) std::this_thread::sleep_until(next - std::chrono::milliseconds(2));
                    while (std::chrono::steady_clock::now() < next) std::this_thread::yield();
                }
            }
            { std::lock_guard<std::mutex> lk(g_vbMutex); g_vblankCounter++; }
            g_vbCv.notify_all();
            W32(kRegisterBase + REG_D1MODE_VBLANK_STATUS * 4, 1);
            Interrupt(0, 2);
        }
    }

    void StatusThread()
    {
        uint64_t lastSwaps = 0;
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            uint64_t s = g_swaps;
#ifndef _WIN32
            static bool dumped = false;
            if (s == lastSwaps && !dumped && getenv("NBASH_STALLDUMP")) { dumped = true; PrintThreadStatus(); ::DumpGuestMemory(); }
#endif
            Log("[status] %.1f fps, %llu frames, %llu draw calls, %llu packets, render %.1f s draw + %.1f s present, GPU thread idle %.1f s\n", (s - lastSwaps) / 10.0,
                (unsigned long long)s, (unsigned long long)g_draws.load(), (unsigned long long)g_packets.load(),
                g_drawNs.exchange(0) / 1e9, g_presentNs.exchange(0) / 1e9, g_gpuIdleNs.exchange(0) / 1e9);
            Log("[status] GPU waits (WAIT_REG_MEM) %.2f s:", g_waitNs.exchange(0) / 1e9);
            {
                std::lock_guard<std::mutex> lk(g_waitStatsLock);
                for (auto& [k, w] : g_waitStats)
                    if (w.first > 50000000) Log(" %s %08X %.2f s/%llu", (k >> 32) ? "mem" : "reg", uint32_t(k), w.first / 1e9, (unsigned long long)w.second);
                g_waitStats.clear();
            }
            Log("\n");
            lastSwaps = s;

            Log("[status] audio frames submitted: %llu (with sound: %llu)\n", (unsigned long long)AudioFramesSubmitted(), (unsigned long long)AudioFramesWithSound());
            if (getenv("NBASH_OPCOUNTS"))
            {
                std::string line;
                for (int i = 0; i < 128; i++) if (uint64_t c = g_opCounts[i].exchange(0)) { char b[32]; snprintf(b, sizeof b, " %02X:%llu", i, (unsigned long long)c); line += b; }
                Log("[status] packet types:%s\n", line.c_str());
            }
            static int ticks = 0;
            if (g_verbose || ++ticks % 3 == 0) PrintThreadStatus();   // every 30 s: shows where a stuck game waits
        }
    }

    void StartGpu()
    {
        bool expected = false;
        if (!g_started.compare_exchange_strong(expected, true)) return;
        // capture request: NBASH_CAPTURE=frame[,count] or a file nbash_capture.txt with the same text
        static char capText[64] = {};
        const char* c = getenv("NBASH_CAPTURE");
        if (!c)
            if (FILE* cf = fopen("nbash_capture.txt", "r")) { if (fgets(capText, sizeof capText, cf)) c = capText; fclose(cf); }
        {
            char cwd[512] = {};
#ifdef _WIN32
            _getcwd(cwd, sizeof cwd);
#endif
            Log("[capture] request: %s (folder %s)\n", c ? c : "none", cwd);
        }
        if (c)
        {
            g_cap.first = strtoull(c, nullptr, 10);
            if (const char* comma = strchr(c, ',')) g_cap.count = strtoull(comma + 1, nullptr, 10);
        }
        W32(kRegisterBase + 0x0F00 * 4, 0x08100748);                  // RB_EDRAM_TIMING
        W32(kRegisterBase + 0x0F01 * 4, 0x0000200E);                  // RB_BC_CONTROL
        W32(kRegisterBase + REG_D1MODE_VBLANK_STATUS * 4, 1);
        W32(kRegisterBase + REG_D1MODE_VIEWPORT_SIZE * 4, 0x050002D0); // 1280x720
        std::thread(GpuThread).detach();
        std::thread(VblankThread).detach();
        std::thread(StatusThread).detach();
    }
}

uint64_t GpuFrameCount() { return g_swaps; }
uint32_t VblankCount() { return g_vblankCounter; }
// waits (at most 50 ms) until the vblank counter is no longer `last`; returns the new count
uint32_t NbashWaitVblank(uint32_t last)
{
    std::unique_lock<std::mutex> lk(g_vbMutex);
    g_vbCv.wait_for(lk, std::chrono::milliseconds(50), [&] { return g_vblankCounter.load() != last; });
    return g_vblankCounter.load();
}
int NbashVblankSource(int* hz100) { if (hz100) *hz100 = g_vbHz100; return g_vbReal; }
uint64_t GpuDrawCount() { return g_draws; }
uint32_t GpuFrontBuffer() { return g_frontBuffer; }

IMPORT(VdInitializeEngines) { TRACE_IMPORT(); RETURN(1); }
IMPORT(VdSetGraphicsInterruptCallback) { TRACE_IMPORT(); g_interruptCallback = ARG(3).u32; g_interruptData = ARG(4).u32; StartGpu(); RETURN(0); }
IMPORT(VdInitializeRingBuffer)
{
    TRACE_IMPORT();
    g_ringBase = Phys(ARG(3).u32);
    g_ringDwords = (1u << (ARG(4).u32 + 3)) / 4;
    g_rptr = 0;
    Log("[gpu] ring buffer at %08X, %u dwords\n", g_ringBase, g_ringDwords);
    StartGpu();
    RETURN(0);
}
IMPORT(VdEnableRingBufferRPtrWriteBack) { TRACE_IMPORT(); g_rptrWriteBack = Phys(ARG(3).u32); RETURN(0); }
IMPORT(VdGetSystemCommandBuffer) { TRACE_IMPORT(); W32(ARG(3).u32, 0xBEEF0000); W32(ARG(4).u32, 0xBEEF0001); RETURN(0); }
IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress) { TRACE_IMPORT(); RETURN(0); }
IMPORT(VdSwap)
{
    TRACE_IMPORT();
    // Frame limiter: the console shows at most one new frame per 60 Hz refresh. A frame that is
    // already late is never held back (no rounding up to the next refresh), and after a long
    // stall the schedule restarts instead of racing to catch up. NBASH_NOFRAMELIMIT=1 disables.
    {
        static const bool off = getenv("NBASH_FRAMELIMIT") == nullptr;   // off by default: the main loop is paced in input.cpp
        static auto next = std::chrono::steady_clock::now();
        constexpr auto period = std::chrono::nanoseconds(16666667);
        if (!off)
        {
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
    // D3D leaves 64 dwords at buffer_ptr for the kernel's swap packets. We put one marker
    // packet there (opcode 0x64, as Xenia does) carrying the front buffer address.
    uint32_t buf = ARG(3).u32, frontBufferPtr = ARG(8).u32;
    W32(buf, (3u << 30) | (62u << 16) | (0x64u << 8));
    W32(buf + 4, 0x53574150);   // 'SWAP'
    W32(buf + 8, frontBufferPtr ? R32(frontBufferPtr) : 0);
    for (uint32_t i = 3; i < 64; i++) W32(buf + i * 4, 0);
    RETURN(0);
}
