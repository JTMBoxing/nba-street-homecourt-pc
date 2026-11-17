// NBA Street Homecourt PC port - BETA installer.
// Builds the playable game from the tester's OWN disc image (.iso). Nothing of the game itself ships with the port:
//   1. reads the disc's file system (XDVDFS) straight from the .iso and checks every file against disc_files.tsv
//      (path, size, md5 of the USA retail disc this port was made from)
//   2. writes the game files to game\ (and default.xex next to the game program)
//   3. applies the port's patches (patches.tsv + patches\*.vcdiff, xdelta3 / VCDIFF): each one turns one of the disc's own files
//      (or one file inside one of its .big archives) into the port's edited version, checked by md5 before and after
//   4. copies the port's own new files from payload\ (new players, gear, shots...)
//   5. asks which controller layout to use (nbash_controls.txt)
// Usage: drag the .iso onto Install.exe, or run it and pick the .iso.  A log is written to install_log.txt.
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <functional>
#include <algorithm>
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif
extern "C" {
#include "xd3dec.h"
}
namespace fs = std::filesystem;

// ------------------------------------------------------------------ log
static FILE* g_log = nullptr;
static void say(const char* fmt, ...)
{
    char buf[2048]; va_list a; va_start(a, fmt); vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
    fputs(buf, stdout); fflush(stdout);
    if (g_log) { fputs(buf, g_log); fflush(g_log); }
}

// ------------------------------------------------------------------ md5 (RFC 1321)
struct Md5
{
    uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 }; uint64_t len = 0; uint8_t buf[64]; size_t n = 0;
    static uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
    void block(const uint8_t* p)
    {
        static const uint32_t K[64] = {
            0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
            0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
            0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
            0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
        static const int R[64] = { 7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };
        uint32_t w[16]; for (int i = 0; i < 16; i++) w[i] = p[i*4] | (p[i*4+1] << 8) | (p[i*4+2] << 16) | (uint32_t(p[i*4+3]) << 24);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; i++)
        {
            uint32_t f; int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; } else if (i < 32) { f = (d & b) | (~d & c); g = (5*i + 1) & 15; }
            else if (i < 48) { f = b ^ c ^ d; g = (3*i + 5) & 15; } else { f = c ^ (b | ~d); g = (7*i) & 15; }
            uint32_t t = d; d = c; c = b; b = b + rol(a + f + K[i] + w[g], R[i]); a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }
    void update(const void* data, size_t sz)
    {
        const uint8_t* p = (const uint8_t*)data; len += sz;
        while (sz) { size_t k = std::min(sz, 64 - n); memcpy(buf + n, p, k); n += k; p += k; sz -= k; if (n == 64) { block(buf); n = 0; } }
    }
    std::string hex()
    {
        uint64_t bits = len * 8; uint8_t pad = 0x80; update(&pad, 1); uint8_t z = 0; while (n != 56) update(&z, 1);
        uint8_t L[8]; for (int i = 0; i < 8; i++) L[i] = uint8_t(bits >> (8*i)); update(L, 8);
        char s[33]; for (int i = 0; i < 16; i++) snprintf(s + 2*i, 3, "%02x", (h[i/4] >> (8*(i%4))) & 0xFF); return s;
    }
};
static std::string md5of(const std::vector<uint8_t>& v) { Md5 m; m.update(v.data(), v.size()); return m.hex(); }

// ------------------------------------------------------------------ the disc (XDVDFS inside the .iso)
static std::string md5of(const std::vector<uint8_t>& v);
struct Disc
{
    FILE* f = nullptr; uint64_t base = 0;
    struct Ent { std::string path; uint32_t sec, size; };
    std::map<std::string, Ent> files;      // key: lower-case path, "/data/..."
    static std::string lower(std::string s) { for (auto& c : s) c = char(tolower((unsigned char)c)); return s; }
    bool seek(uint64_t o) {
#ifdef _WIN32
        return _fseeki64(f, (long long)o, SEEK_SET) == 0;
#else
        return fseeko(f, (off_t)o, SEEK_SET) == 0;
#endif
    }
    bool read(uint64_t o, void* p, size_t n) { return seek(o) && fread(p, 1, n, f) == n; }
    bool open(const fs::path& iso)
    {
#ifdef _WIN32
        f = _wfopen(iso.wstring().c_str(), L"rb");
#else
        f = fopen(iso.string().c_str(), "rb");
#endif
        if (!f) return false;
        for (uint64_t off : { 0x0ull, 0xFD90000ull, 0x2080000ull, 0x18300000ull })
        {
            char h[20]; if (read(off + 32 * 2048ull, h, 20) && !memcmp(h, "MICROSOFT*XBOX*MEDIA", 20)) { base = off; uint32_t rs[2]; read(off + 32 * 2048ull + 20, rs, 8); walk(rs[0], rs[1], "/"); return !files.empty(); }
        }
        return false;
    }
    void walk(uint32_t sec, uint32_t size, const std::string& prefix)
    {
        std::vector<uint8_t> d(size); if (!read(base + uint64_t(sec) * 2048, d.data(), size)) return;
        std::function<void(uint32_t)> node = [&](uint32_t o) {
            if (o * 4 + 14 > d.size()) return;
            uint16_t l, r; uint32_t fsec, fsz; memcpy(&l, &d[o*4], 2); memcpy(&r, &d[o*4+2], 2); memcpy(&fsec, &d[o*4+4], 4); memcpy(&fsz, &d[o*4+8], 4);
            if (l == 0xFFFF) return;
            uint8_t attr = d[o*4+12], nl = d[o*4+13]; std::string name((const char*)&d[o*4+14], nl);
            if (l) node(l);
            if (attr & 0x10) { if (fsz) walk(fsec, fsz, prefix + name + "/"); }
            else files[lower(prefix + name)] = { prefix + name, fsec, fsz };
            if (r) node(r);
        };
        node(0);
    }
    bool bytes(const std::string& path, std::vector<uint8_t>& out, uint64_t sub = 0, int64_t n = -1)
    {
        auto it = files.find(lower(path)); if (it == files.end()) return false;
        uint64_t len = n < 0 ? it->second.size : uint64_t(n); out.resize(len);
        return read(base + uint64_t(it->second.sec) * 2048 + sub, out.data(), len);
    }
    // one file inside one of the disc's EA .big archives (BIGF / BIG4: big-endian index of offset, size, name)
    bool bigEntry(const std::string& arch, const std::string& entry, std::vector<uint8_t>& out, const std::string& wantMd5)
    {
        std::vector<uint8_t> h; if (!bytes(arch, h, 0, 16)) return false;
        auto be = [](const uint8_t* p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; };
        uint32_t n = be(&h[8]), hs = be(&h[12]); std::vector<uint8_t> idx; if (!bytes(arch, idx, 16, hs)) return false;
        std::string want = lower(entry); size_t p = 0;
        for (uint32_t i = 0; i < n && p + 8 < idx.size(); i++)
        {
            uint32_t off = be(&idx[p]), sz = be(&idx[p+4]); p += 8; size_t e = p; while (e < idx.size() && idx[e]) e++;
            std::string nm((const char*)&idx[p], e - p); p = e + 1;
            std::string base_ = lower(nm.substr(nm.find_last_of('/') == std::string::npos ? 0 : nm.find_last_of('/') + 1));
            if (base_ == want && bytes(arch, out, off, sz) && md5of(out) == wantMd5) return true;      // the same name can sit in several folders of one archive: the md5 picks the right one
        }
        return false;
    }
};

// ------------------------------------------------------------------ helpers
static std::vector<std::vector<std::string>> tsv(const fs::path& p)
{
    std::vector<std::vector<std::string>> rows; std::ifstream in(p); std::string line;
    while (std::getline(in, line)) { if (!line.empty() && line.back() == '\r') line.pop_back(); if (line.empty()) continue;
        std::vector<std::string> c; std::stringstream ss(line); std::string x; while (std::getline(ss, x, '\t')) c.push_back(x); while (c.size() < 9) c.push_back(""); rows.push_back(c); }
    return rows;
}
static bool writeFile(const fs::path& p, const std::vector<uint8_t>& d)
{
    std::error_code ec; fs::create_directories(p.parent_path(), ec);
    FILE* o =
#ifdef _WIN32
        _wfopen(p.wstring().c_str(), L"wb");
#else
        fopen(p.string().c_str(), "wb");
#endif
    if (!o) return false; bool ok = fwrite(d.data(), 1, d.size(), o) == d.size(); fclose(o); return ok;
}
static bool readFile(const fs::path& p, std::vector<uint8_t>& d)
{
    std::ifstream in(p, std::ios::binary); if (!in) return false; d.assign(std::istreambuf_iterator<char>(in), {}); return true;
}
static fs::path rel(const fs::path& root, const std::string& slashPath) { fs::path p = root; std::string s = slashPath; if (!s.empty() && s[0] == '/') s.erase(0, 1); std::stringstream ss(s); std::string part; while (std::getline(ss, part, '/')) p /= part; return p; }
static void pauseExit(int code)
{
    say("\nPress Enter to close this window.\n"); if (g_log) fclose(g_log); getchar(); exit(code);
}
static fs::path pickIso()
{
#ifdef _WIN32
    wchar_t file[MAX_PATH] = L""; OPENFILENAMEW of{}; of.lStructSize = sizeof of; of.lpstrFilter = L"Xbox 360 disc image (*.iso)\0*.iso\0All files\0*.*\0";
    of.lpstrFile = file; of.nMaxFile = MAX_PATH; of.lpstrTitle = L"Pick your NBA Street Homecourt (USA) disc image"; of.Flags = OFN_FILEMUSTEXIST;
    if (GetOpenFileNameW(&of)) return fs::path(file);
#endif
    say("Type (or paste) the full path of your NBA Street Homecourt (USA) .iso and press Enter:\n> ");
    char line[4096] = {}; if (!fgets(line, sizeof line, stdin)) return {};
    std::string s = line; while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back(); if (s.size() > 1 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return fs::path(s);
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    int wargc = 0; LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    fs::path self = fs::path(wargv[0]).parent_path(); fs::path isoArg = wargc > 1 ? fs::path(wargv[1]) : fs::path();
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH); self = fs::path(exe).parent_path();
#else
    fs::path self = fs::absolute(fs::path(argv[0])).parent_path(); fs::path isoArg = argc > 1 ? fs::path(argv[1]) : fs::path();
#endif
    const char* rootEnv = getenv("NBASH_INSTALL_DIR"); fs::path root = rootEnv ? fs::path(rootEnv) : self;      // (tests install somewhere else)
    fs::path data = self / "install_data";
    g_log = fopen((root / "install_log.txt").string().c_str(), "w");
    say("NBA Street Homecourt - PC port  (BETA)  installer\n==================================================\n");
    say("This builds the game from YOUR OWN copy of the disc. Install folder: %s\n\n", root.string().c_str());
    if (!fs::exists(data / "disc_files.tsv")) { say("ERROR: install_data folder is missing next to Install.exe - unzip the whole release first.\n"); pauseExit(1); }

    // UPDATES: running a newer release's Install.exe over an existing install only writes what changed. The disc files are
    // checked once; after that game\.disc_ok remembers it and the .iso is only needed again when a patch needs a disc file.
    std::string ver = "?"; { std::ifstream v(data / "version.txt"); std::getline(v, ver); }
    std::string had = ""; { std::ifstream v(root / "installed_version.txt"); std::getline(v, had); }
    say("Release: %s%s\n", ver.c_str(), had.empty() ? "" : ("  (installed now: " + had + ")").c_str());
    std::vector<uint8_t> dt; readFile(data / "disc_files.tsv", dt); std::string discKey = md5of(dt);
    std::string okKey; { std::ifstream k(root / "game" / ".disc_ok"); std::getline(k, okKey); }
    const bool discDone = okKey == discKey;
    fs::path iso = isoArg.empty() ? pickIso() : isoArg;
    Disc disc;
    if (iso.empty() || !disc.open(iso)) { say("ERROR: could not read an Xbox 360 game from \"%s\".\n", iso.string().c_str()); pauseExit(1); }
    say("Disc image: %s (%zu files)\n", iso.string().c_str(), disc.files.size());

    // ---- 1+2: check and write every disc file
    auto want = tsv(data / "disc_files.tsv");      // path size md5
    int bad = 0, missing = 0; uint64_t total = 0, done = 0; for (auto& r : want) total += std::stoull(r[1]);
    fs::path game = root / "game";
    if (discDone) { say("  game files from your disc: already installed and checked (update mode)\n"); want.clear(); }
    // an interrupted install picks up where it stopped: game\.copied lists the disc files already checked and written
    std::map<std::string, bool> copied; { std::ifstream c(game / ".copied"); std::string l; while (std::getline(c, l)) copied[l] = true; }
    FILE* copiedLog = nullptr; { std::error_code ec; fs::create_directories(game, ec); copiedLog = fopen((game / ".copied").string().c_str(), "a"); }
    for (auto& r : want)
    {
        if (copied.count(r[0])) { done += std::stoull(r[1]); continue; }
        std::vector<uint8_t> b;
        if (!disc.bytes(r[0], b)) { say("  MISSING on your disc: %s\n", r[0].c_str()); missing++; continue; }
        std::string m = md5of(b);
        if (m != r[2]) { say("  DIFFERENT from the USA retail disc: %s\n", r[0].c_str()); bad++; }
        if (r[0].rfind("/$SystemUpdate", 0) != 0 && !writeFile(rel(game, r[0]), b)) { say("ERROR: could not write %s (disk full or no permission?)\n", rel(game, r[0]).string().c_str()); pauseExit(1); }
        if (r[0] == "/default.xex") writeFile(root / "default.xex", b);
        if (m == r[2] && copiedLog) { fprintf(copiedLog, "%s\n", r[0].c_str()); fflush(copiedLog); }
        done += b.size(); say("\r  copying game files from your disc: %3d%%", int(done * 100 / std::max<uint64_t>(total, 1)));
    }
    say("\n");
    if (missing || bad)
    {
        say("\nYour disc image is not the one this port was made from (%d files missing, %d different).\n"
            "The port needs NBA Street Homecourt (USA) for Xbox 360 - title ID 454107F7. A bad or partial rip also causes this.\n", missing, bad);
        pauseExit(1);
    }
    if (!want.empty()) { say("  disc checked: all %zu files match the USA retail disc.\n", want.size()); writeFile(game / ".disc_ok", std::vector<uint8_t>(discKey.begin(), discKey.end())); }

    // ---- 3+4: patches and the port's own files
    auto pt = tsv(data / "patches.tsv");      // target kind source entry source_md5 target_md5 target_size patch_size patchfile
    int np = 0, nn = 0, nk = 0, fail = 0;
    for (auto& r : pt)
    {
        const std::string& target = r[0], &kind = r[1], &src = r[2], &entry = r[3], &smd5 = r[4], &tmd5 = r[5];
        std::vector<uint8_t> out;
        { std::vector<uint8_t> cur; if (readFile(rel(game, target), cur) && md5of(cur) == tmd5) { nk++; continue; } }      // already up to date
        if (kind == "new")
        {
            if (!readFile(rel(data / "payload", target), out)) { say("  ERROR: missing in the release: payload%s\n", target.c_str()); fail++; continue; }
            nn++;
        }
        else
        {
            std::vector<uint8_t> s, p;
            bool ok = kind == "disc" ? disc.bytes(src, s) : disc.bigEntry(src, entry, s, smd5);
            if (!ok || md5of(s) != smd5) { say("  ERROR: source for %s not found / not the original (%s %s)\n", target.c_str(), src.c_str(), entry.c_str()); fail++; continue; }
            if (!readFile(data / "patches" / r[8], p)) { say("  ERROR: missing patch file %s\n", r[8].c_str()); fail++; continue; }
            out.resize(std::stoull(r[6]) + 64); usize_t outLen = 0;
            int e = xd3_decode_memory(p.data(), usize_t(p.size()), s.data(), usize_t(s.size()), out.data(), &outLen, usize_t(out.size()), 0);
            if (e) { say("  ERROR: patch %s failed (%d)\n", target.c_str(), e); fail++; continue; }
            out.resize(outLen); np++;
        }
        if (md5of(out) != tmd5) { say("  ERROR: %s came out wrong (md5)\n", target.c_str()); fail++; continue; }
        if (!writeFile(rel(game, target), out)) { say("ERROR: could not write %s\n", target.c_str()); pauseExit(1); }
        say("\r  applying the port's changes: %d / %zu", np + nn, pt.size());
    }
    say("\n");
    if (fail) { say("\n%d file(s) failed - see install_log.txt. Please send that file with your bug report.\n", fail); pauseExit(1); }
    say("  %d patched files and %d new files written, %d already up to date.\n", np, nn, nk);
    { std::vector<uint8_t> v(ver.begin(), ver.end()); writeFile(root / "installed_version.txt", v); }

    // ---- 5: controller layout
    say("\nController layout:\n  1 = NEW (the port's custom layout - see CONTROLS in the README)\n  2 = ORIGINAL (the game's own buttons)\nPick 1 or 2 and press Enter [1]: ");
    char line[64] = {}; if (!fgets(line, sizeof line, stdin)) line[0] = '1';
    bool orig = line[0] == '2';
    if (FILE* c = fopen((root / "nbash_controls.txt").string().c_str(), "w")) { fprintf(c, "%s\n\nController layout: first word = new or original. Change it any time.\n", orig ? "original" : "new"); fclose(c); }
    say("  layout: %s (change it any time in nbash_controls.txt)\n", orig ? "ORIGINAL" : "NEW");

    say("\nDone! Start the game with \"Play NBA Street Homecourt.bat\".\nThis is a BETA: if anything goes wrong, send nbash_log.txt (and install_log.txt) with your report.\n");
    pauseExit(0);
}
