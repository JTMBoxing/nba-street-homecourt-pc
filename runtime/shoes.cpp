// Shoe picker (15 Nov, user: "add more shoe selections to the game, go into the coding").
// Any player can wear any shoe from a list. The list is game/mods/shoes/shoes.txt ("HASH|Name" per line: HASH = a characterModelDB entry whose model
// is a shoe), the picks are saved in game/mods/shoes/picks.txt ("Player Name|HASH"). Picked in the player ratings screen (LB in the menus):
// X = next shoe, Y = previous shoe; "Default" = the player's own shoes.
// How: when the game reads data/database/characterDB.bin (once, at start-up), the shoe part of every outfit row of a player with a pick is swapped
// in the bytes it reads. A pick made later is also written into the game's in-memory copy of that file (if it is still there - checked by the
// player's name), so it shows from the next game; otherwise from the next start of the program.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <algorithm>

std::string GameRoot();
void Log(const char* fmt, ...);
uint8_t* NbashG2H(uint32_t a);
void GuestUsedRanges(uint32_t out[3][2]);

namespace
{
    const size_t kRec = 1252, kModel = 276;
    struct Rec { std::string name, key, nameKey; uint32_t hash; size_t off; uint32_t orig[12][24]; };      // 17:13: key = the record's own hash (one player record, not every record with the name)
    std::mutex g_m; bool g_loaded = false;
    std::vector<Rec> g_recs; std::map<std::string, size_t> g_byKey;
    std::set<uint32_t> g_shoeHashes;
    std::vector<std::pair<uint32_t, std::string>> g_catalog;      // index 0 = Default (hash 0)
    std::map<std::string, uint32_t> g_picks;                       // key -> hash
    struct Chunk { uint64_t fileOff; uint32_t guest; size_t n; };
    std::vector<Chunk> g_chunks;

    uint32_t BE(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
    void WBE(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
    std::vector<uint8_t> Read(const std::string& p)
    {
        std::vector<uint8_t> d; if (FILE* f = fopen(p.c_str(), "rb")) { fseek(f, 0, SEEK_END); d.resize(size_t(ftell(f))); fseek(f, 0, SEEK_SET); if (fread(d.data(), 1, d.size(), f) != d.size()) d.clear(); fclose(f); } return d;
    }
    std::string Key(const std::string& s) { std::string k; for (char c : s) if (isalnum((unsigned char)c)) k += char(tolower((unsigned char)c)); return k; }
    std::string Trim(std::string s) { while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back(); size_t i = 0; while (i < s.size() && isspace((unsigned char)s[i])) i++; return s.substr(i); }
    std::string Dir() { return GameRoot() + "\\mods\\shoes\\"; }

    void LoadLocked()
    {
        if (g_loaded) return; g_loaded = true;
        std::string db = GameRoot() + "\\data\\database\\";
        auto cm = Read(db + "characterModelDB.bin"), cd = Read(db + "characterDB.bin");
        for (size_t o = 0; o + kModel <= cm.size(); o += kModel)
        {
            std::string msh; for (size_t i = 14; i < 78 && cm[o + i]; i++) msh += char(tolower(cm[o + i]));
            if (msh.find("shoes") != std::string::npos) g_shoeHashes.insert(BE(&cm[o]));
        }
        size_t hdr = cd.size() % kRec;
        for (size_t o = hdr; o + kRec <= cd.size(); o += kRec)
        {
            Rec r{}; r.off = o; for (size_t i = 4; i < 36 && cd[o + i]; i++) r.name += char(cd[o + i]); r.nameKey = Key(r.name);
            r.hash = BE(&cd[o]); char hk[16]; snprintf(hk, sizeof hk, "h%08X", r.hash); r.key = hk;
            for (int row = 0; row < 12; row++) for (int k = 0; k < 24; k++) r.orig[row][k] = BE(&cd[o + 36 + row * 96 + k * 4]);
            if (!r.key.empty() && !g_byKey.count(r.key)) g_byKey[r.key] = g_recs.size();
            g_recs.push_back(r);
        }
        g_catalog.push_back({ 0, "Default" });
        if (FILE* f = fopen((Dir() + "shoes.txt").c_str(), "r"))
        {
            char line[512];
            while (fgets(line, sizeof line, f))
            {
                std::string s = Trim(line); auto bar = s.find('|'); if (s.empty() || s[0] == '#' || bar == std::string::npos) continue;
                uint32_t h = uint32_t(strtoul(s.substr(0, bar).c_str(), nullptr, 16)); std::string nm = Trim(s.substr(bar + 1));
                if (h) { g_catalog.push_back({ h, nm }); g_shoeHashes.insert(h); }
            }
            fclose(f);
        }
        if (FILE* f = fopen((Dir() + "picks.txt").c_str(), "r"))
        {
            char line[512];
            while (fgets(line, sizeof line, f))
            {
                std::string s = Trim(line); auto bar = s.find('|'); if (s.empty() || s[0] == '#' || bar == std::string::npos) continue;
                std::string rest = s.substr(bar + 1); auto bar2 = rest.find('|');
                uint32_t h = uint32_t(strtoul(rest.substr(0, bar2).c_str(), nullptr, 16)); if (!h) continue;
                if (bar2 != std::string::npos) { char hk[16]; snprintf(hk, sizeof hk, "h%08X", uint32_t(strtoul(rest.substr(bar2 + 1).c_str(), nullptr, 16))); g_picks[hk] = h; continue; }
                // old line (name only): the custom record with that name (hash 0x1EB.....), else the first one
                std::string nk = Key(s.substr(0, bar)); const Rec* best = nullptr;
                for (auto& r : g_recs) if (r.nameKey == nk && (!best || ((r.hash >> 20) == 0x1EB && (best->hash >> 20) != 0x1EB))) best = &r;
                if (best) g_picks[best->key] = h;
            }
            fclose(f);
        }
        Log("[shoes] %zu players, %zu shoe models, %zu shoes in the list, %zu picks\n", g_recs.size(), g_shoeHashes.size(), g_catalog.size() - 1, g_picks.size());
    }
    void SaveLocked()
    {
        std::string tmp = Dir() + "picks.txt";
        if (FILE* f = fopen(tmp.c_str(), "w"))
        {
            fprintf(f, "# player | shoe | that player's characterDB record (written by the ratings screen)\n");
            for (auto& [k, h] : g_picks) { auto it = g_byKey.find(k); fprintf(f, "%s|%08X|%s\n", it != g_byKey.end() ? g_recs[it->second].name.c_str() : "?", h, k.c_str() + 1); }
            fclose(f);
        }
    }
    // the value a row word should have now
    uint32_t Want(const Rec& r, int row, int k)
    {
        uint32_t o = r.orig[row][k]; if (!g_shoeHashes.count(o)) return o;
        auto it = g_picks.find(r.key); return it == g_picks.end() ? o : it->second;
    }
    void PatchRange(const Rec& r, uint8_t* buf, uint64_t fileOff, size_t n)
    {
        for (int row = 0; row < 12; row++) for (int k = 0; k < 24; k++)
        {
            uint64_t p = r.off + 36 + row * 96 + k * 4; if (p < fileOff || p + 4 > fileOff + n) continue;
            WBE(buf + (p - fileOff), Want(r, row, k));
        }
    }
    // 15 Nov 16:31 (user: picks did not show): the game does not keep the file buffer - it copies the records. So the game's memory is searched for
    // this player's outfit rows as they are now (with the previous pick) and every copy found is rewritten with the new pick.
    void RowBytes(const Rec& r, int row, uint32_t pick, uint8_t* out)
    {
        for (int k = 0; k < 24; k++) { uint32_t o = r.orig[row][k]; WBE(out + 4 * k, g_shoeHashes.count(o) && pick ? pick : o); }
    }
    int MemPatch(const Rec& r, uint32_t oldPick, uint32_t newPick)
    {
        // only memory the game has allocated (touching the rest would commit GBs)
        uint32_t used[3][2]; GuestUsedRanges(used); const uint32_t ranges[2][2] = { { used[0][0], used[0][1] }, { used[1][0], used[1][1] } };
        Log("[shoes] searching %08X-%08X and %08X-%08X\n", ranges[0][0], ranges[0][1], ranges[1][0], ranges[1][1]);
        int n = 0;
        for (int row = 0; row < 12; row++)
        {
            uint8_t a[96], b[96]; RowBytes(r, row, oldPick, a); RowBytes(r, row, newPick, b);
            if (!memcmp(a, b, 96)) continue;
            bool empty = true; for (int k = 0; k < 96; k++) if (a[k]) { empty = false; break; }
            if (empty) continue;
            // aligned word scan for the first two words (both non-zero hashes), then the whole row is compared (a byte searcher crawls over the
            // zero-filled memory because the rows end in zeros)
            uint32_t w0, w1; memcpy(&w0, a, 4); memcpy(&w1, a + 4, 4);
            for (auto& rg : ranges)
            {
                if (rg[1] <= rg[0] + 96) continue;
                const uint32_t* p = (const uint32_t*)NbashG2H(rg[0]); const uint32_t* e = (const uint32_t*)NbashG2H(rg[1] - 96);
                for (; p < e; p++)
                    if (*p == w0 && p[1] == w1 && !memcmp(p, a, 96)) { memcpy((void*)p, b, 96); n++; p += 23; }
            }
        }
        return n;
    }
    std::map<std::string, uint32_t> g_applied;      // pick that is in the game's memory now (0 = his own)
    void LiveLocked(const Rec& r)
    {
        {
            // 17:10 (user: LeBron on the Sixers did not change): two records can carry the same name (stock LeBron + the Sixers LeBron) -
            // every record with this name is rewritten, not just the first
            uint32_t now = g_picks.count(r.key) ? g_picks[r.key] : 0, before = g_applied.count(r.key) ? g_applied[r.key] : 0;
            int n = MemPatch(r, before, now), recs = 1;      // 17:13: only this player's own record (the Sixers LeBron, not the stock ones)
            g_applied[r.key] = now;
            Log("[shoes] %s (%d records): %d outfit rows rewritten in the game's memory\n", r.name.c_str(), recs, n);
            return;
        }
        for (auto& c : g_chunks)
        {
            if (r.off < c.fileOff || r.off + kRec > c.fileOff + c.n) continue;
            uint8_t* b = NbashG2H(c.guest) + (r.off - c.fileOff);
            if (memcmp(b + 4, r.name.c_str(), r.name.size()) != 0) { Log("[shoes] in-memory copy of %s is gone - pick applies at the next start\n", r.name.c_str()); continue; }
            PatchRange(r, b, r.off, kRec); Log("[shoes] %s: in-memory rows updated\n", r.name.c_str());
        }
    }
    int IndexOf(const std::string& key)
    {
        auto it = g_picks.find(key); if (it == g_picks.end()) return 0;
        for (size_t i = 1; i < g_catalog.size(); i++) if (g_catalog[i].first == it->second) return int(i);
        return 0;
    }
}

uint8_t* NbashG2H(uint32_t a);

// kernel.cpp: every read of a game file
void NbashShoesOnRead(const std::string& hostPath, uint64_t fileOff, uint32_t guest, uint8_t* host, size_t n)
{
    if (!n || hostPath.size() < 15) return;
    std::string low; for (char c : hostPath) low += char(tolower((unsigned char)c));
    if (low.compare(low.size() - 15, 15, "characterdb.bin") != 0) return;
    std::lock_guard lk(g_m); LoadLocked();
    g_chunks.push_back({ fileOff, guest, n });
    int changed = 0;
    for (auto& r : g_recs) if (g_picks.count(r.key)) { PatchRange(r, host, fileOff, n); g_applied[r.key] = g_picks[r.key]; changed++; }
    Log("[shoes] characterDB read (%zu bytes at %llu): shoes swapped for %d players\n", n, (unsigned long long)fileOff, changed);
}

// statscreen.cpp
std::string NbashShoeLabel(uint32_t charHash)
{
    std::lock_guard lk(g_m); LoadLocked();
    char hk[16]; snprintf(hk, sizeof hk, "h%08X", charHash); std::string key = hk;
    if (!g_byKey.count(key)) return "";
    if (g_catalog.size() <= 1) return "Default (no shoes in mods\\shoes\\shoes.txt)";
    return g_catalog[size_t(IndexOf(key))].second;
}
void NbashShoeStep(uint32_t charHash, int d)
{
    std::lock_guard lk(g_m); LoadLocked();
    char hk[16]; snprintf(hk, sizeof hk, "h%08X", charHash); std::string key = hk; auto it = g_byKey.find(key);
    if (it == g_byKey.end() || g_catalog.size() <= 1) return;
    int n = int(g_catalog.size()), i = ((IndexOf(key) + d) % n + n) % n;
    if (i == 0) g_picks.erase(key); else g_picks[key] = g_catalog[size_t(i)].first;
    SaveLocked(); LiveLocked(g_recs[it->second]);
    Log("[shoes] %s -> %s\n", g_recs[it->second].name.c_str(), g_catalog[size_t(i)].second.c_str());
}
