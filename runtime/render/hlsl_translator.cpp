// Xenos ucode -> HLSL. Instruction semantics follow Xenia's shader interpreter and
// translators (Direct3D 9 rules: 0 * anything = 0, max/min NaN behavior, etc.).
//
// Control flow is emitted as a "program counter" loop so jumps and loops work:
//   uint pc = 0; [loop] while (true) { if (pc == 0) { ... pc = L1; } if (pc == L1) { ... } ... }
// A jump sets pc and restarts the loop; blocks before the target are skipped.
#include "hlsl_translator.h"
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <algorithm>

using namespace xe::gpu;

namespace render
{
const char* kCommonHlsl = R"(
cbuffer FloatConstants : register(b0) { float4 c[512]; };
cbuffer SystemConstants : register(b1)
{
    uint4 xe_bools[2];
    uint4 xe_loops[8];
    uint4 xe_vfetch[96];
    float4 xe_tsize[32];
    uint4 xe_tinfo[32];
    float4 xe_vp_scale;
    float4 xe_vp_offset;
    uint4 xe_flags;
    float4 xe_rt_size;
    float4 xe_alpha_ref;
};
// half -> float without f16tof32 (not available in every HLSL compiler)
float XeHalf(uint h)
{
    h &= 0xFFFFu;
    uint e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    float v = e == 0 ? float(m) * (1.0 / 16777216.0)
                     : exp2(float(min(e, 30u)) - 15.0) * (1.0 + float(m) * (1.0 / 1024.0));
    return (h & 0x8000u) ? -v : v;
}
struct XeVertexOut
{
    float4 pos : SV_Position;
    float4 t0 : TEXCOORD0;
    float4 t1 : TEXCOORD1;
    float4 t2 : TEXCOORD2;
    float4 t3 : TEXCOORD3;
    float4 t4 : TEXCOORD4;
    float4 t5 : TEXCOORD5;
    float4 t6 : TEXCOORD6;
    float4 t7 : TEXCOORD7;
    float4 t8 : TEXCOORD8;
    float4 t9 : TEXCOORD9;
    float4 t10 : TEXCOORD10;
    float4 t11 : TEXCOORD11;
    float4 t12 : TEXCOORD12;
    float4 t13 : TEXCOORD13;
    float4 t14 : TEXCOORD14;
    float4 t15 : TEXCOORD15;
};
float4 XMul(float4 a, float4 b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
float XMulS(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
float4 XMax(float4 a, float4 b) { return a >= b ? a : b; }
float4 XMin(float4 a, float4 b) { return a < b ? a : b; }
float XMaxS(float a, float b) { return a >= b ? a : b; }
float XMinS(float a, float b) { return a < b ? a : b; }
float XDot4(float4 a, float4 b) { float4 m = XMul(a, b); return m.x + m.y + m.z + m.w; }
float XDot3(float4 a, float4 b) { float4 m = XMul(a, b); return m.x + m.y + m.z; }
float4 XMax4(float4 a)
{
    float r;
    if (a.x >= a.y && a.x >= a.z && a.x >= a.w) r = a.x;
    else if (a.y >= a.z && a.y >= a.w) r = a.y;
    else if (a.z >= a.w) r = a.z;
    else r = a.w;
    return r.xxxx;
}
float4 XCube(float4 s)
{
    float x = s.z, y = s.w, z = s.x;
    float ax = abs(x), ay = abs(y), az = abs(z);
    float4 r;
    if (az >= ax && az >= ay) r = float4(-y, z < 0 ? -x : x, z, z < 0 ? 5.0 : 4.0);
    else if (ay >= ax) r = float4(y < 0 ? -z : z, x, y, y < 0 ? 3.0 : 2.0);
    else r = float4(-y, x < 0 ? z : -z, x, x < 0 ? 1.0 : 0.0);
    r.z *= 2.0;
    return r;
}
bool XBool(uint i) { return ((xe_bools[i >> 7][(i >> 5) & 3] >> (i & 31)) & 1) != 0; }
uint XLoop(uint i) { return xe_loops[i >> 2][i & 3]; }
uint XSwap(uint v, uint endian)
{
    if (endian == 1) return ((v & 0x00FF00FF) << 8) | ((v & 0xFF00FF00) >> 8);
    if (endian == 2) return (v << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
    if (endian == 3) return (v << 16) | (v >> 16);
    return v;
}
float4 XTexSwizzle(float4 v, uint swz)
{
    float s[6] = { v.x, v.y, v.z, v.w, 0.0, 1.0 };
    return float4(s[swz & 7], s[(swz >> 3) & 7], s[(swz >> 6) & 7], s[(swz >> 9) & 7]);
}
float XUnorm(uint v, uint bits) { return float(v) / float((1u << bits) - 1u); }
float XSnorm(uint v, uint bits, uint rf)
{
    int s = int(v << (32 - bits)) >> (32 - bits);
    float m = float((1u << (bits - 1)) - 1u);
    return rf != 0 ? (float(s) + 0.5) / (m + 0.5) : max(float(s) / m, -1.0);
}
float XSint(uint v, uint bits) { return float(int(v << (32 - bits)) >> (32 - bits)); }
)";

static const char kComp[] = { 'x', 'y', 'z', 'w', '0', '1' };

static std::string Fmt(const char* f, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

std::string HlslTranslator::Translate(Shader& shader, std::string& errors)
{
    Shader::Translation* t = shader.GetOrCreateTranslation(0);
    errors_.clear();
    bool ok = TranslateAnalyzedShader(*t);
    errors = errors_;
    if (!ok || failed_) return {};
    const auto& bin = t->translated_binary();
    return std::string(bin.begin(), bin.end());
}

void HlslTranslator::Reset()
{
    ShaderTranslator::Reset();
    body_.clear();
    indent_ = 2;
    failed_ = false;
    texturesUsed_ = 0;
    textures3D_ = 0;
    vfetchUsed_ = 0;
}

void HlslTranslator::EmitTranslationError(const char* message, bool is_fatal)
{
    ShaderTranslator::EmitTranslationError(message, is_fatal);
    errors_ += message;
    errors_ += "\n";
    if (is_fatal) failed_ = true;
}

void HlslTranslator::Line(const std::string& s)
{
    body_.append(indent_ * 4, ' ');
    body_ += s;
    body_ += '\n';
}

std::string HlslTranslator::BoolConstant(uint32_t index) { return Fmt("XBool(%u)", index); }

void HlslTranslator::StartTranslation()
{
    straight_ = current_shader().label_addresses().empty();
    if (straight_)
    {
        Line("{");
        indent_++;
        Line("{");
        indent_++;
        return;
    }
    Line("uint pc = 0;");
    Line("[loop] while (true)");
    Line("{");
    indent_++;
    if (const char* dbg = getenv("NBASH_VSDEBUG_REG"); dbg && dbg[0] == 'k' && is_vertex_shader())
        Line("xot15 = float4(3, 3, 3, 3);   // debug: does a write inside the loop survive?");
    Line("if (pc == 0)");
    Line("{");
    indent_++;
}

void HlslTranslator::ProcessLabel(uint32_t cf_index)
{
    if (cf_index == 0) return;   // the first block already starts at 0
    Line(Fmt("pc = %u;", cf_index));
    indent_--;
    Line("}");
    Line(Fmt("if (pc == %u)", cf_index));
    Line("{");
    indent_++;
}

void HlslTranslator::ProcessExecInstructionBegin(const ParsedExecInstruction& instr)
{
    switch (instr.type)
    {
    case ParsedExecInstruction::Type::kConditional:
        Line(Fmt("if (%s == %s)", BoolConstant(instr.bool_constant_index).c_str(), instr.condition ? "true" : "false"));
        break;
    case ParsedExecInstruction::Type::kPredicated:
        Line(Fmt("if (p0 == %s)", instr.condition ? "true" : "false"));
        break;
    default:
        Line("// exec");
        break;
    }
    Line("{");
    indent_++;
}

void HlslTranslator::ProcessExecInstructionEnd(const ParsedExecInstruction& instr)
{
    indent_--;
    Line("}");
    if (instr.is_end && !straight_) Line("break;");
}

void HlslTranslator::ProcessLoopStartInstruction(const ParsedLoopStartInstruction& instr)
{
    Line("{");
    indent_++;
    Line(Fmt("uint lcst = XLoop(%u);", instr.loop_constant_index));
    Line("ld++; lstk[ld] = aL; lcnt[ld] = lcst & 0xFF;");
    if (!instr.is_repeat) Line("aL = int((lcst >> 8) & 0xFF);");
    Line(Fmt("if (lcnt[ld] == 0) { aL = lstk[ld]; ld--; pc = %u; continue; }", instr.loop_skip_address));
    indent_--;
    Line("}");
}

void HlslTranslator::ProcessLoopEndInstruction(const ParsedLoopEndInstruction& instr)
{
    Line("{");
    indent_++;
    Line(Fmt("uint lcst = XLoop(%u);", instr.loop_constant_index));
    Line("lcnt[ld]--; aL += int(lcst << 8) >> 24;");
    std::string brk = instr.is_predicated_break ? Fmt(" && p0 != %s", instr.predicate_condition ? "true" : "false") : "";
    Line(Fmt("if (lcnt[ld] != 0%s) { pc = %u; continue; }", brk.c_str(), instr.loop_body_address));
    Line("aL = lstk[ld]; ld--;");
    indent_--;
    Line("}");
}

void HlslTranslator::ProcessCallInstruction(const ParsedCallInstruction&)
{
    EmitTranslationError("shader subroutine calls are not supported yet", true);
}

void HlslTranslator::ProcessReturnInstruction(const ParsedReturnInstruction&)
{
    EmitTranslationError("shader subroutine returns are not supported yet", true);
}

void HlslTranslator::ProcessJumpInstruction(const ParsedJumpInstruction& instr)
{
    std::string cond;
    if (instr.type == ParsedJumpInstruction::Type::kConditional)
        cond = Fmt("%s == %s", BoolConstant(instr.bool_constant_index).c_str(), instr.condition ? "true" : "false");
    else if (instr.type == ParsedJumpInstruction::Type::kPredicated)
        cond = Fmt("p0 == %s", instr.condition ? "true" : "false");
    if (cond.empty()) Line(Fmt("{ pc = %u; continue; }", instr.target_address));
    else Line(Fmt("if (%s) { pc = %u; continue; }", cond.c_str(), instr.target_address));
}

// ---------------------------------------------------------------- operands and results
std::string HlslTranslator::Operand(const InstructionOperand& op, uint32_t components)
{
    std::string rel;
    if (op.storage_addressing_mode == InstructionStorageAddressingMode::kAddressRegisterRelative) rel = " + a0";
    else if (op.storage_addressing_mode == InstructionStorageAddressingMode::kLoopRelative) rel = " + aL";
    std::string base;
    switch (op.storage_source)
    {
    case InstructionStorageSource::kRegister:
        base = rel.empty() ? Fmt("r[%u]", op.storage_index) : Fmt("r[clamp(%u%s, 0, %u)]", op.storage_index, rel.c_str(), register_count() - 1);
        break;
    case InstructionStorageSource::kConstantFloat:
    {
        uint32_t offset = is_pixel_shader() ? 256 : 0;
        base = rel.empty() ? Fmt("c[%u]", op.storage_index + offset)
                           : Fmt("c[clamp(%u%s, 0, 255) + %u]", op.storage_index, rel.c_str(), offset);
        break;
    }
    default:
        base = "float4(0, 0, 0, 0)";
        break;
    }
    std::string swz = ".";
    for (uint32_t i = 0; i < components; i++)
    {
        uint32_t s = uint32_t(op.GetComponent(i));
        swz += s < 4 ? kComp[s] : 'x';
    }
    std::string v = base + swz;
    if (op.is_absolute_value) v = "abs(" + v + ")";
    if (op.is_negated) v = "-(" + v + ")";
    return v;
}

std::string HlslTranslator::OperandComponent(const InstructionOperand& op, uint32_t component)
{
    InstructionOperand one = op;
    one.components[0] = op.GetComponent(component);
    one.component_count = 1;
    return Operand(one, 1);
}

void HlslTranslator::Store(const InstructionResult& result, const std::string& value)
{
    uint32_t mask = result.GetUsedWriteMask();
    if (!mask) return;
    std::string target;
    std::string rel;
    if (result.storage_addressing_mode == InstructionStorageAddressingMode::kAddressRegisterRelative) rel = " + a0";
    else if (result.storage_addressing_mode == InstructionStorageAddressingMode::kLoopRelative) rel = " + aL";
    switch (result.storage_target)
    {
    case InstructionStorageTarget::kRegister:
        target = rel.empty() ? Fmt("r[%u]", result.storage_index)
                             : Fmt("r[clamp(%u%s, 0, %u)]", result.storage_index, rel.c_str(), register_count() - 1);
        break;
    case InstructionStorageTarget::kInterpolator:
        if (result.storage_index >= kInterpolators) return;
        target = Fmt("xot%u", result.storage_index);   // separate variables, not an array (see below)
        break;
    case InstructionStorageTarget::kPosition: target = "opos"; break;
    case InstructionStorageTarget::kColor:
        if (result.storage_index >= 4) return;
        target = Fmt("oc[%u]", result.storage_index);
        break;
    case InstructionStorageTarget::kDepth: target = "odepth"; break;
    case InstructionStorageTarget::kExportAddress: target = "xeA"; break;
    case InstructionStorageTarget::kExportData:
        if (result.storage_index >= 5) return;
        target = Fmt("xeM[%u]", result.storage_index);
        Line(Fmt("xeMMask |= %uu;", 1u << result.storage_index));
        break;
    default:
        return;   // point size: not supported yet
    }
    std::string v = result.is_clamped ? "saturate(" + value + ")" : value;
    Line("{");
    indent_++;
    Line("float4 sv = " + v + ";");
    for (uint32_t i = 0; i < 4; i++)
    {
        if (!(mask & (1u << i))) continue;
        uint32_t s = uint32_t(result.components[i]);
        std::string src = s < 4 ? std::string("sv.") + kComp[s] : (s == 4 ? "0.0" : "1.0");
        if (result.storage_target == InstructionStorageTarget::kDepth) { Line("odepth = " + src + ";"); break; }
        Line(target + "." + kComp[i] + " = " + src + ";");
    }
    indent_--;
    Line("}");
}

// ---------------------------------------------------------------- fetches
void HlslTranslator::ProcessVertexFetchInstruction(const ParsedVertexFetchInstruction& instr)
{
    if (!is_vertex_shader()) return;
    const auto& a = instr.attributes;
    uint32_t fc = instr.operands[1].storage_index;
    vfetchUsed_ |= 1;
    if (instr.is_predicated) Line(Fmt("if (p0 == %s)", instr.predicate_condition ? "true" : "false"));
    Line("{");
    indent_++;
    if (!instr.is_mini_fetch)
    {
        Line(Fmt("vfa = int(%u) * int(floor(%s%s)) + int(xe_vfetch[%u].x);", a.stride,
            OperandComponent(instr.operands[0], 0).c_str(), a.is_index_rounded ? " + 0.5" : "", fc));
        Line(Fmt("vfc = %u;", fc));
    }
    if (instr.result.GetUsedWriteMask())
    {
        Line(Fmt("int fa = vfa + (%d);", a.offset));
        Line("uint4 d = uint4(XVfetch(vfc, fa), XVfetch(vfc, fa + 1), XVfetch(vfc, fa + 2), XVfetch(vfc, fa + 3));");
        std::string sg = a.is_signed ? "1" : "0";
        uint32_t rf = a.signed_rf_mode == xenos::SignedRepeatingFractionMode::kNoZero ? 1 : 0;
        bool norm = !a.is_integer;
        auto comp = [&](const char* word, uint32_t shift, uint32_t bits) {
            std::string v = Fmt("((%s >> %u) & 0x%Xu)", word, shift, bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1u);
            if (a.is_signed) return norm ? Fmt("XSnorm(%s, %u, %u)", v.c_str(), bits, rf) : Fmt("XSint(%s, %u)", v.c_str(), bits);
            return norm ? Fmt("XUnorm(%s, %u)", v.c_str(), bits) : Fmt("float(%s)", v.c_str());
        };
        std::string v;
        switch (a.data_format)
        {
        case xenos::VertexFormat::k_8_8_8_8:
            v = "float4(" + comp("d.x", 0, 8) + ", " + comp("d.x", 8, 8) + ", " + comp("d.x", 16, 8) + ", " + comp("d.x", 24, 8) + ")"; break;
        case xenos::VertexFormat::k_2_10_10_10:
            v = "float4(" + comp("d.x", 0, 10) + ", " + comp("d.x", 10, 10) + ", " + comp("d.x", 20, 10) + ", " + comp("d.x", 30, 2) + ")"; break;
        case xenos::VertexFormat::k_10_11_11:
            v = "float4(" + comp("d.x", 0, 11) + ", " + comp("d.x", 11, 11) + ", " + comp("d.x", 22, 10) + ", 0)"; break;
        case xenos::VertexFormat::k_11_11_10:
            v = "float4(" + comp("d.x", 0, 10) + ", " + comp("d.x", 10, 11) + ", " + comp("d.x", 21, 11) + ", 0)"; break;
        case xenos::VertexFormat::k_16_16:
            v = "float4(" + comp("d.x", 0, 16) + ", " + comp("d.x", 16, 16) + ", 0, 0)"; break;
        case xenos::VertexFormat::k_16_16_16_16:
            v = "float4(" + comp("d.x", 0, 16) + ", " + comp("d.x", 16, 16) + ", " + comp("d.y", 0, 16) + ", " + comp("d.y", 16, 16) + ")"; break;
        case xenos::VertexFormat::k_16_16_FLOAT:
            v = "float4(XeHalf(d.x), XeHalf(d.x >> 16), 0, 0)"; break;
        case xenos::VertexFormat::k_16_16_16_16_FLOAT:
            v = "float4(XeHalf(d.x), XeHalf(d.x >> 16), XeHalf(d.y), XeHalf(d.y >> 16))"; break;
        case xenos::VertexFormat::k_32:
        case xenos::VertexFormat::k_32_32:
        case xenos::VertexFormat::k_32_32_32_32:
            if (a.is_signed) v = norm ? "float4(int4(d)) / 2147483647.0" : "float4(int4(d))";
            else v = norm ? "float4(d) / 4294967295.0" : "float4(d)";
            break;
        case xenos::VertexFormat::k_32_FLOAT:
        case xenos::VertexFormat::k_32_32_FLOAT:
        case xenos::VertexFormat::k_32_32_32_FLOAT:
        case xenos::VertexFormat::k_32_32_32_32_FLOAT:
            v = "asfloat(d)"; break;
        default:
            EmitTranslationError("unsupported vertex format", false);
            v = "float4(0, 0, 0, 0)";
            break;
        }
        if (a.exp_adjust) v = Fmt("(%s) * %.9g", v.c_str(), std::ldexp(1.0, a.exp_adjust));
        Store(instr.result, v);
    }
    indent_--;
    Line("}");
}

void HlslTranslator::ProcessTextureFetchInstruction(const ParsedTextureFetchInstruction& instr)
{
    if (!instr.has_result()) return;      // set/get LOD and gradient ops: not needed yet
    uint32_t tf = instr.operands[1].storage_index;
    if (instr.opcode != ucode::FetchOpcode::kTextureFetch || tf >= kMaxTextures)
    {
        Store(instr.result, "float4(0, 0, 0, 0)");
        return;
    }
    texturesUsed_ |= 1u << tf;
    const auto& a = instr.attributes;
    if (instr.is_predicated) Line(Fmt("if (p0 == %s)", instr.predicate_condition ? "true" : "false"));
    Line("{");
    indent_++;
    if (instr.dimension == xenos::FetchOpDimension::k2D || instr.dimension == xenos::FetchOpDimension::k1D)
    {
        Line("float2 uv = " + Operand(instr.operands[0], 2) + ";");
        if (a.unnormalized_coordinates) Line(Fmt("uv *= xe_tsize[%u].zw;", tf));
        if (a.offset_x != 0 || a.offset_y != 0) Line(Fmt("uv += float2(%.9g, %.9g) * xe_tsize[%u].zw;", a.offset_x, a.offset_y, tf));
        std::string sample;
        if (is_pixel_shader() && a.use_computed_lod && !a.use_register_lod)
            sample = a.lod_bias != 0 ? Fmt("xt%u.SampleBias(xs%u, uv, %.9g)", tf, tf, a.lod_bias) : Fmt("xt%u.Sample(xs%u, uv)", tf, tf);
        else
            sample = Fmt("xt%u.SampleLevel(xs%u, uv, 0)", tf, tf);
        Store(instr.result, Fmt("XTexSwizzle(%s, xe_tinfo[%u].x)", sample.c_str(), tf));
    }
    else if (instr.dimension == xenos::FetchOpDimension::k3DOrStacked)
    {
        // 3D textures (e.g. colour-grading lookup tables in the final composite)
        textures3D_ |= 1u << tf;
        Line("float3 uvw = " + Operand(instr.operands[0], 3) + ";");
        if (a.unnormalized_coordinates) Line(Fmt("uvw *= float3(xe_tsize[%u].zw, xe_tsize[%u].zw.x);", tf, tf));
        std::string sample = (is_pixel_shader() && a.use_computed_lod && !a.use_register_lod)
            ? Fmt("xt%u.Sample(xs%u, uvw)", tf, tf) : Fmt("xt%u.SampleLevel(xs%u, uvw, 0)", tf, tf);
        Store(instr.result, Fmt("XTexSwizzle(%s, xe_tinfo[%u].x)", sample.c_str(), tf));
    }
    else
    {
        Store(instr.result, "float4(0, 0, 0, 0)");   // TODO: cube textures
    }
    indent_--;
    Line("}");
}

// memory export: eA/eM written after an "alloc export" are committed at the next alloc (or the end)
void HlslTranslator::ProcessAllocInstruction(const ParsedAllocInstruction&, uint8_t)
{
    if (is_vertex_shader()) Line("XFlushExport(xeA, xeM, xeMMask); xeMMask = 0u;");
}

// ---------------------------------------------------------------- ALU
void HlslTranslator::ProcessAluInstruction(const ParsedAluInstruction& instr, uint8_t)
{
    using V = ucode::AluVectorOpcode;
    using S = ucode::AluScalarOpcode;
    if (instr.IsNop()) return;
    if (instr.is_predicated) Line(Fmt("if (p0 == %s)", instr.predicate_condition ? "true" : "false"));
    Line("{");
    indent_++;

    // ---- vector part
    bool doVector = !instr.IsVectorOpDefaultNop();
    if (doVector)
    {
        auto o = [&](int i) { return Operand(instr.vector_operands[i]); };
        std::string v;
        switch (instr.vector_opcode)
        {
        case V::kAdd: v = o(0) + " + " + o(1); break;
        case V::kMul: v = "XMul(" + o(0) + ", " + o(1) + ")"; break;
        case V::kMax: v = "XMax(" + o(0) + ", " + o(1) + ")"; break;
        case V::kMin: v = "XMin(" + o(0) + ", " + o(1) + ")"; break;
        case V::kSeq: v = "float4(" + o(0) + " == " + o(1) + ")"; break;
        case V::kSgt: v = "float4(" + o(0) + " > " + o(1) + ")"; break;
        case V::kSge: v = "float4(" + o(0) + " >= " + o(1) + ")"; break;
        case V::kSne: v = "float4(" + o(0) + " != " + o(1) + ")"; break;
        case V::kFrc: v = "frac(" + o(0) + ")"; break;
        case V::kTrunc: v = "trunc(" + o(0) + ")"; break;
        case V::kFloor: v = "floor(" + o(0) + ")"; break;
        case V::kMad: v = "XMul(" + o(0) + ", " + o(1) + ") + " + o(2); break;
        case V::kCndEq: v = "(" + o(0) + " == 0.0) ? " + o(1) + " : " + o(2); break;
        case V::kCndGe: v = "(" + o(0) + " >= 0.0) ? " + o(1) + " : " + o(2); break;
        case V::kCndGt: v = "(" + o(0) + " > 0.0) ? " + o(1) + " : " + o(2); break;
        case V::kDp4: v = "XDot4(" + o(0) + ", " + o(1) + ").xxxx"; break;
        case V::kDp3: v = "XDot3(" + o(0) + ", " + o(1) + ").xxxx"; break;
        case V::kDp2Add:
            v = "(XMul(" + o(0) + ", " + o(1) + ").x + XMul(" + o(0) + ", " + o(1) + ").y + (" + o(2) + ").x).xxxx"; break;
        case V::kCube: v = "XCube(" + o(0) + ")"; break;
        case V::kMax4: v = "XMax4(" + o(0) + ")"; break;
        case V::kSetpEqPush:
        case V::kSetpNePush:
        case V::kSetpGtPush:
        case V::kSetpGePush:
        {
            const char* cmp = instr.vector_opcode == V::kSetpEqPush ? "== 0.0" : instr.vector_opcode == V::kSetpNePush ? "!= 0.0"
                            : instr.vector_opcode == V::kSetpGtPush ? "> 0.0" : ">= 0.0";
            Line("float4 pa = " + o(0) + ", pb = " + o(1) + ";");
            Line(Fmt("p0 = pa.w == 0.0 && pb.w %s;", cmp));
            v = Fmt("((pa.x == 0.0 && pb.x %s) ? 0.0 : pa.x + 1.0).xxxx", cmp);
            break;
        }
        case V::kKillEq:
        case V::kKillGt:
        case V::kKillGe:
        case V::kKillNe:
        {
            const char* cmp = instr.vector_opcode == V::kKillEq ? "==" : instr.vector_opcode == V::kKillGt ? ">"
                            : instr.vector_opcode == V::kKillGe ? ">=" : "!=";
            Line(Fmt("bool kv = any(%s %s %s);", o(0).c_str(), cmp, o(1).c_str()));
            if (is_pixel_shader()) Line("if (kv) discard;");
            v = "float(kv).xxxx";
            break;
        }
        case V::kDst:
            v = "float4(1.0, XMulS((" + o(0) + ").y, (" + o(1) + ").y), (" + o(0) + ").z, (" + o(1) + ").w)"; break;
        case V::kMaxA:
            Line("a0 = int(floor(clamp((" + o(0) + ").w, -256.0, 255.0) + 0.5));");
            v = "XMax(" + o(0) + ", " + o(1) + ")";
            break;
        default:
            EmitTranslationError("unknown vector opcode", false);
            v = "float4(0, 0, 0, 0)";
            break;
        }
        Line("float4 vr = " + v + ";");
    }

    // ---- scalar part
    bool doScalar = !instr.IsScalarOpDefaultNop();
    if (doScalar)
    {
        std::string s0, s1;
        if (instr.scalar_operand_count >= 1)
        {
            s0 = OperandComponent(instr.scalar_operands[0], 0);
            if (instr.scalar_operands[0].component_count >= 2) s1 = OperandComponent(instr.scalar_operands[0], 1);
        }
        if (instr.scalar_operand_count >= 2) s1 = OperandComponent(instr.scalar_operands[1], 0);
        if (s0.empty()) s0 = "0.0";
        if (s1.empty()) s1 = "0.0";
        Line("float sa = " + s0 + ", sb = " + s1 + ";");
        std::string r;
        switch (instr.scalar_opcode)
        {
        case S::kAdds: case S::kAddsc0: case S::kAddsc1: r = "sa + sb"; break;
        case S::kAddsPrev: r = "sa + ps"; break;
        case S::kMuls: case S::kMulsc0: case S::kMulsc1: r = "XMulS(sa, sb)"; break;
        case S::kMulsPrev: r = "XMulS(sa, ps)"; break;
        case S::kMulsPrev2: r = "(ps == -3.402823466e38 || !isfinite(ps) || !isfinite(sb) || sb <= 0.0) ? -3.402823466e38 : XMulS(sa, ps)"; break;
        case S::kMaxs: r = "XMaxS(sa, sb)"; break;
        case S::kMins: r = "XMinS(sa, sb)"; break;
        case S::kSeqs: r = "float(sa == 0.0)"; break;
        case S::kSgts: r = "float(sa > 0.0)"; break;
        case S::kSges: r = "float(sa >= 0.0)"; break;
        case S::kSnes: r = "float(sa != 0.0)"; break;
        case S::kFrcs: r = "frac(sa)"; break;
        case S::kTruncs: r = "trunc(sa)"; break;
        case S::kFloors: r = "floor(sa)"; break;
        case S::kExp: r = "exp2(sa)"; break;
        case S::kLogc: r = "max(log2(sa), -3.402823466e38)"; break;
        case S::kLog: r = "log2(sa)"; break;
        case S::kRcpc: r = "clamp(1.0 / sa, -3.402823466e38, 3.402823466e38)"; break;
        case S::kRcpf: r = "(isinf(1.0 / sa) ? 0.0 : 1.0 / sa)"; break;
        case S::kRcp: r = "1.0 / sa"; break;
        case S::kRsqc: r = "clamp(rsqrt(sa), -3.402823466e38, 3.402823466e38)"; break;
        case S::kRsqf: r = "(isinf(rsqrt(sa)) ? 0.0 : rsqrt(sa))"; break;
        case S::kRsq: r = "rsqrt(sa)"; break;
        case S::kMaxAs: Line("a0 = int(floor(clamp(sa, -256.0, 255.0) + 0.5));"); r = "XMaxS(sa, sb)"; break;
        case S::kMaxAsf: Line("a0 = int(floor(clamp(sa, -256.0, 255.0)));"); r = "XMaxS(sa, sb)"; break;
        case S::kSubs: case S::kSubsc0: case S::kSubsc1: r = "sa - sb"; break;
        case S::kSubsPrev: r = "sa - ps"; break;
        case S::kSetpEq: Line("p0 = sa == 0.0;"); r = "float(!p0)"; break;
        case S::kSetpNe: Line("p0 = sa != 0.0;"); r = "float(!p0)"; break;
        case S::kSetpGt: Line("p0 = sa > 0.0;"); r = "float(!p0)"; break;
        case S::kSetpGe: Line("p0 = sa >= 0.0;"); r = "float(!p0)"; break;
        case S::kSetpInv: Line("p0 = sa == 1.0;"); r = "p0 ? 0.0 : (sa == 0.0 ? 1.0 : sa)"; break;
        case S::kSetpPop: Line("float nc = sa - 1.0; p0 = nc <= 0.0;"); r = "p0 ? 0.0 : nc"; break;
        case S::kSetpClr: Line("p0 = false;"); r = "3.402823466e38"; break;
        case S::kSetpRstr: Line("p0 = sa == 0.0;"); r = "p0 ? 0.0 : sa"; break;
        case S::kKillsEq: case S::kKillsGt: case S::kKillsGe: case S::kKillsNe: case S::kKillsOne:
        {
            const char* c = instr.scalar_opcode == S::kKillsEq ? "sa == 0.0" : instr.scalar_opcode == S::kKillsGt ? "sa > 0.0"
                          : instr.scalar_opcode == S::kKillsGe ? "sa >= 0.0" : instr.scalar_opcode == S::kKillsNe ? "sa != 0.0" : "sa == 1.0";
            Line(Fmt("bool ks = %s;", c));
            if (is_pixel_shader()) Line("if (ks) discard;");
            r = "float(ks)";
            break;
        }
        case S::kSqrt: r = "sqrt(sa)"; break;
        case S::kSin: r = "sin(sa)"; break;
        case S::kCos: r = "cos(sa)"; break;
        case S::kRetainPrev: r = "ps"; break;
        default:
            EmitTranslationError("unknown scalar opcode", false);
            r = "0.0";
            break;
        }
        Line("ps = " + r + ";");
    }

    // ---- results (both computed before either is stored)
    if (doVector || instr.vector_and_constant_result.GetUsedWriteMask())
        Store(instr.vector_and_constant_result, doVector ? "vr" : "float4(0, 0, 0, 0)");
    if (doScalar) Store(instr.scalar_result, "ps.xxxx");

    indent_--;
    Line("}");
}

// ---------------------------------------------------------------- whole program
std::vector<uint8_t> HlslTranslator::CompleteTranslation()
{
    // close the last block and the pc loop
    indent_--;
    Line("}");
    if (!straight_) Line("break;");
    indent_--;
    Line("}");

    uint32_t regs = register_count() ? register_count() : 1;
    std::string s = kCommonHlsl;
    std::string locals = Fmt(
        "    float4 r[%u];\n"
        "    float ps = 0.0; bool p0 = false; int a0 = 0; int aL = 0;\n"
        "    uint lcnt[4] = { 0, 0, 0, 0 }; int lstk[4] = { 0, 0, 0, 0 }; int ld = -1;\n", regs);
    for (uint32_t ri = 0; ri < regs; ri++) locals += Fmt("    r[%u] = float4(0, 0, 0, 0);\n", ri);   // no zeroing loop (see xot)

    if (is_vertex_shader())
    {
        s += "Texture2D<uint> vmem : register(t16);\n";
        s += "uint XVmem(uint i) { return vmem.Load(int3(i & 4095u, i >> 12, 0)); }\n";
        // xe_vfetch[fc]: x = fetch base (guest dwords), y = uploaded dwords | endian << 30,
        // z = host offset of the upload, w = upload start relative to the base
        s += "uint XVfetch(uint fc, int w) { uint4 f = xe_vfetch[fc]; int rel = w - int(f.x) - int(f.w); return (rel >= 0 && rel < int(f.y & 0x3FFFFFFFu)) ? XSwap(XVmem(f.z + uint(rel)), f.y >> 30) : 0u; }\n";
        for (uint32_t i = 0; i < kMaxTextures; i++)
            if (texturesUsed_ & (1u << i)) s += Fmt("%s xt%u : register(t%u);\nSamplerState xs%u : register(s%u);\n", (textures3D_ & (1u << i)) ? "Texture3D" : "Texture2D", i, i, i, i, i);
        // memory export: XE_CS builds of the vertex shader (a compute shader over the vertex
        // indices) log every eM write as [eA bits, element offset k, eM bits] for the CPU to apply
        s += R"(
#ifdef XE_CS
RWTexture2D<uint> xe_exp : register(u0);   // arena, 4096 dwords per row
// export streams of this draw: x = guest base address, y = arena offset (dwords), z = bytes
cbuffer XeCsConst : register(b2) { uint4 xe_cs_info; uint4 xe_streams[8]; };
uint XPackC(float f, uint bits, uint nf)
{
    uint mask = bits >= 32u ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    if (f != f) f = 0.0;
    if (nf == 0u) return uint(saturate(f) * float(mask) + 0.5) & mask;
    if (nf == 1u) { float mm = float((1u << (bits - 1u)) - 1u); return uint(int(round(clamp(f, -1.0, 1.0) * mm))) & mask; }
    if (nf == 3u) { float lo = -float(1u << (bits - 1u)), hi = float((1u << (bits - 1u)) - 1u); return uint(int(round(clamp(f, lo, hi)))) & mask; }
    return uint(round(clamp(f, 0.0, float(mask)))) & mask;
}
uint XToHalf(float f)
{
    uint x = asuint(f);
    uint sgn = (x >> 16) & 0x8000u;
    int e = int((x >> 23) & 0xFFu) - 112;
    uint m = x & 0x7FFFFFu;
    if (((x >> 23) & 0xFFu) == 0xFFu) return sgn | 0x7C00u | (m != 0u ? 0x200u : 0u);
    if (e >= 31) return sgn | 0x7C00u;
    if (e <= 0) { if (e < -10) return sgn; m |= 0x800000u; return sgn | (m >> uint(14 - e)); }
    return sgn | (uint(e) << 10) | (m >> 13);
}
void XFlushExport(float4 a, float4 m[5], uint mask)
{
    uint4 ab = asuint(a);
    if (mask == 0u || ab.y < 0x4B000000u) return;
    uint base = (ab.x & 0x3FFFFFFFu) << 2, idx0 = ab.y - 0x4B000000u, cnt = ab.w & 0x7FFFFFu;
    uint endian = ab.z & 7u, fmt = (ab.z >> 8) & 63u, nf = (ab.z >> 16) & 7u;
    uint aoff = 0u, abytes = 0u;
    [loop] for (uint si = 0u; si < xe_cs_info.y; si++)
        if (xe_streams[si].x == base) { aoff = xe_streams[si].y; abytes = xe_streams[si].z; }
    if (abytes == 0u) return;
    [unroll] for (uint k = 0u; k < 5u; k++)
    {
        if ((mask & (1u << k)) == 0u) continue;
        uint idx = idx0 + k;
        if (idx >= cnt) continue;
        float4 v = m[k];
        if ((ab.z >> 19) & 1u) v = v.zyxw;
        uint4 o = uint4(0u, 0u, 0u, 0u); uint size = 4u;
        if (fmt == 36u) o.x = asuint(v.x);
        else if (fmt == 37u) { o.xy = asuint(v.xy); size = 8u; }
        else if (fmt == 38u) { o = asuint(v); size = 16u; }
        else if (fmt == 6u || fmt == 14u) o.x = XPackC(v.x, 8u, nf) | (XPackC(v.y, 8u, nf) << 8) | (XPackC(v.z, 8u, nf) << 16) | (XPackC(v.w, 8u, nf) << 24);
        else if (fmt == 7u) o.x = XPackC(v.x, 10u, nf) | (XPackC(v.y, 10u, nf) << 10) | (XPackC(v.z, 10u, nf) << 20) | (XPackC(v.w, 2u, nf) << 30);
        else if (fmt == 16u) o.x = XPackC(v.x, 11u, nf) | (XPackC(v.y, 11u, nf) << 11) | (XPackC(v.z, 10u, nf) << 22);
        else if (fmt == 17u) o.x = XPackC(v.x, 10u, nf) | (XPackC(v.y, 11u, nf) << 10) | (XPackC(v.z, 11u, nf) << 21);
        else if (fmt == 25u) o.x = XPackC(v.x, 16u, nf) | (XPackC(v.y, 16u, nf) << 16);
        else if (fmt == 26u) { o.x = XPackC(v.x, 16u, nf) | (XPackC(v.y, 16u, nf) << 16); o.y = XPackC(v.z, 16u, nf) | (XPackC(v.w, 16u, nf) << 16); size = 8u; }
        else if (fmt == 31u) o.x = XToHalf(v.x) | (XToHalf(v.y) << 16);
        else if (fmt == 32u) { o.x = XToHalf(v.x) | (XToHalf(v.y) << 16); o.y = XToHalf(v.z) | (XToHalf(v.w) << 16); size = 8u; }
        else continue;
        uint off = idx * size;
        if (off + size > abytes) continue;
        uint d = aoff + off / 4u;
        uint e = endian <= 3u ? endian : 2u;
        xe_exp[uint2(d & 4095u, d >> 12)] = XSwap(o.x, e);
        if (size >= 8u) { uint d1 = d + 1u; xe_exp[uint2(d1 & 4095u, d1 >> 12)] = XSwap(o.y, e); }
        if (size >= 16u) { uint d2 = d + 2u, d3 = d + 3u; xe_exp[uint2(d2 & 4095u, d2 >> 12)] = XSwap(o.z, e); xe_exp[uint2(d3 & 4095u, d3 >> 12)] = XSwap(o.w, e); }
    }
}
#else
#define XFlushExport(a, m, k)
#endif
)";
        s += "XeVertexOut XeGuestVS(uint vid)\n{\n";
        s += locals;
        s += "    float4 xeA = float4(0, 0, 0, 0); float4 xeM[5] = { xeA, xeA, xeA, xeA, xeA }; uint xeMMask = 0u;\n";
        s += "    int vfa = 0; uint vfc = 0;\n";
        s += "    XeVertexOut xo = (XeVertexOut)0;\n";
        // outputs: zeroed with separate statements -- a zeroing loop makes the compiler keep the array
        // indexable, and some Windows drivers then lose the writes made inside the pc loop (black
        // Gamebreaker frames: every interpolator of VS 9813643223229761539 arrived as 0)
        // Outputs live in separate variables. As array elements (xot[k].x = ...) some Windows drivers
        // dropped the component writes made inside the pc loop: every interpolator of the
        // Gamebreaker overlay (VS 9813643223229761539) arrived as 0 -> black frames.
        for (int oi = 0; oi < 16; oi++) s += Fmt("    float4 xot%d = float4(0, 0, 0, 0);\n", oi);
        s += "    float4 opos = float4(0, 0, 0, 1);\n";
        s += "    r[0].x = float(int(vid) + int(xe_flags.y));\n";
        s += body_;
        s += "    XFlushExport(xeA, xeM, xeMMask);\n";
        if (const char* dbg = getenv("NBASH_VSDEBUG_REG"))   // debug: send register r<k> out as interpolator 0
        {
            if (dbg[0] == 'o') s += "    xot0 = float4(opos.x, xot0.x, xot1.x, 0);\n";          // position, outputs 0 and 1
            else if (dbg[0] == 'k') s += "    xot0 = float4(xot15.x, opos.x, xot2.x, 0);\n";    // loop-top write, position, output 2
            else s += Fmt("    xot0 = r[min(%d, %u)];\n", atoi(dbg), (register_count() ? register_count() : 1) - 1);
        }
        // Guest viewport (PA_CL_VTE_CNTL): optional divide by w, then scale/offset to window
        // coordinates; then window -> host clip space for a full-target host viewport.
        s += R"(
    uint vte = xe_flags.x;
    float w = (vte & (1u << 10)) != 0 ? opos.w : 1.0 / opos.w;     // vtx_w0_fmt = 0: shader wrote 1/W
    float3 p = opos.xyz;
    if ((vte & (1u << 8)) == 0) p.xy /= w;                          // vtx_xy_fmt = 0: not divided by W yet
    if ((vte & (1u << 9)) == 0) p.z /= w;
    if (vte & 1u) p.x *= xe_vp_scale.x;
    if (vte & 2u) p.x += xe_vp_offset.x;
    if (vte & 4u) p.y *= xe_vp_scale.y;
    if (vte & 8u) p.y += xe_vp_offset.y;
    if (vte & 16u) p.z *= xe_vp_scale.z;
    if (vte & 32u) p.z += xe_vp_offset.z;
    p.xy *= xe_alpha_ref.zw;                                        // MSAA surface -> samples
    float2 ndc = float2(p.x * xe_rt_size.z - 1.0, 1.0 - p.y * xe_rt_size.w);
    xo.pos = float4(ndc * w, saturate(p.z) * w, w);
    xo.t0 = xot0;
    xo.t1 = xot1;
    xo.t2 = xot2;
    xo.t3 = xot3;
    xo.t4 = xot4;
    xo.t5 = xot5;
    xo.t6 = xot6;
    xo.t7 = xot7;
    xo.t8 = xot8;
    xo.t9 = xot9;
    xo.t10 = xot10;
    xo.t11 = xot11;
    xo.t12 = xot12;
    xo.t13 = xot13;
    xo.t14 = xot14;
    xo.t15 = xot15;
    return xo;
}
)";
        // Entry points: plain, and rectangle lists (3 guest vertices per rectangle, drawn as
        // 6 host vertices; the 4th corner mirrors the right-angle corner across the diagonal).
        s += R"(
XeVertexOut main(uint vid : SV_VertexID) { return XeGuestVS(vid); }
#ifdef XE_CS
[numthreads(64, 1, 1)] void main_cs(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= xe_cs_info.x) return;
    XeGuestVS(id.x);
}
#endif
XeVertexOut XeRectCorner(XeVertexOut a, XeVertexOut b, XeVertexOut c)
{
    XeVertexOut d;
    d.pos = a.pos + b.pos - c.pos;
    d.t0 = a.t0 + b.t0 - c.t0;
    d.t1 = a.t1 + b.t1 - c.t1;
    d.t2 = a.t2 + b.t2 - c.t2;
    d.t3 = a.t3 + b.t3 - c.t3;
    d.t4 = a.t4 + b.t4 - c.t4;
    d.t5 = a.t5 + b.t5 - c.t5;
    d.t6 = a.t6 + b.t6 - c.t6;
    d.t7 = a.t7 + b.t7 - c.t7;
    d.t8 = a.t8 + b.t8 - c.t8;
    d.t9 = a.t9 + b.t9 - c.t9;
    d.t10 = a.t10 + b.t10 - c.t10;
    d.t11 = a.t11 + b.t11 - c.t11;
    d.t12 = a.t12 + b.t12 - c.t12;
    d.t13 = a.t13 + b.t13 - c.t13;
    d.t14 = a.t14 + b.t14 - c.t14;
    d.t15 = a.t15 + b.t15 - c.t15;
    return d;
}
XeVertexOut main_rect(uint hv : SV_VertexID)
{
    uint rect = hv / 6u, k = hv % 6u;
    XeVertexOut v0 = XeGuestVS(rect * 3u), v1 = XeGuestVS(rect * 3u + 1u), v2 = XeGuestVS(rect * 3u + 2u);
    if (k == 0u) return v0;
    if (k == 1u) return v1;
    if (k == 2u) return v2;
    float2 p0 = v0.pos.xy / v0.pos.w, p1 = v1.pos.xy / v1.pos.w, p2 = v2.pos.xy / v2.pos.w;
    float d01 = dot(p0 - p1, p0 - p1), d12 = dot(p1 - p2, p1 - p2), d20 = dot(p2 - p0, p2 - p0);
    XeVertexOut a = v0, b = v1, c = v2;                         // c = right-angle corner
    if (d12 >= d01 && d12 >= d20) { a = v1; b = v2; c = v0; }
    else if (d20 >= d01 && d20 >= d12) { a = v2; b = v0; c = v1; }
    if (k == 3u) return a;
    if (k == 4u) return b;
    return XeRectCorner(a, b, c);
}
// Rectangle lists the usual way: the guest vertex shader runs once per vertex (main), and this
// geometry shader adds the fourth corner. (main_rect ran the whole guest shader three times in
// one program; with big shaders some Windows drivers miscompiled that -- black Gamebreaker.)
// Only compiled with XE_GS defined (Wine's HLSL compiler has no geometry shaders).
#ifdef XE_GS
[maxvertexcount(6)]
void gs_rect(triangle XeVertexOut v[3], inout TriangleStream<XeVertexOut> s)
{
    s.Append(v[0]); s.Append(v[1]); s.Append(v[2]);
    s.RestartStrip();
    float2 p0 = v[0].pos.xy / v[0].pos.w, p1 = v[1].pos.xy / v[1].pos.w, p2 = v[2].pos.xy / v[2].pos.w;
    float d01 = dot(p0 - p1, p0 - p1), d12 = dot(p1 - p2, p1 - p2), d20 = dot(p2 - p0, p2 - p0);
    XeVertexOut a = v[0], b = v[1], c = v[2];
    if (d12 >= d01 && d12 >= d20) { a = v[1]; b = v[2]; c = v[0]; }
    else if (d20 >= d01 && d20 >= d12) { a = v[2]; b = v[0]; c = v[1]; }
    s.Append(a); s.Append(b); s.Append(XeRectCorner(a, b, c));
}
#endif
)";
    }
    else
    {
        for (uint32_t i = 0; i < kMaxTextures; i++)
            if (texturesUsed_ & (1u << i)) s += Fmt("%s xt%u : register(t%u);\nSamplerState xs%u : register(s%u);\n", (textures3D_ & (1u << i)) ? "Texture3D" : "Texture2D", i, i, i, i, i);
        s += "struct XePixelOut { float4 c0 : SV_Target0; float4 c1 : SV_Target1; float4 c2 : SV_Target2; float4 c3 : SV_Target3; };\n";
        s += "XePixelOut main(XeVertexOut xi, bool face : SV_IsFrontFace)\n{\n";
        s += locals;
        {
            const uint32_t n = std::min<uint32_t>(regs, kInterpolators);
            for (uint32_t ii = 0; ii < n; ii++) s += Fmt("    r[%u] = xi.t%u;\n", ii, ii);
        }
        s += Fmt("    if (xe_flags.z & 1u) r[min((xe_flags.z >> 1) & 15u, %uu)] = float4(face ? xi.pos.x * xe_alpha_ref.y : -xi.pos.x * xe_alpha_ref.y, xi.pos.y * xe_alpha_ref.y, 0, 0);\n", regs - 1);
        s += "    float4 oc[4] = { float4(0, 0, 0, 0), float4(0, 0, 0, 0), float4(0, 0, 0, 0), float4(0, 0, 0, 0) };\n";
        s += "    float odepth = xi.pos.z;\n";
        s += body_;
        s += R"(
    uint af = xe_flags.w;
    if (af & 8u)                                       // alpha test
    {
        float a = oc[0].w, ref = xe_alpha_ref.x;
        uint f = af & 7u;
        bool passed = f == 7u || (f == 1u && a < ref) || (f == 2u && a == ref) || (f == 3u && a <= ref) ||
                    (f == 4u && a > ref) || (f == 5u && a != ref) || (f == 6u && a >= ref);
        if (!passed) discard;
    }
    XePixelOut po;
    po.c0 = oc[0]; po.c1 = oc[1]; po.c2 = oc[2]; po.c3 = oc[3];
    return po;
}
)";
    }
    return std::vector<uint8_t>(s.begin(), s.end());
}
}
