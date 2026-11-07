// Storage devices and saved games (XamContent*), the storage device picker and profile settings.
// Modelled on Xenia: one hard drive (device 1), saves stored as plain folders next to the program
// in saves/<content file name>/. When the game "creates" a save container it mounts it under a
// drive name of its choice (e.g. "save:"), and file paths under that name go to the folder.
#include "guest.h"
#include <filesystem>
#include <map>
#include <mutex>
#include <vector>
#include <cstring>

namespace fs = std::filesystem;

void CompleteOverlapped(uint32_t ov, uint32_t result, uint32_t extendedError, uint32_t length);
void BroadcastNotification(uint32_t id, uint32_t param);
void SetContentRoot(const std::string& name, const std::string& hostDir);
uint32_t NewPlainHandle();

namespace
{
    constexpr uint32_t kHdd = 1;
    constexpr uint32_t ERROR_SUCCESS_ = 0, ERROR_IO_PENDING = 997, ERROR_NO_MORE_FILES = 0x12,
        ERROR_FUNCTION_FAILED = 0x65B, ERROR_PATH_NOT_FOUND = 3, ERROR_ALREADY_EXISTS = 0xB7,
        ERROR_DEVICE_NOT_CONNECTED = 0x48F, ERROR_INSUFFICIENT_BUFFER = 0x7A;
    constexpr uint32_t kContentDataSize = 0x134;        // XCONTENT_DATA

    // arguments past the 8th are on the guest stack
    uint32_t StackArg(PPCContext& ctx, int n) { return R32(ctx.r1.u32 + 0x54 + (n - 9) * 8); }

    fs::path SavesRoot()
    {
        fs::path p = fs::path(GameRoot()).parent_path() / "saves";
        std::error_code ec; fs::create_directories(p, ec);
        return p;
    }

    std::string FileName(uint32_t contentData)          // XCONTENT_DATA.szFileName (char[42] at 0x108)
    {
        std::string s;
        for (int i = 0; i < 42; i++) { char c = (char)R8(contentData + 0x108 + i); if (!c) break; s += c; }
        for (auto& c : s) if (c == '/' || c == '\\' || c == ':') c = '_';
        return s;
    }

    // a save's display name, kept beside it so enumeration can return it
    void SaveMeta(const fs::path& dir, uint32_t contentData)
    {
        std::vector<uint8_t> raw(kContentDataSize);
        memcpy(raw.data(), G2H(contentData), kContentDataSize);
        if (FILE* f = fopen((dir.string() + ".meta").c_str(), "wb")) { fwrite(raw.data(), 1, raw.size(), f); fclose(f); }
    }

    struct Enumerator { std::vector<std::vector<uint8_t>> items; size_t next = 0; };
    std::mutex g_lock;
    std::map<uint32_t, Enumerator> g_enums;
    std::map<std::string, fs::path> g_mounted;           // root name -> folder

    uint32_t Finish(uint32_t ov, uint32_t result, uint32_t length = 0)
    {
        if (!ov) return result;
        CompleteOverlapped(ov, result ? ERROR_FUNCTION_FAILED : 0, result, length);
        return ERROR_IO_PENDING;
    }
}

// XamShowDeviceSelectorUI(user, contentType, contentFlags, bytesRequested(64), deviceId*, overlapped)
// No dialog: the hard drive is chosen, as on a console with only a hard drive (and as Xenia does).
IMPORT(XamShowDeviceSelectorUI)
{
    TRACE_IMPORT();
    uint32_t idPtr = ARG(7).u32, ov = ARG(8).u32;
    Log("[content] storage device selector -> hard drive\n");
    if (idPtr) W32(idPtr, kHdd);
    BroadcastNotification(0x09, 1);                      // system UI shown...
    BroadcastNotification(0x09, 0);                      // ...and closed
    if (!ov) RETURN(ERROR_SUCCESS_);
    CompleteOverlapped(ov, 0, 0, 0);
    RETURN(ERROR_IO_PENDING);
}

// XamContentGetDeviceState(deviceId, overlapped)
IMPORT(XamContentGetDeviceState)
{
    TRACE_IMPORT();
    uint32_t dev = ARG(3).u32, ov = ARG(4).u32;
    uint32_t r = dev == kHdd ? 0 : ERROR_DEVICE_NOT_CONNECTED;
    RETURN(Finish(ov, r));
}

// XamContentGetDeviceData(deviceId, X_CONTENT_DEVICE_DATA*): id, type, total, free, name[28]
IMPORT(XamContentGetDeviceData)
{
    TRACE_IMPORT();
    uint32_t dev = ARG(3).u32, out = ARG(4).u32;
    if (dev != kHdd) RETURN(ERROR_DEVICE_NOT_CONNECTED);
    memset(G2H(out), 0, 0x50);
    W32(out + 0, kHdd); W32(out + 4, 1);
    W64(out + 8, 20ull << 30); W64(out + 16, 16ull << 30);
    const char* name = "Hard Drive";
    for (int i = 0; name[i]; i++) W16(out + 24 + i * 2, uint16_t(name[i]));
    RETURN(0);
}

// XamContentCreateEx(user, rootName, contentData, flags, disposition*, license*, cacheSize,
//                    contentSize(64), overlapped)
IMPORT(XamContentCreateEx)
{
    TRACE_IMPORT();
    std::string root = (const char*)G2H(ARG(4).u32);
    uint32_t data = ARG(5).u32, flags = ARG(6).u32, dispPtr = ARG(7).u32, licPtr = ARG(8).u32;
    uint32_t ov = StackArg(ctx, 9);
    std::string file = FileName(data);
    if (file.empty()) RETURN(0x7B);                       // ERROR_INVALID_NAME
    fs::path dir = SavesRoot() / file;
    bool exists = fs::exists(dir);
    uint32_t result = 0, disposition = 0;                 // 1 = created, 2 = opened
    switch (flags & 0xF)
    {
    case 1: if (exists) result = ERROR_ALREADY_EXISTS; else disposition = 1; break;           // create new
    case 2: if (exists) fs::remove_all(dir); disposition = 1; break;                           // create always
    case 3: if (!exists) result = ERROR_PATH_NOT_FOUND; else disposition = 2; break;          // open existing
    case 4: disposition = exists ? 2 : 1; break;                                              // open always
    case 5: if (!exists) result = ERROR_PATH_NOT_FOUND; else { fs::remove_all(dir); disposition = 1; } break;
    default: disposition = exists ? 2 : 1; break;
    }
    if (!result)
    {
        std::error_code ec; fs::create_directories(dir, ec);
        if (disposition == 1) SaveMeta(dir, data);
        SetContentRoot(root, dir.string());
        std::lock_guard lock(g_lock); g_mounted[root] = dir;
    }
    if (dispPtr) W32(dispPtr, disposition);
    if (licPtr && !result) W32(licPtr, 0);
    Log("[content] %s save \"%s\" as %s -> %s\n", result ? "could not open" : disposition == 1 ? "created" : "opened",
        file.c_str(), root.c_str(), result ? "error" : "ok");
    if (!ov) RETURN(result);
    CompleteOverlapped(ov, result ? ERROR_FUNCTION_FAILED : 0, result, disposition);
    RETURN(ERROR_IO_PENDING);
}

// XamContentClose(rootName, overlapped)
IMPORT(XamContentClose)
{
    TRACE_IMPORT();
    std::string root = (const char*)G2H(ARG(3).u32);
    SetContentRoot(root, "");
    { std::lock_guard lock(g_lock); g_mounted.erase(root); }
    RETURN(Finish(ARG(4).u32, 0));
}

// XamContentDelete(user, contentData, overlapped)
IMPORT(XamContentDelete)
{
    TRACE_IMPORT();
    fs::path dir = SavesRoot() / FileName(ARG(4).u32);
    std::error_code ec;
    uint32_t r = fs::exists(dir) ? 0 : ERROR_PATH_NOT_FOUND;
    fs::remove_all(dir, ec); fs::remove(dir.string() + ".meta", ec);
    RETURN(Finish(ARG(5).u32, r));
}

// XamContentGetCreator(user, contentData, isCreator*, creatorXuid*, overlapped)
IMPORT(XamContentGetCreator)
{
    TRACE_IMPORT();
    if (ARG(5).u32) W32(ARG(5).u32, 1);
    if (ARG(6).u32) W64(ARG(6).u32, 0xE000000000000001ull);
    RETURN(Finish(ARG(7).u32, 0));
}

// XamContentSetThumbnail(user, contentData, image, imageSize, overlapped)
IMPORT(XamContentSetThumbnail) { TRACE_IMPORT(); RETURN(Finish(ARG(7).u32, 0)); }

// XamContentCreateEnumerator(user, deviceId, contentType, flags, itemsPerCall, bufferSize*, handle*)
IMPORT(XamContentCreateEnumerator)
{
    TRACE_IMPORT();
    uint32_t dev = ARG(4).u32, type = ARG(5).u32, items = ARG(7).u32, sizePtr = ARG(8).u32, handlePtr = ARG(9).u32;
    if (dev && dev != kHdd) { if (sizePtr) W32(sizePtr, 0); RETURN(0x80070057); }
    if (sizePtr) W32(sizePtr, kContentDataSize * (items ? items : 1));
    Enumerator e;
    std::error_code ec;
    for (auto& d : fs::directory_iterator(SavesRoot(), ec))
    {
        if (!d.is_directory()) continue;
        std::vector<uint8_t> raw(kContentDataSize, 0);
        if (FILE* f = fopen((d.path().string() + ".meta").c_str(), "rb")) { fread(raw.data(), 1, raw.size(), f); fclose(f); }
        else
        {
            std::string n = d.path().filename().string();
            raw[3] = 1; raw[7] = 1;                                        // device 1, type 1 (saved game)
            for (size_t i = 0; i < n.size() && i < 41; i++) { raw[0x108 + i] = uint8_t(n[i]); raw[8 + i * 2 + 1] = uint8_t(n[i]); }
        }
        raw[0] = raw[1] = raw[2] = 0; raw[3] = 1;                          // always on the hard drive
        uint32_t itemType = (uint32_t(raw[4]) << 24) | (raw[5] << 16) | (raw[6] << 8) | raw[7];
        if (type && itemType != type) continue;                            // e.g. saves vs. downloadable content
        e.items.push_back(raw);
    }
    uint32_t h = NewPlainHandle();
    { std::lock_guard lock(g_lock); g_enums[h] = std::move(e); }
    if (handlePtr) W32(handlePtr, h);
    Log("[content] enumerator %08X: %zu saves\n", h, g_enums[h].items.size());
    RETURN(0);
}

// XamEnumerate(handle, flags, buffer, bufferSize, itemsReturned*, overlapped)
IMPORT(XamEnumerate)
{
    TRACE_IMPORT();
    uint32_t h = ARG(3).u32, buf = ARG(5).u32, size = ARG(6).u32, countPtr = ARG(7).u32, ov = ARG(8).u32;
    uint32_t n = 0;
    {
        std::lock_guard lock(g_lock);
        auto it = g_enums.find(h);
        if (it != g_enums.end())
        {
            auto& e = it->second;
            while (e.next < e.items.size() && (n + 1) * kContentDataSize <= size)
            {
                memcpy(G2H(buf + n * kContentDataSize), e.items[e.next].data(), kContentDataSize);
                e.next++; n++;
            }
        }
    }
    uint32_t r = n ? 0 : ERROR_NO_MORE_FILES;
    if (countPtr) W32(countPtr, n);
    if (!ov) RETURN(r);
    CompleteOverlapped(ov, r, r, n);
    RETURN(ERROR_IO_PENDING);
}

IMPORT(XamCreateEnumeratorHandle) { TRACE_IMPORT(); RETURN(ERROR_NO_MORE_FILES); }

// XamUserReadProfileSettings(titleId, user, xuidCount, xuids, settingCount, settingIds,
//                            bufferSize*, buffer, overlapped). No stored profile: every setting
//                            comes back "not set", so the game uses its defaults.
IMPORT(XamUserReadProfileSettings)
{
    TRACE_IMPORT();
    uint32_t user = ARG(4).u32, xuidCount = ARG(5).u32, xuids = ARG(6).u32, count = ARG(7).u32,
             ids = ARG(8).u32, sizePtr = ARG(9).u32, buf = ARG(10).u32, ov = StackArg(ctx, 9);
    if (count < 1 || count > 32 || !sizePtr) RETURN(0x57);
    uint32_t perUser = 0, extra = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t id = R32(ids + i * 4), type = (id >> 28) & 0xF, sz = (id >> 16) & 0xFFF;
        perUser += 40;
        if (type == 4 || type == 6) extra += sz;          // WSTRING / BINARY data follows the table
    }
    uint32_t users = xuids ? (xuidCount ? xuidCount : 1) : 1;
    uint32_t need = 8 + perUser * users + extra * users;
    uint32_t have = R32(sizePtr);
    if (!buf || have < need) { if (!have) W32(sizePtr, need); RETURN(ERROR_INSUFFICIENT_BUFFER); }
    memset(G2H(buf), 0, need);
    W32(buf, count * users); W32(buf + 4, buf + 8);
    uint32_t s = buf + 8;
    for (uint32_t u = 0; u < users; u++)
        for (uint32_t i = 0; i < count; i++, s += 40)
        {
            uint32_t id = R32(ids + i * 4);
            W32(s + 0, 0);                                // source: not set
            if (xuids) W64(s + 8, R64(xuids + u * 8)); else W32(s + 8, user);
            W32(s + 16, id);
            W8(s + 24, uint8_t((id >> 28) & 0xF));        // data type, value 0 / empty
        }
    static int logged = 0;
    if (logged++ < 3) Log("[profile] read %u settings (all defaults)\n", count);
    if (!ov) RETURN(0);
    CompleteOverlapped(ov, 0, 0, 0);
    RETURN(ERROR_IO_PENDING);
}
