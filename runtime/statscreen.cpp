// Player ratings screen (LB in the menus, Tab on the keyboard): each team's players with the game's own
// 8 skills (the same ones as its "More Info" screen), an overall, master skill and dunk class.
// Drawn on top of the game as a 1280x720 overlay; the data comes from data/database/playerstats.bin
// and teams.bin. Input while open: d-pad / left stick left-right = player, up-down = team, LB or B = close.
#ifndef STATTEST
#include <windows.h>
#include <d3d11.h>
#endif
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>
#include "statfont.h"

std::string GameRoot();
std::string NbashShoeLabel(uint32_t);      // shoes.cpp (by the player's characterDB record hash, playerstats +4)
#ifndef STATTEST
ID3DBlob* NbashCompileShader(const char* src, const char* name, const char* profile, const char* entry);
#endif
void Log(const char* fmt, ...);

namespace
{
    const int W = 1280, H = 720;
    struct Player { int id; uint32_t chash = 0; std::string first, last; int team, number, cls; float height, weight; int skill[8]; };
    struct Team { int id; std::string name, city; std::vector<int> players; };
    std::vector<Player> g_players; std::vector<Team> g_teams;
    std::mutex g_mx;
    std::vector<int> g_focus; std::string g_focusTeam;      // 15 Nov: opened from the Select Team & Players screen = that team's players, hovered one first
    std::atomic<bool> g_open{false}; std::atomic<int> g_team{0}, g_slot{0}; std::atomic<int> g_version{1};
    std::atomic<bool> g_camOpen{false}; std::atomic<int> g_camSel{0};      // camera options menu (Back button / F2)
    std::atomic<bool> g_gfxOpen{false}; std::atomic<int> g_gfxSel{0}; std::atomic<bool> g_gfxFromGame{false};      // graphics options menu (pad Back button / F1; LB / RB switches to the camera page)
    uint32_t BE(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
    float BEf(const uint8_t* p) { uint32_t v = BE(p); float f; memcpy(&f, &v, 4); return f; }
    std::vector<uint8_t> Read(const std::string& p)
    {
        std::vector<uint8_t> d; if (FILE* f = fopen(p.c_str(), "rb")) { fseek(f, 0, SEEK_END); d.resize(size_t(ftell(f))); fseek(f, 0, SEEK_SET); fread(d.data(), 1, d.size(), f); fclose(f); } return d;
    }
    std::string Str(const uint8_t* p, size_t n) { std::string s; for (size_t i = 0; i < n && p[i]; i++) s += char(p[i]); return s; }

    void Load()
    {
        std::lock_guard lk(g_mx);
        if (!g_players.empty()) return;
        std::string db = GameRoot() + "\\data\\database\\";
        auto ps = Read(db + "playerstats.bin"), ts = Read(db + "teams.bin");
        for (size_t r = 0; r + 288 <= ps.size(); r += 288)
        {
            const uint8_t* q = &ps[r]; Player p{};
            p.id = int(BE(q)); p.chash = BE(q + 4); p.last = Str(q + 8, 32); p.first = Str(q + 40, 32);
            while (!p.last.empty() && !isalpha((unsigned char)p.last[0])) p.last.erase(0, 1);
            p.height = BEf(q + 144); p.weight = BEf(q + 148); p.team = int(BE(q + 152)); p.number = int(BE(q + 156)); p.cls = int(BE(q + 184));
            for (int k = 0; k < 8; k++) p.skill[k] = int(BE(q + 188 + 4 * k));
            g_players.push_back(p);
        }
        for (size_t r = 0; r + 128 <= ts.size(); r += 128)
        {
            const uint8_t* q = &ts[r]; Team t{}; t.id = int(BE(q)); t.name = Str(q + 4, 32); t.city = Str(q + 36, 32);
            if (t.city.empty() || t.city[0] == '~' || t.name.find("All-Star") != std::string::npos || t.city == "Home" || t.city == "Away") continue;
            for (int s = 0; s < 6; s++)
            {
                int pid = int(BE(q + 68 + 4 * s));
                for (size_t i = 0; i < g_players.size(); i++) if (g_players[i].id == pid && pid > 0) t.players.push_back(int(i));
            }
            if (!t.players.empty()) g_teams.push_back(t);
        }
        Log("[stats] %zu players, %zu teams with rosters\n", g_players.size(), g_teams.size());
    }

    // ---------------- CPU drawing into a 1280x720 RGBA canvas ----------------
    std::vector<uint32_t> g_px;
    void Fill(int x0, int y0, int x1, int y1, uint32_t c, float a = 1)
    {
        for (int y = std::max(0, y0); y < std::min(H, y1); y++)
            for (int x = std::max(0, x0); x < std::min(W, x1); x++)
            {
                uint32_t& d = g_px[size_t(y) * W + x];
                auto mix = [&](int sh) { int s = (c >> sh) & 255, t = (d >> sh) & 255; return uint32_t(t + (s - t) * a) << sh; };
                uint32_t da = std::max<uint32_t>(d >> 24, uint32_t(a * 255));
                d = mix(0) | mix(8) | mix(16) | (da << 24);
            }
    }
    struct Font { const uint8_t* d; const StatGlyph* g; };
    const Font M26{ kFM26Data, kFM26Glyphs }, M40{ kFM40Data, kFM40Glyphs }, M70{ kFM70Data, kFM70Glyphs }, S20{ kFS20Data, kFS20Glyphs }, S34{ kFS34Data, kFS34Glyphs }, S64{ kFS64Data, kFS64Glyphs };
    int TextW(const Font& f, const std::string& s) { int w = 0; for (char c : s) if (c >= 32 && c < 127) w += f.g[c - 32].adv; return w; }
    void Text(const Font& f, int x, int y, const std::string& s, uint32_t col, int align = 0, float alpha = 1)
    {
        if (align) x -= TextW(f, s) / (align == 1 ? 2 : 1);
        for (char c : s)
        {
            if (c < 32 || c >= 127) continue;
            const StatGlyph& g = f.g[c - 32]; const uint8_t* a = f.d + g.off;
            for (int j = 0; j < g.h; j++)
                for (int i = 0; i < g.w; i++)
                {
                    int px = x + g.ox + i, py = y + g.oy + j; float al = a[j * g.w + i] / 255.f * alpha;
                    if (al > 0 && px >= 0 && py >= 0 && px < W && py < H) Fill(px, py, px + 1, py + 1, col, al);
                }
            x += g.adv;
        }
    }
    uint32_t Rgb(int r, int g, int b) { return uint32_t(std::clamp(r, 0, 255)) | uint32_t(std::clamp(g, 0, 255)) << 8 | uint32_t(std::clamp(b, 0, 255)) << 16; }
    // rounded rectangle (anti-aliased edge)
    void RRect(int x0, int y0, int x1, int y1, int r, uint32_t c, float a = 1)
    {
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++)
            {
                float dx = std::max({ float(x0 + r) - x - 0.5f, 0.f, x + 0.5f - float(x1 - r) }), dy = std::max({ float(y0 + r) - y - 0.5f, 0.f, y + 0.5f - float(y1 - r) });
                float cov = std::clamp(r - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.f, 1.f);
                if (cov > 0) Fill(x, y, x + 1, y + 1, c, a * cov);
            }
    }
    void Star(float cx, float cy, float R, uint32_t c, uint32_t edge)
    {
        float px[10], py[10];
        for (int k = 0; k < 10; k++) { float ang = -1.5707963f + k * 0.6283185f, rr = (k & 1) ? R * 0.45f : R; px[k] = cx + rr * cosf(ang); py[k] = cy + rr * sinf(ang); }
        for (int y = int(cy - R - 2); y <= int(cy + R + 2); y++)
            for (int x = int(cx - R - 2); x <= int(cx + R + 2); x++)
            {
                int cov = 0;
                for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++)
                {
                    float X = x + (sx + 0.5f) / 4, Y = y + (sy + 0.5f) / 4; bool in = false;
                    for (int i = 0, j = 9; i < 10; j = i++) if (((py[i] > Y) != (py[j] > Y)) && (X < (px[j] - px[i]) * (Y - py[i]) / (py[j] - py[i]) + px[i])) in = !in;
                    cov += in;
                }
                if (cov) Fill(x, y, x + 1, y + 1, cov == 16 ? c : edge, cov / 16.f);
            }
    }
    const char* kSkill[8] = { "Shooting", "Passing", "Dunking", "Blocks / Rebounds", "Handles", "Speed", "Power", "Stealing" };
    const char* kClass[3] = { "SMALL MAN", "HIGH-FLYER", "BIG MAN" };
    int Overall(const Player& p) { int s = 0; for (int k = 0; k < 8; k++) s += std::min(p.skill[k], 100); return (s + 4) / 8; }
    std::string Upper(std::string s) { for (auto& c : s) c = char(toupper((unsigned char)c)); return s; }

    void DrawCamMenu(); void DrawGfxMenu();
    void Draw()
    {
        g_px.assign(size_t(W) * H, 0);
        if (g_gfxOpen && !g_open) { DrawGfxMenu(); return; }
        if (g_camOpen && !g_open) { DrawCamMenu(); return; }
        std::lock_guard lk(g_mx);
        const uint32_t ink = Rgb(28, 24, 22), inkSoft = Rgb(92, 80, 68), red = Rgb(110, 28, 28), redDark = Rgb(70, 16, 16),
                       gold = Rgb(214, 160, 52), goldHi = Rgb(255, 205, 70), cream = Rgb(246, 240, 222), track = Rgb(214, 202, 176);
        const int X0 = 110, Y0 = 46, X1 = 1170, Y1 = 674;
        // the card: a worn, cream photo card with a dark frame and vignette
        RRect(X0 - 14, Y0 - 14, X1 + 14, Y1 + 14, 28, Rgb(8, 8, 8), 0.96f);
        for (int y = Y0; y < Y1; y++)
            for (int x = X0; x < X1; x++)
            {
                float u = (x - X0) / float(X1 - X0), v = (y - Y0) / float(Y1 - Y0);
                float ex = std::min(u, 1 - u) * 2, ey = std::min(v, 1 - v) * 2;
                float vig = std::clamp(std::min(ex * 6.f, ey * 4.5f), 0.f, 1.f); vig = vig * vig * (3 - 2 * vig);
                uint32_t h = uint32_t(x * 374761393u + y * 668265263u); h = (h ^ (h >> 13)) * 1274126177u; float n = ((h >> 24) / 255.f - 0.5f) * 10;
                float warm = 1 - 0.05f * std::sin(u * 3.1f + v * 2.3f);                       // uneven, aged paper
                int r = int((246 * warm + n) * (0.35f + 0.65f * vig)), g = int((240 * warm + n) * (0.33f + 0.67f * vig)), b = int((220 * warm + n) * (0.28f + 0.72f * vig));
                g_px[size_t(y) * W + x] = Rgb(r, g, b) | 0xFF000000u;
            }
        if (g_teams.empty()) { Text(M40, 640, 330, "NO PLAYER DATA FOUND", ink, 1); return; }
        int ti = ((g_team % int(g_teams.size())) + int(g_teams.size())) % int(g_teams.size());
        Team ft; if (!g_focus.empty()) { ft.city = g_focusTeam; ft.players = g_focus; }
        const Team& t = g_focus.empty() ? g_teams[ti] : ft;
        int si = ((g_slot % int(t.players.size())) + int(t.players.size())) % int(t.players.size());
        const Player& p = g_players[t.players[si]];
        // header
        Text(M26, 160, 74, "PLAYER RATINGS", red);
        std::string own; for (auto& tm : g_teams) if (tm.id == p.team) own = tm.city + " " + tm.name;
        Text(S20, 1120, 82, g_focus.empty() ? "<  " + Upper(t.city + " " + t.name) + "  >" : Upper(own), inkSoft, 2);
        Fill(160, 114, 1120, 117, inkSoft, 0.5f);
        // name plate
        Text(M70, 160, 120, Upper(p.first + " " + p.last), ink);
        int inches = int(lroundf(p.height)), lbs = int(lroundf(p.weight));
        char info[128]; snprintf(info, sizeof info, "#%d    %d'%d\"    %d LBS    %s", p.number, inches / 12, inches % 12, lbs, p.cls >= 0 && p.cls < 3 ? kClass[p.cls] : "");
        Text(S20, 164, 214, info, inkSoft);
        char rs[32]; snprintf(rs, sizeof rs, "PLAYER %d OF %zu", si + 1, t.players.size()); if (g_focus.empty()) Text(S20, 900, 214, rs, inkSoft, 2);
        // shoes (shoes.cpp): X / Y change them
        {
            std::string sh = NbashShoeLabel(p.chash);
            if (!sh.empty()) { Text(S20, 164, 240, "SHOES:", red); Text(S20, 164 + TextW(S20, "SHOES:  "), 240, "<  " + Upper(sh) + "  >", ink); }
        }
        // overall: the red tile with gold trim
        RRect(926, 128, 1120, 244, 18, gold); RRect(931, 133, 1115, 239, 14, red);
        char ov[8]; snprintf(ov, sizeof ov, "%d", Overall(p)); Text(S64, 1023, 140, ov, cream, 1);
        Text(S20, 1023, 210, "OVERALL", goldHi, 1);
        // skills
        for (int k = 0; k < 8; k++)
        {
            int col = k / 4, row = k % 4; int x = 160 + col * 490, y = 272 + row * 88;
            int v = p.skill[k]; bool master = v > 100; int shown = std::min(v, 100);
            Text(M26, x, y, kSkill[k], ink);
            if (master) for (int s = 0; s < 3; s++) Star(float(x + 250 + s * 30), float(y + 16), 12.f, goldHi, gold);
            // value tile
            RRect(x + 382, y - 4, x + 450, y + 40, 10, master ? goldHi : gold); RRect(x + 385, y - 1, x + 447, y + 37, 8, master ? Rgb(150, 100, 20) : red);
            char vs[8]; snprintf(vs, sizeof vs, "%d", shown); Text(S34, x + 416, y - 2, vs, cream, 1);
            // bar
            RRect(x, y + 46, x + 450, y + 64, 9, track); RRect(x + 2, y + 48, x + 2 + std::max(18, 446 * shown / 100), y + 62, 7, master ? goldHi : red);
            if (master) Text(S20, x + 6, y + 44, "MASTER", redDark, 0, 0.0f);
        }
        Fill(160, 626, 1120, 628, inkSoft, 0.4f);
        Text(S20, 640, 638, g_focus.empty() ? "LEFT / RIGHT:  PLAYER      UP / DOWN:  TEAM      X / Y:  SHOES      LB:  CLOSE" : "LEFT / RIGHT  OR  X / Y:  CHANGE SHOES        LB:  CLOSE", inkSoft, 1);
    }

#ifndef STATTEST
    // ---------------- D3D11 overlay ----------------
    ID3D11Texture2D* g_tex = nullptr; ID3D11ShaderResourceView* g_srv = nullptr;
    ID3D11VertexShader* g_vs = nullptr; ID3D11PixelShader* g_ps = nullptr; ID3D11BlendState* g_blend = nullptr; ID3D11SamplerState* g_smp = nullptr;
    int g_drawn = 0;
    const char* kSrc = R"(
Texture2D<float4> t : register(t0); SamplerState s : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs_main(uint id : SV_VertexID) { V o; o.uv = float2((id << 1) & 2, id & 2); o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }
float4 ps_main(V i) : SV_Target { return t.Sample(s, i.uv); }
)";
#endif
}

int NbashCamGet(); void NbashCamSet(int); const char* NbashCamName(int); const char* NbashCamHint(int); bool NbashCamAvailable(); extern const int kNbashCamCount;
extern const int kNbashGfxRows; const char* NbashGfxName(int); const char* NbashGfxValue(int); const char* NbashGfxHint(int); void NbashGfxStep(int row, int d);
namespace
{
    void DrawCamMenu()
    {
        const uint32_t gold = Rgb(255, 205, 70), cream = Rgb(246, 240, 222), soft = Rgb(170, 160, 140), dark = Rgb(10, 10, 12), red = Rgb(140, 34, 34);
        int n = kNbashCamCount, rowH = 62, X0 = 400, X1 = 880, Y0 = 150, Y1 = Y0 + 96 + n * rowH + 86, sel = g_camSel.load();
        RRect(X0 - 6, Y0 - 6, X1 + 6, Y1 + 6, 22, gold, 0.9f);
        RRect(X0, Y0, X1, Y1, 18, dark, 0.93f);
        Text(M40, (X0 + X1) / 2, Y0 + 26, "CAMERA", gold, 1);
        for (int i = 0; i < n; i++)
        {
            int y = Y0 + 92 + i * rowH;
            if (i == sel) RRect(X0 + 22, y - 4, X1 - 22, y + rowH - 10, 12, red, 0.95f);
            Text(S34, X0 + 44, y + 2, NbashCamName(i), i == sel ? cream : soft);
            if (i == sel) Text(S20, X0 + 44, y + 34 - 6 + 6, NbashCamHint(i), cream, 0, 0.0f);
        }
        Text(S20, (X0 + X1) / 2, Y1 - 78, NbashCamHint(sel), cream, 1);
        Text(S20, (X0 + X1) / 2, Y1 - 44, "Up / Down: choose     LB / RB: Graphics     A or Back: close", soft, 1);
    }
    void DrawGfxMenu()
    {
        const uint32_t gold = Rgb(255, 205, 70), cream = Rgb(246, 240, 222), soft = Rgb(170, 160, 140), dark = Rgb(10, 10, 12), red = Rgb(140, 34, 34);
        int n = kNbashGfxRows, rowH = 62, X0 = 340, X1 = 940, Y0 = 150, Y1 = Y0 + 96 + n * rowH + 86, sel = g_gfxSel.load();
        RRect(X0 - 6, Y0 - 6, X1 + 6, Y1 + 6, 22, gold, 0.9f);
        RRect(X0, Y0, X1, Y1, 18, dark, 0.93f);
        Text(M40, (X0 + X1) / 2, Y0 + 26, "GRAPHICS", gold, 1);
        for (int i = 0; i < n; i++)
        {
            int y = Y0 + 92 + i * rowH;
            if (i == sel) RRect(X0 + 22, y - 4, X1 - 22, y + rowH - 10, 12, red, 0.95f);
            Text(S34, X0 + 44, y + 2, NbashGfxName(i), i == sel ? cream : soft);
            Text(S34, X1 - 44, y + 2, std::string(i == sel ? "<  " : "") + NbashGfxValue(i) + (i == sel ? "  >" : ""), i == sel ? gold : soft, 2);
        }
        Text(S20, (X0 + X1) / 2, Y1 - 78, NbashGfxHint(sel), cream, 1);
        Text(S20, (X0 + X1) / 2, Y1 - 44, NbashCamAvailable() ? "Up / Down: choose     Left / Right: change     LB / RB: Camera     B: back" : "Up / Down: choose     Left / Right: change     B: back", soft, 1);
    }
}
namespace { std::atomic<uint64_t> g_acceptMs{0}; }
uint64_t NbashMsSinceAccept() { uint64_t t = g_acceptMs.load(); return t ? GetTickCount64() - t : 1000000; }
void NbashGfxMenuOpen()      // from the game's own Options screen (optionsmenu.cpp)
{
    if (g_open || g_gfxOpen) return;
    g_camOpen = false; g_gfxOpen = true; g_gfxFromGame = true; g_version++; Log("[graphics menu] opened from the Options screen\n");
}
void NbashGfxMenuToggle()
{
    if (g_open) return;
    bool o = !g_gfxOpen.load(); if (o) g_camOpen = false;
    g_gfxOpen = o; g_version++; Log("[graphics menu] %s\n", o ? "opened" : "closed");
}
// camera options menu: toggled by the pad's Back button or F2; up / down picks (applies at once and is remembered)
void NbashGfxMenuToggle();
void NbashCamMenuToggle()
{
    if (g_gfxOpen) { NbashGfxMenuToggle(); return; }
    if (!NbashCamAvailable()) { NbashGfxMenuToggle(); return; }
    if (g_open) return;
    bool o = !g_camOpen.load(); if (o) g_camSel = NbashCamGet();
    g_camOpen = o; g_version++; Log("[camera menu] %s\n", o ? "opened" : "closed");
}
void NbashCamMenuMove(int d)
{
    if (!g_camOpen) return;
    int s = ((g_camSel.load() + d) % kNbashCamCount + kNbashCamCount) % kNbashCamCount; g_camSel = s; NbashCamSet(s); g_version++;
}
bool NbashStatsOpen() { return g_open.load() || g_camOpen.load() || g_gfxOpen.load(); }
std::vector<int> NbashStatsPlayerIds() { Load(); std::vector<int> v; std::lock_guard lk(g_mx); for (auto& p : g_players) v.push_back(p.id); return v; }

// called by the input code with player 1's fresh button presses (XInput bits); returns true if it took the input
bool NbashStatsInput(uint16_t pressed, bool inMatch)
{
    // the menus and the game read the pad through two routes; a press seen by both counts once
    {
        static uint64_t last[16];
        uint64_t now = GetTickCount64(); uint16_t fresh = 0;
        for (int b = 0; b < 16; b++) if (pressed & (1 << b)) { if (now - last[b] > 150) fresh |= uint16_t(1 << b); last[b] = now; }
        if (!fresh) return g_open.load() || g_camOpen.load() || g_gfxOpen.load();
        pressed = fresh;
    }
    if (pressed & 0x1000) g_acceptMs = GetTickCount64();
    if (g_gfxOpen)
    {
        if (pressed & (0x2000 | 0x0010 | 0x0020)) { NbashGfxMenuToggle(); return true; }      // B, Start or Back closes
        if ((pressed & (0x0100 | 0x0200)) && NbashCamAvailable()) { g_gfxOpen = false; g_camSel = NbashCamGet(); g_camOpen = true; g_version++; return true; }
        if (pressed & 0x0001) { g_gfxSel = (g_gfxSel.load() + kNbashGfxRows - 1) % kNbashGfxRows; g_version++; }
        if (pressed & 0x0002) { g_gfxSel = (g_gfxSel.load() + 1) % kNbashGfxRows; g_version++; }
        if (pressed & 0x0004) { NbashGfxStep(g_gfxSel.load(), -1); g_version++; }
        if (pressed & 0x0008) { NbashGfxStep(g_gfxSel.load(), +1); g_version++; }      // right = next value (A does nothing: the panel is opened with A from the game's Options row)
        return true;
    }
    if (g_camOpen)
    {
        if (pressed & (0x0100 | 0x0200)) { g_camOpen = false; g_gfxOpen = true; g_version++; return true; }      // LB / RB: graphics page
        if (pressed & 0x0020) { NbashCamMenuToggle(); return true; }
        if (pressed & (0x1000 | 0x2000 | 0x0010)) NbashCamMenuToggle();        // A, B or Start closes
        if (pressed & 0x0001) NbashCamMenuMove(-1);
        if (pressed & 0x0002) NbashCamMenuMove(+1);
        return true;
    }
    if (!g_open)
    {
        // (14 Nov: the pad's Back button no longer opens the graphics overlay - the user wants these settings inside the game's own Options screen; F1 only)
        if (false && !inMatch && (pressed & 0x0020)) { NbashGfxMenuToggle(); return true; }      // pad Back button (keyboard: Backspace) in the menus: graphics options. In a match Back stays the game's own (camera); there use F1
        // 15 Nov: opens ONLY on the Select Team & Players screen while a player is hovered (selecthover.cpp), on that player
        void NbashSelectInput(uint16_t); int NbashSelectHovered(std::vector<uint32_t>*, int*);
        if (!inMatch && (pressed & 0x0100))
        {
            std::vector<uint32_t> ids; int idx = 0; int hov = NbashSelectHovered(&ids, &idx);
            { static int n = 0; if (n++ < 200) Log("[stats] LB in the menus: hovered player id %d (cursor %d of %d)\n", hov, idx, int(ids.size())); }
            if (hov >= 0)
            {
                Load(); std::lock_guard lk(g_mx); g_focus.clear(); g_focusTeam.clear();
                for (size_t i = 0; i < g_players.size(); i++) if (g_players[i].id == hov) { g_focus.push_back(int(i)); break; }      // 16:22: that player ONLY
                if (!g_focus.empty())
                {
                    int pi = -1; for (size_t i = 0; i < g_players.size(); i++) if (g_players[i].id == hov) pi = int(i);
                    g_slot = 0; for (size_t k = 0; k < g_focus.size(); k++) if (g_focus[k] == pi) g_slot = int(k);
                    for (auto& tm : g_teams) for (int q : tm.players) if (q == pi) g_focusTeam = tm.city + " " + tm.name;
                    g_open = true; g_version++; Log("[stats] opened on the hovered player (id %d)\n", hov); return true;
                }
            }
            return false;
        }
        if (!inMatch) NbashSelectInput(pressed);
        return false;
    }
    if ((pressed & 0x0100) || (g_focus.empty() && (pressed & 0x2000))) { g_open = false; Log("[stats] closed\n"); return true; }      // from the select screen only LB closes (B also reached the game there and sent it back to the team card)
    if (!g_focus.empty())      // opened from the select screen: that player only, no paging; left / right change his shoes too (like X / Y)
    {
        if (pressed & 0x0008) pressed |= 0x4000;
        if (pressed & 0x0004) pressed |= 0x8000;
        pressed &= uint16_t(~(0x0001 | 0x0002 | 0x0004 | 0x0008));
    }
    if (pressed & (0x4000 | 0x8000))      // X = next shoe, Y = previous (shoes.cpp)
    {
        void NbashShoeStep(uint32_t, int);
        std::string f, l; uint32_t ch = 0;
        {
            std::lock_guard lk(g_mx);
            Team ft; if (!g_focus.empty()) ft.players = g_focus;
            if (!g_teams.empty())
            {
                const Team& t = !g_focus.empty() ? ft : g_teams[size_t(((g_team % int(g_teams.size())) + int(g_teams.size())) % int(g_teams.size()))];
                const Player& p = g_players[size_t(t.players[size_t(((g_slot % int(t.players.size())) + int(t.players.size())) % int(t.players.size()))])];
                f = p.first; l = p.last; ch = p.chash;
            }
        }
        if (ch) NbashShoeStep(ch, (pressed & 0x4000) ? +1 : -1);
        g_version++;
    }
    if (pressed & 0x0004) { g_slot--; g_version++; }
    if (pressed & 0x0008) { g_slot++; g_version++; }
    if (pressed & 0x0001) { g_team--; g_slot = 0; g_version++; }
    if (pressed & 0x0002) { g_team++; g_slot = 0; g_version++; }
    return true;
}

#ifndef STATTEST
// called by the renderer after the game picture is drawn into the window's viewport
void NbashStatsDraw(ID3D11Device* dev, ID3D11DeviceContext* ctx)
{
    if (!g_open && !g_camOpen && !g_gfxOpen) return;
    if (!g_vs)
    {
        ID3DBlob* vb = NbashCompileShader(kSrc, "stats_vs", "vs_5_0", "vs_main");
        ID3DBlob* pb = NbashCompileShader(kSrc, "stats_ps", "ps_5_0", "ps_main");
        if (!vb || !pb) { Log("[stats] shader compile failed\n"); g_open = false; g_camOpen = false; g_gfxOpen = false; return; }
        dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &g_vs);
        dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g_ps);
        D3D11_TEXTURE2D_DESC td{}; td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        dev->CreateTexture2D(&td, nullptr, &g_tex); dev->CreateShaderResourceView(g_tex, nullptr, &g_srv);
        D3D11_BLEND_DESC bd{}; auto& rt = bd.RenderTarget[0]; rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_SRC_ALPHA; rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlendAlpha = D3D11_BLEND_ZERO; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD; rt.RenderTargetWriteMask = 15;
        dev->CreateBlendState(&bd, &g_blend);
        D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&sd, &g_smp);
    }
    if (g_drawn != g_version) { g_drawn = g_version; Draw(); ctx->UpdateSubresource(g_tex, 0, nullptr, g_px.data(), W * 4, 0); }
    ctx->VSSetShader(g_vs, nullptr, 0); ctx->PSSetShader(g_ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &g_srv); ctx->PSSetSamplers(0, 1, &g_smp);
    float bf[4] = {}; ctx->OMSetBlendState(g_blend, bf, 0xFFFFFFFF);
    ctx->Draw(3, 0);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
}
#endif
