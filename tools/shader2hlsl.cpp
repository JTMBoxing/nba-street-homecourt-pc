// Translates captured shaders to HLSL files: shader2hlsl v|p out.hlsl in.bin
#include "xenia/gpu/shader.h"
#include "../runtime/render/hlsl_translator.h"
#include <cstdio>
#include <vector>
using namespace xe::gpu;
int main(int argc, char** argv)
{
    FILE* f = fopen(argv[3], "rb");
    std::vector<uint32_t> d;
    uint32_t x;
    while (fread(&x, 4, 1, f) == 1) d.push_back(__builtin_bswap32(x));
    fclose(f);
    Shader s(argv[1][0] == 'p' ? xenos::ShaderType::kPixel : xenos::ShaderType::kVertex, 0, d.data(), d.size());
    xe::StringBuffer sb;
    s.AnalyzeUcode(sb);
    render::HlslTranslator t;
    std::string err;
    std::string hlsl = t.Translate(s, err);
    if (!err.empty()) fprintf(stderr, "%s: %s", argv[3], err.c_str());
    FILE* o = fopen(argv[2], "w");
    fputs(hlsl.c_str(), o);
    fclose(o);
    return hlsl.empty();
}
