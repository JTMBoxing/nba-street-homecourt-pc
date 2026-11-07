// Renderer interface used by the GPU command processor (gpu.cpp) and the capture replay tool.
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

namespace render
{
    struct DrawCall
    {
        const uint32_t* regs;                 // full Xenos register file (0x6000 dwords)
        const std::vector<uint32_t>* vs;      // vertex shader microcode (host-endian dwords)
        const std::vector<uint32_t>* ps;      // pixel shader microcode
        uint64_t vsHash, psHash;
        uint32_t initiator;                   // VGT_DRAW_INITIATOR
        uint32_t dmaBase, dmaSize;            // index buffer (indexed draws)
    };

    // window may be null (offscreen, used by the replay tool)
    bool Init(void* window, uint32_t width, uint32_t height);
    void Draw(const DrawCall& draw);
    void Present(uint32_t frontBuffer = 0);   // frontBuffer: physical address the game presents (VdSwap)
    // reads the current render target as RGBA8 (for tests)
    extern std::atomic<bool> g_screenshotRequest;
    extern std::atomic<int> g_colorPreset;     // F8 cycles colour grading: 0 original, 1 natural, 2 vivid
    extern std::atomic<bool> g_tileCollapse;   // F9 toggles drawing tiled frames in one pass   // set by F12: save the next presented frame
    bool ReadBack(std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height, bool liveTarget = false);
    // physical address (0x00000000-0x1FFFFFFF) -> host pointer
    extern uint8_t* (*PhysToHost)(uint32_t physical);
    // true if [addr, addr+bytes) overlaps memory written by GPU memory export (replay uses this
    // to keep captured snapshots from overwriting freshly exported vertex data)
    bool ExportedOverlap(uint32_t addr, uint32_t bytes);
    extern void (*LogFn)(const char* text);
    // Tiled rendering collapse: a surface drawn in screen pieces (window offsets) last frame is
    // drawn once at full size: 0 = normal, 1 = first piece (draw everything, full scissor),
    // 2 = a later piece (skip). The command processor uses it to run predicated draws in piece 1.
    int TileCollapseQuery(const uint32_t* regs);
    void EndFrame();
    void DumpTargets(const char* prefix);   // saves the copy targets as <prefix>_<addr>.bmp   // called by Present; the replay tool calls it between passes
}
