#include <algorithm>
#include <cmath>
#include <chrono>
// Direct3D 11 renderer for Xenos draws.
//
// First version: everything renders into one host render target the size of the screen
// (EDRAM surfaces and resolves to textures come later). Per draw it translates/compiles the
// guest shaders (cached), uploads the vertex data the shader can reach, decodes textures
// (untiling + format conversion, cached by content), and maps the Xenos render state.
#ifdef _WIN32
#include "renderer.h"
#include "hlsl_translator.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/texture_address.h"
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_6.h>
#include <cwctype>
#include <map>
#include <set>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <string>
#include <unordered_map>
std::string GameRoot();      // main: the folder that holds data\ and mods\

using namespace xe::gpu;

void NbashStatsDraw(ID3D11Device*, ID3D11DeviceContext*);
ID3DBlob* NbashCompileShader(const char* src, const char* name, const char* profile, const char* entry);
namespace render
{
uint8_t* (*PhysToHost)(uint32_t physical) = nullptr;
void (*LogFn)(const char* text) = nullptr;

namespace
{
    template <typename T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

    void Logf(const char* f, ...)
    {
        char buf[2048];
        va_list ap;
        va_start(ap, f);
        vsnprintf(buf, sizeof(buf), f, ap);
        va_end(ap);
        if (LogFn) LogFn(buf); else fputs(buf, stdout);
    }

    using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    D3DCompileFn g_compile = nullptr;

    ID3D11Device* g_dev = nullptr;
    ID3D11DeviceContext* g_ctx = nullptr;
    IDXGISwapChain* g_swap = nullptr;
    ID3D11Texture2D* g_rt = nullptr;       // single-sample copy of the drawing surface (copies read this)
    ID3D11Texture2D* g_front = nullptr;    // last resolved frame (what gets presented)
    // 14 Nov (GPU optimisation): the picture shown is read straight from the game's front-buffer copy target - no full-frame copies into g_front
    // every frame any more (2 x 15 MB at 2560x1440 = ~3.5 GB/s of memory traffic, a lot for graphics that share the system memory).
    // g_front is only filled when the game's front buffer is NOT a copy target (then the last full-frame copy is shown, as before).
    ID3D11Texture2D* g_showTex = nullptr;          // what this frame shows (a copy target, or g_front)
    ID3D11ShaderResourceView* g_showSrv = nullptr; // its view (nullptr = g_front's)
    bool g_needFallback = true;                    // last Present did not find the front buffer -> keep g_front up to date
    ID3D11RenderTargetView* g_rtv = nullptr;   // the drawing surface (multisampled when g_msaa > 1)
    ID3D11Texture2D* g_ds = nullptr;
    ID3D11DepthStencilView* g_dsv = nullptr;
    // Anti-aliasing: the console draws matches with 4x MSAA. We draw into a multisampled surface
    // and average it into g_rt whenever something reads the picture (copies, readback).
    // NBASH_MSAA=1/2/4/8 picks the sample count (default 4).
    uint32_t g_msaa = 1;
    ID3D11Texture2D* g_rtMs = nullptr;
    bool g_rtDirty = false;                // drawn since the last average into g_rt
    void SyncRt()
    {
        if (g_msaa > 1 && g_rtDirty) g_ctx->ResolveSubresource(g_rt, 0, g_rtMs, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
        g_rtDirty = false;
    }
    // Copies of small areas (shadow maps, bloom steps: 128x128 .. 320x180) only need that area
    // averaged out of the anti-aliased target, not the whole 1920x1080 picture (21 copies a frame).
    // A tiny shader does it; big areas still use ResolveSubresource. NBASH_NOREGIONRESOLVE=1 = old way.
    ID3D11ShaderResourceView* g_rtMsSrv = nullptr;
    ID3D11RenderTargetView* g_rtResolveRtv = nullptr;
    void SyncRtRegion(const D3D11_BOX& b);
    uint32_t g_width = 1280, g_height = 720;   // the console's picture size (guest pixels)
    // Internal resolution: everything is drawn at g_scale x the console's size and then scaled
    // to the window with a good filter; NBASH_RES_SCALE=1..4, fractions allowed (1.5 = 1920x1080).
    // Guest coordinates are unchanged; only the viewport, scissors, copy rectangles and copy
    // targets are scaled.
    float g_scale = 1.0f;
    uint32_t g_rtW = 1280, g_rtH = 720;
    inline UINT Sc(int x) { return UINT(float(x) * g_scale + 0.5f); }   // console pixels -> drawn pixels
    HWND g_hwnd = nullptr;

    ID3D11Buffer* g_cbFloat = nullptr;     // b0
    ID3D11Buffer* g_cbSystem = nullptr;    // b1
    ID3D11Buffer* g_ib = nullptr;
    uint32_t g_ibSize = 0, g_ibOffset = 0;   // ring: append with NO_OVERWRITE, DISCARD when full
    ID3D11Texture2D* g_vmem = nullptr;     // vertex data, kVmemRowDwords per row
    ID3D11ShaderResourceView* g_vmemSrv = nullptr;
    // rows [0, kVmemCacheRows): vertex data kept between frames (models that do not change);
    // rows [kVmemCacheRows, kVmemRows): this frame's other vertex data, refilled every frame
    constexpr uint32_t kVmemRows = 4096;   // 16M dwords, 64 MB
    constexpr uint32_t kVmemCacheRows = 3072;
    uint32_t g_vmemNextRow = kVmemCacheRows;

    // Kept vertex data: the same guest range seen in two frames in a row gets a permanent place;
    // each later use only compares the guest memory with our copy (memcmp) and re-sends it if it
    // changed. Saves ~6 MB of uploads and ~700 driver calls a frame. NBASH_NOVCACHE=1 turns it off.
    struct VCacheEntry { int64_t offset = -1; uint64_t seenFrame = 0; std::vector<uint32_t> shadow; };
    std::unordered_map<uint64_t, VCacheEntry> g_vcache;
    uint32_t g_vcacheUsed = 0;              // dwords handed out in the kept area
    uint32_t g_vcacheHits = 0, g_vcacheUploads = 0, g_vcacheRing = 0;
    // writes n dwords at dword offset `at` of vmem (it may cross rows: up to 3 rectangles)
    void UploadVmem(uint32_t at, const uint32_t* src, uint32_t n);


    // ---------------------------------------------------------------- shaders
    struct CachedShader
    {
        std::unique_ptr<Shader> shader;
        ID3D11VertexShader* vs = nullptr;
        ID3D11VertexShader* vsRect = nullptr;     // rectangle-list entry point, compiled on first use
        ID3D11PixelShader* ps = nullptr;
        std::string hlsl;
        bool failed = false, rectFailed = false;
        ID3D11ComputeShader* exportCs = nullptr;  // memory-export vertex shaders run as compute
        bool exportFailed = false;
        uint32_t exportBlocks = 0;                 // "alloc export" count (records per vertex = 5x)
    };
    std::unordered_map<uint64_t, CachedShader> g_shaders[2];

    uint32_t g_shaderCompiles = 0;
    using D3DCreateBlobFn = HRESULT(WINAPI*)(SIZE_T, ID3DBlob**);
    D3DCreateBlobFn g_createBlob = nullptr;

    // Compiled shaders are kept in shadercache\ next to the program (key: hash of the HLSL,
    // entry point and profile), so each one is compiled only once, ever.
    ID3DBlob* Compile(const std::string& src, const char* name, const char* profile, const char* entry = "main")
    {
        // NBASH_SHADERFLAGS: "ieee" = strict IEEE float rules, "skip" = no optimisation (both: "ieee,skip")
        static const char* flagEnv = getenv("NBASH_SHADERFLAGS");
        static const UINT extraFlags = (flagEnv && strstr(flagEnv, "ieee") ? D3DCOMPILE_IEEE_STRICTNESS : 0) |
                                       (flagEnv && strstr(flagEnv, "skip") ? D3DCOMPILE_SKIP_OPTIMIZATION : 0);
        uint64_t key = 1469598103934665603ull;
        auto mix = [&](const char* p, size_t n) { for (size_t i = 0; i < n; i++) key = (key ^ uint8_t(p[i])) * 1099511628211ull; };
        mix(src.data(), src.size()); mix(entry, strlen(entry)); mix(profile, strlen(profile));
        if (extraFlags) mix((const char*)&extraFlags, sizeof extraFlags);
        char path[64];
        snprintf(path, sizeof path, "shadercache\\%016llX.bin", (unsigned long long)key);
        if (g_createBlob)
            if (FILE* f = fopen(path, "rb"))
            {
                fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
                ID3DBlob* b = nullptr;
                if (n > 0 && SUCCEEDED(g_createBlob(size_t(n), &b)) && fread(b->GetBufferPointer(), 1, size_t(n), f) == size_t(n)) { fclose(f); return b; }
                if (b) b->Release();
                fclose(f);
            }
        g_shaderCompiles++;
        ID3DBlob *code = nullptr, *err = nullptr;
        HRESULT hr = g_compile(src.data(), src.size(), name, nullptr, nullptr, entry, profile,
                               (extraFlags & D3DCOMPILE_SKIP_OPTIMIZATION ? 0 : D3DCOMPILE_OPTIMIZATION_LEVEL0) | extraFlags, 0, &code, &err);
        if (SUCCEEDED(hr) && code)
        {
            CreateDirectoryA("shadercache", nullptr);
            if (FILE* f = fopen(path, "wb")) { fwrite(code->GetBufferPointer(), 1, code->GetBufferSize(), f); fclose(f); }
        }
        if (FAILED(hr))
        {
            Logf("[render] shader %s failed to compile: %s\n", name, err ? (const char*)err->GetBufferPointer() : "?");
            SafeRelease(code);
        }
        SafeRelease(err);
        return code;
    }

    CachedShader* GetShader(bool pixel, uint64_t hash, const std::vector<uint32_t>& ucode)
    {
        auto& c = g_shaders[pixel][hash];
        if (c.shader || c.failed) return c.failed ? nullptr : &c;
        std::vector<uint32_t> be(ucode.size());
        for (size_t i = 0; i < ucode.size(); i++) be[i] = __builtin_bswap32(ucode[i]);
        c.shader = std::make_unique<Shader>(pixel ? xenos::ShaderType::kPixel : xenos::ShaderType::kVertex, hash, be.data(), be.size());
        xe::StringBuffer sb;
        c.shader->AnalyzeUcode(sb);
        HlslTranslator t;
        std::string errors;
        std::string hlsl = t.Translate(*c.shader, errors);
        char name[64];
        snprintf(name, sizeof(name), "%s_%016llX", pixel ? "ps" : "vs", (unsigned long long)hash);
        if (!errors.empty()) Logf("[render] %s translation notes: %s", name, errors.c_str());
        if (getenv("NBASH_DUMPHLSL")) { std::string fn = std::string("hlsl_") + name + ".txt"; if (FILE* f = fopen(fn.c_str(), "w")) { fputs(hlsl.c_str(), f); fclose(f); } }
        ID3DBlob* blob = hlsl.empty() ? nullptr : Compile(hlsl, name, pixel ? "ps_5_0" : "vs_5_0");
        if (!blob)
        {
            c.failed = true;
            Logf("[render] %s unusable; disassembly:\n%s\n", name, c.shader->ucode_disassembly().c_str());
            return nullptr;
        }
        HRESULT hr = pixel ? g_dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &c.ps)
                           : g_dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &c.vs);
        blob->Release();
        if (FAILED(hr)) { c.failed = true; return nullptr; }
        if (!pixel) c.hlsl = std::move(hlsl);
        return &c;
    }

    ID3D11VertexShader* GetRectShader(CachedShader& c)
    {
        if (c.vsRect || c.rectFailed) return c.vsRect;
        ID3DBlob* blob = Compile(c.hlsl, "vs_rect", "vs_5_0", "main_rect");
        if (!blob || FAILED(g_dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &c.vsRect)))
            c.rectFailed = true;
        if (blob) blob->Release();
        return c.vsRect;
    }



    // ---------------------------------------------------------------- textures
    struct CachedTexture
    {
        ID3D11Texture2D* tex = nullptr;
        ID3D11Texture3D* tex3d = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        uint64_t contentHash = 0;
        uint64_t checkedFrame = ~0ull;        // when the content was last re-checked
        uint32_t uploads = 0;
        bool courtCube = false;               // 3D colour table that changes colours (a court's cc_*.f1), not the identity table
        uint32_t interval = 1;                // frames between re-checks: 1 while it keeps changing,
                                              // doubling up to 32 while it stays the same
        uint32_t width = 0, height = 0;
        // full mip chain built on the GPU from the base level (RGBA8), for textures the game mipmaps
        ID3D11Texture2D* mipTex = nullptr;
        ID3D11ShaderResourceView* mipSrv = nullptr;
        ID3D11UnorderedAccessView* mipUav = nullptr;
    };
    uint64_t g_frame = 0;
    uint64_t g_cleanCompFrame = ~0ull;      // last frame in which the game's final composite was replaced by the clean one (see CLEAN COMPOSITE)
    uint64_t g_courtLutFrame = ~0ull;      // last frame in which a COURT colour cube (a 3D table that is not the identity) was used: only the courts have one, the menus do not
    int64_t g_hashTicks = 0; uint32_t g_hashCount = 0;
    int64_t g_dbgT[4];   // texture change checks (for the stats line)
    uint32_t g_textureUploads = 0;
    // textures are found by their fetch constant (4 dwords) -- a plain number key, no string building per draw
    struct TexKey { uint64_t a, b; bool operator==(const TexKey& o) const { return a == o.a && b == o.b; } };
    struct TexKeyHash { size_t operator()(const TexKey& k) const { return size_t((k.a * 0x9E3779B97F4A7C15ull) ^ (k.b + (k.a >> 29))); } };
    std::unordered_map<TexKey, CachedTexture, TexKeyHash> g_textures, g_textures3D;
    inline TexKey KeyOf(const xenos::xe_gpu_texture_fetch_t& f)
    { return { (uint64_t(f.dword_0) << 32) | f.dword_1, (uint64_t(f.dword_2) << 32) | f.dword_3 }; }

    struct FormatInfo { DXGI_FORMAT dxgi; uint32_t blockW, blockH, bytesPerBlock; bool convertTo8888; };
    bool GetFormat(xenos::TextureFormat f, FormatInfo& out)
    {
        using F = xenos::TextureFormat;
        switch (f)
        {
        case F::k_8_8_8_8: case F::k_8_8_8_8_A: out = { DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 4, false }; return true;
        case F::k_8: case F::k_8_A: case F::k_8_B: out = { DXGI_FORMAT_R8_UNORM, 1, 1, 1, false }; return true;
        case F::k_8_8: out = { DXGI_FORMAT_R8G8_UNORM, 1, 1, 2, false }; return true;
        case F::k_DXT1: out = { DXGI_FORMAT_BC1_UNORM, 4, 4, 8, false }; return true;
        case F::k_DXT2_3: out = { DXGI_FORMAT_BC2_UNORM, 4, 4, 16, false }; return true;
        case F::k_DXT4_5: out = { DXGI_FORMAT_BC3_UNORM, 4, 4, 16, false }; return true;
        case F::k_5_6_5: case F::k_4_4_4_4: case F::k_1_5_5_5: out = { DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 2, true }; return true;
        case F::k_2_10_10_10: out = { DXGI_FORMAT_R10G10B10A2_UNORM, 1, 1, 4, false }; return true;
        case F::k_16_16_16_16_FLOAT: out = { DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1, 8, false }; return true;
        case F::k_16_16_FLOAT: out = { DXGI_FORMAT_R16G16_FLOAT, 1, 1, 4, false }; return true;
        case F::k_32_FLOAT: out = { DXGI_FORMAT_R32_FLOAT, 1, 1, 4, false }; return true;
        default: return false;
        }
    }

    uint32_t Swap32(uint32_t v, uint32_t endian)
    {
        switch (endian & 3)
        {
        case 1: return ((v & 0x00FF00FF) << 8) | ((v & 0xFF00FF00) >> 8);
        case 2: return __builtin_bswap32(v);
        case 3: return (v << 16) | (v >> 16);
        default: return v;
        }
    }

    // Resolves (EDRAM -> memory copies) kept on the GPU, keyed by destination address.
    // A later texture fetch from that address samples the copy directly.
    // Games that render in tiles (e.g. 1280x720 with 4x MSAA in four 640x384 pieces) copy each
    // tile to an address inside one big destination texture; such a copy is placed into that
    // texture at the tile's position (32x32-texel tiled layout, 4 bytes per texel).
    struct Resolved { ID3D11Texture2D* tex = nullptr; ID3D11ShaderResourceView* srv = nullptr; uint32_t w = 0, h = 0, pitch = 0, bytes = 0; bool swapRB = false; };
    std::unordered_map<uint32_t, Resolved> g_resolved;
    float g_scissorScaleX = 1, g_scissorScaleY = 1;
    uint32_t g_fullW = 0, g_fullH = 0;
    std::string g_compositeNotes[8];          // per frame (mod 8): what the colour-grading pass used        // set while drawing a collapsed first piece of a tiled frame

    void Resolve(const uint32_t* regs)
    {
        uint32_t dest = regs[XE_GPU_REG_RB_COPY_DEST_BASE] & 0x1FFFFFFF;
        uint32_t pitchReg = regs[XE_GPU_REG_RB_COPY_DEST_PITCH], info = regs[XE_GPU_REG_RB_COPY_DEST_INFO];
        uint32_t pitch = pitchReg & 0x3FFF, height = (pitchReg >> 16) & 0x3FFF;
        if (!dest || !pitch || !height) return;

        // source: the current window scissor, in full-frame coordinates
        uint32_t tl = regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL], br = regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
        int sx0 = int((tl & 0x7FFF) * g_scissorScaleX), sy0 = int(((tl >> 16) & 0x7FFF) * g_scissorScaleY);
        int sx1 = int((br & 0x7FFF) * g_scissorScaleX), sy1 = int(((br >> 16) & 0x7FFF) * g_scissorScaleY);
        if (g_fullW) { sx0 = sy0 = 0; sx1 = int(g_fullW); sy1 = int(g_fullH); }
        sx1 = std::min<int>(sx1, int(g_width)); sy1 = std::min<int>(sy1, int(g_height));

        // destination: inside an existing copy target, or a new one
        Resolved* target = nullptr;
        int dx = 0, dy = 0;
        for (auto& [base, r] : g_resolved)
            if (r.tex && dest > base && dest < base + r.bytes && r.pitch)
            {
                uint32_t off = (dest - base) / 4, block = r.pitch * 32;
                dy = int(off / block) * 32; dx = int((off % block) / 32);
                target = &r;
                break;
            }
        if (!target)
        {
            uint32_t w = std::min<uint32_t>(pitch, g_width), h = std::min<uint32_t>(height, g_height);
            auto& r = g_resolved[dest];
            if (!r.tex || r.w != w || r.h != h)
            {
                static int created = 0;
                if (created++ < 400)
                    Logf("[resolve] frame %llu: %s copy target %08X %ux%u (was %ux%u), source rect (%d,%d)-(%d,%d)\n", (unsigned long long)g_frame,
                         r.tex ? "re-creating" : "new", dest, w, h, r.w, r.h, sx0, sy0, sx1, sy1);
                if (r.tex && r.tex == g_showTex) { g_showTex = nullptr; g_showSrv = nullptr; }      // (the shown picture is read from g_front then)
                SafeRelease(r.srv); SafeRelease(r.tex);
                D3D11_TEXTURE2D_DESC td{};
                td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                td.Width = Sc(int(w)); td.Height = Sc(int(h));
                td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &r.tex))) return;
                g_dev->CreateShaderResourceView(r.tex, nullptr, &r.srv);
                r.w = w; r.h = h;
            }
            r.pitch = (pitch + 31) & ~31u;
            r.bytes = r.pitch * ((height + 31) & ~31u) * 4;
            target = &r;
            dx = dy = 0;
        }
        // copy size: the scissor rect, clipped to what fits at (dx, dy)
        int w = std::min(sx1 - sx0, int(target->w) - dx), h = std::min(sy1 - sy0, int(target->h) - dy);
        if (w <= 0 || h <= 0) return;
        D3D11_BOX box{ Sc(sx0), Sc(sy0), 0, Sc(sx0 + w), Sc(sy0 + h), 1 };
        // rounding at fractional scales: keep the copy inside the target and the source
        box.right = std::min({ box.right, box.left + (Sc(int(target->w)) - Sc(dx)), g_rtW });
        box.bottom = std::min({ box.bottom, box.top + (Sc(int(target->h)) - Sc(dy)), g_rtH });
        if (box.right <= box.left || box.bottom <= box.top) return;
        SyncRtRegion(box);
        g_ctx->CopySubresourceRegion(target->tex, 0, Sc(dx), Sc(dy), 0, g_rt, 0, &box);
        target->swapRB = (info >> 24) & 1;
        static int logged = 0;
        static const bool resolveLog = getenv("NBASH_RESOLVELOG") != nullptr;
        if (resolveLog && logged++ < 60)
            Logf("[resolve] %08X pitch %u h %u: rect (%d,%d)-(%d,%d) -> target %ux%u at (%d,%d)\n", dest, pitch, height, sx0, sy0, sx0 + w, sy0 + h, target->w, target->h, dx, dy);
    }

    // ---------------------------------------------------------------- memory export
    // Vertex shaders that write memory (eA/eM, e.g. GPU skinning into a vertex buffer that
    // later draws read) run a second time as a compute shader over the vertex indices.
    // Each eM write is logged as a 48-byte record; before the next draw that does not export,
    // the log is read back and the writes are applied to guest memory in their formats.
    // GPU-resident memory export: each exported stream (base address + size from the stream
    // constants) gets a region in an "arena" buffer, initialised from guest memory; the export
    // compute shader writes packed guest-order dwords there, and vertex uploads of draws that
    // read those addresses copy from the arena instead of guest memory (no CPU readback).
    constexpr uint32_t kArenaRows = 4096, kArenaBytes = kArenaRows * kVmemRowDwords * 4;   // 64 MB
    ID3D11Texture2D* g_arena = nullptr;
    ID3D11UnorderedAccessView* g_arenaUav = nullptr;
    ID3D11Buffer* g_expConst = nullptr;
    struct ArenaRegion { uint32_t base, bytes, offset; };
    std::vector<ArenaRegion> g_arenaRegions;     // this frame's exported streams
    uint32_t g_arenaUsed = 0;
    bool g_exportPending = false;                // unused (kept for the Present hook)
    std::vector<std::pair<uint32_t, uint32_t>> g_exportedRanges;   // [start, end) physical
    uint32_t g_lastExportAddr = 0, g_lastExportEnd = 0;
    void FlushExports() {}

    bool ArenaLocate(uint32_t guestAddr, uint32_t bytes, uint32_t& row, uint32_t& x0)
    {
        for (const auto& r : g_arenaRegions)
            if (guestAddr >= r.base && guestAddr + bytes <= r.base + r.bytes)
            {
                uint32_t dw = (r.offset + (guestAddr - r.base)) / 4;
                row = dw / kVmemRowDwords; x0 = dw % kVmemRowDwords;
                return (guestAddr & 3) == 0;
            }
        return false;
    }

    uint32_t ExportElementSize(uint32_t fmt)
    {
        switch (fmt)
        {
        case 37: case 26: case 32: return 8;
        case 38: return 16;
        case 2: return 1;
        case 10: case 24: case 30: return 2;
        default: return 4;
        }
    }

    ID3D11ComputeShader* GetExportShader(CachedShader& c)
    {
        if (c.exportCs || c.exportFailed) return c.exportCs;
        {
            const std::string& dis = c.shader->ucode_disassembly();
            for (size_t p = dis.find("alloc export"); p != std::string::npos; p = dis.find("alloc export", p + 1)) c.exportBlocks++;
        }
        ID3DBlob* blob = Compile("#define XE_CS 1\n" + c.hlsl, "vs_export", "cs_5_0", "main_cs");
        if (!blob || FAILED(g_dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &c.exportCs)))
        {
            c.exportFailed = true;
            Logf("[render] memory-export compute shader failed to build\n");
        }
        if (blob) blob->Release();
        return c.exportCs;
    }

    bool InitExport()
    {
        if (g_arena) return true;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kVmemRowDwords; td.Height = kArenaRows; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R32_UINT;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_arena))) return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_UINT; ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (FAILED(g_dev->CreateUnorderedAccessView(g_arena, &ud, &g_arenaUav))) return false;
        D3D11_BUFFER_DESC cd{};
        cd.ByteWidth = 16 * 9; cd.Usage = D3D11_USAGE_DEFAULT; cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(g_dev->CreateBuffer(&cd, nullptr, &g_expConst))) return false;
        return true;
    }

    void NoteExportedRange(uint32_t start, uint32_t end)
    {
        for (auto& r : g_exportedRanges)
            if (start <= r.second && end >= r.first) { r.first = std::min(r.first, start); r.second = std::max(r.second, end); return; }
        g_exportedRanges.push_back({ start, end });
    }

    void RunExport(CachedShader& vs, uint32_t count, const uint32_t* regs, ID3D11ShaderResourceView* const* srvs, ID3D11SamplerState* const* samplers,
                   ID3D11Buffer* cbFloat, ID3D11Buffer* cbSystem, ID3D11ShaderResourceView* vmemSrv)
    {
        if (!InitExport()) return;
        ID3D11ComputeShader* cs = GetExportShader(vs);
        if (!cs) return;
        uint32_t cc[4 * 9] = {};
        uint32_t ns = 0;
        for (uint32_t ci : vs.shader->memexport_stream_constants())
        {
            if (ns >= 8 || ci >= 256) break;
            const uint32_t* c = &regs[0x4000 + ci * 4];
            uint32_t base = (c[0] & 0x3FFFFFFFu) << 2, fmt = (c[2] >> 8) & 63, cnt = c[3] & 0x7FFFFF;
            uint32_t bytes = (cnt * ExportElementSize(fmt) + 3) & ~3u;
            if (!bytes || (base & 3) || uint64_t(base) + bytes > 0x20000000ull) continue;
            ArenaRegion* reg = nullptr;
            for (auto& r : g_arenaRegions) if (r.base == base && r.bytes >= bytes) reg = &r;
            if (!reg)
            {
                if (g_arenaUsed + bytes > kArenaBytes)
                {
                    static bool w = false;
                    if (!w) { w = true; Logf("[render] memory export arena full\n"); }
                    continue;
                }
                g_arenaRegions.push_back({ base, bytes, g_arenaUsed });
                reg = &g_arenaRegions.back();
                g_arenaUsed += (bytes + kVmemRowDwords * 4 - 1) / (kVmemRowDwords * 4) * (kVmemRowDwords * 4);   // row aligned
                // start from what guest memory holds (dwords the shader does not write stay valid);
                // regions start on a row, so the upload is whole rows
                uint32_t r0 = reg->offset / (kVmemRowDwords * 4), nrows = (bytes + kVmemRowDwords * 4 - 1) / (kVmemRowDwords * 4);
                static std::vector<uint8_t> tmp; if (tmp.size() < size_t(nrows) * kVmemRowDwords * 4) tmp.resize(size_t(nrows) * kVmemRowDwords * 4);
                memcpy(tmp.data(), PhysToHost(base), bytes);
                D3D11_BOX box{ 0, r0, 0, kVmemRowDwords, r0 + nrows, 1 };
                g_ctx->UpdateSubresource(g_arena, 0, &box, tmp.data(), kVmemRowDwords * 4, 0);
                NoteExportedRange(base, base + bytes);
            }
            cc[4 + ns * 4] = base; cc[4 + ns * 4 + 1] = reg->offset / 4; cc[4 + ns * 4 + 2] = bytes;
            ns++;
        }
        if (!ns) return;
        cc[0] = count; cc[1] = ns;
        g_ctx->UpdateSubresource(g_expConst, 0, nullptr, cc, 0, 0);
        ID3D11Buffer* cbs[3] = { cbFloat, cbSystem, g_expConst };
        g_ctx->CSSetConstantBuffers(0, 3, cbs);
        g_ctx->CSSetShaderResources(0, kMaxTextures, srvs);
        g_ctx->CSSetShaderResources(16, 1, &vmemSrv);
        g_ctx->CSSetSamplers(0, kMaxTextures, samplers);
        g_ctx->CSSetUnorderedAccessViews(0, 1, &g_arenaUav, nullptr);
        g_ctx->CSSetShader(cs, nullptr, 0);
        g_ctx->Dispatch((count + 63) / 64, 1, 1);
        ID3D11UnorderedAccessView* nullUav = nullptr;
        g_ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        static bool logged = false;
        if (!logged) { logged = true; Logf("[render] memory export running on the GPU (%u streams)\n", ns); }
    }

    // 3D textures (small colour-grading LUTs and the like): untiled with Xenos 3D tiling,
    // uncompressed formats only. Re-checked like 2D textures.
    ID3D11ShaderResourceView* GetTexture3D(const xenos::xe_gpu_texture_fetch_t& f, float size[4])
    {
        uint32_t w = f.size_3d.width + 1, h = f.size_3d.height + 1, d = f.size_3d.depth + 1;
        size[0] = float(w); size[1] = float(h); size[2] = 1.0f / w; size[3] = 1.0f / h;
        FormatInfo fi;
        if (!GetFormat(f.format, fi) || fi.blockW != 1 || fi.convertTo8888 || uint64_t(w) * h * d > (1u << 24))
        {
            static int warned = 0;
            if (warned++ < 5) Logf("[render] 3D texture %ux%ux%u format %u not supported yet\n", w, h, d, unsigned(f.format));
            return nullptr;
        }
        if (f.sign_x == xenos::TextureSign::kSigned && fi.dxgi == DXGI_FORMAT_R8G8B8A8_UNORM) fi.dxgi = DXGI_FORMAT_R8G8B8A8_SNORM;
        uint32_t bpbLog2 = fi.bytesPerBlock == 1 ? 0 : fi.bytesPerBlock == 2 ? 1 : fi.bytesPerBlock == 4 ? 2 : fi.bytesPerBlock == 8 ? 3 : 4;
        uint32_t pitch = std::max<uint32_t>(f.pitch * 32, w);
        uint32_t pitchAligned = (pitch + 31) & ~31u, heightAligned = (h + 31) & ~31u, depthAligned = (d + 3) & ~3u;
        uint32_t base = f.base_address << 12;
        uint64_t span = f.tiled ? uint64_t(pitchAligned) * heightAligned * depthAligned * fi.bytesPerBlock
                                : uint64_t((pitch * fi.bytesPerBlock + 255) & ~255u) * h * d;
        if (base + span > 0x20000000ull) return nullptr;
        const uint8_t* src = PhysToHost(base);

        auto& ct = g_textures3D[KeyOf(f)];
        if (ct.srv && ct.courtCube) g_courtLutFrame = g_frame;
        if (ct.srv && g_frame - ct.checkedFrame < ct.interval) return ct.srv;
        uint64_t hsh = 1469598103934665603ull;
        for (uint64_t i = 0; i + 8 <= span; i += (span > (1u << 20) ? (span / 65536) & ~7ull : 8)) { uint64_t v; memcpy(&v, src + i, 8); hsh = (hsh ^ v) * 1099511628211ull; }
        ct.checkedFrame = g_frame;
        if (ct.srv && ct.contentHash == hsh) return ct.srv;     // 3D tables are checked every frame
        ct.interval = 1;
        g_textureUploads++;

        uint32_t bpb = fi.bytesPerBlock, linRow = (pitch * bpb + 255) & ~255u;
        std::vector<uint8_t> texels(size_t(w) * h * d * bpb);
        for (uint32_t z = 0; z < d; z++)
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++)
                {
                    uint64_t off = f.tiled ? uint64_t(texture_address::Tiled3D(int32_t(x), int32_t(y), int32_t(z), pitchAligned, heightAligned, bpbLog2))
                                           : (uint64_t(z) * h + y) * linRow + uint64_t(x) * bpb;
                    if (off + bpb > span) continue;
                    memcpy(&texels[((size_t(z) * h + y) * w + x) * bpb], src + off, bpb);
                }
        if (bpb == 2 && ((uint32_t(f.endianness) & 3) == 1 || (uint32_t(f.endianness) & 3) == 2))
            for (size_t i = 0; i + 2 <= texels.size(); i += 2) std::swap(texels[i], texels[i + 1]);
        else if (bpb >= 4)
            for (size_t i = 0; i + 4 <= texels.size(); i += 4) { uint32_t v; memcpy(&v, &texels[i], 4); v = Swap32(v, uint32_t(f.endianness)); memcpy(&texels[i], &v, 4); }

        if (!ct.tex3d || ct.width != w || ct.height != h)
        {
            SafeRelease(ct.srv); SafeRelease(ct.tex3d);
            D3D11_TEXTURE3D_DESC td{};
            td.Width = w; td.Height = h; td.Depth = d; td.MipLevels = 1; td.Format = fi.dxgi;
            td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(g_dev->CreateTexture3D(&td, nullptr, &ct.tex3d))) return nullptr;
            g_dev->CreateShaderResourceView(ct.tex3d, nullptr, &ct.srv);
            ct.width = w; ct.height = h;
            Logf("[render] 3D texture %ux%ux%u format %u at %08X\n", w, h, d, unsigned(f.format), base);
        }
        g_ctx->UpdateSubresource(ct.tex3d, 0, nullptr, texels.data(), w * bpb, w * h * bpb);
        ct.contentHash = hsh;
        if (bpb == 4 && w == 32 && h == 32 && d == 32)      // identity table: every channel averages 127.5 and has only the 32 grid values
        {
            double sum[3] = {}; bool seen[3][256] = {}; int kinds[3] = {};
            for (size_t i = 0; i + 4 <= texels.size(); i += 4) for (int c = 0; c < 3; c++) { uint8_t v = texels[i + c]; sum[c] += v; if (!seen[c][v]) { seen[c][v] = true; kinds[c]++; } }
            bool ident = true; for (int c = 0; c < 3; c++) if (fabs(sum[c] / 32768.0 - 127.5) > 1.0 || kinds[c] > 40) ident = false;
            if (ct.courtCube != !ident) Logf("[render] colour cube at %08X: %s\n", base, ident ? "identity (menu)" : "court colours");
            ct.courtCube = !ident; if (ct.courtCube) g_courtLutFrame = g_frame;
        }
        return ct.srv;
    }


    // ---------------------------------------------------------------- tiled rendering collapse
    // In-game frames are drawn as 4 pieces of 640x384 (EDRAM is too small for 1280x720 with 4x
    // MSAA), each piece re-submitting the scene with its own window offset and scissor. Our
    // target is the full frame, so the first piece is drawn unscissored (with every predicated
    // draw) and the others are skipped: about 4x fewer scene draws.
    struct TileExtent { uint32_t w = 0, h = 0; };
    std::unordered_map<uint64_t, TileExtent> g_tiledCur, g_tiledPrev;
    bool TileCollapseEnabled() { static const bool off = getenv("NBASH_NOTILECOLLAPSE") != nullptr; return !off && g_tileCollapse; }
    uint64_t TileKey(const uint32_t* regs) { return (uint64_t(regs[XE_GPU_REG_RB_SURFACE_INFO]) << 32) | regs[XE_GPU_REG_RB_DEPTH_INFO]; }
    int TileCollapseState(const uint32_t* regs, bool record, uint32_t& w, uint32_t& h)
    {
        uint32_t woff = regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET];
        uint64_t key = TileKey(regs);
        if (woff && record)
        {
            uint32_t br = regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
            auto& t = g_tiledCur[key];
            t.w = std::max(t.w, br & 0x7FFF); t.h = std::max(t.h, (br >> 16) & 0x7FFF);
        }
        if (!TileCollapseEnabled()) return 0;
        auto it = g_tiledPrev.find(key);
        if (it == g_tiledPrev.end() || !it->second.w || !it->second.h) return 0;
        if (woff) return 2;
        w = std::min(it->second.w, g_width); h = std::min(it->second.h, g_height);
        return 1;
    }

    // Mipmaps: the console textures carry their own smaller versions, which we did not load, so
    // distant surfaces (court lines, fences, buildings) sparkled and looked rough. We copy the base
    // level into an RGBA8 texture with a compute shader and let the GPU build the chain.
    // NBASH_NOMIPS=1 turns it off.
    bool BuildMips(CachedTexture& ct, uint32_t w, uint32_t h)
    {
        static ID3D11ComputeShader* cs = nullptr;
        static bool tried = false;
        if (!tried)
        {
            tried = true;
            ID3DBlob* blob = Compile("Texture2D<float4> src : register(t0); RWTexture2D<float4> dst : register(u0);\n"
                                     "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) { dst[id.xy] = src.Load(int3(id.xy, 0)); }\n",
                                     "mipcopy", "cs_5_0");
            if (!blob || FAILED(g_dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &cs))) cs = nullptr;
            Logf(cs ? "[render] mipmaps: on\n" : "[render] mipmaps: could not create the copy shader\n");
        }
        if (!cs) return false;
        if (!ct.mipTex)
        {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = w; td.Height = h; td.MipLevels = 0; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
            td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &ct.mipTex))) { ct.mipTex = nullptr; return false; }
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM; ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D; ud.Texture2D.MipSlice = 0;
            if (FAILED(g_dev->CreateShaderResourceView(ct.mipTex, nullptr, &ct.mipSrv)) ||
                FAILED(g_dev->CreateUnorderedAccessView(ct.mipTex, &ud, &ct.mipUav)))
            { SafeRelease(ct.mipSrv); SafeRelease(ct.mipUav); SafeRelease(ct.mipTex); return false; }
        }
        g_ctx->CSSetShader(cs, nullptr, 0);
        g_ctx->CSSetShaderResources(0, 1, &ct.srv);
        g_ctx->CSSetUnorderedAccessViews(0, 1, &ct.mipUav, nullptr);
        g_ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        ID3D11ShaderResourceView* nullSrv = nullptr; ID3D11UnorderedAccessView* nullUav = nullptr;
        g_ctx->CSSetShaderResources(0, 1, &nullSrv);
        g_ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        g_ctx->GenerateMips(ct.mipSrv);
        return true;
    }
    bool WantsMips(const xenos::xe_gpu_texture_fetch_t& f, DXGI_FORMAT fmt, uint32_t w, uint32_t h)
    {
        static const bool off = getenv("NBASH_NOMIPS") != nullptr;
        if (off || f.mip_max_level == 0 || f.mip_filter == xenos::TextureFilter::kBaseMap || w < 8 || h < 8) return false;
        switch (fmt)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC3_UNORM: return true;
        default: return false;
        }
    }

    // NBASH_DUMPTEX=1: every small texture (up to 256x256) the game loads is saved once as
    // texdump\<hash>_<w>x<h>_f<format>.bmp, to find images such as the button icons.
    void WriteBmpFile(const char* name, const std::vector<uint32_t>& px, uint32_t w, uint32_t h);
    void DecodeBlockBC(const uint8_t* b, int fmt, uint32_t out[16])   // fmt 1 = BC1, 2 = BC2, 3 = BC3
    {
        const uint8_t* c = fmt == 1 ? b : b + 8;
        uint16_t c0 = uint16_t(c[0] | c[1] << 8), c1 = uint16_t(c[2] | c[3] << 8);
        auto rgb = [](uint16_t v, int k) { int r = (v >> 11) * 255 / 31, g = ((v >> 5) & 63) * 255 / 63, bl = (v & 31) * 255 / 31; return k == 0 ? r : k == 1 ? g : bl; };
        uint32_t pal[4];
        for (int i = 0; i < 4; i++)
        {
            int ch[3];
            for (int k = 0; k < 3; k++)
            {
                int a0 = rgb(c0, k), a1 = rgb(c1, k);
                ch[k] = i == 0 ? a0 : i == 1 ? a1 : (fmt != 1 || c0 > c1) ? (i == 2 ? (2 * a0 + a1) / 3 : (a0 + 2 * a1) / 3) : (i == 2 ? (a0 + a1) / 2 : 0);
            }
            uint32_t alpha = (fmt == 1 && c0 <= c1 && i == 3) ? 0 : 255;
            pal[i] = uint32_t(ch[0]) | uint32_t(ch[1]) << 8 | uint32_t(ch[2]) << 16 | alpha << 24;
        }
        uint32_t idx = uint32_t(c[4] | c[5] << 8 | c[6] << 16 | uint32_t(c[7]) << 24);
        for (int i = 0; i < 16; i++) out[i] = pal[(idx >> (2 * i)) & 3];
        if (fmt == 2)
            for (int i = 0; i < 16; i++) { uint32_t a = (b[i / 2] >> (4 * (i & 1))) & 15; out[i] = (out[i] & 0xFFFFFF) | (a * 17) << 24; }
        else if (fmt == 3)
        {
            uint8_t a0 = b[0], a1 = b[1]; uint8_t ap[8] = { a0, a1 };
            for (int i = 2; i < 8; i++) ap[i] = a0 > a1 ? uint8_t(((8 - i) * a0 + (i - 1) * a1) / 7) : i < 6 ? uint8_t(((6 - i) * a0 + (i - 1) * a1) / 5) : i == 6 ? 0 : 255;
            uint64_t bits = 0; for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (8 * i);
            for (int i = 0; i < 16; i++) out[i] = (out[i] & 0xFFFFFF) | uint32_t(ap[(bits >> (3 * i)) & 7]) << 24;
        }
    }
    void DumpTexture(uint64_t hash, uint32_t w, uint32_t h, const FormatInfo& fi, const uint8_t* data, uint32_t rowPitch)
    {
        static const bool on = getenv("NBASH_DUMPTEX") != nullptr;
        if (!on || w > 256 || h > 256 || w < 8 || h < 8) return;
        static std::set<uint64_t> done;
        if (!done.insert(hash).second || done.size() > 4000) return;
        std::vector<uint32_t> px(size_t(w) * h, 0);
        int bc = fi.dxgi == DXGI_FORMAT_BC1_UNORM ? 1 : fi.dxgi == DXGI_FORMAT_BC2_UNORM ? 2 : fi.dxgi == DXGI_FORMAT_BC3_UNORM ? 3 : 0;
        if (bc)
        {
            uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, blk[16];
            for (uint32_t by = 0; by < bh; by++)
                for (uint32_t bx = 0; bx < bw; bx++)
                {
                    DecodeBlockBC(data + size_t(by) * rowPitch + bx * fi.bytesPerBlock, bc, blk);
                    for (int i = 0; i < 16; i++) { uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2); if (x < w && y < h) px[size_t(y) * w + x] = blk[i]; }
                }
        }
        else if (fi.dxgi == DXGI_FORMAT_R8G8B8A8_UNORM)
            for (uint32_t y = 0; y < h; y++) memcpy(&px[size_t(y) * w], data + size_t(y) * rowPitch, w * 4);
        else if (fi.dxgi == DXGI_FORMAT_R8_UNORM)
            for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) { uint32_t v = data[size_t(y) * rowPitch + x]; px[size_t(y) * w + x] = v | v << 8 | v << 16 | 0xFF000000u; }
        else return;
        CreateDirectoryA("texdump", nullptr);
        char name[128]; snprintf(name, sizeof name, "texdump\\%016llx_%ux%u_f%u.bmp", (unsigned long long)hash, w, h, unsigned(fi.dxgi));
        WriteBmpFile(name, px, w, h);
    }

    // ---------------------------------------------------------------- HD texture replacement
    // game\mods\hd\<16 hex digits>.dds : an ordinary DDS (DXT1 / DXT3 / DXT5, with its mip chain, ANY size) that is shown instead of a game
    // texture.  The name is the FNV-1a hash (8-byte words, little-endian) of the game texture's top-level block data once it is un-tiled and
    // byte-swapped - which is byte for byte the top mip level of the PC-style DDS in the game's archives, so the names can be worked out
    // offline (cloud meshtools/hdname.py).  The big picture lives only in the PC's video memory: nothing is added to the game's own
    // (very tight) memory.  NBASH_HD=0 off;  NBASH_HDLOG=1 logs the hash of every block-compressed texture as it is first loaded.
    uint64_t HdHash(const std::vector<uint8_t>& b)
    {
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i + 8 <= b.size(); i += 8) { uint64_t v; memcpy(&v, &b[i], 8); h = (h ^ v) * 1099511628211ull; }
        return h;
    }
    ID3D11ShaderResourceView* HdLookup(uint64_t hash)
    {
        static std::unordered_map<uint64_t, ID3D11ShaderResourceView*> loaded;      // nullptr = no file / failed
        static std::unordered_map<uint64_t, std::string> files; static bool scanned = false;
        struct PakEnt { uint64_t off; uint32_t size; }; static std::unordered_map<uint64_t, PakEnt> pak; static std::string pakPath;      // game\mods\hd.pak: ONE file with all HD textures ("NBHD", count, then hash / offset / size records, then the DDS files); loose files in mods\hd win over it
        static const bool off = [] { const char* v = getenv("NBASH_HD"); return v && *v == '0'; }();
        if (off) return nullptr;
        if (!scanned)
        {
            scanned = true;
            std::string dir = ::GameRoot() + "/mods/hd/"; WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((dir + "????????????????.dds").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE)
            {
                do { std::string fn = fd.cFileName; uint64_t k = strtoull(fn.substr(0, 16).c_str(), nullptr, 16); if (k) files[k] = dir + fn; } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
            pakPath = ::GameRoot() + "/mods/hd.pak";
            if (FILE* pf = fopen(pakPath.c_str(), "rb"))
            {
                uint8_t hd[8]; uint32_t n = 0;
                if (fread(hd, 1, 8, pf) == 8 && !memcmp(hd, "NBHD", 4)) { memcpy(&n, hd + 4, 4); if (n > 100000) n = 0; }
                for (uint32_t i = 0; i < n; i++) { uint8_t r[24]; if (fread(r, 1, 24, pf) != 24) break; uint64_t k, o; uint32_t sz; memcpy(&k, r, 8); memcpy(&o, r + 8, 8); memcpy(&sz, r + 16, 4); pak[k] = { o, sz }; }
                fclose(pf);
            }
            Logf("[hd] %u HD textures in mods\\hd.pak, %u loose files in mods\\hd\n", unsigned(pak.size()), unsigned(files.size()));
        }
        auto it = loaded.find(hash); if (it != loaded.end()) return it->second;
        ID3D11ShaderResourceView* srv = nullptr;
        auto f = files.find(hash); auto pk = pak.find(hash);
        bool fromPak = f == files.end() && pk != pak.end();
        char pakName[40]; snprintf(pakName, sizeof pakName, "hd.pak:%016llx", (unsigned long long)hash);
        const std::string srcName = fromPak ? std::string(pakName) : (f != files.end() ? f->second : std::string());
        if (f != files.end() || fromPak)
        {
            std::vector<uint8_t> d; FILE* fp = fopen(fromPak ? pakPath.c_str() : srcName.c_str(), "rb");
            if (fp && fromPak) { _fseeki64(fp, (long long)pk->second.off, SEEK_SET); d.resize(pk->second.size); if (fread(d.data(), 1, d.size(), fp) != d.size()) d.clear(); fclose(fp); }
            else if (fp) { fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET); d.resize(size_t(n > 0 ? n : 0)); if (n <= 0 || fread(d.data(), 1, size_t(n), fp) != size_t(n)) d.clear(); fclose(fp); }
            auto u32 = [&](size_t o) { uint32_t v; memcpy(&v, &d[o], 4); return v; };
            if (d.size() > 128 && !memcmp(d.data(), "DDS ", 4))
            {
                uint32_t h = u32(12), w = u32(16), mips = u32(28) ? u32(28) : 1, cc = u32(84);
                DXGI_FORMAT fmt = cc == 0x31545844 ? DXGI_FORMAT_BC1_UNORM : cc == 0x33545844 ? DXGI_FORMAT_BC2_UNORM : cc == 0x35545844 ? DXGI_FORMAT_BC3_UNORM : DXGI_FORMAT_UNKNOWN;
                uint32_t blk = fmt == DXGI_FORMAT_BC1_UNORM ? 8 : 16;
                if (fmt != DXGI_FORMAT_UNKNOWN && w && h && w <= 8192 && h <= 8192 && mips <= 14)
                {
                    std::vector<D3D11_SUBRESOURCE_DATA> init; size_t off = 128; uint32_t lw = w, lh = h; bool ok = true;
                    for (uint32_t m = 0; m < mips; m++)
                    {
                        uint32_t bw = (lw + 3) / 4 ? (lw + 3) / 4 : 1, bh = (lh + 3) / 4 ? (lh + 3) / 4 : 1; size_t sz = size_t(bw) * bh * blk;
                        if (off + sz > d.size()) { if (m == 0) ok = false; mips = m; break; }
                        D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = d.data() + off; sd.SysMemPitch = bw * blk; init.push_back(sd); off += sz;
                        lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1;
                    }
                    if (ok && mips)
                    {
                        D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h; td.MipLevels = mips; td.ArraySize = 1; td.Format = fmt; td.SampleDesc.Count = 1;
                        td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                        ID3D11Texture2D* t = nullptr;
                        if (SUCCEEDED(g_dev->CreateTexture2D(&td, init.data(), &t)) && t) { g_dev->CreateShaderResourceView(t, nullptr, &srv); t->Release(); }
                    }
                    Logf("[hd] %016llx: %s %ux%u, %u mips - %s\n", (unsigned long long)hash, srcName.c_str(), w, h, mips, srv ? "in use" : "could not be created");
                }
                else Logf("[hd] %s: not a DXT1 / DXT3 / DXT5 DDS - ignored\n", srcName.c_str());
            }
        }
        loaded[hash] = srv; return srv;
    }

    bool g_lastSwapRB = false;
    ID3D11ShaderResourceView* GetTexture(const xenos::xe_gpu_texture_fetch_t& f, float size[4])
    {
        g_lastSwapRB = false;
        if (f.dimension == xenos::DataDimension::k3D) return GetTexture3D(f, size);
        {
            auto it = g_resolved.find((f.base_address << 12) & 0x1FFFFFFF);
            if (it != g_resolved.end() && it->second.srv)
            {
                uint32_t w = f.size_2d.width + 1, h = f.size_2d.height + 1;
                size[0] = float(w); size[1] = float(h); size[2] = 1.0f / w; size[3] = 1.0f / h;
                g_lastSwapRB = it->second.swapRB;
                return it->second.srv;
            }
        }
        uint32_t w = f.size_2d.width + 1, h = f.size_2d.height + 1;
        size[0] = float(w); size[1] = float(h); size[2] = 1.0f / w; size[3] = 1.0f / h;
        FormatInfo fi;
        if (!GetFormat(f.format, fi))
        {
            static uint32_t warned = 0;
            if (!(warned & (1u << (uint32_t(f.format) & 31)))) { warned |= 1u << (uint32_t(f.format) & 31); Logf("[render] texture format %u not supported yet\n", uint32_t(f.format)); }
            return nullptr;
        }
        // components flagged signed (all of them): sample as signed normalized values
        if (f.sign_x == xenos::TextureSign::kSigned && f.sign_y == xenos::TextureSign::kSigned &&
            f.sign_z == xenos::TextureSign::kSigned && f.sign_w == xenos::TextureSign::kSigned)
        {
            if (fi.dxgi == DXGI_FORMAT_R8G8B8A8_UNORM && !fi.convertTo8888) fi.dxgi = DXGI_FORMAT_R8G8B8A8_SNORM;
            else if (fi.dxgi == DXGI_FORMAT_R8_UNORM) fi.dxgi = DXGI_FORMAT_R8_SNORM;
            else if (fi.dxgi == DXGI_FORMAT_R8G8_UNORM) fi.dxgi = DXGI_FORMAT_R8G8_SNORM;
        }
        uint32_t bw = (w + fi.blockW - 1) / fi.blockW, bh = (h + fi.blockH - 1) / fi.blockH;
        uint32_t pitchBlocks = (f.pitch * 32 + fi.blockW - 1) / fi.blockW;   // pitch is in 32-texel units
        // A row pitch far smaller than the width means this is not a real texture (an unset or
        // stale fetch slot); reading it would mean hashing/converting hundreds of MB per frame.
        if (pitchBlocks * 4 < bw || uint64_t(w) * h > 4096ull * 4096ull)
        {
            static int warned = 0;
            if (warned++ < 5) Logf("[render] ignoring implausible texture %ux%u pitch %u at %08X\n", w, h, unsigned(f.pitch), f.base_address << 12);
            return nullptr;
        }
        if (pitchBlocks < bw) pitchBlocks = bw;
        uint32_t bpbLog2 = fi.bytesPerBlock == 1 ? 0 : fi.bytesPerBlock == 2 ? 1 : fi.bytesPerBlock == 4 ? 2 : fi.bytesPerBlock == 8 ? 3 : 4;
        uint32_t base = f.base_address << 12;
        uint32_t pitchAligned = (pitchBlocks + 31) & ~31u;
        uint32_t heightAligned = (bh + 31) & ~31u;
        uint32_t linearRowBytes = (pitchBlocks * fi.bytesPerBlock + 255) & ~255u;
        uint32_t spanBytes = f.tiled ? pitchAligned * heightAligned * fi.bytesPerBlock : linearRowBytes * bh;
        if (base + uint64_t(spanBytes) > 0x20000000) return nullptr;
        const uint8_t* src = PhysToHost(base);

        auto& ct = g_textures[KeyOf(f)];
        auto result = [&]() { return ct.mipSrv ? ct.mipSrv : ct.srv; };
        if (ct.srv && g_frame - ct.checkedFrame < ct.interval) return result();
        // content hash: FNV over the first 4 KB plus up to ~2K words spread over the rest
        // (was 16K: on big textures every read is a cache miss, which cost ~3 ms a frame)
        LARGE_INTEGER h0; QueryPerformanceCounter(&h0);
        uint64_t hsh = 1469598103934665603ull;
        {
            uint32_t head = spanBytes < 4096 ? spanBytes : 4096;
            for (uint32_t i = 0; i + 8 <= head; i += 8) { uint64_t v; memcpy(&v, src + i, 8); hsh = (hsh ^ v) * 1099511628211ull; }
            uint32_t step = spanBytes / 2048 > 8 ? (spanBytes / 2048) & ~7u : 8;
            for (uint32_t i = head; i + 8 <= spanBytes; i += step) { uint64_t v; memcpy(&v, src + i, 8); hsh = (hsh ^ v) * 1099511628211ull; }
        }
        { LARGE_INTEGER h1; QueryPerformanceCounter(&h1); g_hashTicks += h1.QuadPart - h0.QuadPart; g_hashCount++; }
        ct.checkedFrame = g_frame;
        // Unchanged textures are re-checked less often (up to every 32 frames) -- but only big ones:
        // small textures are cheap to hash and are the ones the game animates (colour tables,
        // effect ramps); checking those late gave black Gamebreaker frames.
        uint32_t maxInterval = spanBytes <= (16u << 10) ? 1 : 64;   // tiny ones (fade/parameter textures) every frame
        if (ct.srv && ct.contentHash == hsh) { if (ct.interval < maxInterval) ct.interval *= 2; return result(); }
        ct.interval = 1;
        g_textureUploads++;
        if (++ct.uploads == 4)
            Logf("[render] texture re-uploaded often: base %08X %ux%u format %u tiled %u pitch %u span %u KB (fetch %08X %08X %08X %08X)\n",
                 base, w, h, unsigned(f.format), unsigned(f.tiled), unsigned(f.pitch), spanBytes / 1024, f.dword_0, f.dword_1, f.dword_2, f.dword_3);

        // untile + endian swap into a linear block array
        std::vector<uint8_t> blocks(size_t(bw) * bh * fi.bytesPerBlock);
        for (uint32_t y = 0; y < bh; y++)
            for (uint32_t x = 0; x < bw; x++)
            {
                uint32_t off = f.tiled ? uint32_t(texture_address::Tiled2D(int32_t(x), int32_t(y), pitchAligned, bpbLog2))
                                       : y * linearRowBytes + x * fi.bytesPerBlock;
                uint8_t* dst = &blocks[(size_t(y) * bw + x) * fi.bytesPerBlock];
                memcpy(dst, src + off, fi.bytesPerBlock);
            }
        if (fi.bytesPerBlock >= 2)
        {
            if (fi.bytesPerBlock == 2)
                for (size_t i = 0; i + 2 <= blocks.size(); i += 2)
                {
                    if ((uint32_t(f.endianness) & 3) == 1 || (uint32_t(f.endianness) & 3) == 2) std::swap(blocks[i], blocks[i + 1]);
                }
            else
                for (size_t i = 0; i + 4 <= blocks.size(); i += 4)
                {
                    uint32_t v; memcpy(&v, &blocks[i], 4); v = Swap32(v, uint32_t(f.endianness)); memcpy(&blocks[i], &v, 4);
                }
        }
        std::vector<uint8_t> converted;
        const uint8_t* upload = blocks.data();
        uint32_t rowPitch = bw * fi.bytesPerBlock;
        if (fi.convertTo8888)
        {
            converted.resize(size_t(bw) * bh * 4);
            for (size_t i = 0; i < size_t(bw) * bh; i++)
            {
                uint16_t v; memcpy(&v, &blocks[i * 2], 2);
                uint8_t* o = &converted[i * 4];
                if (f.format == xenos::TextureFormat::k_5_6_5)
                { o[0] = uint8_t((v & 31) * 255 / 31); o[1] = uint8_t(((v >> 5) & 63) * 255 / 63); o[2] = uint8_t((v >> 11) * 255 / 31); o[3] = 255; }
                else if (f.format == xenos::TextureFormat::k_4_4_4_4)
                { o[0] = uint8_t((v & 15) * 17); o[1] = uint8_t(((v >> 4) & 15) * 17); o[2] = uint8_t(((v >> 8) & 15) * 17); o[3] = uint8_t((v >> 12) * 17); }
                else
                { o[0] = uint8_t((v & 31) * 255 / 31); o[1] = uint8_t(((v >> 5) & 31) * 255 / 31); o[2] = uint8_t(((v >> 10) & 31) * 255 / 31); o[3] = (v >> 15) ? 255 : 0; }
            }
            upload = converted.data();
            rowPitch = bw * 4;
        }

        if (fi.blockW > 1 && !fi.convertTo8888 && (fi.dxgi == DXGI_FORMAT_BC1_UNORM || fi.dxgi == DXGI_FORMAT_BC2_UNORM || fi.dxgi == DXGI_FORMAT_BC3_UNORM))
        {
            uint64_t bhash = HdHash(blocks);
            static const bool hdLog = getenv("NBASH_HDLOG") != nullptr;
            if ((hdLog || w >= 1024) && ct.uploads == 1) Logf("[hd] game texture %ux%u dxgi %u: %016llx\n", w, h, unsigned(fi.dxgi), (unsigned long long)bhash);
            if (ID3D11ShaderResourceView* hd = HdLookup(bhash))
            {
                SafeRelease(ct.srv); SafeRelease(ct.tex); SafeRelease(ct.mipSrv); SafeRelease(ct.mipUav); SafeRelease(ct.mipTex);
                hd->AddRef(); ct.srv = hd; ct.width = 0; ct.height = 0; ct.contentHash = hsh;      // width 0: a later change of the game texture builds a normal one again
                return ct.srv;
            }
        }
        if (!ct.tex || ct.width != w || ct.height != h)
        {
            SafeRelease(ct.srv); SafeRelease(ct.tex);
            SafeRelease(ct.mipSrv); SafeRelease(ct.mipUav); SafeRelease(ct.mipTex);
            D3D11_TEXTURE2D_DESC td{};
            td.Width = fi.blockW > 1 ? bw * fi.blockW : w;
            td.Height = fi.blockH > 1 ? bh * fi.blockH : h;
            td.MipLevels = 1; td.ArraySize = 1; td.Format = fi.dxgi; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &ct.tex))) return nullptr;
            g_dev->CreateShaderResourceView(ct.tex, nullptr, &ct.srv);
            ct.width = w; ct.height = h;
        }
        g_ctx->UpdateSubresource(ct.tex, 0, nullptr, upload, rowPitch, 0);
        DumpTexture(hsh, w, h, fi, upload, rowPitch);
        ct.contentHash = hsh;
        static const bool texLog = getenv("NBASH_TEXLOG") != nullptr;
        if (texLog && ct.uploads == 1)
            Logf("[tex] %08X %ux%u fmt %u mips %u-%u mipfilt %u min %u mag %u aniso %u packed %u mipaddr %08X want %d\n", base, w, h, unsigned(f.format),
                 unsigned(f.mip_min_level), unsigned(f.mip_max_level), unsigned(f.mip_filter), unsigned(f.min_filter), unsigned(f.mag_filter),
                 unsigned(f.aniso_filter), unsigned(f.packed_mips), unsigned(f.mip_address) << 12, int(WantsMips(f, fi.dxgi, w, h)));
        if (WantsMips(f, fi.dxgi, w, h) && !BuildMips(ct, w, h)) { SafeRelease(ct.mipSrv); SafeRelease(ct.mipUav); SafeRelease(ct.mipTex); }
        return result();
    }

    // state objects cached by their description bytes (creating them per draw is slow on some drivers)
    template <typename Desc, typename Obj, typename CreateFn>
    Obj* Cached(std::unordered_map<uint64_t, Obj*>& cache, const Desc& desc, CreateFn create)
    {
        uint64_t key = 1469598103934665603ull;     // 64-bit FNV-1a of the description bytes
        for (size_t i = 0; i < sizeof(desc); i++) key = (key ^ ((const uint8_t*)&desc)[i]) * 1099511628211ull;
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
        Obj* o = nullptr;
        create(&desc, &o);
        cache[key] = o;
        return o;
    }
    std::unordered_map<uint64_t, ID3D11BlendState*> g_blendCache;
    std::unordered_map<uint64_t, ID3D11DepthStencilState*> g_depthCache;
    std::unordered_map<uint64_t, ID3D11RasterizerState*> g_rasterCache;
    std::unordered_map<uint64_t, ID3D11SamplerState*> g_samplerCache;

    ID3D11SamplerState* GetSampler(const xenos::xe_gpu_texture_fetch_t& f)
    {
        auto addr = [](xenos::ClampMode m) {
            switch (m)
            {
            case xenos::ClampMode::kRepeat: return D3D11_TEXTURE_ADDRESS_WRAP;
            case xenos::ClampMode::kMirroredRepeat: return D3D11_TEXTURE_ADDRESS_MIRROR;
            case xenos::ClampMode::kClampToBorder: case xenos::ClampMode::kMirrorClampToBorder: return D3D11_TEXTURE_ADDRESS_BORDER;
            case xenos::ClampMode::kMirrorClampToEdge: case xenos::ClampMode::kMirrorClampToHalfway: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
            default: return D3D11_TEXTURE_ADDRESS_CLAMP;
            }
        };
        D3D11_SAMPLER_DESC sd{};
        bool linMag = f.mag_filter == xenos::TextureFilter::kLinear, linMin = f.min_filter == xenos::TextureFilter::kLinear;
        bool linMip = f.mip_filter == xenos::TextureFilter::kLinear;
        sd.Filter = D3D11_FILTER(D3D11_FILTER_MIN_MAG_MIP_POINT | (linMin ? 0x10 : 0) | (linMag ? 0x4 : 0) | (linMip ? 0x1 : 0));
        sd.AddressU = addr(f.clamp_x); sd.AddressV = addr(f.clamp_y); sd.AddressW = addr(f.clamp_z);
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        // textures with mipmaps: anisotropic filtering keeps the court sharp at a slant (NBASH_ANISO=1..16, default 16)
        static const int aniso = getenv("NBASH_ANISO") ? std::min(std::max(atoi(getenv("NBASH_ANISO")), 1), 16) : 16;
        if (f.mip_filter == xenos::TextureFilter::kBaseMap || f.mip_max_level == 0) sd.MaxLOD = 0;
        else if (linMin && linMag && aniso > 1) { sd.Filter = D3D11_FILTER_ANISOTROPIC; sd.MaxAnisotropy = aniso; }
        // (getenv is slow -- it took 5 us per call, i.e. ~3 ms a frame here -- so it is read once)
        static const float minLod = getenv("NBASH_MINLOD") ? float(atof(getenv("NBASH_MINLOD"))) : 0.0f;
        sd.MinLOD = minLod;   // test: force blurrier mips
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        if (f.border_color == xenos::BorderColor::k_ABGR_White) for (float& c : sd.BorderColor) c = 1.0f;
        return Cached(g_samplerCache, sd, [](const D3D11_SAMPLER_DESC* d, ID3D11SamplerState** o) { g_dev->CreateSamplerState(d, o); });
    }

    // ---------------------------------------------------------------- render state
    D3D11_BLEND Blend(xenos::BlendFactor f)
    {
        switch (uint32_t(f))
        {
        case 0: return D3D11_BLEND_ZERO;
        case 1: return D3D11_BLEND_ONE;
        case 4: return D3D11_BLEND_SRC_COLOR;
        case 5: return D3D11_BLEND_INV_SRC_COLOR;
        case 6: return D3D11_BLEND_SRC_ALPHA;
        case 7: return D3D11_BLEND_INV_SRC_ALPHA;
        case 8: return D3D11_BLEND_DEST_COLOR;
        case 9: return D3D11_BLEND_INV_DEST_COLOR;
        case 10: return D3D11_BLEND_DEST_ALPHA;
        case 11: return D3D11_BLEND_INV_DEST_ALPHA;
        case 12: case 14: return D3D11_BLEND_BLEND_FACTOR;
        case 13: case 15: return D3D11_BLEND_INV_BLEND_FACTOR;
        case 16: return D3D11_BLEND_SRC_ALPHA_SAT;
        default: return D3D11_BLEND_ONE;
        }
    }
    D3D11_BLEND_OP BlendOp(xenos::BlendOp o)
    {
        switch (uint32_t(o))
        {
        case 1: return D3D11_BLEND_OP_SUBTRACT;
        case 2: return D3D11_BLEND_OP_MIN;
        case 3: return D3D11_BLEND_OP_MAX;
        case 4: return D3D11_BLEND_OP_REV_SUBTRACT;
        default: return D3D11_BLEND_OP_ADD;
        }
    }
    D3D11_STENCIL_OP StencilOp(xenos::StencilOp o)
    {
        static const D3D11_STENCIL_OP map[8] = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_ZERO, D3D11_STENCIL_OP_REPLACE,
            D3D11_STENCIL_OP_INCR_SAT, D3D11_STENCIL_OP_DECR_SAT, D3D11_STENCIL_OP_INVERT, D3D11_STENCIL_OP_INCR, D3D11_STENCIL_OP_DECR };
        return map[uint32_t(o) & 7];
    }

    void ApplyState(const uint32_t* regs, bool rectList, float sx, float sy)
    {
        // blend (RT0)
        reg::RB_BLENDCONTROL bc; bc.value = regs[XE_GPU_REG_RB_BLENDCONTROL0];
        uint32_t mask = regs[XE_GPU_REG_RB_COLOR_MASK] & 0xF;
        reg::RB_MODECONTROL mcs; mcs.value = regs[XE_GPU_REG_RB_MODECONTROL];
        if (mcs.edram_mode == xenos::EdramMode::kDepthOnly) mask = 0;     // depth-only pass: no color writes
        D3D11_BLEND_DESC bd{};
        auto& rt = bd.RenderTarget[0];
        rt.SrcBlend = Blend(bc.color_srcblend); rt.DestBlend = Blend(bc.color_destblend); rt.BlendOp = BlendOp(bc.color_comb_fcn);
        rt.SrcBlendAlpha = Blend(bc.alpha_srcblend); rt.DestBlendAlpha = Blend(bc.alpha_destblend); rt.BlendOpAlpha = BlendOp(bc.alpha_comb_fcn);
        // alpha factors cannot use color blend modes in D3D11
        auto alphaOnly = [](D3D11_BLEND b) {
            switch (b) { case D3D11_BLEND_SRC_COLOR: return D3D11_BLEND_SRC_ALPHA; case D3D11_BLEND_INV_SRC_COLOR: return D3D11_BLEND_INV_SRC_ALPHA;
                         case D3D11_BLEND_DEST_COLOR: return D3D11_BLEND_DEST_ALPHA; case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA; default: return b; } };
        rt.SrcBlendAlpha = alphaOnly(rt.SrcBlendAlpha); rt.DestBlendAlpha = alphaOnly(rt.DestBlendAlpha);
        rt.BlendEnable = !(rt.SrcBlend == D3D11_BLEND_ONE && rt.DestBlend == D3D11_BLEND_ZERO && rt.BlendOp == D3D11_BLEND_OP_ADD &&
                           rt.SrcBlendAlpha == D3D11_BLEND_ONE && rt.DestBlendAlpha == D3D11_BLEND_ZERO && rt.BlendOpAlpha == D3D11_BLEND_OP_ADD);
        rt.RenderTargetWriteMask = uint8_t(mask);
        ID3D11BlendState* bs = Cached(g_blendCache, bd, [](const D3D11_BLEND_DESC* d, ID3D11BlendState** o) { g_dev->CreateBlendState(d, o); });
        float factor[4];
        memcpy(factor, &regs[XE_GPU_REG_RB_BLEND_RED], 16);
        g_ctx->OMSetBlendState(bs, factor, 0xFFFFFFFF);

        // depth / stencil
        reg::RB_DEPTHCONTROL dc; dc.value = regs[XE_GPU_REG_RB_DEPTHCONTROL];
        reg::RB_STENCILREFMASK sr; sr.value = regs[XE_GPU_REG_RB_STENCILREFMASK];
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = dc.z_enable;
        dd.DepthWriteMask = dc.z_write_enable ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = D3D11_COMPARISON_FUNC(uint32_t(dc.zfunc) + 1);
        dd.StencilEnable = dc.stencil_enable;
        dd.StencilReadMask = uint8_t(sr.stencilmask); dd.StencilWriteMask = uint8_t(sr.stencilwritemask);
        dd.FrontFace = { StencilOp(dc.stencilfail), StencilOp(dc.stencilzfail), StencilOp(dc.stencilzpass), D3D11_COMPARISON_FUNC(uint32_t(dc.stencilfunc) + 1) };
        dd.BackFace = dc.backface_enable
            ? D3D11_DEPTH_STENCILOP_DESC{ StencilOp(dc.stencilfail_bf), StencilOp(dc.stencilzfail_bf), StencilOp(dc.stencilzpass_bf), D3D11_COMPARISON_FUNC(uint32_t(dc.stencilfunc_bf) + 1) }
            : dd.FrontFace;
        static const bool noDepth = getenv("NBASH_RENDER_NODEPTH") != nullptr;
        if (noDepth) { dd.DepthEnable = FALSE; dd.StencilEnable = FALSE; }
        ID3D11DepthStencilState* ds = Cached(g_depthCache, dd, [](const D3D11_DEPTH_STENCIL_DESC* d, ID3D11DepthStencilState** o) { g_dev->CreateDepthStencilState(d, o); });
        g_ctx->OMSetDepthStencilState(ds, sr.stencilref);

        // rasterizer
        reg::PA_SU_SC_MODE_CNTL sc; sc.value = regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL];
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = rectList ? D3D11_CULL_NONE : (sc.cull_front && sc.cull_back) ? D3D11_CULL_NONE
                    : sc.cull_front ? D3D11_CULL_FRONT : sc.cull_back ? D3D11_CULL_BACK : D3D11_CULL_NONE;
        rd.FrontCounterClockwise = sc.face == 0;
        rd.DepthClipEnable = TRUE;
        rd.ScissorEnable = TRUE;
        ID3D11RasterizerState* rs = Cached(g_rasterCache, rd, [](const D3D11_RASTERIZER_DESC* d, ID3D11RasterizerState** o) { g_dev->CreateRasterizerState(d, o); });
        g_ctx->RSSetState(rs);

        // scissor (window scissor, in pixels)
        uint32_t tl = regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL], br = regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
        D3D11_RECT r{ LONG((tl & 0x7FFF) * sx), LONG(((tl >> 16) & 0x7FFF) * sy), LONG((br & 0x7FFF) * sx), LONG(((br >> 16) & 0x7FFF) * sy) };
        if (g_fullW) r = { 0, 0, LONG(g_fullW), LONG(g_fullH) };
        if (r.right <= r.left || r.bottom <= r.top) r = { 0, 0, LONG(g_width), LONG(g_height) };
        r = { LONG(Sc(r.left)), LONG(Sc(r.top)), LONG(Sc(r.right)), LONG(Sc(r.bottom)) };
        g_ctx->RSSetScissorRects(1, &r);
    }

    bool CreateTargets()
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = g_rtW; td.Height = g_rtH; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_rt))) return false;
        if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_front))) return false;
        uint32_t want = getenv("NBASH_MSAA") ? uint32_t(atoi(getenv("NBASH_MSAA"))) : 2;
        for (uint32_t n = want; n > 1; n /= 2)
        {
            UINT q1 = 0, q2 = 0;
            if (SUCCEEDED(g_dev->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, n, &q1)) && q1 &&
                SUCCEEDED(g_dev->CheckMultisampleQualityLevels(DXGI_FORMAT_D24_UNORM_S8_UINT, n, &q2)) && q2)
            { g_msaa = n; break; }
        }
        if (g_msaa > 1)
        {
            D3D11_TEXTURE2D_DESC md = td;
            md.SampleDesc.Count = g_msaa; md.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(g_dev->CreateTexture2D(&md, nullptr, &g_rtMs))) g_msaa = 1;
        }
        g_dev->CreateRenderTargetView(g_msaa > 1 ? g_rtMs : g_rt, nullptr, &g_rtv);
        td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL; td.SampleDesc.Count = g_msaa;
        if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_ds))) return false;
        g_dev->CreateDepthStencilView(g_ds, nullptr, &g_dsv);
        Logf("[render] drawing at %ux%u (%.2fx the console), anti-aliasing: %ux MSAA\n", g_rtW, g_rtH, g_scale, g_msaa);
        float black[4] = { 0, 0, 0, 1 };
        g_ctx->ClearRenderTargetView(g_rtv, black);
        g_ctx->ClearDepthStencilView(g_dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        return true;
    }
}

namespace
{
    void SyncRtRegion(const D3D11_BOX& b)
    {
        if (g_msaa <= 1 || !g_rtDirty) return;
        static const bool off = getenv("NBASH_NOREGIONRESOLVE") != nullptr;
        static ID3D11VertexShader* vs = nullptr; static ID3D11PixelShader* ps = nullptr; static ID3D11RasterizerState* rs = nullptr;
        static bool tried = false;
        if (!tried && !off)
        {
            tried = true;
            std::string src = "#define N " + std::to_string(g_msaa) + "\n" + R"(
Texture2DMS<float4> ms : register(t0);
float4 vs_main(uint id : SV_VertexID) : SV_Position { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }
float4 ps_main(float4 pos : SV_Position) : SV_Target
{
    int2 p = int2(pos.xy); float4 c = 0;
    [unroll] for (int i = 0; i < N; i++) c += ms.Load(p, i);
    return c / N;
}
)";
            ID3DBlob* vb = Compile(src, "regionresolve_vs", "vs_5_0", "vs_main");
            ID3DBlob* pb = Compile(src, "regionresolve_ps", "ps_5_0", "ps_main");
            if (vb) g_dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs);
            if (pb) g_dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &ps);
            D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.ScissorEnable = TRUE; rd.DepthClipEnable = TRUE;
            g_dev->CreateRasterizerState(&rd, &rs);
            g_dev->CreateShaderResourceView(g_rtMs, nullptr, &g_rtMsSrv);
            g_dev->CreateRenderTargetView(g_rt, nullptr, &g_rtResolveRtv);
            Logf(vs && ps && rs && g_rtMsSrv && g_rtResolveRtv ? "[render] small copies resolve only their area\n" : "[render] area resolve unavailable\n");
        }
        uint64_t area = uint64_t(b.right - b.left) * (b.bottom - b.top);
        if (off || !vs || !ps || !rs || !g_rtMsSrv || !g_rtResolveRtv || area * 3 >= uint64_t(g_rtW) * g_rtH) { SyncRt(); return; }
        // g_rt stays "dirty" elsewhere, so a later full sync still averages everything
        D3D11_VIEWPORT vp{ 0, 0, float(g_rtW), float(g_rtH), 0, 1 };
        D3D11_RECT sc{ LONG(b.left), LONG(b.top), LONG(b.right), LONG(b.bottom) };
        g_ctx->OMSetRenderTargets(1, &g_rtResolveRtv, nullptr);
        g_ctx->RSSetViewports(1, &vp);
        g_ctx->RSSetScissorRects(1, &sc);
        g_ctx->RSSetState(rs);
        g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        g_ctx->OMSetDepthStencilState(nullptr, 0);
        g_ctx->IASetInputLayout(nullptr);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->VSSetShader(vs, nullptr, 0);
        g_ctx->GSSetShader(nullptr, nullptr, 0);
        g_ctx->PSSetShader(ps, nullptr, 0);
        g_ctx->PSSetShaderResources(0, 1, &g_rtMsSrv);
        g_ctx->Draw(3, 0);
        ID3D11ShaderResourceView* nullSrv = nullptr;
        g_ctx->PSSetShaderResources(0, 1, &nullSrv);
        ID3D11RenderTargetView* nullRtv = nullptr;
        g_ctx->OMSetRenderTargets(1, &nullRtv, nullptr);   // g_rt is about to be a copy source; the next draw rebinds its own targets
    }
}

// Graphics card choice: the strongest one by default -- Windows' "high performance" order
// (the same list as Settings > Display > Graphics), falling back to the card with the most
// memory of its own, preferring a separate card over one built into the processor.
// NBASH_GPU=<number or part of the name> picks one by hand, e.g. NBASH_GPU=nvidia or NBASH_GPU=1.
// Works with NVIDIA, AMD and Intel: everything is plain Direct3D 11.
static IDXGIAdapter1* PickAdapter()
{
    IDXGIFactory1* f1 = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f1))) return nullptr;
    std::vector<IDXGIAdapter1*> cards;   // hardware cards, strongest first
    IDXGIFactory6* f6 = nullptr;
    if (SUCCEEDED(f1->QueryInterface(__uuidof(IDXGIFactory6), (void**)&f6)))
    {
        IDXGIAdapter1* a = nullptr;
        for (UINT i = 0; SUCCEEDED(f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1), (void**)&a)); i++)
            cards.push_back(a);
        f6->Release();
    }
    else
    {
        IDXGIAdapter1* a = nullptr;
        for (UINT i = 0; f1->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) cards.push_back(a);
        // no preference list (old Windows): separate cards first (NVIDIA, or AMD with lots of its own
        // memory), then by memory
        auto score = [](IDXGIAdapter1* a) {
            DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
            uint64_t s = d.DedicatedVideoMemory;
            if (d.VendorId == 0x10DE) s += 1ull << 40;   // NVIDIA: always a separate card
            return s; };
        std::stable_sort(cards.begin(), cards.end(), [&](IDXGIAdapter1* x, IDXGIAdapter1* y) { return score(x) > score(y); });
    }
    f1->Release();
    // drop software renderers (Microsoft Basic Render Driver)
    std::vector<IDXGIAdapter1*> hw;
    for (auto* a : cards)
    {
        DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
        if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || d.VendorId == 0x1414) a->Release(); else hw.push_back(a);
    }
    const char* want = getenv("NBASH_GPU");
    int pick = hw.empty() ? -1 : 0; bool matched = false;
    for (size_t i = 0; i < hw.size(); i++)
    {
        DXGI_ADAPTER_DESC1 d{}; hw[i]->GetDesc1(&d);
        char name[128] = {}; WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, nullptr, nullptr);
        Logf("[render] graphics card %zu: %s (%u MB of its own)\n", i, name, unsigned(d.DedicatedVideoMemory >> 20));
        if (want)
        {
            std::string n = name, w = want;
            for (auto& c : n) c = char(tolower(c));
            for (auto& c : w) c = char(tolower(c));
            if ((isdigit((unsigned char)w[0]) && size_t(atoi(want)) == i) || (!isdigit((unsigned char)w[0]) && n.find(w) != std::string::npos)) { if (!matched) pick = int(i); matched = true; }
        }
    }
    IDXGIAdapter1* chosen = nullptr;
    for (size_t i = 0; i < hw.size(); i++) if (int(i) == pick) chosen = hw[i]; else hw[i]->Release();
    if (want && !matched) Logf("[render] NBASH_GPU=%s matches no graphics card, using the strongest\n", want);
    if (chosen) Logf("[render] using graphics card %d%s\n", pick, matched ? " (chosen with NBASH_GPU)" : " (the strongest)");
    return chosen;
}

namespace
{
    void UploadVmem(uint32_t at, const uint32_t* src, uint32_t n)
    {
        while (n)
        {
            uint32_t row = at / kVmemRowDwords, x = at % kVmemRowDwords;
            if (x == 0 && n >= kVmemRowDwords)   // whole rows in one go
            {
                uint32_t rows = n / kVmemRowDwords;
                D3D11_BOX box{ 0, row, 0, kVmemRowDwords, row + rows, 1 };
                g_ctx->UpdateSubresource(g_vmem, 0, &box, src, kVmemRowDwords * 4, 0);
                at += rows * kVmemRowDwords; src += rows * kVmemRowDwords; n -= rows * kVmemRowDwords;
                continue;
            }
            uint32_t len = std::min(n, kVmemRowDwords - x);
            D3D11_BOX box{ x, row, 0, x + len, row + 1, 1 };
            g_ctx->UpdateSubresource(g_vmem, 0, &box, src, kVmemRowDwords * 4, 0);
            at += len; src += len; n -= len;
        }
    }
}

bool Init(void* window, uint32_t width, uint32_t height)
{
    g_width = width; g_height = height;
    g_scale = getenv("NBASH_RES_SCALE") ? std::min(std::max(float(atof(getenv("NBASH_RES_SCALE"))), 1.0f), 4.0f) : 2.0f;   // default 2560x1440
    g_rtW = Sc(int(width)); g_rtH = Sc(int(height));
    g_hwnd = (HWND)window;
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    g_compile = dc ? (D3DCompileFn)GetProcAddress(dc, "D3DCompile") : nullptr;
    g_createBlob = dc ? (D3DCreateBlobFn)GetProcAddress(dc, "D3DCreateBlob") : nullptr;
    if (!g_compile) { Logf("[render] d3dcompiler_47.dll not found\n"); return false; }

    D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0, got;
    HRESULT hr;
    if (window)
    {
        DXGI_SWAP_CHAIN_DESC sd{};
        RECT cr{};   // the window's own size: the picture is scaled to it by Present
        GetClientRect((HWND)window, &cr);
        sd.BufferDesc.Width = std::max<LONG>(cr.right - cr.left, 64); sd.BufferDesc.Height = std::max<LONG>(cr.bottom - cr.top, 64);
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.OutputWindow = (HWND)window;
        sd.Windowed = TRUE;
        // 14 Nov (GPU optimisation): FLIP model swap chain (Windows 10+). The old "blt" model made Windows copy every frame once more to put it on
        // screen; flip hands the buffer over (and in full screen the display can scan it out directly). Falls back to the old model if refused.
        sd.SwapEffect = getenv("NBASH_BLTSWAP") ? DXGI_SWAP_EFFECT_DISCARD : DXGI_SWAP_EFFECT_FLIP_DISCARD;
        IDXGIAdapter1* adapter = PickAdapter();
        hr = D3D11CreateDeviceAndSwapChain(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &want, 1,
                                           D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
        if (FAILED(hr) && sd.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD)
        {
            Logf("[render] flip-model swap chain refused (%08lX), using the old one\n", hr);
            sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            hr = D3D11CreateDeviceAndSwapChain(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &want, 1,
                                               D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
        }
        else if (SUCCEEDED(hr)) Logf("[render] swap chain: %s\n", sd.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ? "flip model" : "blt model (NBASH_BLTSWAP)");
        if (FAILED(hr) && adapter)   // the chosen card refused: let Windows pick
        {
            Logf("[render] could not start on the chosen graphics card (%08lX), using the Windows default\n", hr);
            hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &want, 1, D3D11_SDK_VERSION,
                                               &sd, &g_swap, &g_dev, &got, &g_ctx);
        }
        if (adapter) adapter->Release();
    }
    else
    {
        IDXGIAdapter1* adapter = PickAdapter();
        hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &want, 1, D3D11_SDK_VERSION, &g_dev, &got, &g_ctx);
        if (FAILED(hr) && adapter) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &want, 1, D3D11_SDK_VERSION, &g_dev, &got, &g_ctx);
        if (adapter) adapter->Release();
    }
    if (FAILED(hr)) { Logf("[render] Direct3D 11 device creation failed (%08lX)\n", hr); return false; }
    {
        IDXGIDevice* xd = nullptr; IDXGIAdapter* ad = nullptr; DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&xd)) && SUCCEEDED(xd->GetAdapter(&ad)) && SUCCEEDED(ad->GetDesc(&desc)))
        {
            char name[128] = {}; WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof name, nullptr, nullptr);
            Logf("[render] graphics card: %s (%u MB)\n", name, unsigned(desc.DedicatedVideoMemory >> 20));
        }
        if (ad) ad->Release(); if (xd) xd->Release();
    }
    // 14 Nov (GPU optimisation): at most one frame queued ahead (less waiting in the driver, less lag) ...
    { IDXGIDevice1* d1 = nullptr; if (SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&d1))) { d1->SetMaximumFrameLatency(1); d1->Release(); } }
    // ... and graphics built into the processor (shared system memory: AMD Radeon 7xxM / 8xxM, Intel) draw at 1.5x (1920x1080) by default instead of
    // 2x (2560x1440); separate cards keep 2x. NBASH_RES_SCALE still decides when it is set.
    {
        D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2{};
        const bool uma = SUCCEEDED(g_dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof o2)) && o2.UnifiedMemoryArchitecture;
        if (uma && !getenv("NBASH_RES_SCALE")) { g_scale = 1.5f; g_rtW = Sc(int(width)); g_rtH = Sc(int(height)); }
        Logf("[render] graphics memory: %s\n", uma ? "shared with the system (built-in graphics)" : "its own (separate card)");
    }
    if (!CreateTargets()) return false;

    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = 512 * 16;
    g_dev->CreateBuffer(&bd, nullptr, &g_cbFloat);
    bd.ByteWidth = (sizeof(SystemConstants) + 15) & ~15u;
    g_dev->CreateBuffer(&bd, nullptr, &g_cbSystem);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = kVmemRowDwords; td.Height = kVmemRows; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_UINT; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    g_dev->CreateTexture2D(&td, nullptr, &g_vmem);
    g_dev->CreateShaderResourceView(g_vmem, nullptr, &g_vmemSrv);

    Logf("[render] Direct3D 11 ready (%ux%u)\n", width, height);
    return true;
}

namespace { double g_phase[8]; LARGE_INTEGER g_pt; void Mark(int i) { LARGE_INTEGER n; QueryPerformanceCounter(&n); g_phase[i] += double(n.QuadPart - g_pt.QuadPart); g_pt = n; } }
static uint64_t g_drawCounter = 0;
void Draw(const DrawCall& d)
{
    g_drawCounter++;
    QueryPerformanceCounter(&g_pt);
    if (!g_dev) return;
    const uint32_t* regs = d.regs;
    {
        uint32_t w = 0, h = 0;
        int tc = TileCollapseState(regs, true, w, h);
        if (tc == 2) return;
        g_fullW = tc == 1 ? w : 0; g_fullH = tc == 1 ? h : 0;
    }
    reg::RB_MODECONTROL mc; mc.value = regs[XE_GPU_REG_RB_MODECONTROL];

    // Resolve / copy pass: the frame so far becomes the picture to show, then the clears run.
    // (Resolving into textures the game samples later comes with EDRAM emulation.)
    if (mc.edram_mode == xenos::EdramMode::kCopy)
    {
        reg::RB_COPY_CONTROL cc; cc.value = regs[XE_GPU_REG_RB_COPY_CONTROL];
        if (cc.copy_command != xenos::CopyCommand::kNull) Resolve(regs);
        // the "last finished picture" fallback for Present: only full-frame copies matter
        uint32_t pr = regs[XE_GPU_REG_RB_COPY_DEST_PITCH];
        if (g_needFallback && (pr & 0x3FFF) >= g_width && ((pr >> 16) & 0x3FFF) >= g_height) { SyncRt(); g_ctx->CopyResource(g_front, g_rt); }
        if (cc.color_clear_enable)
        {
            uint32_t c = regs[XE_GPU_REG_RB_COLOR_CLEAR];   // ARGB
            float col[4] = { ((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, (c >> 24) / 255.0f };
            g_ctx->ClearRenderTargetView(g_rtv, col);
            g_rtDirty = true;
        }
        if (cc.depth_clear_enable)
        {
            uint32_t z = regs[XE_GPU_REG_RB_DEPTH_CLEAR];
            reg::RB_DEPTH_INFO di; di.value = regs[XE_GPU_REG_RB_DEPTH_INFO];
            float depth = di.depth_format == xenos::DepthRenderTargetFormat::kD24FS8
                ? xenos::Float20e4To32(z >> 8) : (z >> 8) / 16777215.0f;
            g_ctx->ClearDepthStencilView(g_dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, std::min(std::max(depth, 0.0f), 1.0f), uint8_t(z));
        }
        return;
    }
    if (mc.edram_mode != xenos::EdramMode::kColorDepth && mc.edram_mode != xenos::EdramMode::kDepthOnly) return;

    uint32_t prim = d.initiator & 0x3F, source = (d.initiator >> 6) & 3, count = d.initiator >> 16;
    if (!count || !d.vs || !d.ps) return;
    CachedShader* vs = GetShader(false, d.vsHash, *d.vs);
    CachedShader* ps = GetShader(true, d.psHash, *d.ps);
    static const bool drawLog = getenv("NBASH_DRAWLOG") != nullptr;
    if (!vs || !ps)
    {
        if (drawLog) Logf("[draw %llu] skipped: shader failed (vs %p ps %p)\n", (unsigned long long)(g_drawCounter - 1), (void*)vs, (void*)ps);
        return;
    }
    const bool exports = vs->shader->memexport_eM_written() != 0;
    if (g_exportPending && !exports) FlushExports();     // later draws may read what was exported

    Mark(0);
    // ---- indices (read now so the vertex range is known)
    static std::vector<uint32_t> indices;     // reused between draws (no allocation per draw)
    indices.clear();
    uint32_t minIndex = 0, maxIndex = count - 1;
    if (source == 0)   // DMA
    {
        bool idx32 = (d.initiator >> 11) & 1;
        uint32_t endian = d.dmaSize >> 30;
        uint32_t base = d.dmaBase & 0x1FFFFFFF;
        indices.resize(count);
        minIndex = 0xFFFFFFFF; maxIndex = 0;
        if (idx32)
        {
            memcpy(indices.data(), PhysToHost(base), size_t(count) * 4);
            for (uint32_t& v : indices) { v = Swap32(v, endian); minIndex = std::min(minIndex, v); maxIndex = std::max(maxIndex, v); }
        }
        else
        {
            const uint16_t* src = (const uint16_t*)PhysToHost(base);
            const bool swap = endian == 1 || endian == 2;
            for (uint32_t i = 0; i < count; i++)
            {
                uint16_t sv = src[i]; uint32_t v = swap ? __builtin_bswap16(sv) : sv;
                indices[i] = v; minIndex = std::min(minIndex, v); maxIndex = std::max(maxIndex, v);
            }
        }
    }
    else if (source != 2)
    {
        static bool warned = false;
        if (!warned) { warned = true; Logf("[render] immediate-index draws not supported yet\n"); }
        return;
    }

    Mark(1);
    // ---- constants
    SystemConstants sys{};
    memcpy(sys.bools, &regs[0x4900], sizeof(sys.bools));
    memcpy(sys.loops, &regs[0x4908], sizeof(sys.loops));
    uint32_t indexOffset = regs[XE_GPU_REG_VGT_INDX_OFFSET];

    // vertex data: copy the dwords this draw can reach into the vmem texture
    uint32_t row = g_vmemNextRow;
    for (const auto& vb : vs->shader->vertex_bindings())
    {
        uint32_t fc = vb.fetch_constant;
        uint32_t d0 = regs[0x4800 + fc * 2], d1 = regs[0x4800 + fc * 2 + 1];
        uint32_t addr = d0 >> 2, size = (d1 >> 2) & 0xFFFFFF, endian = d1 & 3;
        uint32_t stride = vb.stride_words ? vb.stride_words : 1;
        uint64_t first = uint64_t(minIndex + indexOffset) * stride, last = uint64_t(maxIndex + indexOffset + 1) * stride + 16;
        if (first >= size) first = 0;
        if (last > size) last = size;
        uint32_t n = uint32_t(last - first);
        uint32_t guestStart = (addr + uint32_t(first)) * 4;
        // exported this frame? then the data lives in the arena: copy whole rows on the GPU, keeping
        // the in-row position (x0) so source and destination line up
        uint32_t x0 = 0, srcRow = 0;
        bool fromArena = ArenaLocate(guestStart, n * 4, srcRow, x0);
        uint32_t rows = (x0 + n + kVmemRowDwords - 1) / kVmemRowDwords;
        if (!n || rows > kVmemRows - kVmemCacheRows) continue;
        if (row + rows > kVmemRows) row = kVmemCacheRows;
        static const bool noCache = getenv("NBASH_NOVCACHE") != nullptr;
        if (!fromArena && !noCache)
        {
            uint64_t key = (uint64_t(guestStart) << 32) | n;
            auto& e = g_vcache[key];
            const uint32_t* src = (const uint32_t*)PhysToHost(guestStart);
            bool kept = e.offset >= 0;
            if (!kept && e.seenFrame == g_frame)   // stored as frame+1: equal to g_frame = seen last frame
            {
                // seen in the previous (or this) frame too: give it a permanent place
                if (g_vcacheUsed + n > kVmemCacheRows * kVmemRowDwords)
                {
                    for (auto& [k, v] : g_vcache) { v.offset = -1; v.shadow.clear(); v.shadow.shrink_to_fit(); }
                    g_vcacheUsed = 0;
                    Logf("[render] kept vertex data area full: starting it over\n");
                }
                e.offset = g_vcacheUsed; g_vcacheUsed += (n + 3) & ~3u;
                e.shadow.assign(src, src + n);
                UploadVmem(uint32_t(e.offset), src, n);
                g_vcacheUploads++;
                kept = true;
            }
            else if (kept)
            {
                if (memcmp(e.shadow.data(), src, size_t(n) * 4)) { memcpy(e.shadow.data(), src, size_t(n) * 4); UploadVmem(uint32_t(e.offset), src, n); g_vcacheUploads++; }
                else g_vcacheHits++;
            }
            e.seenFrame = g_frame + 1;   // +1 so that 0 means "never"
            if (kept)
            {
                sys.vfetch[fc][0] = addr;
                sys.vfetch[fc][1] = n | (endian << 30);
                sys.vfetch[fc][2] = uint32_t(e.offset);
                sys.vfetch[fc][3] = uint32_t(first);
                continue;
            }
            g_vcacheRing++;
        }
        if (fromArena)
        {
            D3D11_BOX box{ 0, srcRow, 0, kVmemRowDwords, srcRow + rows, 1 };
            g_ctx->CopySubresourceRegion(g_vmem, 0, 0, row, 0, g_arena, 0, &box);
        }
        else
        {
            static std::vector<uint32_t> tmp; if (tmp.size() < size_t(rows) * kVmemRowDwords) tmp.resize(size_t(rows) * kVmemRowDwords);
            static const bool vstat = getenv("NBASH_VSTAT") != nullptr;
            if (vstat)
            {
                static std::map<std::pair<uint32_t, uint32_t>, uint64_t> seen; static uint64_t fr = ~0ull; static uint32_t total, same, bytesAll, bytesSame;
                if (fr != g_frame) { if (fr != ~0ull) Logf("[vstat] frame %llu: %u uploads, %u repeats (same range+content), %u KB, %u KB repeated\n", (unsigned long long)fr, total, same, bytesAll >> 10, bytesSame >> 10); seen.clear(); fr = g_frame; total = same = bytesAll = bytesSame = 0; }
                uint64_t h = 1469598103934665603ull; const uint8_t* p = PhysToHost(guestStart);
                for (uint32_t i = 0; i < n * 4; i += 8) { uint64_t v = 0; memcpy(&v, p + i, std::min(8u, n * 4 - i)); h = (h ^ v) * 1099511628211ull; }
                auto& e = seen[{ guestStart, n }];
                total++; bytesAll += n * 4;
                if (e == h) { same++; bytesSame += n * 4; }
                e = h;
            }
            memcpy(tmp.data(), PhysToHost(guestStart), size_t(n) * 4);
            // upload just the n dwords: whole rows, then the part of the last row that is used
            if (rows > 1)
            {
                D3D11_BOX box{ 0, row, 0, kVmemRowDwords, row + rows - 1, 1 };
                g_ctx->UpdateSubresource(g_vmem, 0, &box, tmp.data(), kVmemRowDwords * 4, 0);
            }
            uint32_t tail = n - (rows - 1) * kVmemRowDwords;
            D3D11_BOX box{ 0, row + rows - 1, 0, tail, row + rows, 1 };
            g_ctx->UpdateSubresource(g_vmem, 0, &box, tmp.data() + size_t(rows - 1) * kVmemRowDwords, kVmemRowDwords * 4, 0);
        }
        sys.vfetch[fc][0] = addr;
        sys.vfetch[fc][1] = n | (endian << 30);
        sys.vfetch[fc][2] = row * kVmemRowDwords + x0;
        sys.vfetch[fc][3] = uint32_t(first);
        row += rows;
    }
    g_vmemNextRow = row;

    Mark(2);
    // textures
    ID3D11ShaderResourceView* srvs[kMaxTextures] = {};
    ID3D11SamplerState* samplers[kMaxTextures] = {};
    for (CachedShader* s : { vs, ps })
        for (const auto& tb : s->shader->texture_bindings())
        {
            uint32_t tf = tb.fetch_constant;
            if (tf >= kMaxTextures || srvs[tf]) continue;
            xenos::xe_gpu_texture_fetch_t f;
            memcpy(&f, &regs[0x4800 + tf * 6], sizeof(f));
            srvs[tf] = GetTexture(f, sys.tsize[tf]);
            sys.tinfo[tf][0] = f.swizzle;
            if (g_lastSwapRB)   // the copy was red/blue swapped: swap component 0 and 2 in the swizzle
            {
                uint32_t sw = 0;
                for (int k = 0; k < 4; k++) { uint32_t c = (f.swizzle >> (3 * k)) & 7; c = c == 0 ? 2 : c == 2 ? 0 : c; sw |= c << (3 * k); }
                sys.tinfo[tf][0] = sw;
            }
            samplers[tf] = GetSampler(f);
        }
    // flash diagnostics: remember what the final colour-grading pass (ps 14284948471810615086)
    // bound this frame; CheckSceneBlack logs it if the shown picture turns dark
    if (d.psHash == 14284948471810615086ull)
    {
        std::string note;
        for (uint32_t tf = 0; tf < kMaxTextures; tf++)
        {
            xenos::xe_gpu_texture_fetch_t f;
            memcpy(&f, &regs[0x4800 + tf * 6], sizeof(f));
            if (uint32_t(f.type) != 2) continue;
            uint32_t base = f.base_address << 12;
            char buf[160];
            int n = snprintf(buf, sizeof buf, " t%u %08X%s %s", tf, base, f.dimension == xenos::DataDimension::k3D ? "(3D)" : "",
                             srvs[tf] ? "ok" : "NONE");
            if (f.dimension == xenos::DataDimension::k3D && base + 4096 <= 0x20000000u)
            {
                const uint8_t* p = PhysToHost(base);
                uint32_t nz = 0; for (int i = 0; i < 4096; i++) nz += p[i] != 0;
                snprintf(buf + n, sizeof buf - n, " [mem nonzero %u/4096, first %02X%02X%02X%02X]", nz, p[0], p[1], p[2], p[3]);
            }
            note += buf;
        }
        char hdr[64]; snprintf(hdr, sizeof hdr, "frame %llu draw %llu:", (unsigned long long)g_frame, (unsigned long long)(g_drawCounter - 1));
        g_compositeNotes[g_frame % 8] = hdr + note;
    }

    Mark(3);
    reg::PA_CL_VTE_CNTL vte; vte.value = regs[XE_GPU_REG_PA_CL_VTE_CNTL];
    memcpy(&sys.vpScale[0], &regs[XE_GPU_REG_PA_CL_VPORT_XSCALE], 4);
    memcpy(&sys.vpOffset[0], &regs[XE_GPU_REG_PA_CL_VPORT_XOFFSET], 4);
    memcpy(&sys.vpScale[1], &regs[XE_GPU_REG_PA_CL_VPORT_YSCALE], 4);
    memcpy(&sys.vpOffset[1], &regs[XE_GPU_REG_PA_CL_VPORT_YOFFSET], 4);
    memcpy(&sys.vpScale[2], &regs[XE_GPU_REG_PA_CL_VPORT_ZSCALE], 4);
    memcpy(&sys.vpOffset[2], &regs[XE_GPU_REG_PA_CL_VPORT_ZOFFSET], 4);
    sys.flags[0] = vte.value;
    sys.flags[1] = indexOffset;
    reg::SQ_PROGRAM_CNTL pc; pc.value = regs[XE_GPU_REG_SQ_PROGRAM_CNTL];
    reg::SQ_CONTEXT_MISC misc; misc.value = regs[XE_GPU_REG_SQ_CONTEXT_MISC];
    sys.flags[2] = pc.param_gen ? (1u | ((misc.param_gen_pos & 15) << 1)) : 0;
    reg::RB_COLORCONTROL cc; cc.value = regs[XE_GPU_REG_RB_COLORCONTROL];
    sys.flags[3] = uint32_t(cc.alpha_func) | (cc.alpha_test_enable ? 8u : 0u);
    sys.rtSize[0] = float(g_width); sys.rtSize[1] = float(g_height); sys.rtSize[2] = 2.0f / g_width; sys.rtSize[3] = 2.0f / g_height;
    memcpy(&sys.alphaRef[0], &regs[XE_GPU_REG_RB_ALPHA_REF], 4);
    // A surface with MSAA covers 2 (2x: vertical) or 2x2 (4x) host pixels per guest pixel:
    // D3D fast-clears the screen by drawing into a half-size 4x MSAA view of the same EDRAM.
    reg::RB_SURFACE_INFO si; si.value = regs[XE_GPU_REG_RB_SURFACE_INFO];
    float sx = si.msaa_samples == xenos::MsaaSamples::k4X ? 2.0f : 1.0f;
    float sy = si.msaa_samples != xenos::MsaaSamples::k1X ? 2.0f : 1.0f;
    // experiment switch: 0 = scale everything (old), 1 = only pre-transformed positions, 2 = none
    // Geometry and scissors are already in full-frame pixels; only pre-transformed rectangles
    // (D3D's fast clears and resolve quads) are given in MSAA-surface units and need the scale.
    static const int msaaMode = getenv("NBASH_MSAA_MODE") ? atoi(getenv("NBASH_MSAA_MODE")) : 1;
    bool preTransformed = (regs[XE_GPU_REG_PA_CL_VTE_CNTL] >> 8) & 1;
    if (msaaMode == 2 || (msaaMode == 1 && !preTransformed)) sx = sy = 1.0f;
    sys.alphaRef[2] = sx; sys.alphaRef[3] = sy;
    sys.alphaRef[1] = 1.0f / g_scale;     // pixel shaders' screen position -> console pixels
    g_scissorScaleX = msaaMode == 0 ? sx : 1.0f; g_scissorScaleY = msaaMode == 0 ? sy : 1.0f;

    D3D11_MAPPED_SUBRESOURCE m;
    // upload the constants only when they changed since the last draw (most draws share them)
    {
        static uint32_t lastFloat[512 * 4]; static SystemConstants lastSys; static bool valid = false;
        if (!valid || memcmp(lastFloat, &regs[0x4000], sizeof(lastFloat)))
        {
            memcpy(lastFloat, &regs[0x4000], sizeof(lastFloat));
            if (SUCCEEDED(g_ctx->Map(g_cbFloat, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, lastFloat, sizeof(lastFloat)); g_ctx->Unmap(g_cbFloat, 0); }
        }
        if (!valid || memcmp(&lastSys, &sys, sizeof(sys)))
        {
            lastSys = sys;
            if (SUCCEEDED(g_ctx->Map(g_cbSystem, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &sys, sizeof(sys)); g_ctx->Unmap(g_cbSystem, 0); }
        }
        valid = true;
    }
    static const bool noExport = getenv("NBASH_NOMEMEXPORT") != nullptr;
    if (exports && !noExport)
    {
        if (source == 2) RunExport(*vs, count, regs, srvs, samplers, g_cbFloat, g_cbSystem, g_vmemSrv);
        else { static bool warned = false; if (!warned) { warned = true; Logf("[render] indexed memory-export draw not supported yet\n"); } }
    }

    Mark(4);
    // ---- primitive type -> host topology (+ index rewriting for quads and fans)
    bool rectList = false;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    auto idx = [&](uint32_t i) { return indices.empty() ? i : indices[i]; };
    static std::vector<uint32_t> rewritten;
    rewritten.clear();
    switch (prim)
    {
    case 1: topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; break;
    case 2: topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; break;
    case 3: topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; break;
    case 4: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
    case 5:   // triangle fan
        topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        for (uint32_t i = 2; i < count; i++) { rewritten.push_back(idx(0)); rewritten.push_back(idx(i - 1)); rewritten.push_back(idx(i)); }
        break;
    case 6: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
    case 8: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; rectList = true; break;
    case 13:  // quad list
        topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        for (uint32_t q = 0; q + 3 < count; q += 4)
            for (uint32_t k : { 0u, 1u, 2u, 0u, 2u, 3u }) rewritten.push_back(idx(q + k));
        break;
    default:
    {
        static uint32_t warned = 0;
        if (!(warned & (1u << (prim & 31)))) { warned |= 1u << (prim & 31); Logf("[render] primitive type %u not supported yet\n", prim); }
        return;
    }
    }
    if (!rewritten.empty()) indices.swap(rewritten);

    Mark(5);
    // ---- bind and draw
    ApplyState(regs, rectList, g_scissorScaleX, g_scissorScaleY);
    D3D11_VIEWPORT vp{ 0, 0, float(g_rtW), float(g_rtH), 0, 1 };
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->OMSetRenderTargets(1, &g_rtv, g_dsv);
    g_rtDirty = true;
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(topo);
    ID3D11Buffer* cbs[2] = { g_cbFloat, g_cbSystem };
    g_ctx->VSSetConstantBuffers(0, 2, cbs);
    g_ctx->PSSetConstantBuffers(0, 2, cbs);
    ID3D11VertexShader* vsObj = vs->vs;
    static const bool rectVs = getenv("NBASH_RECTVS") != nullptr;   // old path: whole shader x3 in one VS
    ID3D11GeometryShader* gsObj = nullptr;
    if (rectList)
    {
        if (!indices.empty()) return;                     // indexed rectangle lists: not seen yet
        if (rectVs) { vsObj = GetRectShader(*vs); if (!vsObj) return; }
        else
        {
            static ID3D11GeometryShader* rectGs = nullptr;
            static bool rectGsFailed = false;
            if (!rectGs && !rectGsFailed)
            {
                ID3DBlob* blob = Compile("#define XE_GS 1\n" + vs->hlsl, "gs_rect", "gs_5_0", "gs_rect");
                if (!blob || FAILED(g_dev->CreateGeometryShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &rectGs)))
                { rectGsFailed = true; Logf("[render] rectangle geometry shader failed\n"); }
                if (blob) blob->Release();
            }
            gsObj = rectGs;
            if (!gsObj) { vsObj = GetRectShader(*vs); if (!vsObj) return; }
        }
    }
    g_ctx->VSSetShader(vsObj, nullptr, 0);
    g_ctx->GSSetShader(gsObj, nullptr, 0);
    if (gsObj) g_ctx->GSSetConstantBuffers(0, 2, cbs);
    static ID3D11PixelShader* debugPs = nullptr;
    static const char* psDebug = getenv("NBASH_RENDER_PSDEBUG");   // 1 = magenta, 2 = texture only, 3 = interpolator 1
    if (psDebug && !debugPs)
    {
        std::string src = std::string(kCommonHlsl) + "Texture2D xt0 : register(t0); SamplerState xs0 : register(s0);\n"
            "float4 main(XeVertexOut i) : SV_Target { " +
            (psDebug[0] == '1' ? "return float4(1, 0, 1, 1);" : psDebug[0] == '2' ? "return xt0.Sample(xs0, i.t0.xy);" :
             psDebug[0] == '4' ? "return float4(frac(i.pos.z * 8), frac(i.pos.z * 64), i.pos.z, 1);" :
             psDebug[0] == '6' ? "return float4((i.t0.y != i.t0.y) ? 1.0 : 0.0, (i.t1.x != i.t1.x) ? 1.0 : 0.0, (i.t2.x != i.t2.x) ? 1.0 : 0.0, 1);" :
             psDebug[0] == '5' ? "return float4((i.t0.x != i.t0.x) ? 1.0 : 0.0, frac(abs(i.t0.x)), frac(abs(i.t0.y)), 1);" :
             psDebug[0] == '7' ? "return float4(saturate(i.t0.x), saturate(i.t0.y), saturate(i.t0.z), 1);" :
             psDebug[0] == '8' ? "return float4(saturate(i.t1.x), saturate(i.t1.y), saturate(i.t1.z), 1);" :
             psDebug[0] == 'a' ? "uint ux = asuint(i.t0.x), uy = asuint(i.t0.y); bool nx = (ux & 0x7fffffffu) > 0x7f800000u, ny = (uy & 0x7fffffffu) > 0x7f800000u;"
                                 " return float4(nx ? 1.0 : saturate(abs(i.t0.x)), ny ? 1.0 : saturate(abs(i.t0.y)), (nx || ny) ? 1.0 : 0.0, 1);" :
             psDebug[0] == 'b' ? "uint ux = asuint(i.t0.x), uy = asuint(i.t0.y);"
                                 " return float4(float((ux >> 23) & 255u) / 255.0, float((uy >> 23) & 255u) / 255.0, float(((ux >> 31) & 1u) * 2u + ((uy >> 31) & 1u)) / 3.0, 1);" :
             psDebug[0] == 'c' ? "uint3 u = asuint(i.t0.xyz); return float4(float3((u >> 23) & 255u) / 255.0, 1);" :
             psDebug[0] == '9' ? "return float4(saturate(abs(i.t2.x)), saturate(abs(i.t2.x) / 16), (i.t2.x != i.t2.x) ? 1.0 : 0.0, 1);" : "return i.t1;") + " }";
        if (ID3DBlob* b = Compile(src, "debug_ps", "ps_5_0")) { g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &debugPs); b->Release(); }
    }
    static const uint64_t debugFrom = getenv("NBASH_RENDER_PSDEBUG_FROM") ? strtoull(getenv("NBASH_RENDER_PSDEBUG_FROM"), nullptr, 10) : 0;
    static const uint64_t debugTo = getenv("NBASH_RENDER_PSDEBUG_TO") ? strtoull(getenv("NBASH_RENDER_PSDEBUG_TO"), nullptr, 10) : ~0ull;
    bool useDebug = debugPs && g_drawCounter - 1 >= debugFrom && g_drawCounter - 1 <= debugTo;
    // CLEAN COMPOSITE (12 Nov): on a court the game's last full-screen pass (ps 14284948471810615086) lays the colour cube, a pink sky haze, bloom and a
    // heavy vignette over a scene that is clean daylight before it (F12 captures: target 18003000 vs the finished frame).  While a court cube is bound,
    // that pass is replaced by a plain copy of the scene texture (t0); the look is then added by the exe's own final pass (CourtLook).  NBASH_CLEANCOMP=0 off.
    static const bool cleanOn = [] { const char* v = getenv("NBASH_CLEANCOMP"); return !(v && *v == '0'); }();
    static ID3D11PixelShader* cleanPs = nullptr; static bool cleanTried = false;
    bool cleanNow = cleanOn && !useDebug && d.psHash == 14284948471810615086ull && g_courtLutFrame == g_frame;
    if (cleanNow && !cleanTried)
    {
        cleanTried = true;
        std::string src = std::string(kCommonHlsl) + "Texture2D xt0 : register(t0); SamplerState xs0 : register(s0);\n"
            "float4 main(XeVertexOut i) : SV_Target { return float4(xt0.Sample(xs0, i.t0.xy).rgb, 1); }";
        if (ID3DBlob* b = Compile(src, "clean_composite_ps", "ps_5_0")) { g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &cleanPs); b->Release(); }
        std::string note;
        for (uint32_t tf = 0; tf < 6; tf++)
        {
            xenos::xe_gpu_texture_fetch_t f; memcpy(&f, &regs[0x4800 + tf * 6], sizeof(f)); if (uint32_t(f.type) != 2) continue;
            char buf[64]; snprintf(buf, sizeof buf, " t%u %08X%s", tf, uint32_t(f.base_address) << 12, f.dimension == xenos::DataDimension::k3D ? "(3D)" : ""); note += buf;
        }
        Logf("[render] clean composite %s; the game's composite reads:%s\n", cleanPs ? "ON" : "could not be built", note.c_str());
    }
    if (cleanNow && cleanPs) g_cleanCompFrame = g_frame;
    g_ctx->PSSetShader(useDebug ? debugPs : (cleanNow && cleanPs) ? cleanPs : ps->ps, nullptr, 0);
    g_ctx->VSSetShaderResources(0, kMaxTextures, srvs);
    g_ctx->VSSetShaderResources(16, 1, &g_vmemSrv);
    g_ctx->VSSetSamplers(0, kMaxTextures, samplers);
    g_ctx->PSSetShaderResources(0, kMaxTextures, srvs);
    g_ctx->PSSetSamplers(0, kMaxTextures, samplers);

    Mark(6);
    static const bool stats = getenv("NBASH_RENDER_STATS") != nullptr;
    ID3D11Query* q = nullptr;
    if (stats)
    {
        D3D11_QUERY_DESC qd{ D3D11_QUERY_PIPELINE_STATISTICS, 0 };
        g_dev->CreateQuery(&qd, &q);
        g_ctx->Begin(q);
    }
    struct StatsEnd { ID3D11Query*& q; uint32_t prim, count; ~StatsEnd() {
        if (!q) return;
        g_ctx->End(q);
        D3D11_QUERY_DATA_PIPELINE_STATISTICS st{};
        while (g_ctx->GetData(q, &st, sizeof(st), 0) == S_FALSE) {}
        Logf("[render] draw #%llu prim %u count %u: vs %llu, prims %llu -> %llu, ps %llu\n", (unsigned long long)(g_drawCounter - 1), prim, count,
             (unsigned long long)st.VSInvocations, (unsigned long long)st.CInvocations, (unsigned long long)st.CPrimitives, (unsigned long long)st.PSInvocations);
        q->Release(); } } statsEnd{ q, prim, count };
    if (rectList && gsObj)
        g_ctx->Draw(count / 3 * 3, 0);
    else if (rectList)
        g_ctx->Draw(count / 3 * 6, 0);
    else if (indices.empty())
        g_ctx->Draw(count, 0);
    else
    {
        uint32_t bytes = uint32_t(indices.size() * 4);
        if (bytes > g_ibSize)
        {
            SafeRelease(g_ib);
            g_ibSize = std::max<uint32_t>(bytes, 16u << 20);
            D3D11_BUFFER_DESC bd{};
            bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_INDEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; bd.ByteWidth = g_ibSize;
            g_dev->CreateBuffer(&bd, nullptr, &g_ib);
            g_ibOffset = g_ibSize;    // forces a DISCARD on first use
        }
        D3D11_MAP mapType = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (g_ibOffset + bytes > g_ibSize) { mapType = D3D11_MAP_WRITE_DISCARD; g_ibOffset = 0; }
        uint32_t start = g_ibOffset;
        if (SUCCEEDED(g_ctx->Map(g_ib, 0, mapType, 0, &m))) { memcpy((uint8_t*)m.pData + start, indices.data(), bytes); g_ctx->Unmap(g_ib, 0); }
        g_ibOffset += bytes;
        g_ctx->IASetIndexBuffer(g_ib, DXGI_FORMAT_R32_UINT, 0);
        g_ctx->DrawIndexed(uint32_t(indices.size()), start / 4, 0);
    }
    Mark(7);
}

bool ExportedOverlap(uint32_t addr, uint32_t bytes)
{
    addr &= 0x1FFFFFFF;
    for (auto& r : g_exportedRanges) if (addr < r.second && addr + bytes > r.first) return true;
    return false;
}

int TileCollapseQuery(const uint32_t* regs)
{
    uint32_t w, h;
    return TileCollapseState(regs, false, w, h);
}

void EndFrame()
{
    g_tiledPrev.swap(g_tiledCur);
    g_tiledCur.clear();
    static bool logged = false;
    if (!logged && !g_tiledPrev.empty() && TileCollapseEnabled())
    {
        logged = true;
        for (auto& [k, t] : g_tiledPrev) Logf("[render] tiled surface %08X: drawing once at %ux%u instead of in pieces\n", uint32_t(k >> 32), t.w, t.h);
    }
}

// Black-scene detector: each frame, copy the centre pixel of the resolved 3D scene (0x18003000)
// to a staging texture and check it 3 frames later; log when it turns black.
static void WriteBmp(const char* name, const std::vector<uint32_t>& px, uint32_t w, uint32_t h);

void CheckSceneBlack()
{
    // every frame: centre 8x8 of the scene image (0x18003000) and of the shown picture are copied
    // to staging textures and read 4 frames later. A dark picture over a lit scene = a flash.
    static ID3D11Texture2D* stage[4][2] = {};
    static uint64_t stageFrame[4] = {};
    static int reports = 0;
    auto it = g_resolved.find(0x18003000);
    if (it == g_resolved.end() || !it->second.tex || reports > 60) return;
    int slot = int(g_frame % 4);
    for (int k = 0; k < 2; k++)
        if (!stage[slot][k])
        {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = 8; td.Height = 8; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &stage[slot][k]))) return;
            stageFrame[slot] = 0;
        }
    if (stageFrame[slot])
    {
        uint32_t maxc[2] = {};
        bool got = true;
        for (int k = 0; k < 2 && got; k++)
        {
            D3D11_MAPPED_SUBRESOURCE m;
            if (FAILED(g_ctx->Map(stage[slot][k], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) { got = false; break; }
            for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
            {
                uint32_t c = ((uint32_t*)((uint8_t*)m.pData + y * m.RowPitch))[x];
                maxc[k] = std::max({ maxc[k], c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF });
            }
            g_ctx->Unmap(stage[slot][k], 0);
        }
        if (got && maxc[0] > 20 && maxc[1] < 12)
        {
            reports++;
            uint64_t fr = stageFrame[slot];
            Logf("[flash] frame %llu: scene lit (max %u) but shown picture dark (max %u). Colour pass: %s\n",
                 (unsigned long long)fr, maxc[0], maxc[1], g_compositeNotes[fr % 8].c_str());
        }
    }
    uint32_t cx = Sc(int(it->second.w / 2)), cy = Sc(int(it->second.h / 2));
    D3D11_BOX box{ cx - 4, cy - 4, 0, cx + 4, cy + 4, 1 };
    g_ctx->CopySubresourceRegion(stage[slot][0], 0, 0, 0, 0, it->second.tex, 0, &box);
    D3D11_BOX box2{ g_rtW / 2 - 4, g_rtH / 2 - 4, 0, g_rtW / 2 + 4, g_rtH / 2 + 4, 1 };
    g_ctx->CopySubresourceRegion(stage[slot][1], 0, 0, 0, 0, g_showTex ? g_showTex : g_front, 0, &box2);
    stageFrame[slot] = g_frame;
}

// Scales the finished picture to the window (keeping 16:9, black bars if the window is another
// shape): sharp bicubic (Catmull-Rom) when enlarging, an averaging filter when shrinking.
// The swap chain follows the window's size. NBASH_SCALER=bilinear for plain bilinear.
static void WriteBmp(const char* name, const std::vector<uint32_t>& px, uint32_t w, uint32_t h);
// colour grading preset (F8): 0 original, 1 natural (default), 2 vivid; NBASH_COLORS=0/1/2
std::atomic<int> g_colorPreset{ [] { const char* v = getenv("NBASH_COLORS"); return v ? atoi(v) : 1; }() };
static void PresentScaled()
{
    static ID3D11VertexShader* vs = nullptr; static ID3D11PixelShader* ps = nullptr; static ID3D11SamplerState* lin = nullptr;
    static ID3D11ShaderResourceView* frontSrv = nullptr; static ID3D11RenderTargetView* backRtv = nullptr;
    static ID3D11Buffer* gradeCb = nullptr;
    static uint32_t backW = 0, backH = 0; static bool tried = false;
    if (!tried)
    {
        tried = true;
        const char* bilinear = getenv("NBASH_SCALER") && !strcmp(getenv("NBASH_SCALER"), "bilinear") ? "#define BILINEAR 1\n" : "";
        std::string src = std::string(bilinear) + R"(
Texture2D<float4> pic : register(t0); SamplerState lin : register(s0);
cbuffer Grade : register(b0) { float4 g0; float4 g1; float4 g2; };   // g0: on, black, gamma, contrast  g1: sat, vibrance, highlights, white balance  g2: clarity, de-pink, -, -
// COURT FRESH (g2.z, only while a court's colour cube is on screen - menus never get it): the game's old pink / violet / rose haze taken out of the picture
float3 CourtFresh(float3 c)
{
    float k = g2.z;
    float bl = saturate((c.b - max(c.r, c.g)) * 8);                          // violet -> clean blue: where blue leads, red goes under green
    c.r = lerp(c.r, min(c.r, c.g * 0.90), bl * k);
    float m = max(min(c.r, c.b) - c.g, 0) * k;                               // magenta / pink: red and blue both above green
    c.r -= m; c.b -= m * 0.5; c.g += m * 0.15;
    float l = dot(c, float3(0.2126, 0.7152, 0.0722)), mx = max(c.g, c.b);    // rose / salmon (red-led, blue close to green - so not skin, whose blue is well below green) -> neutral, at every brightness
    float rose = saturate((l - 0.12) * 5) * saturate((c.r - mx) * 12) * saturate((c.b - c.g * 0.86) * 14) * k;
    c.r = lerp(c.r, mx + (c.r - mx) * 0.12, rose);
    // (the straw-grass -> green nudge and the raised black point of v2 are gone: with the clean cubes of 11 Nov the picture needs neither, and yellow jerseys must stay yellow)
    return c;
}
// COURT LOOK (g2.z = 2): the picture arrives clean from the clean composite - only a light polish, no menu grade on top
float3 CourtLook(float3 c)
{
    c = pow(saturate(c), 0.94); c += (c * c * (3 - 2 * c) - c) * 0.20;
    float l = dot(c, float3(0.2126, 0.7152, 0.0722)); float sat = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
    return saturate(l + (c - l) * (1.16 + 0.25 * (1 - sat) * (1 - sat)));
}
float3 GradeColor(float3 c)
{
    if (g2.z > 1.5) return CourtLook(c);
    if (g2.z > 0.01) c = CourtFresh(c);
    if (g0.x < 0.5) return c;
    // daylight white balance (g1.w): pulls the courts' pink/orange sunset cast back towards neutral,
    // keeping overall brightness; warm colours stay warm, greys and whites stop being pink
    float3 wb = lerp(float3(1, 1, 1), float3(0.90, 1.0, 1.12), g1.w);
    float lb = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = c * wb; c *= lb / max(dot(c, float3(0.2126, 0.7152, 0.0722)), 1e-4);
    // de-pink: magenta/pink tones (red and blue both above green) are pulled back towards neutral
    float pk = saturate(min(c.r - c.g, c.b - c.g) * 6) * g2.y;
    float pm = min(c.r - c.g, c.b - c.g) * pk;
    c.r -= pm; c.b -= pm;
    c = saturate((c - g0.y) / (1 - g0.y));                        // deeper blacks: removes the grey haze
    c = pow(c, g0.z);                                             // richer mid-tones
    c = lerp(c, c * c * (3 - 2 * c), g0.w);                       // contrast S-curve
    float l0 = dot(c, float3(0.2126, 0.7152, 0.0722));
    c -= saturate(l0 - 0.7) * saturate(1 - l0) * 4 * g1.z;       // soften bright bloom/sky, pure white stays white
    float l = dot(c, float3(0.2126, 0.7152, 0.0722));
    float sat = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
    c = l + (c - l) * (g1.x + g1.y * (1 - sat) * (1 - sat));      // vibrance: dull colours gain most
    return saturate(c);
}
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
// clarity: local contrast against the average of a ring of samples (about 6 source pixels away)
float3 Clarity(float2 uv, float3 c)
{
    if (g2.w > 0.001) { float2 q = (uv - 0.5) * 2; float dd = dot(q, q) * 0.5; c *= 1 + g2.w * dd * dd; }      // the game's dark corners (vignette) lifted back, courts only
    if (g0.x < 0.5 || g2.x <= 0) return c;
    float2 size; pic.GetDimensions(size.x, size.y);
    float2 r = 6.0 / size; float3 a = 0;
    a += pic.SampleLevel(lin, uv + float2( r.x, 0), 0).rgb; a += pic.SampleLevel(lin, uv + float2(-r.x, 0), 0).rgb;
    a += pic.SampleLevel(lin, uv + float2(0,  r.y), 0).rgb; a += pic.SampleLevel(lin, uv + float2(0, -r.y), 0).rgb;
    a += pic.SampleLevel(lin, uv + 0.7 * float2( r.x,  r.y), 0).rgb; a += pic.SampleLevel(lin, uv + 0.7 * float2(-r.x,  r.y), 0).rgb;
    a += pic.SampleLevel(lin, uv + 0.7 * float2( r.x, -r.y), 0).rgb; a += pic.SampleLevel(lin, uv + 0.7 * float2(-r.x, -r.y), 0).rgb;
    a *= 0.125;
    float l = dot(c, float3(0.2126, 0.7152, 0.0722)), la = dot(a, float3(0.2126, 0.7152, 0.0722));
    float d = clamp(l - la, -0.12, 0.12) * g2.x;                  // limited so edges don't halo
    return saturate(c + d);
}
V vs_main(uint id : SV_VertexID) { V o; o.uv = float2((id << 1) & 2, id & 2); o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }
float4 ps_main(V i) : SV_Target
{
    float2 size; pic.GetDimensions(size.x, size.y);
    float2 ratio = abs(float2(ddx(i.uv.x), ddy(i.uv.y))) * size;   // source pixels per window pixel
#ifndef BILINEAR
    if (ratio.x > 1.01 || ratio.y > 1.01)
#endif
    {
        if (ratio.x <= 1.01 && ratio.y <= 1.01) return float4(GradeColor(Clarity(i.uv, pic.SampleLevel(lin, i.uv, 0).rgb)), 1);
        // shrinking: average 4 bilinear taps spread over the window pixel's footprint
        float2 d = 0.25 * max(ratio, 1) / size;
        float3 c = pic.SampleLevel(lin, i.uv + float2(-d.x, -d.y), 0).rgb + pic.SampleLevel(lin, i.uv + float2(d.x, -d.y), 0).rgb +
                   pic.SampleLevel(lin, i.uv + float2(-d.x, d.y), 0).rgb + pic.SampleLevel(lin, i.uv + float2(d.x, d.y), 0).rgb;
        return float4(GradeColor(Clarity(i.uv, c * 0.25)), 1);
    }
    // enlarging: Catmull-Rom bicubic in 9 bilinear taps
    float2 sp = i.uv * size, t1 = floor(sp - 0.5) + 0.5, f = sp - t1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2, t12 = (t1 + w2 / w12) / size, t0 = (t1 - 1) / size, t3 = (t1 + 2) / size;
    float3 c = 0;
    c += pic.SampleLevel(lin, float2(t0.x, t0.y), 0).rgb * w0.x * w0.y;
    c += pic.SampleLevel(lin, float2(t12.x, t0.y), 0).rgb * w12.x * w0.y;
    c += pic.SampleLevel(lin, float2(t3.x, t0.y), 0).rgb * w3.x * w0.y;
    c += pic.SampleLevel(lin, float2(t0.x, t12.y), 0).rgb * w0.x * w12.y;
    c += pic.SampleLevel(lin, float2(t12.x, t12.y), 0).rgb * w12.x * w12.y;
    c += pic.SampleLevel(lin, float2(t3.x, t12.y), 0).rgb * w3.x * w12.y;
    c += pic.SampleLevel(lin, float2(t0.x, t3.y), 0).rgb * w0.x * w3.y;
    c += pic.SampleLevel(lin, float2(t12.x, t3.y), 0).rgb * w12.x * w3.y;
    c += pic.SampleLevel(lin, float2(t3.x, t3.y), 0).rgb * w3.x * w3.y;
    return float4(GradeColor(Clarity(i.uv, saturate(c))), 1);
}
)";
        ID3DBlob* vb = Compile(src, "present_vs", "vs_5_0", "vs_main");
        ID3DBlob* pb = Compile(src, "present_ps", "ps_5_0", "ps_main");
        if (vb) g_dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs);
        if (pb) g_dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &ps);
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        g_dev->CreateSamplerState(&sd, &lin);
        g_dev->CreateShaderResourceView(g_front, nullptr, &frontSrv);
        D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = 48; cbd.Usage = D3D11_USAGE_DEFAULT; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        g_dev->CreateBuffer(&cbd, nullptr, &gradeCb);
        Logf(vs && ps ? "[render] picture is scaled to the window with a bicubic filter\n" : "[render] window scaler unavailable, plain copy\n");
    }
    // follow the window's size
    RECT cr{};
    if (g_hwnd) GetClientRect(g_hwnd, &cr);
    uint32_t ww = uint32_t(cr.right - cr.left), wh = uint32_t(cr.bottom - cr.top);
    if (ww >= 16 && wh >= 16 && (ww != backW || wh != backH))
    {
        SafeRelease(backRtv);
        g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
        if (backW && FAILED(g_swap->ResizeBuffers(0, ww, wh, DXGI_FORMAT_UNKNOWN, 0))) { ww = backW; wh = backH; }
        else if (!backW) { DXGI_SWAP_CHAIN_DESC d{}; g_swap->GetDesc(&d); if (d.BufferDesc.Width != ww || d.BufferDesc.Height != wh) g_swap->ResizeBuffers(0, ww, wh, DXGI_FORMAT_UNKNOWN, 0); }
        backW = ww; backH = wh;
        Logf("[render] window is %ux%u\n", ww, wh);
    }
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) return;
    D3D11_TEXTURE2D_DESC bd{}; back->GetDesc(&bd);
    if (!vs || !ps || !frontSrv)
    {
        if (bd.Width == g_rtW && bd.Height == g_rtH) g_ctx->CopyResource(back, g_showTex ? g_showTex : g_front);
        back->Release();
        return;
    }
    if (!backRtv) g_dev->CreateRenderTargetView(back, nullptr, &backRtv);
    back->Release();
    float black[4] = { 0, 0, 0, 1 };
    g_ctx->ClearRenderTargetView(backRtv, black);
    // largest 16:9 area in the window, centred
    float vw = float(bd.Width), vh = float(bd.Height);
    if (vw * g_height > vh * g_width) vw = vh * g_width / g_height; else vh = vw * g_height / g_width;
    D3D11_VIEWPORT vp{ floorf((bd.Width - vw) / 2), floorf((bd.Height - vh) / 2), floorf(vw + 0.5f), floorf(vh + 0.5f), 0, 1 };
    D3D11_RECT sc{ 0, 0, LONG(bd.Width), LONG(bd.Height) };
    g_ctx->OMSetRenderTargets(1, &backRtv, nullptr);
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetScissorRects(1, &sc);
    g_ctx->RSSetState(nullptr);
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(nullptr, 0);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(vs, nullptr, 0);
    g_ctx->GSSetShader(nullptr, nullptr, 0);
    g_ctx->PSSetShader(ps, nullptr, 0);
    { ID3D11ShaderResourceView* showSrv = g_showSrv ? g_showSrv : frontSrv; g_ctx->PSSetShaderResources(0, 1, &showSrv); }
    g_ctx->PSSetSamplers(0, 1, &lin);
    if (gradeCb)
    {
        // colour presets (F8 cycles): 0 original, 1 natural, 2 vivid
        // natural (default): clean daylight - neutral white balance, no pink, bright, rich, crisp
        static const float presets[3][12] = {
            { 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0 },
            { 1, 0.015f, 0.90f, 0.16f, 1.04f, 0.15f, 0.35f, 1.0f, 0, 0, 0, 0 },   // the look from the sound-test build
            { 1, 0.035f, 1.06f, 0.22f, 1.05f, 0.40f, 0.00f, 0, 0, 0, 0, 0 } };
        int pr = std::clamp(g_colorPreset.load(), 0, 2);
        static int lastPr = -1;
        // court frames (a court colour cube was used within the last 2 frames): pink / violet haze out + corners lifted.  NBASH_FRESH=0 off, NBASH_FRESH=<0..1> strength, NBASH_UNVIGNETTE=<0..1>
        static const float freshK = [] { const char* v = getenv("NBASH_FRESH"); return v ? float(atof(v)) : 1.0f; }();
        static const float unvig = [] { const char* v = getenv("NBASH_UNVIGNETTE"); return v ? float(atof(v)) : 0.55f; }();
        bool court = freshK > 0 && pr != 0 && g_courtLutFrame != ~0ull && g_frame - g_courtLutFrame <= 2;
        static int lastCourt = -1;
        bool cleanComp = court && g_cleanCompFrame != ~0ull && g_frame - g_cleanCompFrame <= 2;
        if (int(court) + 2 * int(cleanComp) != lastCourt)
        {
            lastCourt = int(court) + 2 * int(cleanComp); lastPr = pr; float cb[12]; memcpy(cb, presets[pr], sizeof cb); cb[10] = cleanComp ? 2.0f : court ? freshK : 0; cb[11] = cleanComp ? 0 : court ? unvig : 0;
            g_ctx->UpdateSubresource(gradeCb, 0, nullptr, cb, 0, 0); Logf("[render] court colours %s\n", court ? "ON (fresh look)" : "off (menu)");
        }
        if (pr != lastPr) { lastPr = pr; lastCourt = -1; g_ctx->UpdateSubresource(gradeCb, 0, nullptr, presets[pr], 0, 0); Logf("[render] colours: %s\n", pr == 0 ? "original" : pr == 1 ? "natural" : "vivid"); }
        g_ctx->PSSetConstantBuffers(0, 1, &gradeCb);
    }
    g_ctx->Draw(3, 0);
    ::NbashStatsDraw(g_dev, g_ctx);   // player ratings screen
    ID3D11ShaderResourceView* nullSrv = nullptr;
    g_ctx->PSSetShaderResources(0, 1, &nullSrv);
    g_ctx->OMSetRenderTargets(1, &g_rtv, g_dsv);    // the next draw rebinds everything else
    static const bool dumpBack = getenv("NBASH_DUMPBACK") != nullptr;
    if (dumpBack)   // test: save what the window shows
    {
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)))
        {
            D3D11_TEXTURE2D_DESC td = bd; td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
            ID3D11Texture2D* st = nullptr;
            if (SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &st)))
            {
                g_ctx->CopyResource(st, bb);
                D3D11_MAPPED_SUBRESOURCE m;
                if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m)))
                {
                    std::vector<uint32_t> px(size_t(td.Width) * td.Height);
                    for (uint32_t y = 0; y < td.Height; y++) memcpy(&px[size_t(y) * td.Width], (uint8_t*)m.pData + y * m.RowPitch, td.Width * 4);
                    g_ctx->Unmap(st, 0);
                    WriteBmp("backbuffer.bmp", px, td.Width, td.Height);
                }
                st->Release();
            }
            bb->Release();
        }
    }
}

void Present(uint32_t frontBuffer)
{
    // Show the image the game asked for (its front buffer, a copy target), like the console --
    // not simply whatever the render target held at the last copy of the frame: Gamebreaker
    // frames copy other things after the finished picture.
    {
        static const bool off = getenv("NBASH_LASTCOPYPRESENT") != nullptr;
        static int logged = 0;
        auto it = g_resolved.find(frontBuffer & 0x1FFFFFFF);
        bool ok = !off && it != g_resolved.end() && it->second.tex && it->second.w == g_width && it->second.h == g_height;
        if (logged < 6 && frontBuffer)
        {
            logged++;
            Logf("[render] presenting front buffer %08X: %s\n", frontBuffer, ok ? "found" : "not a copy target, showing the last copy");
        }
        if (ok) { g_showTex = it->second.tex; g_showSrv = it->second.srv; g_needFallback = false; }
        else { g_showTex = g_front; g_showSrv = nullptr; g_needFallback = true; }
    }
    CheckSceneBlack();
    EndFrame();
    g_arenaRegions.clear(); g_arenaUsed = 0;
    if (!g_swap) return;
    PresentScaled();
    g_swap->Present(0, 0);
    // ---- 12 Nov 23:00 SHOT RECORDER: the first 5 standing shots of a custom player in a session are saved as a strip of frames (what is really on screen,
    // every frame from the shoot press, scaled down to ~640 wide): capture\shotN_FF.bmp, the same names every session.  Started by shotswap.cpp.
    {
        extern std::atomic<int> g_shotBurstFrames;
        static std::vector<std::vector<uint32_t>> frames; static uint32_t bw = 0, bh = 0; static int shotNo = 0;
        int left = g_shotBurstFrames.load();
        if (left > 0)
        {
            std::vector<uint32_t> px; uint32_t w = 0, h = 0;
            if (ReadBack(px, w, h, false) && w && h)
            {
                uint32_t k = w / 640; if (k < 1) k = 1; bw = w / k; bh = h / k; std::vector<uint32_t> sm(size_t(bw) * bh);
                for (uint32_t y = 0; y < bh; y++) for (uint32_t x = 0; x < bw; x++) sm[size_t(y) * bw + x] = px[size_t(y * k) * w + x * k];
                frames.push_back(std::move(sm));
            }
            if (g_shotBurstFrames.fetch_sub(1) == 1 && !frames.empty())
            {
                int no = ++shotNo; auto fr = std::make_shared<std::vector<std::vector<uint32_t>>>(std::move(frames)); frames.clear(); uint32_t fw = bw, fh = bh;
                CreateDirectoryA("capture", nullptr);
                std::thread([no, fr, fw, fh] { for (size_t i = 0; i < fr->size(); i++) { char name[64]; snprintf(name, sizeof name, "capture\\shot%d_%02d.bmp", no, int(i)); WriteBmp(name, (*fr)[i], fw, fh); } Logf("[render] shot recorder: shot %d saved (%d frames, %ux%u)\n", no, int(fr->size()), fw, fh); }).detach();
            }
        }
    }
    if (g_screenshotRequest.exchange(false))
    {
        std::vector<uint32_t> px; uint32_t w = 0, h = 0;
        if (ReadBack(px, w, h, false))
        {
            char name[64]; snprintf(name, sizeof name, "capture\\screen%llu.bmp", (unsigned long long)g_frame);
            CreateDirectoryA("capture", nullptr);
            {
                WriteBmp(name, px, w, h);
                char prefix[64]; snprintf(prefix, sizeof prefix, "capture\\screen%llu", (unsigned long long)g_frame);
                DumpTargets(prefix);
                Logf("[render] screenshot saved: %s (plus the frame's copy targets)\n", name);
            }
        }
    }
    static LARGE_INTEGER lastReport{};
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    static uint64_t framesAtReport = 0;
    LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);
    if (now.QuadPart - lastReport.QuadPart > 10 * freq.QuadPart)
    {
        LARGE_INTEGER f = freq;
        static uint64_t drawsAtReport = 0;
        Logf("[render] %llu frames, %llu draws, %u shaders compiled so far, %u texture uploads; ms spent:", (unsigned long long)(g_frame - framesAtReport), (unsigned long long)(g_drawCounter - drawsAtReport), (unsigned)g_shaderCompiles, g_textureUploads);
        drawsAtReport = g_drawCounter;
        g_textureUploads = 0;
        lastReport = now; framesAtReport = g_frame;
        Logf(" shaders %.0f, indices %.0f, vertices %.0f, textures %.0f, constants %.0f, state %.0f, bind %.0f, draw %.0f\n",
             g_phase[0] * 1000 / f.QuadPart, g_phase[1] * 1000 / f.QuadPart, g_phase[2] * 1000 / f.QuadPart, g_phase[3] * 1000 / f.QuadPart,
             g_phase[4] * 1000 / f.QuadPart, g_phase[5] * 1000 / f.QuadPart, g_phase[6] * 1000 / f.QuadPart, g_phase[7] * 1000 / f.QuadPart);
        Logf("[render] texture change checks: %u, %.0f ms; vertex data: %u reused, %u re-sent, %u per-frame\n", g_hashCount, g_hashTicks * 1000.0 / f.QuadPart,
             g_vcacheHits, g_vcacheUploads, g_vcacheRing);
        g_vcacheHits = g_vcacheUploads = g_vcacheRing = 0;
        g_hashTicks = 0; g_hashCount = 0;
        for (double& x : g_phase) x = 0;
    }
    g_vmemNextRow = kVmemCacheRows;
    if (g_frame % 600 == 0)   // forget ranges seen once long ago (dynamic data at moving addresses)
        for (auto it = g_vcache.begin(); it != g_vcache.end();)
            if (it->second.offset < 0 && it->second.seenFrame + 120 < g_frame) it = g_vcache.erase(it); else ++it;
    g_frame++;
}

std::atomic<bool> g_tileCollapse{true};
std::atomic<bool> g_screenshotRequest{false};
std::atomic<int> g_shotBurstFrames{0};      // shot recorder: frames still to save (set by shotswap.cpp on a custom player's shoot press)

// Writes a 24-bit BMP (pixels as 0xAABBGGRR, i.e. R in the low byte).
static void WriteBmp(const char* name, const std::vector<uint32_t>& px, uint32_t w, uint32_t h)
{
    FILE* f = fopen(name, "wb");
    if (!f) return;
    uint32_t rowBytes = w * 3, pad = (4 - rowBytes % 4) % 4, size = 54 + (rowBytes + pad) * h;
    uint8_t hdr[54] = { 'B', 'M' };
    auto put32 = [&](int o, uint32_t v) { memcpy(hdr + o, &v, 4); };
    put32(2, size); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> row(rowBytes + pad);
    for (int y = int(h) - 1; y >= 0; y--)
    {
        for (uint32_t x = 0; x < w; x++) { uint32_t c = px[size_t(y) * w + x]; row[x * 3] = uint8_t(c >> 16); row[x * 3 + 1] = uint8_t(c >> 8); row[x * 3 + 2] = uint8_t(c); }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
}

namespace { void WriteBmpFile(const char* name, const std::vector<uint32_t>& px, uint32_t w, uint32_t h) { WriteBmp(name, px, w, h); } }

// Saves every copy target of at least 64x64 (the frame's intermediate images) as
// <prefix>_<address>.bmp, so live and replayed frames can be compared step by step.
void DumpTargets(const char* prefix)
{
    for (auto& [addr, r] : g_resolved)
    {
        if (!r.tex || r.w * r.h < 4096) continue;
        D3D11_TEXTURE2D_DESC td{};
        r.tex->GetDesc(&td);
        if (td.Format != DXGI_FORMAT_R8G8B8A8_UNORM) continue;
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
        ID3D11Texture2D* st = nullptr;
        if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &st))) continue;
        g_ctx->CopyResource(st, r.tex);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m)))
        {
            std::vector<uint32_t> px(size_t(td.Width) * td.Height);
            for (uint32_t y = 0; y < td.Height; y++) memcpy(&px[size_t(y) * td.Width], (uint8_t*)m.pData + y * m.RowPitch, td.Width * 4);
            g_ctx->Unmap(st, 0);
            char name[256]; snprintf(name, sizeof name, "%s_%08X.bmp", prefix, addr);
            WriteBmp(name, px, td.Width, td.Height);
        }
        st->Release();
    }
}

bool ReadBack(std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height, bool liveTarget)
{
    if (!g_dev) return false;
    if (liveTarget) SyncRt();
    ID3D11Texture2D* src = liveTarget ? g_rt : (g_showTex ? g_showTex : g_front);
    if (getenv("NBASH_READDEPTH"))      // debug: depth as grey levels (x8 for visibility), stencil in blue
    {
        D3D11_TEXTURE2D_DESC dd{};
        g_ds->GetDesc(&dd);
        dd.Usage = D3D11_USAGE_STAGING; dd.BindFlags = 0; dd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* st = nullptr;
        if (FAILED(g_dev->CreateTexture2D(&dd, nullptr, &st))) return false;
        g_ctx->CopyResource(st, g_ds);
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) { st->Release(); return false; }
        width = dd.Width; height = dd.Height;
        pixels.resize(size_t(width) * height);
        for (uint32_t y = 0; y < height; y++)
            for (uint32_t x = 0; x < width; x++)
            {
                uint32_t v = ((uint32_t*)((uint8_t*)m.pData + y * m.RowPitch))[x];
                float z = (v & 0xFFFFFF) / 16777215.0f;
                uint32_t g = uint32_t(std::min(z * 8.0f, 1.0f) * 255), hi = z >= 0.999f ? 255 : 0;
                pixels[size_t(y) * width + x] = 0xFF000000u | (hi << 16) | (g << 8) | g;   // red tint = depth 1.0
            }
        g_ctx->Unmap(st, 0); st->Release();
        return true;
    }
    D3D11_TEXTURE2D_DESC td{};
    g_front->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &st))) return false;
    g_ctx->CopyResource(st, src);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) { st->Release(); return false; }
    width = td.Width; height = td.Height;
    pixels.resize(size_t(width) * height);
    for (uint32_t y = 0; y < height; y++) memcpy(&pixels[size_t(y) * width], (uint8_t*)m.pData + y * m.RowPitch, width * 4);
    g_ctx->Unmap(st, 0);
    st->Release();
    return true;
}
}
#endif

ID3DBlob* NbashCompileShader(const char* src, const char* name, const char* profile, const char* entry) { return render::Compile(src, name, profile, entry); }


// ---- 14 Nov, smooth frame pacing at the CHOSEN frame rate (graphics menu: 30 / 60 / 120; user: "we gotta do either 30, 60 or 120").
// The game's one clock (gpu.cpp VblankThread) follows the real vblank of the display the window is on whenever the display's refresh rate is the chosen rate
// or a whole multiple of it (60 on a 60 / 120 / 240 Hz display, 120 on 120 / 240, 30 on 60 / 120 ...): every n-th vblank is one game frame, so every frame
// is on screen for exactly the same time. Otherwise (e.g. 60 on a 75 Hz display, 120 on a 60 Hz one) it returns 0 and the caller runs its precise timer.
// The display's rate is read cheaply every 2 s (docking / another screen / a mode change) and only measured exactly (24 vblanks, ~0.3 s) when it has changed.
// NBASH_VBLANK=timer forces the timer.
extern "C" HWND NbashWindow();
namespace render
{
static int PaceDiv(double hz, int fps) { int n = int(hz / fps + 0.5); return (n >= 1 && n <= 8 && std::fabs(hz / n - fps) <= 0.025 * fps) ? n : 0; }
int WaitDisplayVblank(double* hzOut, int fps)
{
    static const bool off = [] { const char* e = getenv("NBASH_VBLANK"); return e && !strcmp(e, "timer"); }();
    static IDXGIOutput* out = nullptr; static int div = 0, lastFps = 0, lastMode = -1, bad = 0, fails = 0; static double hz = 0; static bool never = false;
    static std::chrono::steady_clock::time_point retry{}, polled{};
    auto now = std::chrono::steady_clock::now();
    if (hzOut) *hzOut = hz;
    if (off || never || !g_swap) return 0;
    auto drop = [&] { if (out) { out->Release(); out = nullptr; } div = 0; bad = 0; };
    if (now - polled > std::chrono::seconds(2))      // which display, which mode?  (cheap, no waiting)
    {
        polled = now; int mode = 0;
        if (HWND w = NbashWindow())
        {
            MONITORINFOEXW mi{}; mi.cbSize = sizeof mi;
            if (GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi))
            {
                DEVMODEW dm{}; dm.dmSize = sizeof dm;
                if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) mode = int(dm.dmDisplayFrequency) + (int(mi.rcMonitor.left) * 31 + int(mi.rcMonitor.top)) * 1000;
            }
        }
        if (mode != lastMode) { lastMode = mode; drop(); hz = 0; retry = now; }      // new display or new refresh rate: measure again
    }
    if (now < retry) return 0;
    if (!out)
    {
        if (FAILED(g_swap->GetContainingOutput(&out)) || !out) { out = nullptr; if (++fails > 50) never = true; retry = now + std::chrono::seconds(5); return 0; }
        if (hz == 0)
        {
            double dts[24]; bool ok = SUCCEEDED(out->WaitForVBlank());
            auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; ok && i < 24; i++)
            {
                ok = SUCCEEDED(out->WaitForVBlank());
                auto t1 = std::chrono::steady_clock::now(); dts[i] = std::chrono::duration<double>(t1 - t0).count(); t0 = t1;
                if (dts[i] > 0.2) ok = false;
            }
            if (!ok) { drop(); retry = std::chrono::steady_clock::now() + std::chrono::seconds(10); return 0; }
            std::sort(dts, dts + 24); hz = dts[12] > 0.0005 ? 1.0 / dts[12] : 0; lastFps = 0;
        }
    }
    if (fps != lastFps)
    {
        lastFps = fps; div = PaceDiv(hz, fps);
        if (div) Logf("[pace] display %.2f Hz, frame rate %d: one game frame every %d vblank%s of the display\n", hz, fps, div, div > 1 ? "s" : "");
        else Logf("[pace] display %.2f Hz, frame rate %d: not a whole multiple - timer pacing (frames cannot all stay on screen equally long)\n", hz, fps);
    }
    if (hzOut) *hzOut = hz;
    if (!div) return 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < div; i++) if (FAILED(out->WaitForVBlank())) { drop(); retry = std::chrono::steady_clock::now() + std::chrono::seconds(10); lastFps = 0; return 0; }
    double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (dt < 0.002 || dt > 0.08)
    {
        if (++bad > 8) { Logf("[pace] display vblank waits are off (%.1f ms) - timer pacing for 30 s\n", dt * 1000); if (++fails > 50) never = true; drop(); lastFps = 0; retry = std::chrono::steady_clock::now() + std::chrono::seconds(30); return 0; }
    }
    else bad = 0;
    return 1;
}
}
