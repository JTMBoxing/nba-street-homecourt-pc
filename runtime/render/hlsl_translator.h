// Xenos shader microcode -> HLSL (Shader Model 5) source.
// The ucode decoding is Xenia's (ShaderTranslator); this class emits HLSL for each parsed
// instruction. All draw-dependent state (constants, fetch constants, viewport, alpha test)
// comes from constant buffers, so one translation works for every draw using the shader.
#pragma once
#include <string>
#include "xenia/gpu/shader_translator.h"

namespace render
{
    // Shared resource layout (also used by the D3D11 renderer):
    //   b0  float4 c[512]                (vertex shader uses c0-255, pixel shader c256-511)
    //   b1  SystemConstants              (see kSystemConstantsHlsl)
    //   t0-t15 + s0-s15                  texture fetch constants tf0-tf15
    //   t16 Texture2D<uint> vmem         vertex data for this draw, 4096 dwords per row (R32_UINT)
    constexpr uint32_t kMaxTextures = 16;
    constexpr uint32_t kInterpolators = 16;
    constexpr uint32_t kVmemRowDwords = 4096;

    // Host-side mirror of cbuffer b1 (16-byte aligned rows).
    struct SystemConstants
    {
        uint32_t bools[8];          // 256 boolean constants
        uint32_t loops[32];         // loop constants
        uint32_t vfetch[96][4];     // per vertex fetch constant: base dword, uploaded dwords | endian<<30, host dword, upload start
        float    tsize[32][4];      // per texture: width, height, 1/width, 1/height
        uint32_t tinfo[32][4];      // per texture: swizzle, signs, exp adjust, unused
        float    vpScale[4];        // guest viewport scale x, y, z
        float    vpOffset[4];       // guest viewport offset x, y, z
        uint32_t flags[4];          // x: PA_CL_VTE_CNTL, y: vertex index offset, z: param gen (bit0 on, bits1-4 reg), w: alpha test func | enable<<3
        float    rtSize[4];         // render target width, height, 2/width, 2/height
        float    alphaRef[4];       // x: alpha test reference, zw: sample scale (MSAA surfaces in EDRAM)
    };

    class HlslTranslator : public xe::gpu::ShaderTranslator
    {
    public:
        // Returns the HLSL source for the analyzed shader, or an empty string on failure
        // (errors are appended to `errors`).
        std::string Translate(xe::gpu::Shader& shader, std::string& errors);

    protected:
        void Reset() override;
        void StartTranslation() override;
        std::vector<uint8_t> CompleteTranslation() override;
        void EmitTranslationError(const char* message, bool is_fatal) override;

        void ProcessLabel(uint32_t cf_index) override;
        void ProcessExecInstructionBegin(const xe::gpu::ParsedExecInstruction& instr) override;
        void ProcessExecInstructionEnd(const xe::gpu::ParsedExecInstruction& instr) override;
        void ProcessLoopStartInstruction(const xe::gpu::ParsedLoopStartInstruction& instr) override;
        void ProcessLoopEndInstruction(const xe::gpu::ParsedLoopEndInstruction& instr) override;
        void ProcessCallInstruction(const xe::gpu::ParsedCallInstruction& instr) override;
        void ProcessReturnInstruction(const xe::gpu::ParsedReturnInstruction& instr) override;
        void ProcessJumpInstruction(const xe::gpu::ParsedJumpInstruction& instr) override;
        void ProcessVertexFetchInstruction(const xe::gpu::ParsedVertexFetchInstruction& instr) override;
        void ProcessTextureFetchInstruction(const xe::gpu::ParsedTextureFetchInstruction& instr) override;
        void ProcessAluInstruction(const xe::gpu::ParsedAluInstruction& instr, uint8_t) override;
        void ProcessAllocInstruction(const xe::gpu::ParsedAllocInstruction& instr, uint8_t export_eM) override;

    private:
        std::string Operand(const xe::gpu::InstructionOperand& op, uint32_t components = 4);
        std::string OperandComponent(const xe::gpu::InstructionOperand& op, uint32_t component);
        void Store(const xe::gpu::InstructionResult& result, const std::string& value);
        void Line(const std::string& s);
        std::string BoolConstant(uint32_t index);

        std::string body_, errors_;
        int indent_ = 2;
        bool failed_ = false;
        bool straight_ = false;   // no jumps, loops or calls: no program-counter loop needed
        uint32_t texturesUsed_ = 0;
        uint32_t textures3D_ = 0;      // slots fetched as 3D textures (declared Texture3D)
        uint32_t vfetchUsed_ = 0;
    };

    extern const char* kCommonHlsl;
}
