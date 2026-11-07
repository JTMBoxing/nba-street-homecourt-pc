// Mod archive overlay: extra or replacement files for the game's BIG archives without touching
// the (huge) originals.
//
// An archive is X.big (data) + X.bh (index, "Viv4": 'Viv4', u32 LE file size, u32 BE count,
// u32 BE header size, then count x 16-byte BE entries {offset, stored size, unpacked size,
// djb2(lowercase path)}, then a 16-byte tail whose first word is the .big size, BE). The game finds
// files by the hash of their path, e.g. djb2("data/characters/textures/x.f1").
//
// Files under  game/mods/<X>/<path inside the archive>  (e.g. game/mods/charplatform/data/
// characters/textures/lebron23-0-head-cm.f1) are appended virtually: the game is given a generated
// X_mod.bh listing them at offsets just past the end of X.big, and reads past the real end of X.big
// are served from the mod files. A mod file with the same path as an original entry replaces it.
#include "guest.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;
std::string GameRoot();

struct ArchiveOverlay
{
    uint64_t realSize = 0;          // size of the real X.big
    std::vector<uint8_t> blob;      // mod data, logically at offset realSize
    std::string bhPath;             // generated index
    std::vector<std::pair<uint64_t, std::array<uint8_t, 8>>> hdrPatches;   // BIG4 name-table entries redirected to mod data
    uint32_t total() const { return uint32_t(realSize + blob.size()); }
};

namespace
{
    std::mutex g_lock;
    std::map<std::string, std::unique_ptr<ArchiveOverlay>> g_archives;   // by lowercase base name

    uint32_t Djb2(const std::string& s) { uint32_t h = 5381; for (unsigned char c : s) h = h * 33 + c; return h; }
    uint32_t BE(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
    void PutBE(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }

    std::string Lower(std::string s) { for (auto& c : s) c = char(tolower((unsigned char)c)); return s; }

    std::vector<uint8_t> ReadAll(const fs::path& p)
    {
        std::vector<uint8_t> d;
        if (FILE* f = fopen(p.string().c_str(), "rb")) { fseek(f, 0, SEEK_END); d.resize(size_t(ftell(f))); fseek(f, 0, SEEK_SET); if (!d.empty()) fread(d.data(), 1, d.size(), f); fclose(f); }
        return d;
    }

    uint32_t UnpackedSize(const std::vector<uint8_t>& d)
    {
        if (d.size() >= 5 && d[1] == 0xFB && (d[0] & 0x3E) == 0x10)
        {
            bool big = d[0] & 0x80; size_t p = 2 + ((d[0] & 0x01) ? (big ? 4 : 3) : 0);
            if (big) return BE(&d[p]);
            return uint32_t(d[p]) << 16 | uint32_t(d[p + 1]) << 8 | d[p + 2];
        }
        return uint32_t(d.size());
    }

    // builds the overlay for archive <dir>/<base>.big|.bh if game/mods/<base>/ has files
    ArchiveOverlay* Build(const fs::path& dir, const std::string& base)
    {
        // several archives share a name (every court has data/environment/<court>/environment.big),
        // so the overlay is per archive path; the mod folder is still game/mods/<name>/
        std::string key = Lower((dir / base).generic_string());
        auto it = g_archives.find(key);
        if (it != g_archives.end()) return it->second.get();
        g_archives[key] = nullptr;
        fs::path modDir = fs::path(GameRoot()) / "mods" / base;
        std::error_code ec;
        // camera options: NBASH_CAM=drive uses game/mods/camera_drive/ (low camera close to the action)
        if (Lower(base) == "camera")
        {
            const char* cam = getenv("NBASH_CAM");
            if (cam && *cam) { fs::path alt = fs::path(GameRoot()) / "mods" / ("camera_" + std::string(cam)); if (fs::is_directory(alt, ec)) modDir = alt; }
        }
        if (!fs::is_directory(modDir, ec))
        {
            // case-insensitive folder match
            fs::path mods = fs::path(GameRoot()) / "mods";
            if (!fs::is_directory(mods, ec)) return nullptr;
            bool found = false;
            for (auto& e : fs::directory_iterator(mods, ec)) if (e.is_directory() && Lower(e.path().filename().string()) == Lower(base)) { modDir = e.path(); found = true; }
            if (!found) return nullptr;
        }
        fs::path bh, big;
        for (auto& e : fs::directory_iterator(dir, ec))
        {
            std::string n = Lower(e.path().filename().string());
            if (n == Lower(base) + ".bh") bh = e.path();
            if (n == Lower(base) + ".big") big = e.path();
        }
        if (bh.empty() || big.empty()) return nullptr;
        std::vector<uint8_t> idx = ReadAll(bh);
        if (idx.size() < 32 || memcmp(idx.data(), "Viv4", 4) != 0) { Log("[mods] %s: index format not recognised\n", bh.string().c_str()); return nullptr; }
        uint32_t count = BE(&idx[8]);
        if (idx.size() != 16 + size_t(count) * 16 + 16) { Log("[mods] %s: unexpected index size\n", bh.string().c_str()); return nullptr; }

        auto ov = std::make_unique<ArchiveOverlay>();
        ov->realSize = fs::file_size(big, ec);
        std::map<uint32_t, size_t> byHash;
        for (uint32_t i = 0; i < count; i++) byHash[BE(&idx[16 + i * 16 + 12])] = i;
        // the .big has its own name table too (BIG4: magic, LE size, BE count, BE header size, then
        // {BE offset, BE size, name\0} per file); some loaders (the menu portraits) use it instead of the .bh
        std::map<std::string, uint64_t> nameEntry;
        {
            std::vector<uint8_t> h(16);
            if (FILE* f = fopen(big.string().c_str(), "rb"))
            {
                if (fread(h.data(), 1, 16, f) == 16 && !memcmp(h.data(), "BIG", 3))
                {
                    uint32_t hs = BE(&h[12]), cnt = BE(&h[8]);
                    if (hs > 16 && hs < (64u << 20)) { h.resize(hs); fread(h.data() + 16, 1, hs - 16, f); }
                    size_t q = 16;
                    for (uint32_t i = 0; i < cnt && q + 9 <= h.size(); i++)
                    {
                        size_t e = q; q += 8; std::string nm;
                        while (q < h.size() && h[q]) nm += char(h[q++]);
                        q++; nameEntry[Lower(nm)] = e;
                    }
                }
                fclose(f);
            }
        }
        std::vector<uint8_t> extra;
        int added = 0, replaced = 0;
        for (auto& e : fs::recursive_directory_iterator(modDir, ec))
        {
            if (!e.is_regular_file()) continue;
            std::string rel = Lower(fs::relative(e.path(), modDir, ec).generic_string());
            std::vector<uint8_t> d = ReadAll(e.path());
            if (d.empty()) continue;
            while (ov->blob.size() % 16) ov->blob.push_back(0);
            uint32_t off = uint32_t(ov->realSize + ov->blob.size());
            ov->blob.insert(ov->blob.end(), d.begin(), d.end());
            uint8_t ent[16];
            PutBE(ent, off); PutBE(ent + 4, uint32_t(d.size())); PutBE(ent + 8, UnpackedSize(d)); PutBE(ent + 12, Djb2(rel));
            auto ne = nameEntry.find(rel);
            if (ne != nameEntry.end()) { std::array<uint8_t, 8> pe; PutBE(pe.data(), off); PutBE(pe.data() + 4, uint32_t(d.size())); ov->hdrPatches.push_back({ ne->second, pe }); }
            auto h = byHash.find(Djb2(rel));
            if (h != byHash.end()) { memcpy(&idx[16 + h->second * 16], ent, 16); replaced++; }
            else { extra.insert(extra.end(), ent, ent + 16); added++; }
            Log("[mods] %s: %s %s (%zu bytes)\n", base.c_str(), h != byHash.end() ? "replaces" : "adds", rel.c_str(), d.size());
        }
        if (!added && !replaced) return nullptr;
        // new index: header, original entries (some replaced), added entries, tail
        std::vector<uint8_t> out(idx.begin(), idx.begin() + 16 + size_t(count) * 16);
        out.insert(out.end(), extra.begin(), extra.end());
        out.insert(out.end(), idx.end() - 16, idx.end());
        uint32_t n = count + uint32_t(extra.size() / 16), sz = uint32_t(out.size());
        out[4] = uint8_t(sz); out[5] = uint8_t(sz >> 8); out[6] = uint8_t(sz >> 16); out[7] = uint8_t(sz >> 24);
        PutBE(&out[8], n); PutBE(&out[12], sz);
        PutBE(&out[out.size() - 16], ov->total());
        ov->bhPath = (dir / (base + "_mod.bh")).string();
        if (FILE* f = fopen(ov->bhPath.c_str(), "wb")) { fwrite(out.data(), 1, out.size(), f); fclose(f); }
        else { Log("[mods] cannot write %s\n", ov->bhPath.c_str()); return nullptr; }
        Log("[mods] %s: %d files added, %d replaced (%zu KB)\n", base.c_str(), added, replaced, ov->blob.size() / 1024);
        g_archives[key] = std::move(ov);
        return g_archives[key].get();
    }
}

// host path of X.bh -> generated index if X has mods
std::string ArchiveModIndex(const std::string& hostPath)
{
    fs::path p(hostPath);
    if (Lower(p.extension().string()) != ".bh") return hostPath;
    std::lock_guard lk(g_lock);
    ArchiveOverlay* ov = Build(p.parent_path(), p.stem().string());
    return ov ? ov->bhPath : hostPath;
}

// host path of X.big -> overlay (nullptr if none)
const ArchiveOverlay* ArchiveModOverlay(const std::string& hostPath)
{
    fs::path p(hostPath);
    if (Lower(p.extension().string()) != ".big") return nullptr;
    std::lock_guard lk(g_lock);
    return Build(p.parent_path(), p.stem().string());
}

// Reads `len` bytes at `pos` of an archive with an overlay into dst (file already positioned
// for the real part). Returns bytes read.
size_t ArchiveModRead(const ArchiveOverlay* ov, FILE* f, uint64_t pos, uint8_t* dst, size_t len)
{
    size_t n = 0;
    if (pos < ov->realSize)
    {
        size_t want = size_t(std::min<uint64_t>(len, ov->realSize - pos));
        n = fread(dst, 1, want, f);
        if (n < want) return n;
    }
    uint64_t p = pos + n;
    if (n < len && p >= ov->realSize)
    {
        uint64_t o = p - ov->realSize;
        if (o < ov->blob.size())
        {
            size_t m = size_t(std::min<uint64_t>(len - n, ov->blob.size() - o));
            memcpy(dst + n, ov->blob.data() + o, m);
            n += m;
        }
    }
    for (auto& [at, pe] : ov->hdrPatches)
        for (int i = 0; i < 8; i++) { uint64_t a = at + i; if (a >= pos && a < pos + n) dst[a - pos] = pe[i]; }
    // the archive's own header holds its size (bytes 4..7, little-endian): keep it consistent
    if (pos < 8 && pos + n > 4)
    {
        uint32_t t = ov->total();
        uint8_t le[4] = { uint8_t(t), uint8_t(t >> 8), uint8_t(t >> 16), uint8_t(t >> 24) };
        for (int i = 0; i < 4; i++) { uint64_t a = 4 + i; if (a >= pos && a < pos + n) dst[a - pos] = le[i]; }
    }
    return n;
}

uint64_t ArchiveOverlayTotal(const ArchiveOverlay* ov) { return ov->realSize + ov->blob.size(); }
