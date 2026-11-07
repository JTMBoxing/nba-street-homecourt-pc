// Uniform mod: shorter jerseys and slightly shorter shorts.
//
// Player meshes (.f1, RefPack-compressed) decompress to a little-endian attribute container
// (magic 3D CD 00 01): attribute count at +16, name table offset at +32, data offset at +40,
// 160-byte attribute records from +0x58 (count at word 6, components at word 13, type at word 14;
// type sizes 4:1 5:2 6:4 7:8 9:4), data blocks 16-byte aligned in record order. Each mesh is
// parsed by sub_82285CF8(this, data); just before that, jersey ("tcloth") and shorts ("bcloth")
// meshes are recognised by their height range and the part below a pivot is raised:
//   jersey: hem (~0.83 m) lifted NBASH_JERSEY_LIFT metres (default 0.19), pivot 1.25 m (chest)
//   shorts: hem (~0.45 m) lifted NBASH_SHORTS_LIFT metres (default 0.07), pivot 1.05 m (hip)
// The cloth-simulation cage (Maya_CageVertices) gets the same change. Byte +15 of the header is
// set to 0x5A once a mesh is done so it is never shortened twice. NBASH_UNIFORM=original: off.
// Survey of all 6939 models: every tcloth (480) and bcloth (478) mesh matches, nothing else does.
#include "guest.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

extern uint8_t* g_base;

namespace
{
    uint32_t U32(uint32_t a) { uint32_t v; memcpy(&v, G2H(a), 4); return v; }   // little-endian data
    float F32(uint32_t a) { float v; memcpy(&v, G2H(a), 4); return v; }
    void SetF32(uint32_t a, float v) { memcpy(G2H(a), &v, 4); }

    struct Block { std::string name; uint32_t count, comps, type, off; };

    bool Parse(uint32_t m, Block* out, int& n)
    {
        uint32_t cnt = U32(m + 16), names = U32(m + 32), data = U32(m + 40);
        if (cnt == 0 || cnt > 32 || names < 0x58 || names > 0x10000 || data < names || data > 0x20000) return false;
        static const uint32_t ts[10] = { 4, 4, 4, 4, 1, 2, 4, 8, 4, 4 };
        uint32_t p = m + names, q = m + data;
        for (uint32_t i = 0; i < cnt; i++)
        {
            uint32_t len = U32(p);
            if (len == 0 || len > 64) return false;
            out[i].name.assign(reinterpret_cast<const char*>(G2H(p + 4)), len);
            p += 4 + len;
            uint32_t rec = m + 0x58 + i * 160;
            out[i].count = U32(rec + 24); out[i].comps = U32(rec + 52); out[i].type = U32(rec + 56);
            if (out[i].count > 200000 || out[i].comps > 16 || out[i].type > 9) return false;
            q = (q + 15) & ~15u;
            out[i].off = q;
            q += out[i].count * out[i].comps * ts[out[i].type];
        }
        n = int(cnt);
        return true;
    }

    std::atomic<int> g_jerseys{0}, g_shorts{0}, g_slimmed{0};
    void SlimImpl(const Block* pos, const Block* cage, uint32_t m);

    // ---- slimmer arms and legs: every character uses one skeleton (T-pose bind), so limb centre
    // lines measured from the body mesh fit all players. Limb vertices move towards the centre line
    // by NBASH_LIMB_SCALE (default 0.88 = 12% thinner), easing in from shoulder/hip and out towards
    // the wrist/ankle so hands, shoes and joins are untouched. Applied to every skinned mesh except
    // jersey/shorts (body, legs, accessories like sleeves and wristbands), so seams stay matched.
    const float kArm[13][3] = {   // |x| -> centre y, z
        {0.30f, 1.6492f, -0.0099f}, {0.35f, 1.6415f, -0.0159f}, {0.40f, 1.6414f, -0.0169f}, {0.45f, 1.6413f, -0.0150f},
        {0.50f, 1.6404f, -0.0155f}, {0.55f, 1.6430f, -0.0114f}, {0.60f, 1.6445f, -0.0020f}, {0.65f, 1.6441f, 0.0058f},
        {0.70f, 1.6423f, 0.0149f}, {0.75f, 1.6382f, 0.0233f}, {0.80f, 1.6359f, 0.0353f}, {0.85f, 1.6322f, 0.0588f},
        {0.90f, 1.6222f, 0.0775f} };
    const float kLeg[14][3] = {   // y -> centre |x|, z
        {0.15f, 0.1154f, -0.0028f}, {0.20f, 0.1184f, -0.0103f}, {0.25f, 0.1252f, -0.0107f}, {0.30f, 0.1184f, -0.0084f},
        {0.35f, 0.1184f, -0.0068f}, {0.40f, 0.1174f, -0.0118f}, {0.45f, 0.1169f, -0.0118f}, {0.50f, 0.1170f, -0.0052f},
        {0.55f, 0.1188f, 0.0101f}, {0.60f, 0.1162f, 0.0246f}, {0.65f, 0.1141f, 0.0374f}, {0.70f, 0.1171f, 0.0403f},
        {0.75f, 0.1139f, 0.0441f}, {0.80f, 0.1129f, 0.0519f} };
    template <size_t N> void Lookup(const float (&t)[N][3], float k, float& a, float& c)
    {
        if (k <= t[0][0]) { a = t[0][1]; c = t[0][2]; return; }
        for (size_t i = 0; i + 1 < N; i++)
            if (k <= t[i + 1][0]) { float u = (k - t[i][0]) / (t[i + 1][0] - t[i][0]); a = t[i][1] + (t[i + 1][1] - t[i][1]) * u; c = t[i][2] + (t[i + 1][2] - t[i][2]) * u; return; }
        a = t[N - 1][1]; c = t[N - 1][2];
    }
    float Ramp(float v, float a, float b) { return std::fmin(1.0f, std::fmax(0.0f, (v - a) / (b - a))); }

    void SlimPoint(float& x, float& y, float& z, float f)
    {
        float ax = std::fabs(x);
        if (ax > 0.30f && y > 1.45f && y < 1.85f)        // arm
        {
            float w = Ramp(ax, 0.30f, 0.38f) * (1 - Ramp(ax, 0.76f, 0.84f));
            if (w <= 0) return;
            float cy, cz; Lookup(kArm, ax, cy, cz);
            float s = 1 - (1 - f) * w;
            y = cy + (y - cy) * s; z = cz + (z - cz) * s;
        }
        else if (ax < 0.30f && y > 0.22f && y < 0.84f)   // leg
        {
            float w = Ramp(y, 0.22f, 0.30f) * (1 - Ramp(y, 0.76f, 0.84f));
            if (w <= 0) return;
            float cx, cz; Lookup(kLeg, y, cx, cz);
            if (x < 0) cx = -cx;
            float s = 1 - (1 - f) * w;
            x = cx + (x - cx) * s; z = cz + (z - cz) * s;
        }
    }

    void Slim(const Block*, int, const Block* pos, const Block* cage, uint32_t m) { SlimImpl(pos, cage, m); }

    void SlimImpl(const Block* pos, const Block* cage, uint32_t m)
    {
        static const float f = [] { const char* v = getenv("NBASH_LIMB_SCALE"); float x = v ? float(atof(v)) : 0.88f; return x < 0.5f ? 0.5f : x > 1.2f ? 1.2f : x; }();
        if (std::fabs(f - 1.0f) < 0.001f) return;
        // only character-sized meshes (never court geometry)
        float mx = 0, lo = 1e9f, hi = -1e9f, mz = 0;
        for (uint32_t v = 0; v < pos->count; v++)
        {
            uint32_t a = pos->off + v * 12;
            mx = std::fmax(mx, std::fabs(F32(a))); float y = F32(a + 4); lo = std::fmin(lo, y); hi = std::fmax(hi, y); mz = std::fmax(mz, std::fabs(F32(a + 8)));
        }
        if (mx > 1.25f || mz > 0.6f || lo < -0.1f || hi > 2.4f) return;
        int moved = 0;
        auto apply = [&](const Block* blk) {
            for (uint32_t v = 0; v < blk->count; v++)
            {
                uint32_t a = blk->off + v * 12;
                float x = F32(a), y = F32(a + 4), z = F32(a + 8), x0 = x, y0 = y, z0 = z;
                SlimPoint(x, y, z, f);
                if (x != x0 || y != y0 || z != z0) { SetF32(a, x); SetF32(a + 4, y); SetF32(a + 8, z); moved++; }
            }
        };
        apply(pos);
        if (cage) apply(cage);
        G2H(m)[15] = 0x5A;
        if (moved && ++g_slimmed <= 3) Log("[uniform] slimmer limbs: %d vertices (mesh of %u, height %.2f..%.2f)\n", moved, pos->count, lo, hi);
    }

    void Shorten(uint32_t m)
    {
        static const bool off = [] { const char* v = getenv("NBASH_UNIFORM"); return v && !strcmp(v, "original"); }();
        if (off) return;
        if (G2H(m)[0] != 0x3D || G2H(m)[1] != 0xCD || G2H(m)[15] == 0x5A) return;
        Block b[32]; int n = 0;
        if (!Parse(m, b, n)) return;
        const Block* pos = nullptr; const Block* cage = nullptr;
        for (int i = 0; i < n; i++)
        {
            if (b[i].name == "Common_Position" && b[i].comps == 3 && b[i].type == 9) pos = &b[i];
            if (b[i].name == "Maya_CageVertices" && b[i].comps == 3 && b[i].type == 9) cage = &b[i];
        }
        if (!pos || pos->count < 50) return;
        float lo = 1e9f, hi = -1e9f;
        for (uint32_t v = 0; v < pos->count; v++) { float y = F32(pos->off + v * 12 + 4); lo = std::fmin(lo, y); hi = std::fmax(hi, y); }
        float pivot, lift;
        // player garments only (crowd clothing is smaller: 120-250 vertices, other hem heights)
        bool jersey = lo >= 0.75f && lo <= 1.0f && hi >= 1.55f && hi <= 1.95f && pos->count >= 400;
        bool shorts = lo >= 0.3f && lo <= 0.56f && hi >= 1.0f && hi <= 1.4f && pos->count >= 400;
        bool skinned = false;
        for (int i = 0; i < n; i++) if (b[i].name == "boneweights" || b[i].name == "boneindices") skinned = true;
        if (!jersey && !shorts) { if (skinned) Slim(b, n, pos, cage, m); return; }
        static const float jl = [] { const char* v = getenv("NBASH_JERSEY_LIFT"); return v ? float(atof(v)) : 0.19f; }();
        static const float sl = [] { const char* v = getenv("NBASH_SHORTS_LIFT"); return v ? float(atof(v)) : 0.07f; }();
        if (jersey) { pivot = 1.25f; lift = jl; }
        else { pivot = 1.05f; lift = sl; }
        if (lift <= 0 || lo + lift >= pivot - 0.05f) return;
        const float s = (pivot - (lo + lift)) / (pivot - lo);   // below the pivot, distances shrink by s
        auto apply = [&](const Block* blk) {
            for (uint32_t v = 0; v < blk->count; v++)
            {
                uint32_t a = blk->off + v * 12 + 4;
                float y = F32(a);
                if (y < pivot) SetF32(a, pivot - (pivot - y) * s);
            }
        };
        apply(pos);
        if (cage) apply(cage);
        G2H(m)[15] = 0x5A;
        int k = jersey ? ++g_jerseys : ++g_shorts;
        if (k <= 3) Log("[uniform] %s: hem %.2f m -> %.2f m (%u vertices%s)\n", jersey ? "jersey" : "shorts", lo, lo + lift, pos->count, cage ? ", cloth cage too" : "");
    }
}

// sub_82285CF8(this, meshData): reads a decompressed mesh
extern "C" PPC_FUNC(__imp__sub_82285CF8);
PPC_FUNC(sub_82285CF8)
{
    if (ctx.r4.u32) Shorten(ctx.r4.u32);
    __imp__sub_82285CF8(ctx, base);
}

// ---- "Gamebreaker earned" look all the time.
// The screen effects (vignette, bloom, 4-corner colour gradient, fog) are tweakables in a table at
// 0x82573960 (12-byte entries: group name, value name, 0). The renderer reads each value through
// its entry: sub_8222FB50(entry) -> float, sub_822301B8(out, entry) -> colour. The 24 Gameplay
// entries (0x825739FC..) and the 24 GBEarned entries (0x82573B34..) are in the same order, so a
// Gameplay read is sent to the matching GBEarned entry. NBASH_GBLOOK=0 keeps the normal look.
namespace
{
    bool GbLook() { static const bool on = [] { const char* v = getenv("NBASH_GBLOOK"); return v && v[0] == '1'; }(); return on; }
    uint32_t MapEntry(uint32_t e)
    {
        if (GbLook() && e >= 0x825739FCu && e < 0x825739FCu + 24 * 12 && (e - 0x825739FCu) % 12 == 0)
            return e + (0x82573B34u - 0x825739FCu);
        return e;
    }
}
extern "C" PPC_FUNC(__imp__sub_8222FB50);
PPC_FUNC(sub_8222FB50)
{
    ctx.r3.u64 = MapEntry(ctx.r3.u32);
    __imp__sub_8222FB50(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_822301B8);
PPC_FUNC(sub_822301B8)
{
    ctx.r4.u64 = MapEntry(ctx.r4.u32);
    __imp__sub_822301B8(ctx, base);
}
