// Loads default.xex into guest memory: decrypts/decompresses it (XenonUtils), fixes the
// import tables, copies the XEX header for RtlImageXexHeaderField, and fills the
// recompiler's guest->host function table.
#include "guest.h"
#include <image.h>
#include <file.h>
#include <unordered_map>
#include <vector>
#ifndef _WIN32
#include <sys/mman.h>
#endif

LoadedModule g_module;
PPCFunc** g_funcTable = nullptr;

namespace
{
    enum ExportType { kFunction, kVariable };
    struct ExportInfo { const char* name; ExportType type; };

    std::unordered_map<uint32_t, ExportInfo> g_kernelExports = {
#define XE_EXPORT(MODULE, ORDINAL, NAME, TYPE) { (ORDINAL), { #NAME, TYPE } }
#include "xbox/xboxkrnl_table.inc"
#undef XE_EXPORT
    };
    std::unordered_map<uint32_t, ExportInfo> g_xamExports = {
#define XE_EXPORT(MODULE, ORDINAL, NAME, TYPE) { (ORDINAL), { #NAME, TYPE } }
#include "xbox/xam_table.inc"
#undef XE_EXPORT
    };

    uint32_t BE32(const uint8_t* p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
    uint16_t BE16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

    const uint8_t* FindOptHeader(const std::vector<uint8_t>& xex, uint32_t key)
    {
        uint32_t count = BE32(&xex[20]);
        for (uint32_t i = 0; i < count; i++)
        {
            const uint8_t* e = &xex[24 + i * 8];
            if (BE32(e) == key)
                return e;
        }
        return nullptr;
    }

    // Kernel variables the game imports. Each gets a small zeroed block; a few get real values.
    uint32_t MakeVariable(const std::string& name)
    {
        uint32_t v = AllocSystem(0x400, 0x40);
        if (name == "XboxKrnlVersion")
        {
            W16(v + 0, 2); W16(v + 2, 0); W16(v + 4, 4929); W8(v + 6, 0x80); W8(v + 7, 0);
        }
        else if (name == "VdGpuClockInMHz")
        {
            W32(v, 500);
        }
        else if (name == "ExLoadedCommandLine")
        {
            strcpy((char*)G2H(v), "\"default.xex\"");
        }
        else if (name == "XexExecutableModuleHandle")
        {
            // the variable holds a pointer to the module
            W32(v, g_module.moduleHandle);
        }
        else if (name == "KeTimeStampBundle")
        {
            g_timeStampBundle = v;
        }
        return v;
    }
}

bool LoadXex(const char* path)
{
    std::vector<uint8_t> xex = LoadFile(path);
    if (xex.empty())
    {
        Log("Could not read %s\n", path);
        return false;
    }

    Image image = Image::ParseImage(xex.data(), xex.size());
    if (image.data == nullptr || image.base != PPC_IMAGE_BASE)
    {
        Log("default.xex could not be decrypted/decompressed (or is a different build)\n");
        return false;
    }
    memcpy(G2H(PPC_IMAGE_BASE), image.data.get(), PPC_IMAGE_SIZE);
    g_module.entryPoint = uint32_t(image.entry_point);

    // copy the XEX header into guest memory
    uint32_t headerSize = BE32(&xex[8]);
    g_module.xexHeader = AllocSystem(headerSize, 0x1000);
    memcpy(G2H(g_module.xexHeader), xex.data(), headerSize);

    if (auto* e = FindOptHeader(xex, 0x00020200)) // default stack size
        g_module.defaultStackSize = BE32(e + 4);

    // LDR_DATA_TABLE_ENTRY-like module block
    uint32_t mod = AllocSystem(0x100, 0x10);
    W32(mod + 0x18, PPC_IMAGE_BASE);              // dll base
    W32(mod + 0x1C, PPC_IMAGE_BASE);              // image base
    W32(mod + 0x20, PPC_IMAGE_SIZE);              // image size
    W32(mod + 0x3C, g_module.entryPoint);         // entry point
    W32(mod + 0x58, g_module.xexHeader);          // xex header base
    g_module.moduleHandle = mod;

    // import tables
    const uint8_t* imp = FindOptHeader(xex, 0x000103FF);
    if (imp)
    {
        const uint8_t* hdr = &xex[BE32(imp + 4)];
        uint32_t strSize = BE32(hdr + 4), libCount = BE32(hdr + 8);
        std::vector<std::string> libNames;
        for (const char* s = (const char*)hdr + 12; libNames.size() < libCount; )
        {
            libNames.emplace_back(s);
            s += ((libNames.back().size() + 1) + 3) & ~3u;
        }
        const uint8_t* lib = hdr + 12 + strSize;
        int vars = 0, funcs = 0;
        for (uint32_t l = 0; l < libCount; l++)
        {
            uint16_t nameIndex = BE16(lib + 36), n = BE16(lib + 38);
            auto& table = libNames[nameIndex] == "xam.xex" ? g_xamExports : g_kernelExports;
            uint32_t pendingIat = 0;
            for (uint32_t i = 0; i < n; i++)
            {
                uint32_t slot = BE32(lib + 40 + i * 4);
                uint32_t raw; memcpy(&raw, G2H(slot), 4);   // ParseImage left it host-endian
                bool isThunkCode = R32(slot) == 0x60000000;  // ParseImage wrote a nop/blr thunk
                if (isThunkCode)
                {
                    if (pendingIat) { W32(pendingIat, slot); funcs++; }  // IAT -> thunk address
                    pendingIat = 0;
                    continue;
                }
                uint32_t ordinal = raw & 0xFFFF;
                auto it = table.find(ordinal);
                if (it != table.end() && it->second.type == kVariable)
                {
                    uint32_t v = MakeVariable(it->second.name);
                    W32(slot, v);
                    vars++;
                    if (g_verbose) Log("[load] variable %s -> %08X\n", it->second.name, v);
                    pendingIat = 0;
                }
                else
                {
                    W32(slot, 0);      // filled in when the thunk entry follows
                    pendingIat = slot;
                }
            }
            lib += BE32(lib);
        }
        Log("[load] imports: %d functions, %d variables\n", funcs, vars);
    }

    // guest -> host function table used for indirect calls
    g_funcTable = new PPCFunc*[PPC_CODE_SIZE / 4 + 1]();
    size_t count = 0;
    for (auto* m = PPCFuncMappings; m->host; m++, count++)
        g_funcTable[(m->guest - PPC_CODE_BASE) / 4] = m->host;
    Log("[load] %zu functions registered, entry point %08X\n", count, g_module.entryPoint);
    return true;
}
