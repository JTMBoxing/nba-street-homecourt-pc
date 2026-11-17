// Per-player jump shots.
// The game has ONE standard jump-shot clip for everybody (SHOT_REG_PEAK = ebo.big entry d334a74c-5a6d-4ab1-af9b-a4fb8a72a1aa.ebo,
// EA::Ant::ClipControllerAsset, 14996 bytes).  A replacement clip with exactly the same layout (same header, same sizes -
// written by the cloud tool anim/build_shot.py) only differs in the range tables and the frame data, i.e. in the bytes
// from file offset 3260 on, and the loaded asset keeps that part byte-for-byte as in the file.  So a player's own shot is
// "installed" by copying that part over the loaded clip while he has the ball, and the stock bytes are copied back for
// everybody else.
//   files:  game/mods/shots/<character hash, 8 hex digits>_shot_reg_peak.ebo / _early.ebo / _late.ebo   (1eb30008 = Paul George)
//   off:    NBASH_SHOTS=0
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "guest.h"
#ifdef _WIN32
#include <windows.h>
#endif

void Log(const char* fmt, ...);
namespace render { extern std::atomic<int> g_shotBurstFrames; }
uint64_t NbashMs();
std::string GameRoot();
uint32_t NbashCarrier(bool* lost);      // drivecam.cpp: the player object that has (or last had) the ball

namespace
{
    const uint32_t kPlayerSize = 0x140C0;
    // the three clips of the standing jump shot (the shot starts on one of them and the game moves to EARLY / PEAK / LATE
    // by when the button is let go; their first frames are the same motion).  dataFrom = start of the constant block:
    // from there on the loaded clip is byte-for-byte the file, and only that part is exchanged.
    struct Clip { const char* tag = nullptr; const char* guid = nullptr; uint32_t size, dataFrom, frames; std::vector<uint8_t> stock; std::atomic<uint32_t> at{0}; };
    // + the five standing-dribble clips (Seq_STAND_DRIBBLE_OFFENCE_V01..V05; same clip layout): a player's own STANCE with the ball
    //   files:  game/mods/shots/<hash>_stand_1.ebo .. _stand_5.ebo   (e.g. LaMelo Ball: feet close together before he shoots)
    const int kMaxClips = 96; int kClips = 13;      // + the three FADEAWAY clips (Seq_SHOT_FADE_PEAK / _EARLY / _LATE; LATE is also the clip of Seq_SHOT_RUN_REVERSE_LATE):  <hash>_shot_fade_peak / _early / _late.ebo
    Clip g_clips[kMaxClips] = { { "shot_reg_peak", "d334a74c-5a6d-4ab1-af9b-a4fb8a72a1aa", 14996, 3260, 3776 },
                        { "shot_reg_early", "29f59098-b167-4fec-82d2-076997403f22", 14740, 3208, 3724 },
                        { "shot_reg_late", "fc43937d-2095-485d-bdc8-4141bf6594b8", 14484, 3160, 3676 },
                        { "stand_1", "10c174ed", 17708, 3760, 4276 }, { "stand_2", "451b2230", 24748, 5060, 5576 }, { "stand_3", "61b4180c", 17964, 3808, 4324 },
                        { "stand_4", "5ce93d9f", 19372, 4060, 4576 }, { "stand_5", "71d44970", 18860, 3960, 4476 },
                        { "shot_fade_peak", "9b51e3b8-6a4b-4c29-aa4a-8ce06f9b619d", 12692, 2860, 3376 }, { "shot_fade_early", "6e76184e-515f-4ee1-80c8-f89385fef02b", 10520, 2460, 2976 },
                        { "shot_fade_late", "5e4036c7-ea62-4731-a3b3-96e098850f5d", 12180, 2760, 3276 },
                        // + dribble moves (14 Nov): the standing front trick clips (Seq_TRICK_HI_FRNT_SPIN / Seq_TRICK_LO_FRNT_SHOWGIRL, _L and _R share the clip): <hash>_trick_hi_frnt.ebo / _trick_lo_frnt.ebo (cloud drib/build_drib.py)
                        { "trick_hi_frnt", "ff2dff6b", 8092, 2008, 2524 }, { "trick_lo_frnt", "1f0f344c", 9636, 2308, 2824 } };
    struct Var { int id; std::vector<uint8_t> d[3]; float after = 0; };      // one more jump shot of the same player: <hash>_shot<N>_reg_peak / _early / _late.ebo (N = 2, 3, ...); after = its own speed after the release (variants.txt)
    struct Shot { uint32_t hash; std::vector<uint8_t> data[kMaxClips]; std::vector<uint8_t> rt[kMaxClips]; std::vector<uint8_t> extra[3][kMaxClips];      /* extra: <hash>_<tag>_bhb.ebo / _spin.ebo (14 Nov: more signature moves);  rt = <hash>_<tag>_rt.ebo: the same slot's second move (turbo held), see NbashSigDribble */ std::vector<Var> vars; };      // alt = a SECOND jump shot (<hash>_shot2_reg_peak / _early / _late.ebo): one of the two is picked at random every time he gets the ball after a shot
    std::vector<Shot> g_shots;
    std::atomic<int> g_state{0};            // 0 not started, 1 loading/searching, 2 ready, 3 off / failed
    uint32_t g_installed = 0;               // hash whose shot is in the clips now (0 = stock)
    void FindSeqs(); void LoadSpeeds(); void LoadMarkers();

    bool ReadFile(const std::string& path, std::vector<uint8_t>& out)
    {
        FILE* f = fopen(path.c_str(), "rb"); if (!f) return false;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); out.resize(size_t(n > 0 ? n : 0));
        bool ok = n > 0 && fread(out.data(), 1, size_t(n), f) == size_t(n); fclose(f); return ok;
    }
    int LoadStock()
    {
        FILE* f = fopen((GameRoot() + "/data/ebo/ebo.big").c_str(), "rb"); if (!f) return 0;
        uint8_t h[16]; int got = 0;
        if (fread(h, 1, 16, f) == 16 && !memcmp(h, "BIG4", 4))
        {
            uint32_t n = (h[8] << 24) | (h[9] << 16) | (h[10] << 8) | h[11], hs = (h[12] << 24) | (h[13] << 16) | (h[14] << 8) | h[15];
            std::vector<uint8_t> idx(hs); fseek(f, 0, SEEK_SET);
            if (fread(idx.data(), 1, hs, f) == hs)
            {
                size_t p = 16;
                for (uint32_t i = 0; i < n && p + 9 < hs; i++)
                {
                    uint32_t off = (idx[p] << 24) | (idx[p + 1] << 16) | (idx[p + 2] << 8) | idx[p + 3], sz = (idx[p + 4] << 24) | (idx[p + 5] << 16) | (idx[p + 6] << 8) | idx[p + 7];
                    const char* name = (const char*)&idx[p + 8]; size_t len = strnlen(name, hs - p - 8);
                    for (int ci = 0; ci < kClips; ci++) { Clip& c = g_clips[ci];
                        if (c.guid && c.stock.empty() && !strncmp(name, c.guid, strlen(c.guid)) && sz == c.size)
                        { c.stock.resize(sz); long keep = ftell(f); fseek(f, long(off), SEEK_SET); if (fread(c.stock.data(), 1, sz, f) == sz) got++; else c.stock.clear(); fseek(f, keep, SEEK_SET); } }
                    p += 8 + len + 1;
                }
            }
        }
        fclose(f); return got;
    }
    void LoadShots()
    {
#ifdef _WIN32
        std::string dir = GameRoot() + "/mods/shots/"; WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((dir + "????????_*.ebo").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do
        {
            std::string fn = fd.cFileName; uint32_t hash = uint32_t(strtoul(fn.substr(0, 8).c_str(), nullptr, 16)); if (!hash) continue;
            for (int k = 0; k < kClips; k++)
            {
                Clip& c = g_clips[k]; int altId = 0;
                if (k < 3 && fn.compare(8, 5, "_shot") == 0 && fn.size() > 14 && isdigit((unsigned char)fn[13]))
                {
                    size_t p = 13; int id = 0; while (p < fn.size() && isdigit((unsigned char)fn[p])) id = id * 10 + (fn[p++] - '0');
                    if (id >= 2 && fn.substr(p) == std::string(c.tag + 4) + ".ebo") altId = id;
                }
                bool isAlt = altId != 0;
                bool isRt = !isAlt && fn == fn.substr(0, 8) + "_" + c.tag + "_rt.ebo";
                int ex = isAlt || isRt ? -1 : fn == fn.substr(0, 8) + "_" + c.tag + "_bhb.ebo" ? 0 : fn == fn.substr(0, 8) + "_" + c.tag + "_spin.ebo" ? 1 : -1;
                if ((!isAlt && !isRt && ex < 0 && fn != fn.substr(0, 8) + "_" + c.tag + ".ebo") || c.stock.empty()) continue;
                std::vector<uint8_t> d;
                if (!ReadFile(dir + fn, d) || d.size() != c.size || memcmp(d.data(), c.stock.data(), c.dataFrom)) { Log("[shots] %s: not the %s layout (size / header differ) - ignored\n", fn.c_str(), c.tag); continue; }
                Shot* s = nullptr; for (auto& x : g_shots) if (x.hash == hash) s = &x;
                if (!s) { g_shots.emplace_back(); s = &g_shots.back(); s->hash = hash; }
                if (isAlt)
                {
                    Var* v = nullptr; for (auto& x : s->vars) if (x.id == altId) v = &x;
                    if (!v) { s->vars.emplace_back(); v = &s->vars.back(); v->id = altId; }
                    v->d[k] = std::move(d);
                }
                else if (ex >= 0) { s->extra[ex][k] = std::move(d); Log("[shots] own clip for character %08X: %s (extra move %d)\n", hash, fn.c_str(), ex); }
                else if (isRt) { s->rt[k] = std::move(d); Log("[shots] own clip for character %08X: %s (turbo move)\n", hash, fn.c_str()); }
                else { s->data[k] = std::move(d); Log("[shots] own clip for character %08X: %s\n", hash, fn.c_str()); }
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        // shot variations: only complete ones (all three clips) count; each may have its own after-release speed:  variants.txt  "<hash> <N> <speed after the release>"
        for (auto& s : g_shots)
        {
            for (size_t i = 0; i < s.vars.size();) { if (s.vars[i].d[0].empty() || s.vars[i].d[1].empty() || s.vars[i].d[2].empty()) s.vars.erase(s.vars.begin() + i); else i++; }
            if (!s.vars.empty()) Log("[shots] character %08X: %d more jump shot(s) - one of %d is picked at random each time he gets the ball\n", s.hash, int(s.vars.size()), int(s.vars.size()) + 1);
        }
        if (FILE* f = fopen((dir + "variants.txt").c_str(), "r"))
        {
            char line[128]; int n = 0;
            while (fgets(line, sizeof line, f))
            {
                unsigned hh = 0; int id = 0; float af = 0;
                if (line[0] == '#' || sscanf(line, "%x %d %f", &hh, &id, &af) != 3 || af < 0.4f || af > 2.0f) continue;
                for (auto& s : g_shots) if (s.hash == hh) for (auto& v : s.vars) if (v.id == id) { v.after = af; n++; }
            }
            fclose(f); Log("[shots] variants.txt: %d after-release speeds\n", n);
        }
#endif
    }
    // the loaded clips: the stock frame data is searched in the game's memory (background thread, repeated until found)
    void Finder()
    {
        // 14 Nov: more exchangeable clips (dribble moves) come from game/mods/shots/clips.txt: "<tag> <guid start> <size> <start of the frame data part> <frames offset>"
        // (cloud drib/tricklib.json); a player's file is <hash>_<tag>.ebo
        if (FILE* cf = fopen((GameRoot() + "/mods/shots/clips.txt").c_str(), "r"))
        {
            char line[256], tag[64], guid[64]; unsigned sz = 0, cq = 0, dat = 0; int added = 0;
            while (fgets(line, sizeof line, cf) && kClips < kMaxClips)
            {
                if (line[0] == '#' || sscanf(line, "%63s %63s %u %u %u", tag, guid, &sz, &cq, &dat) != 5 || sz < 3000 || cq >= dat || dat >= sz) continue;
                bool dup = false; for (int k = 0; k < kClips; k++) if (!strcmp(g_clips[k].tag, tag) || !strncmp(g_clips[k].guid, guid, 8)) dup = true;
                if (dup) continue;
                Clip& c = g_clips[kClips++]; c.tag = _strdup(tag); c.guid = _strdup(guid); c.size = sz; c.dataFrom = cq; c.frames = dat; added++;
            }
            fclose(cf); Log("[shots] clips.txt: %d more exchangeable clips\n", added);
        }
        if (!LoadStock()) { Log("[shots] the stock jump-shot clips were not found in ebo.big - own jump shots are off\n"); g_state = 3; return; }
        LoadShots(); LoadSpeeds(); LoadMarkers();
        if (g_shots.empty()) { g_state = 3; return; }
#ifdef _WIN32
        const size_t sigLen = 64;
        for (int attempt = 0; attempt < 400; attempt++)      // 14 Nov: quick passes over the asset area (A0000000-C0000000; the same memory shows again at C.. and E..) every 0.3 s for the first minutes, a full pass every 12th
        {
            uint64_t t0 = NbashMs(); int missing = 0;
            bool full = attempt % 12 == 11; for (uint64_t a = full ? 0x40000000ull : 0xA0000000ull; a < (full ? 0xFFFF0000ull : 0xC0000000ull);)
            {
                MEMORY_BASIC_INFORMATION mi;
                if (!VirtualQuery(g_base + a, &mi, sizeof mi)) break;
                uint64_t e = uint64_t((uint8_t*)mi.BaseAddress + mi.RegionSize - g_base); if (e > 0xFFFF0000ull) e = 0xFFFF0000ull;
                if (mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && !(a >= 0x82000000ull && a < 0x82800000ull) && e - a > 16000)
                {
                    // 14 Nov: the region is COPIED in pieces (ReadProcessMemory fails cleanly if the game frees it meanwhile; reading it directly could
                    // kill this thread without a word - then nobody had his own shot for that session)
                    static std::vector<uint8_t> buf; const size_t CH = 8u << 20, TAIL = 40000; buf.resize(CH);
                    for (uint64_t c0 = a; c0 + TAIL < e; c0 += CH - 2 * TAIL)
                    {
                        size_t len = size_t(e - c0 < CH ? e - c0 : CH); SIZE_T got = 0;
                        if (!ReadProcessMemory(GetCurrentProcess(), g_base + c0, buf.data(), len, &got) || got != len) continue;
                        const uint8_t* m = buf.data(); size_t n = len - TAIL;
                        for (auto& c : g_clips)
                        {
                            if (!c.guid || c.stock.empty() || c.at.load() || c.size + 64 > TAIL) continue;
                            const uint8_t* sig = c.stock.data() + c.frames;
                            for (size_t k = c.frames & ~size_t(3); k < n + c.frames; k += 4)
                                if (m[k] == sig[0] && m[k + 1] == sig[1] && m[k + 2] == sig[2] && m[k + 3] == sig[3] && !memcmp(m + k, sig, sigLen) && k >= c.frames)
                                {
                                    const uint8_t* base = m + k - c.frames;
                                    if (!memcmp(base + c.dataFrom, c.stock.data() + c.dataFrom, c.size - c.dataFrom))
                                    { c.at = uint32_t(c0 + k - c.frames); Log("[shots] stock %s clip is loaded at %08X (try %d)\n", c.tag, c.at.load(), attempt + 1); break; }
                                    else { static int nd = 0; if (nd < 60) { nd++; size_t first = 0, cnt = 0; for (size_t q = c.dataFrom; q < c.size; q++) if (base[q] != c.stock[q]) { if (!cnt) first = q; cnt++; } Log("[shots] %s: frame data like the stock clip at %08X but %u of %u bytes differ (first at %u, data starts %u) - pass %d\n", c.tag, uint32_t(c0 + k - c.frames), unsigned(cnt), unsigned(c.size - c.dataFrom), unsigned(first), unsigned(c.frames), attempt + 1); } }
                                }
                        }
                    }
                }
                if (e <= a) break;
                a = e;
            }
            int missShot = 0; static bool seqDone = false;
            for (int k = 0; k < kClips; k++) if (!g_clips[k].stock.empty() && !g_clips[k].at.load()) { missing++; if (k < 3) missShot++; }
            if (missShot < 3) g_state = 2;                 // work with what is there, keep looking for the rest
            if (attempt < 4 || !(attempt % 25)) Log("[shots] search pass %d: %d clips still missing (%llu ms)\n", attempt + 1, missing, (unsigned long long)(NbashMs() - t0));
            if (!missShot && !seqDone) { seqDone = true; Log("[shots] all jump-shot clips found (%llu ms for the last pass); %d player(s) with own clips\n", (unsigned long long)(NbashMs() - t0), int(g_shots.size())); FindSeqs(); }
            if (!missing) { Log("[shots] all clips (shots + stances) found\n"); return; }
            Sleep(attempt < 200 ? 300 : 3000);
        }
        Log("[shots] not every jump-shot clip was found in memory\n");
        if (g_state.load() != 2)
#endif
        g_state = 3;
    }
    // ---- playback speed of the standing-shot SEQUENCES (Seq_SHOT_STAND_PEAK / _EARLY / _LATE in ebo.big, EA::Ant::SequenceAnimation).
    // Each has a rate right after a 12-byte header: 1.0 in PEAK and EARLY, 1.0728 in LATE (the long hold - that odd value is what
    // gives it away as a speed).  The ball leaves at a frame of the CLIP the sequence plays, so a lower rate slows the whole shot of
    // ONE player - gather, jump, release, landing - while everybody else moves at normal speed.
    //   game/mods/shots/speed.txt:  "<character hash> <speed>"   (1.0 = normal)
    struct SeqRate { const char* tag; uint8_t sig[16]; uint32_t chkOff; float chkVal; float stock; std::atomic<uint32_t> at[16]; std::atomic<int> n{0}; };      // the game keeps several copies of each sequence (3 of PEAK / EARLY, 6 rate fields of LATE): all are set
    SeqRate g_seq[6] = { { "PEAK",  { 0xff,0xd8,0x00,0x28, 0x00,0x83,0x00,0x00, 0x00,0x5b,0x00,0x04, 0x3f,0x80,0x00,0x00 }, 24, 40.0f, 1.0f },
                         { "EARLY", { 0xff,0xdf,0x00,0x21, 0x00,0xa4,0x00,0x00, 0x00,0x83,0x00,0x04, 0x3f,0x80,0x00,0x00 }, 24, 35.0f, 1.0f },
                         { "LATE",  { 0xff,0xfa,0x00,0x06, 0x00,0x97,0x00,0x00, 0x00,0x91,0x00,0x04, 0x3f,0x89,0x53,0x13 }, 28, 15.0f, 1.0728477f },
                         // the FADEAWAY sequences (Seq_SHOT_FADE_PEAK / _EARLY / _LATE): own speed, 4th / 5th number of a speed.txt line
                         { "FADE_PEAK",  { 0xff,0xdd,0x00,0x23, 0x00,0x88,0x00,0x00, 0x00,0x65,0x00,0x04, 0x3f,0x80,0x00,0x00 }, 24, 35.0f, 1.0f },
                         { "FADE_EARLY", { 0xff,0xea,0x00,0x16, 0x00,0x68,0x00,0x00, 0x00,0x52,0x00,0x04, 0x3f,0x80,0x00,0x00 }, 24, 50.0f, 1.0f },
                         { "FADE_LATE",  { 0xff,0xf8,0x00,0x08, 0x00,0x80,0x00,0x00, 0x00,0x78,0x00,0x04, 0x3f,0x80,0x00,0x00 }, 28, 15.0f, 1.0f } };
    // ---- 12 Nov 21:40: the sequence's own TIMING MARKERS (tags).  Seq_SHOT_STAND_PEAK / _EARLY hold, after the rate field, a tag at 16.0 (rate+340 / +344;
    // where the ball leaves?) and one at 8.0 (rate+380 / +384; where the game picks EARLY / PEAK / LATE?).  markers.txt "<character hash> <release frame>"
    // sets the first tag to <release frame> + 1 while that player has the ball (stock = 15 -> 16.0); everybody else keeps the stock value.
    const uint32_t kTagRel[6] = { 340, 344, 1060, 0, 0, 0 }, kTagBranch[6] = { 380, 384, 0, 0, 0, 0 };      // 22:20: Seq_SHOT_STAND_LATE (the sequence every shot STARTS on) has the same 16.0 tag at rate+1060 (in sequence frames; its clip runs 1.0728 x)
    std::atomic<uint8_t> g_tagValid[3][16];
    std::atomic<int> g_tagsOk{0};
    struct Marker { uint32_t hash; float rel; };
    std::vector<Marker> g_markers;
    std::atomic<uint64_t> g_pressMs{0};      // the shoot button went down (input.cpp -> NbashShotSlowPress)
    std::atomic<uint32_t> g_holdMs{0};       // how long the shoot button was held (set when it comes up; 0 = still down)
    std::atomic<uint32_t> g_holderHash{0};   // character hash of the ball handler (0 = not one of ours), for the input thread
    int g_varId = 1;                        // which of the holder's shots is installed (1 = main)
    void LoadMarkers()
    {
        FILE* f = fopen((GameRoot() + "/mods/shots/markers.txt").c_str(), "r"); if (!f) return;
        char line[128];
        while (fgets(line, sizeof line, f)) { unsigned h = 0; float r = 0; if (line[0] != '#' && sscanf(line, "%x %f", &h, &r) == 2 && h && r >= 10.0f && r <= 30.0f) { g_markers.push_back({ h, r }); Log("[shots] character %08X: release marker at frame %.0f\n", h, r); } }
        fclose(f);
    }
    float MarkerOf(uint32_t hash) { for (auto& m : g_markers) if (m.hash == hash) return m.rel + 1.0f; return 16.0f; }
    struct Speed { uint32_t hash; float speed, after, fade, fadeAfter; };      // after = speed from the moment the ball has left his hands (flick, coming down, landing); same as speed if not given
    std::vector<Speed> g_speeds;
    void LoadSpeeds()
    {
        FILE* f = fopen((GameRoot() + "/mods/shots/speed.txt").c_str(), "r"); if (!f) return;
        char line[128];
        while (fgets(line, sizeof line, f))
        {
            unsigned h = 0; float sp = 0, af = 0, fd = 0, fa = 0; int got = line[0] == '#' ? 0 : sscanf(line, "%x %f %f %f %f", &h, &sp, &af, &fd, &fa);
            if (got >= 2 && h && sp >= 0.4f && sp <= 1.5f) { if (got < 3 || af < 0.4f || af > 2.0f) af = sp; if (got < 4 || fd < 0.4f || fd > 1.5f) fd = 1.0f; if (got < 5 || fa < 0.4f || fa > 2.0f) fa = fd; g_speeds.push_back({ h, sp, af, fd, fa }); if (fd != 1.0f || fa != 1.0f) Log("[shots] character %08X: fadeaway plays at %.0f%% speed, %.0f%% after the release\n", h, fd * 100, fa * 100); Log("[shots] character %08X: standing jump shot plays at %.0f%% speed, %.0f%% after the release\n", h, sp * 100, af * 100); }
        }
        fclose(f);
    }
    void FindSeqs()
    {
#ifdef _WIN32
        if (g_speeds.empty()) return;
        for (int attempt = 0; attempt < 40; attempt++)
        {
            int hits[6] = {};
            for (uint64_t a = 0x40000000ull; a < 0xFFFF0000ull;)
            {
                MEMORY_BASIC_INFORMATION mi;
                if (!VirtualQuery(g_base + a, &mi, sizeof mi)) break;
                uint64_t e = uint64_t((uint8_t*)mi.BaseAddress + mi.RegionSize - g_base); if (e > 0xFFFF0000ull) e = 0xFFFF0000ull;
                if (mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && !(a >= 0x82000000ull && a < 0x82800000ull) && e - a > 64)
                {
                    const uint8_t* m = g_base + a; size_t n = size_t(e - a) - 64;
                    for (size_t k = 0; k < n; k += 4)
                    {
                        if (m[k] != 0xff) continue;
                        for (int q = 0; q < 6; q++)
                        {
                            SeqRate& r = g_seq[q];
                            if (memcmp(m + k, r.sig, 16)) continue;
                            uint32_t b; memcpy(&b, m + k + r.chkOff, 4); b = __builtin_bswap32(b); float chk; memcpy(&chk, &b, 4);
                            if (chk != r.chkVal) continue;
                            if (hits[q] < 16) r.at[hits[q]] = uint32_t(m + k + 12 - g_base);
                            hits[q]++;
                        }
                    }
                }
                if (e <= a) break;
                a = e;
            }
            int found = 0;
            for (int q = 0; q < 6; q++) { g_seq[q].n = hits[q] > 16 ? 16 : hits[q]; if (hits[q]) found++; }
            if (found == 6)
            {
                int ok = 0, bad = 0, okLate = 0;
                for (int q = 0; q < 3; q++) for (int i = 0, n = g_seq[q].n.load(); i < n; i++)
                {
                    g_tagValid[q][i] = 0; uint32_t ra = g_seq[q].at[i].load(); if (!ra) continue;
                    uint32_t b1, b2 = 0; memcpy(&b1, g_base + ra + kTagRel[q], 4); if (kTagBranch[q]) memcpy(&b2, g_base + ra + kTagBranch[q], 4); b1 = __builtin_bswap32(b1); b2 = __builtin_bswap32(b2); float t1, t2; memcpy(&t1, &b1, 4); memcpy(&t2, &b2, 4);
                    if (t1 == 16.0f && (q == 2 || t2 == 8.0f)) { g_tagValid[q][i] = 1; if (q == 2) okLate++; else ok++; }
                    else if (q < 2) { bad++; Log("[shots] %s sequence copy %d: tags read %.3f / %.3f (expected 16 / 8)\n", g_seq[q].tag, i, t1, t2); }
                    else Log("[shots] LATE sequence copy %d: tag reads %.3f (not 16) - left alone\n", i, t1);
                }
                Log("[shots] LATE sequence: %d copies with the 16 tag\n", okLate);
                g_tagsOk = (ok > 0 && bad == 0) ? 1 : 0; Log("[shots] sequence timing markers: %d copies with 16 / 8, %d different -> markers %s\n", ok, bad, g_tagsOk.load() ? "usable" : "NOT used");
            }
            if (found == 6) { Log("[shots] fadeaway sequences found: %d / %d / %d rate fields\n", hits[3], hits[4], hits[5]); Log("[shots] standing-shot sequences found: %d / %d / %d rate fields (PEAK / EARLY / LATE), first at %08X / %08X / %08X (try %d)\n", hits[0], hits[1], hits[2], g_seq[0].at[0].load(), g_seq[1].at[0].load(), g_seq[2].at[0].load(), attempt + 1); return; }
            Sleep(3000);
        }
        Log("[shots] the standing-shot sequences were not all found in memory - per-player shot speed may be incomplete\n");
#endif
    }
    float SpeedOf(uint32_t hash) { for (auto& s : g_speeds) if (s.hash == hash) return s.speed; return 1.0f; }
    float AfterOf(uint32_t hash) { for (auto& s : g_speeds) if (s.hash == hash) return s.after; return 1.0f; }
    float FadeOf(uint32_t hash) { for (auto& s : g_speeds) if (s.hash == hash) return s.fade; return 1.0f; }
    float FadeAfterOf(uint32_t hash) { for (auto& s : g_speeds) if (s.hash == hash) return s.fadeAfter; return 1.0f; }
    void WriteMarker(float tag) { if (!g_tagsOk.load()) return; uint32_t b; memcpy(&b, &tag, 4); b = __builtin_bswap32(b); for (int q = 0; q < 3; q++) for (int i = 0, n = g_seq[q].n.load(); i < n; i++) { uint32_t ra = g_seq[q].at[i].load(); if (ra && g_tagValid[q][i].load()) memcpy(g_base + ra + kTagRel[q], &b, 4); } }
    void WriteRates(float sp, float fsp) { for (int q = 0; q < 6; q++) { SeqRate& r = g_seq[q]; float v = r.stock * (q < 3 ? sp : fsp); uint32_t b; memcpy(&b, &v, 4); b = __builtin_bswap32(b); for (int i = 0, n = r.n.load(); i < n; i++) { uint32_t ra = r.at[i].load(); if (ra) memcpy(g_base + ra, &b, 4); } } }
    float g_varAfter = 0;                   // after-release speed of the shot variation installed now (0 = the player's own from speed.txt)
    int g_ratePhase = 0;                    // 0 = the holder's own speed is written, 1 = his after-release speed
    // which of our characters is this player object?  (the character hash is somewhere in the 82 KB player object)
    uint32_t HashOf(uint32_t pl)
    {
        static uint32_t lastPl = 0, lastHash = 0; static uint64_t lastAt = 0;
        uint64_t now = NbashMs();
        if (pl == lastPl && now - lastAt < 5000) return lastHash;
        uint32_t found = 0;
        for (uint32_t o = 0; o + 4 <= kPlayerSize && !found; o += 4)
        {
            uint32_t v = R32(pl + o);
            if ((v >> 20) != 0x1EB) continue;
            for (auto& s : g_shots) if (s.hash == v) { found = v; static int n = 0; if (n++ < 6) Log("[shots] player %08X is character %08X (hash at player+%05X)\n", pl, v, o); break; }
        }
        lastPl = pl; lastHash = found; lastAt = now; return found;
    }
}

// ---- 13 Nov 01:40 POSE FINDER (first step of a 2K shot player that drives the skeleton itself): 0.3 s into the 3rd shot of a custom player the whole
// read/write memory is searched ONCE for skeleton pose buffers = at least 90 joints in a row of 48 bytes each: scale (1 1 1 .), translation, unit quaternion
// (big-endian floats; the rig has 102 joints, hips = joint 2).  Every hit is logged, and read again 0.2 s and 1.5 s later to see which ones move with the shot.
namespace
{
    float BEf(const uint8_t* p) { uint32_t b; memcpy(&b, p, 4); b = __builtin_bswap32(b); float f; memcpy(&f, &b, 4); return f; }
    bool JointOk(const uint8_t* p)
    {
        float sx = BEf(p), sy = BEf(p + 4), sz = BEf(p + 8); if (!(sx > 0.98f && sx < 1.02f && sy > 0.98f && sy < 1.02f && sz > 0.98f && sz < 1.02f)) return false;
        float x = BEf(p + 32), y = BEf(p + 36), z = BEf(p + 40), w = BEf(p + 44), n = x * x + y * y + z * z + w * w; return n > 0.97f && n < 1.03f;
    }
    void PoseFinder(uint32_t shooter)
    {
#ifdef _WIN32
        uint64_t t0 = NbashMs(); std::vector<uint32_t> hits;
        for (uint64_t a = 0x40000000ull; a < 0xFFFF0000ull;)
        {
            MEMORY_BASIC_INFORMATION mi; if (!VirtualQuery(g_base + a, &mi, sizeof mi)) break;
            uint64_t e = uint64_t((uint8_t*)mi.BaseAddress + mi.RegionSize - g_base); if (e > 0xFFFF0000ull) e = 0xFFFF0000ull;
            if (mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && !(a >= 0x82000000ull && a < 0x82800000ull) && e - a > 48 * 104)
            {
                const uint8_t* m = g_base + a; size_t n = size_t(e - a) - 48 * 104;
                for (size_t k = 0; k < n; k += 16)
                {
                    if (!JointOk(m + k) || !JointOk(m + k + 48) || !JointOk(m + k + 96)) continue;
                    int run = 0; while (run < 110 && k + size_t(run + 1) * 48 <= size_t(e - a) && JointOk(m + k + size_t(run) * 48)) run++;
                    if (run >= 90) { if (hits.size() < 400) hits.push_back(uint32_t(a + k)); k += size_t(run) * 48 - 16; }
                }
            }
            if (e <= a) break; a = e;
        }
        Log("[posefind] shooter %08X: %d pose-like buffers found in %d ms\n", shooter, int(hits.size()), int(NbashMs() - t0));
        auto dump = [&](const char* when) { int n = 0; for (uint32_t h : hits) { const uint8_t* p = g_base + h; if (n++ >= 120) break;
            Log("[posefind] %s %08X hipsT %.3f %.3f %.3f hipsQ %.3f %.3f %.3f %.3f j4Q %.3f %.3f %.3f %.3f j61Q %.3f %.3f %.3f %.3f j63Q %.3f %.3f %.3f %.3f\n", when, h, BEf(p + 96 + 16), BEf(p + 96 + 20), BEf(p + 96 + 24), BEf(p + 96 + 32), BEf(p + 96 + 36), BEf(p + 96 + 40), BEf(p + 96 + 44),
                BEf(p + 192 + 32), BEf(p + 192 + 36), BEf(p + 192 + 40), BEf(p + 192 + 44), BEf(p + 61 * 48 + 32), BEf(p + 61 * 48 + 36), BEf(p + 61 * 48 + 40), BEf(p + 61 * 48 + 44), BEf(p + 63 * 48 + 32), BEf(p + 63 * 48 + 36), BEf(p + 63 * 48 + 40), BEf(p + 63 * 48 + 44)); } };
        dump("t0"); Sleep(200); dump("t+0.2"); Sleep(1300); dump("t+1.5");
#endif
    }
}

// called once per game-clock tick (actiontrace.cpp)
void NbashSigDribbleTick();
void NbashShotSwapTick()
{
    NbashSigDribbleTick();
    { extern void NbashDribSeqApply(uint32_t); NbashDribSeqApply(g_installed); }      // dribseq.cpp: the length of the dribble moves follows the same player
    static const bool off = [] { const char* v = getenv("NBASH_SHOTS"); return v && *v == '0'; }();
    if (off) return;
    int st = g_state.load();
    if (st == 0) { int z = 0; if (g_state.compare_exchange_strong(z, 1)) std::thread(Finder).detach(); return; }
    if (st != 2) return;
    static uint64_t lastCheck = 0, pendingSince = 0; static uint32_t pending = 0;
    uint64_t now = NbashMs();
    bool lost = false; uint32_t pl = NbashCarrier(&lost);
    // two-part speed: the instant the ball has left the shooter's hands his sequences go to his after-release speed (checked every tick)
    static bool reroll = false;      // the ball has left a player with two shots: choose again when the ball is held next
    static uint32_t holder = 0; static float curRate = 1.0f, curFade = 1.0f;      // holder = character (of ours) who has / last had the ball; curRate = speed now written
    if (lost && holder && !reroll) for (auto& s : g_shots) if (s.hash == holder && !s.vars.empty()) reroll = true;
    if (lost && holder && !g_ratePhase)
    {
        float af = (g_varAfter > 0 && holder == g_installed) ? g_varAfter : AfterOf(holder);
        float ff = FadeAfterOf(holder);
        if (af != curRate || ff != curFade) { WriteRates(af, ff); curRate = af; curFade = ff; static int m = 0; if (m++ < 30) Log("[shots] t=%llu ball released: sequences at %.0f%%\n", (unsigned long long)now, af * 100); }
        g_ratePhase = 1;
    }
    {   // how long after the shoot press the ball really leaves (log only; this is what the markers / clips are checked against)
        static uint64_t seenPress = 0, armed = 0; uint64_t pr = g_pressMs.load();
        if (pr != seenPress) { seenPress = pr; armed = (pl >= 0x40000000 && !lost) ? pr : 0; }
        { static const bool recOn = [] { FILE* f = fopen((GameRoot() + "/mods/shots/record_on.txt").c_str(), "r"); if (f) fclose(f); return f != nullptr; }();      // 13 Nov: the recorder only runs when game\mods\shots\record_on.txt exists (it halves the frame rate while it records)
          static int rec = 0; static uint64_t recFor = 0; if (recOn && armed && armed != recFor && rec < 8 && render::g_shotBurstFrames.load() == 0) { recFor = armed; rec++; render::g_shotBurstFrames = 56; Log("[shots] shot recorder: recording shot %d (character %08X)\n", rec, holder); } }
        if (armed && lost) { static int m = 0; if (m++ < 400) Log("[shots] t=%llu shoot press -> ball left the hands after %d ms (character %08X, shot %d, marker %.0f, button held %d ms%s)\n", (unsigned long long)now, int(now - armed), holder, g_varId, MarkerOf(holder), int(g_holdMs.load() ? g_holdMs.load() : now - armed), g_holdMs.load() ? "" : " and still down"); armed = 0; }
        { static int shotsSeen = 0; static uint64_t cnt = 0; static bool done = false; if (armed && armed != cnt && holder) { cnt = armed; shotsSeen++; } static const bool pf = getenv("NBASH_POSEFIND") != nullptr;      // 13 Nov 02:20: the pose finder only runs when asked for (it found nothing and may stutter)
          if (pf && !done && armed && holder && shotsSeen == 3 && now - armed >= 300) { done = true; uint32_t sh = pl; std::thread([sh] { PoseFinder(sh); }).detach(); } }
        if (armed && now - armed > 2500) armed = 0;
    }
    if (now - lastCheck < 100) return; lastCheck = now;
    if (pl < 0x40000000 || lost) return;                       // nobody holds the ball (a shot may be in the air): leave the clip alone
    // The speed always belongs to whoever holds the ball NOW (written at once, also back to 100% for a player without an entry), so one
    // player's slow shot can never carry over to the next ball handler.  Only the clip exchange below waits half a second.
    {
        holder = HashOf(pl); g_holderHash = holder; float sp = SpeedOf(holder), fs = FadeOf(holder);
        static float curTag = 16.0f; float tg = MarkerOf(holder);
        if (tg != curTag) { WriteMarker(tg); curTag = tg; static int m2 = 0; if (m2++ < 40) Log("[shots] ball handler %08X: release marker %.0f\n", pl, tg); }
        if (g_ratePhase || sp != curRate || fs != curFade) { WriteRates(sp, fs); curFade = fs; static int m = 0; if (sp != curRate && m++ < 40) Log("[shots] ball handler %08X: standing-shot sequences at %.0f%% speed\n", pl, sp * 100); curRate = sp; g_ratePhase = 0; }
    }
    uint32_t want = HashOf(pl);
    if (want == g_installed && !reroll) { pending = want; pendingSince = now; return; }
    if (want != pending) { pending = want; pendingSince = now; return; }
    if (now - pendingSince < 500) return;                      // the previous shooter's landing may still be playing
    const Shot* sh = nullptr;
    for (auto& s : g_shots) if (s.hash == want) sh = &s;
    static uint32_t rnd = uint32_t(now) | 1; rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5; rnd ^= uint32_t(now);
    int pick = (sh && !sh->vars.empty()) ? int((rnd >> 7) % uint32_t(sh->vars.size() + 1)) : 0;      // 0 = his main shot, 1.. = a variation
    const Var* var = pick ? &sh->vars[size_t(pick - 1)] : nullptr; bool second = var != nullptr;
    for (int k = 0; k < kClips; k++)
    {
        Clip& c = g_clips[k]; uint32_t a = c.at.load(); if (!a) continue;
        bool sig = !strncmp(c.tag, "sig_", 4);      // signature dribble slots: stock unless a right-stick gesture has just put his move in (NbashSigDribble)
        const std::vector<uint8_t>& src = sig ? c.stock : (var && k < 3) ? var->d[k] : (sh && !sh->data[k].empty()) ? sh->data[k] : c.stock;
        memcpy(g_base + a + c.dataFrom, src.data() + c.dataFrom, c.size - c.dataFrom);
    }
    static int n = 0; if (n++ < 80) Log("[shots] ball handler %08X: %s jump shot installed (variation %d)\n", pl, second ? "a VARIATION of his" : want ? "his own" : "the standard", var ? var->id : 1);
    g_varAfter = var ? var->after : 0.0f; g_varId = var ? var->id : 1;
    reroll = false;
    g_installed = want;
}

// ---------------------------------------------------------------- slower jump shot for chosen players
// The moment the ball leaves the hand is fixed by the game's shot sequence (frame 15 = half a second after the press), so
// inside the clip a jump can only be made slower by making it lower.  Here the GAME CLOCK itself runs slower while such a
// player shoots (its own speed factor, clock+24 - the same one the inbound fast-forward uses): the whole shot, the release
// and the landing all play slower - and so does everything else on the court for that second and a half.
//   file:  game/mods/shots/speed.txt   lines "<character hash, 8 hex digits> <speed>"   e.g.  1eb20023 0.80
//   Starts on the shoot press when that player has the ball; called off if the press becomes a dunk / layup, and after
//   0.9 s if no ball has left his hands (pump fake).  Only for the player the pad controls.
bool NbashSimDunkActive();
int NbashHumanOnDefense();
namespace
{
    struct Slow { uint32_t hash; float speed; };
    std::vector<Slow> g_slow; std::atomic<int> g_slowState{0};      // 0 not loaded, 1 loaded
    std::atomic<uint64_t> g_slowPress{0};
    void LoadSlow()
    {
        FILE* f = fopen((GameRoot() + "/mods/shots/speed.txt").c_str(), "r");
        if (f)
        {
            char line[128];
            while (fgets(line, sizeof line, f))
            {
                unsigned h = 0; float s = 0;
                if (line[0] != '#' && sscanf(line, "%x %f", &h, &s) == 2 && h && s >= 0.4f && s <= 1.0f) { g_slow.push_back({ h, s }); Log("[shots] character %08X: jump shot plays at %.0f%% speed\n", h, s * 100); }
            }
            fclose(f);
        }
        g_slowState = 1;
    }
    float SlowFor(uint32_t pl)
    {
        static uint32_t lastPl = 0; static float last = 1.0f; static uint64_t lastAt = 0;
        uint64_t now = NbashMs(); if (pl == lastPl && now - lastAt < 5000) return last;
        float sp = 1.0f;
        for (uint32_t o = 0; o + 4 <= kPlayerSize && sp == 1.0f; o += 4)
        {
            uint32_t v = R32(pl + o); if ((v >> 20) != 0x1EB) continue;
            for (auto& s : g_slow) if (s.hash == v) { sp = s.speed; break; }
        }
        lastPl = pl; last = sp; lastAt = now; return sp;
    }
}
uint64_t NbashShotPressMs() { return g_pressMs.load(); } uint32_t NbashShotHolderHash() { return g_holderHash.load(); } uint32_t NbashShotHoldMs() { return g_holdMs.load(); }
void NbashShotSlowPress() { g_slowPress = NbashMs(); g_pressMs = NbashMs(); g_holdMs = 0; }
// input.cpp, on a shoot press: how long the GAME should see the button held for the ball handler (0 = leave the press alone).
// = (his release frame + 1) / 30 s minus the ~155 ms the game takes from button-up to letting the ball go.  markers.txt gives the release frame.
uint32_t NbashShotAutoHoldMs()
{
    static const bool off = [] { const char* v = getenv("NBASH_SHOTAUTO"); return v && *v == '0'; }();
    if (off || g_state.load() != 2 || NbashHumanOnDefense() == 1) return 0;
    bool lost = false; uint32_t pl = NbashCarrier(&lost); if (pl < 0x40000000 || lost) return 0;
    uint32_t h = g_holderHash.load(); if (!h) return 0;
    for (auto& m : g_markers) if (m.hash == h) { float ms = (m.rel + 1.0f) * 1000.0f / 30.0f - 155.0f; return ms > 120.0f ? uint32_t(ms) : 0; }
    return 0;
}
void NbashShotButtonUp(uint64_t heldMs) { g_holdMs = uint32_t(heldMs > 1 ? heldMs : (heldMs == 1 && g_pressMs.load()) ? (NbashMs() - g_pressMs.load()) : 1); }       // input.cpp: the shoot button went down
// called once per game-clock tick (actiontrace.cpp); 1.0 = normal speed
float NbashShotSlowSpeed()
{
    static const bool off = [] { const char* v = getenv("NBASH_SHOTSLOW"); return v && *v == '0'; }();
    static const bool clockMode = [] { const char* v = getenv("NBASH_SHOTSLOW_CLOCK"); return v && *v == '1'; }();      // the whole-game slow-down (first try, 17:00) is only used when asked for
    if (off || !clockMode) return 1.0f;
    if (!g_slowState.load()) { LoadSlow(); }
    if (g_slow.empty()) return 1.0f;
    static uint64_t start = 0, released = 0; static float sp = 1.0f;
    uint64_t now = NbashMs(), p = g_slowPress.exchange(0);
    bool lost = false; uint32_t pl = NbashCarrier(&lost);
    if (p && now - p < 250 && !start && pl >= 0x40000000 && !lost && NbashHumanOnDefense() != 1)
    {
        float s = SlowFor(pl);
        if (s < 0.999f) { start = now; released = 0; sp = s; static int n = 0; if (n++ < 30) Log("[shots] t=%llu shoot press by a slow-shot player: game clock %.2f\n", (unsigned long long)now, s); }
    }
    if (!start) return 1.0f;
    double t = double(now - start), dur = 1450.0 / sp;                 // the shot clip is 1.43 s of game time
    if (lost && !released) released = now;
    bool cancel = (t < 700 && NbashSimDunkActive()) || (!released && t > 900);       // dunk / layup, or no shot came (pump fake)
    static uint64_t outFrom = 0;
    if ((cancel || t > dur) && !outFrom) { outFrom = now; if (cancel) { static int n = 0; if (n++ < 30) Log("[shots] t=%llu slow shot called off (%s)\n", (unsigned long long)now, t < 700 && NbashSimDunkActive() ? "dunk / layup" : "no shot"); } }
    if (outFrom)
    {
        double u = double(now - outFrom) / 300.0;                       // back to normal speed over 0.3 s
        if (u >= 1.0) { start = 0; outFrom = 0; return 1.0f; }
        return sp + (1.0f - sp) * float(u);
    }
    if (t < 100) return 1.0f + (sp - 1.0f) * float(t / 100.0);         // eased in over 0.1 s
    return sp;
}

// ---------------------------------------------------------------- signature dribble moves on the right stick (14 Nov)
// The quick dribble moves of the trick button (Seq_X_*) last 0.15-0.3 s with fixed feet - too short for a real move.  The Y tricks (Seq_TRICK_Y_STD_HI_*)
// last a second and animate everything.  A custom player's 2K moves are files for those slots:  <hash>_sig_<slot>.ebo (plain right stick) and
// <hash>_sig_<slot>_rt.ebo (right stick with turbo held).  They are copied into the loaded clips only at the moment of a right-stick flick (input.cpp sends
// Y for it); the game's own Y tricks (LT + stick) get the stock clips back first, and the slots go back to stock 1.5 s after a flick anyway.
// variant: 0 = stock back, 1 = plain move, 2 = turbo move.  Returns 1 when the ball handler has such a move and it is in place now.
static std::atomic<uint64_t> g_sigUntil{0};
static std::atomic<int> g_sigVariant{0};
int NbashSigVariant() { return g_sigVariant.load(); }      // the last move put in (1 crossover, 2 turbo, 3 behind-the-back, 4 spin)
int NbashSigDribble(int variant)
{
    if (variant) g_sigVariant = variant;
    if (g_state.load() != 2) return 0;
    const Shot* sh = nullptr; if (variant) for (auto& s : g_shots) if (s.hash == g_installed) sh = &s;
    int n = 0;
    for (int k = 0; k < kClips; k++)
    {
        Clip& c = g_clips[k]; uint32_t a = c.at.load(); if (!a || strncmp(c.tag, "sig_", 4)) continue;
        const std::vector<uint8_t>* src = &c.stock;
        if (sh)
        {
            if (variant >= 3 && variant <= 5) { if (!sh->extra[variant - 3][k].empty()) { src = &sh->extra[variant - 3][k]; n++; } else if (!sh->data[k].empty()) { src = &sh->data[k]; n++; } }      // 3 bhb, 4 spin
            else if (variant == 2 && !sh->rt[k].empty()) { src = &sh->rt[k]; n++; } else if (!sh->data[k].empty()) { src = &sh->data[k]; n++; }
        }
        memcpy(g_base + a + c.dataFrom, src->data() + c.dataFrom, c.size - c.dataFrom);
    }
    g_sigUntil = n ? NbashMs() + 8000 : 0;      // 14 Nov 20:55: slow (full-length) moves last up to ~4 s; the clips go back when the sequence is over (NbashSigOverSoon)
    static int said = 0; if (n && said++ < 30) Log("[shots] signature dribble (%s) in place for character %08X: %d clips\n", variant == 2 ? "turbo" : variant == 3 ? "behind-the-back" : variant == 4 ? "spin" : "plain", g_installed, n);
    return n ? 1 : 0;
}
void NbashSigOverSoon() { if (g_sigUntil.load()) g_sigUntil = NbashMs() + 400; }      // the move's sequence ended: stock clips back after the blend out
void NbashSigDribbleTick() { uint64_t u = g_sigUntil.load(); if (u && NbashMs() > u) NbashSigDribble(0); }
// gameplay.cpp (signature speed): is this address inside one of the loaded signature clips?
bool NbashIsSigClipAddr(uint32_t a)
{
    for (int k = 0; k < kClips; k++) { Clip& c = g_clips[k]; uint32_t at = c.at.load(); if (at && !strncmp(c.tag, "sig_", 4) && a >= at && a < at + c.size) return true; }
    return false;
}

// does the ball handler have an extra move (3 bhb, 4 spin)?  input.cpp only waits for a half circle when he does
bool NbashSigHasMove(int variant)
{
    if (g_state.load() != 2) return false;
    for (auto& s : g_shots) if (s.hash == g_installed) for (int k = 0; k < kClips; k++) if (!strncmp(g_clips[k].tag, "sig_", 4) && !s.extra[variant - 3][k].empty()) return true;
    return false;
}
