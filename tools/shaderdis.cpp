// Disassembles captured Xenos shaders (capture/frameN/shaders/*.bin, host-endian dwords).
#include "xenia/gpu/shader.h"
#include <cstdio>
#include <vector>
using namespace xe::gpu;
int main(int argc, char** argv)
{
    for (int a = 2; a < argc; a++)
    {
        FILE* f = fopen(argv[a], "rb");
        std::vector<uint32_t> d;
        uint32_t x;
        while (fread(&x, 4, 1, f) == 1) d.push_back(__builtin_bswap32(x));   // back to guest order
        fclose(f);
        Shader s(argv[1][0] == 'p' ? xenos::ShaderType::kPixel : xenos::ShaderType::kVertex, 0, d.data(), (uint32_t)d.size());
        xe::StringBuffer sb;
        s.AnalyzeUcode(sb);
        printf("==== %s (%zu dwords)\n%s\n", argv[a], d.size(), s.ucode_disassembly().c_str());
    }
}
