// Replays a captured frame (capture/frameN) through the renderer and writes frame.bmp.
// Windows tool (runs under Wine in Claude's workspace).
#include "../runtime/render/renderer.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

static std::vector<uint8_t> g_phys(0x20000000);
static uint8_t* Phys(uint32_t a) { return g_phys.data() + (a & 0x1FFFFFFF); }

static std::vector<uint32_t> LoadDwords(const std::string& path)
{
    std::vector<uint32_t> v;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return v;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    v.resize(n / 4);
    fread(v.data(), 4, v.size(), f);
    fclose(f);
    return v;
}

static void WriteBmp(const char* path, const std::vector<uint32_t>& px, uint32_t w, uint32_t h)
{
    FILE* f = fopen(path, "wb");
    uint32_t size = 54 + w * h * 4;
    uint8_t hdr[54] = { 'B', 'M' };
    memcpy(hdr + 2, &size, 4); uint32_t off = 54; memcpy(hdr + 10, &off, 4);
    uint32_t hs = 40; memcpy(hdr + 14, &hs, 4); memcpy(hdr + 18, &w, 4); int32_t nh = -int32_t(h); memcpy(hdr + 22, &nh, 4);
    uint16_t planes = 1, bpp = 32; memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);
    fwrite(hdr, 1, 54, f);
    for (uint32_t p : px) { uint32_t bgra = (p & 0xFF00FF00) | ((p & 0xFF) << 16) | ((p >> 16) & 0xFF); fwrite(&bgra, 4, 1, f); }
    fclose(f);
}

// Captures are saved as bundle.bin (+ physmem.bin): unpack it once next to itself.
static void Unbundle(const std::string& dir)
{
    if (FILE* t = fopen((dir + "/draws.txt").c_str(), "r")) { fclose(t); return; }
    FILE* f = fopen((dir + "/bundle.bin").c_str(), "rb");
    if (!f) return;
    printf("unpacking %s/bundle.bin ...\n", dir.c_str());
#ifdef _WIN32
    CreateDirectoryA((dir + "/blobs").c_str(), nullptr);
    CreateDirectoryA((dir + "/shaders").c_str(), nullptr);
#endif
    std::vector<uint8_t> buf;
    uint32_t ln;
    while (fread(&ln, 4, 1, f) == 1)
    {
        std::string name(ln, '\0');
        fread(&name[0], 1, ln, f);
        uint32_t size; fread(&size, 4, 1, f);
        buf.resize(size);
        fread(buf.data(), 1, size, f);
        if (FILE* o = fopen((dir + "/" + name).c_str(), "wb")) { fwrite(buf.data(), 1, size, o); fclose(o); }
    }
    fclose(f);
}

int main(int argc, char** argv)
{
    std::string dir = argc > 1 ? argv[1] : "capture/frame600";
    // replay.exe <dir> stepsfrom <psHash>: after the first draw using that pixel shader, save the
    // render target after every draw as <dir>/step_NNNN.bmp (to find which draw goes wrong)
    bool steps = argc > 3 && !strcmp(argv[2], "stepsfrom");
    unsigned long long stepPs = steps ? strtoull(argv[3], nullptr, 10) : 0;
    bool stepping = false;
    int lastDraw = (argc > 2 && !steps) ? atoi(argv[2]) : 1 << 30;
    Unbundle(dir);
    render::PhysToHost = Phys;
    FILE* m = fopen((dir + "/physmem.bin").c_str(), "rb");
    uint32_t page;
    while (fread(&page, 4, 1, m) == 1) fread(&g_phys[size_t(page) * 4096], 1, 4096, m);
    fclose(m);
    void* win = nullptr;
#ifdef _WIN32
    if (const char* ws = getenv("NBASH_REPLAYWIN"))   // test the window scaler: "1920x1080"
    {
        int ww = 1920, wh = 1080; sscanf(ws, "%dx%d", &ww, &wh);
        WNDCLASSA wc{}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "rp"; RegisterClassA(&wc);
        RECT r{ 0, 0, ww, wh }; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        win = CreateWindowA("rp", "replay", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    }
#endif
    if (!render::Init(win, 1280, 720)) return 1;

    std::map<std::string, std::vector<uint32_t>> shaders;
    int n = 0;
    // pass 0 sets up the end-of-frame clears (the game clears for the next frame), pass 1 is the real frame
    for (int pass = 0; pass < 2; pass++)
    {
    FILE* dl = fopen((dir + "/draws.txt").c_str(), "r");
    char line[512];
    n = 0;
    while (fgets(line, sizeof line, dl) && (pass == 0 || n <= lastDraw))
    {
        unsigned idx, op, init, dma, size;
        char vs[32], ps[32];
        if (sscanf(line, "%u op=%X init=%X dma=%X size=%X vs=%31s ps=%31s", &idx, &op, &init, &dma, &size, vs, ps) != 7) continue;
        char rn[32]; snprintf(rn, sizeof rn, "/regs%04u.bin", idx);
        std::vector<uint32_t> regs = LoadDwords(dir + rn);
        for (const char* h : { vs, ps })
            if (!shaders.count(h)) shaders[h] = LoadDwords(dir + "/shaders/" + h + ".bin");
        // restore the dynamic buffers exactly as this draw saw them (captures from Nov 5 on)
        char mn[32]; snprintf(mn, sizeof mn, "/mem%04u.txt", idx);
        if (FILE* ml = fopen((dir + mn).c_str(), "r"))
        {
            unsigned a, bytes; char h[32];
            while (fscanf(ml, "%X %u %31s", &a, &bytes, h) == 3)
                if (render::ExportedOverlap(a, bytes)) continue;   // our own memory export wrote it
                else if (FILE* bf = fopen((dir + "/blobs/" + h + ".bin").c_str(), "rb")) { fread(Phys(a), 1, bytes, bf); fclose(bf); }
            fclose(ml);
        }
        render::DrawCall d{};
        d.regs = regs.data();
        d.vs = &shaders[vs]; d.ps = &shaders[ps];
        d.vsHash = strtoull(vs, nullptr, 10); d.psHash = strtoull(ps, nullptr, 10);
        d.initiator = init; d.dmaBase = dma; d.dmaSize = size;
        render::Draw(d);
        n++;
        if (pass == 1 && steps)
        {
            if (d.psHash == stepPs) stepping = true;
            if (stepping)
            {
                std::vector<uint32_t> spx; uint32_t sw, sh;
                char sn[512]; snprintf(sn, sizeof sn, "%s/step_%04u.bmp", dir.c_str(), idx);
                if (render::ReadBack(spx, sw, sh, true)) WriteBmp(sn, spx, sw, sh);
            }
        }
    }
    fclose(dl);
    if (pass == 0) render::EndFrame();
    }
    // NBASH_REPLAYLOOP=N: draw the frame N more times (everything preloaded) to time the renderer
    if (const char* lp = getenv("NBASH_REPLAYLOOP"))
    {
        struct Pre { std::vector<uint32_t> regs; std::string vs, ps; render::DrawCall d; std::vector<std::pair<uint32_t, std::vector<uint8_t>>> mem; };
        std::vector<Pre> pre;
        FILE* dl = fopen((dir + "/draws.txt").c_str(), "r");
        char line[512];
        while (fgets(line, sizeof line, dl))
        {
            unsigned idx, op, init, dma, size; char vs[32], ps[32];
            if (sscanf(line, "%u op=%X init=%X dma=%X size=%X vs=%31s ps=%31s", &idx, &op, &init, &dma, &size, vs, ps) != 7) continue;
            Pre p; char rn[32]; snprintf(rn, sizeof rn, "/regs%04u.bin", idx);
            p.regs = LoadDwords(dir + rn); p.vs = vs; p.ps = ps;
            p.d = {}; p.d.vsHash = strtoull(vs, nullptr, 10); p.d.psHash = strtoull(ps, nullptr, 10);
            p.d.initiator = init; p.d.dmaBase = dma; p.d.dmaSize = size;
            char mn[32]; snprintf(mn, sizeof mn, "/mem%04u.txt", idx);
            if (FILE* ml = fopen((dir + mn).c_str(), "r"))
            {
                unsigned a, bytes; char h[32];
                while (fscanf(ml, "%X %u %31s", &a, &bytes, h) == 3)
                    if (FILE* bf = fopen((dir + "/blobs/" + h + ".bin").c_str(), "rb"))
                    { std::vector<uint8_t> v(bytes); fread(v.data(), 1, bytes, bf); fclose(bf); p.mem.push_back({ a, std::move(v) }); }
                fclose(ml);
            }
            pre.push_back(std::move(p));
        }
        fclose(dl);
        int loops = atoi(lp);
        extern void ProfRegisterThread(uint32_t);
        ProfRegisterThread(1);
        LARGE_INTEGER f0, t0, t1; QueryPerformanceFrequency(&f0); QueryPerformanceCounter(&t0);
        for (int it = 0; it < loops; it++)
        {
            for (auto& p : pre)
            {
                for (auto& [a, v] : p.mem) if (!render::ExportedOverlap(a, uint32_t(v.size()))) memcpy(Phys(a), v.data(), v.size());
                p.d.regs = p.regs.data(); p.d.vs = &shaders[p.vs]; p.d.ps = &shaders[p.ps];
                render::Draw(p.d);
            }
            render::Present(0);
        }
        QueryPerformanceCounter(&t1);
        printf("replayed %d frames: %.2f ms per frame\n", loops, (t1.QuadPart - t0.QuadPart) * 1000.0 / f0.QuadPart / loops);
    }
    if (win) render::Present(0);
    std::vector<uint32_t> px; uint32_t w, h;
    std::string out = steps ? dir + "/frame.bmp" : std::string("frame.bmp");
    if (render::ReadBack(px, w, h, argc > 2 && !steps)) { WriteBmp(out.c_str(), px, w, h); printf("wrote %s (%u draws)\n", out.c_str(), n); }
    if (steps) render::DumpTargets((dir + "/rtarget").c_str());
    if (getenv("NBASH_DUMPTARGETS")) render::DumpTargets("rtarget");
    return 0;
}
