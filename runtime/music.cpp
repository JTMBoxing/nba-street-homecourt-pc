// Your own music: every audio file in the "music" folder next to the game (mp3, flac, wma, wav,
// m4a, aac) plays in shuffled order, looping, and replaces the game's own soundtrack (the game's
// music category is muted - see XAudioGetVoiceCategoryVolume in audio.cpp).
//
// Songs are decoded with Windows Media Foundation and evened out: each song's loudness is
// measured (gated RMS, like broadcast loudness meters) and brought to the same level, with a soft
// limiter so loud masters never clip. Then they play through their own XAudio2 voice, below the
// game's sound. If Media Foundation can't decode a file, Windows' MCI player plays it instead
// (no levelling).
//
// Keys: F5 next song, F6 pause/resume, F3 / F4 music quieter / louder.
// NBASH_MUSIC_VOLUME = 0..1000 (default 550: music level against the game's sound),
// NBASH_NOMUSIC=1 off (game soundtrack plays), NBASH_GAMEMUSIC=1 keeps the game soundtrack too.
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <xaudio2.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

void Log(const char* fmt, ...);
namespace fs = std::filesystem;

namespace
{
    std::mutex g_lock;
    std::wstring g_nowPlaying;
    std::atomic<int> g_command{0};   // 1 next, 2 pause/resume
    std::atomic<bool> g_started{false};
    std::atomic<float> g_level{0.55f};
    std::string g_title, g_artist, g_album;   // UTF-8, for the game's song popup

    std::string Utf8(const std::wstring& w)
    {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(size_t(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }

    // title / artist / album from the file's tags (as Explorer shows them), else "Artist - Title" filename
    struct Info { std::string title, artist, album; };
    Info ReadInfo(const fs::path& path)
    {
        std::wstring title, artist, album;
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(SHGetPropertyStoreFromParsingName(fs::absolute(path).wstring().c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&ps))))
        {
            auto get = [&](const PROPERTYKEY& k) {
                std::wstring r; PROPVARIANT v; PropVariantInit(&v);
                if (SUCCEEDED(ps->GetValue(k, &v)) && v.vt != VT_EMPTY)
                {
                    PWSTR str = nullptr;
                    if (SUCCEEDED(PropVariantToStringAlloc(v, &str)) && str) { r = str; CoTaskMemFree(str); }
                }
                PropVariantClear(&v);
                return r;
            };
            title = get(PKEY_Title); artist = get(PKEY_Music_Artist); album = get(PKEY_Music_AlbumTitle);
            ps->Release();
        }
        if (title.empty())
        {
            std::wstring stem = path.stem().wstring();
            size_t dash = stem.find(L" - ");
            if (dash != std::wstring::npos) { if (artist.empty()) artist = stem.substr(0, dash); title = stem.substr(dash + 3); }
            else title = stem;
        }
        return { Utf8(title), Utf8(artist), Utf8(album) };
    }

    bool IsAudio(const fs::path& p)
    {
        std::wstring e = p.extension().wstring();
        for (auto& c : e) c = wchar_t(towlower(c));
        return e == L".mp3" || e == L".wma" || e == L".wav" || e == L".m4a" || e == L".aac" || e == L".flac";
    }

    std::string GameDir() { const char* e = getenv("NBASH_GAME_DIR"); return e ? e : "game"; }

    std::vector<fs::path> FindSongs()
    {
        std::vector<fs::path> songs;
        for (std::string dir : { GameDir() + "/data/audio/custom", std::string("music") })
        {
            std::error_code ec;
            if (!fs::is_directory(dir, ec)) continue;
            for (auto& e : fs::recursive_directory_iterator(dir, ec))
                if (e.is_regular_file() && IsAudio(e.path())) songs.push_back(e.path());
        }
        std::sort(songs.begin(), songs.end());   // fixed order: song slot N always gets the same song
        return songs;
    }

    // your songs with their names (read once)
    struct Entry { fs::path path; Info info; };
    const std::vector<Entry>& Library()
    {
        static std::vector<Entry> lib;
        static std::mutex m; static bool done = false;
        std::lock_guard lk(m);
        if (!done)
        {
            done = true;
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            for (auto& p : FindSongs()) lib.push_back({ p, ReadInfo(p) });
        }
        return lib;
    }
    std::string Trunc(const std::string& t) { return t.substr(0, 63); }
    std::atomic<int> g_request{-1};   // song the game asked for (index into Library)

    // ------------------------------------------------------------------ decoding + levelling
    struct Song
    {
        std::vector<float> pcm;   // interleaved
        uint32_t rate = 0, channels = 0;
        float gain = 1.0f, loudnessDb = 0;
        bool ok = false;
    };

    Song Decode(const fs::path& path)
    {
        Song s;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // decoder threads need COM for Media Foundation
        IMFSourceReader* reader = nullptr;
        if (FAILED(MFCreateSourceReaderFromURL(fs::absolute(path).wstring().c_str(), nullptr, &reader))) return s;
        reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
        IMFMediaType* want = nullptr;
        MFCreateMediaType(&want);
        want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        HRESULT hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, want);
        want->Release();
        IMFMediaType* got = nullptr;
        if (SUCCEEDED(hr) && SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &got)))
        {
            UINT32 v = 0;
            if (SUCCEEDED(got->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &v))) s.rate = v;
            if (SUCCEEDED(got->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &v))) s.channels = v;
            got->Release();
        }
        if (!s.rate || !s.channels || s.channels > 8) { reader->Release(); return s; }
        s.pcm.reserve(size_t(s.rate) * s.channels * 300);
        for (;;)
        {
            DWORD flags = 0; IMFSample* sample = nullptr;
            if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &sample))) break;
            if (sample)
            {
                IMFMediaBuffer* buf = nullptr;
                if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)))
                {
                    BYTE* data = nullptr; DWORD len = 0;
                    if (SUCCEEDED(buf->Lock(&data, nullptr, &len)))
                    {
                        const float* f = reinterpret_cast<const float*>(data);
                        s.pcm.insert(s.pcm.end(), f, f + len / 4);
                        buf->Unlock();
                    }
                    buf->Release();
                }
                sample->Release();
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        }
        reader->Release();
        if (s.pcm.size() < size_t(s.rate) * s.channels) return s;   // under a second: treat as failed

        // loudness: mean power of 400 ms blocks, gated at -70 dB and then 10 dB below the
        // ungated mean (the EBU R128 gating idea, without its frequency weighting)
        const size_t block = size_t(s.rate) * 4 / 10 * s.channels;
        std::vector<double> powers;
        for (size_t i = 0; i + block <= s.pcm.size(); i += block)
        {
            double sum = 0;
            for (size_t k = i; k < i + block; k++) sum += double(s.pcm[k]) * s.pcm[k];
            powers.push_back(sum / block);
        }
        auto meanAbove = [&](double gate) { double sum = 0; size_t n = 0; for (double p : powers) if (p > gate) { sum += p; n++; } return n ? sum / n : 0.0; };
        double m = meanAbove(1e-7);                 // -70 dB
        m = meanAbove(m * 0.1);                     // relative gate -10 dB
        if (m <= 0) m = 1e-4;
        s.loudnessDb = float(10 * std::log10(m));
        const float targetDb = -16.0f;              // similar to how loud game soundtracks sit
        float gainDb = std::clamp(targetDb - s.loudnessDb, -20.0f, 9.0f);
        s.gain = std::pow(10.0f, gainDb / 20.0f);

        // apply the gain with a soft limiter (smooth above 0.8, never past 1.0) and short fades
        const size_t fade = size_t(s.rate) / 50 * s.channels;   // 20 ms
        for (size_t i = 0; i < s.pcm.size(); i++)
        {
            float x = s.pcm[i] * s.gain, a = std::fabs(x);
            if (a > 0.8f) x = std::copysign(0.8f + 0.2f * std::tanh((a - 0.8f) / 0.2f), x);
            if (i < fade) x *= float(i) / fade;
            if (s.pcm.size() - i < fade) x *= float(s.pcm.size() - i) / fade;
            s.pcm[i] = x;
        }
        s.ok = true;
        return s;
    }

    // ------------------------------------------------------------------ output
    IXAudio2* g_xa = nullptr;
    IXAudio2MasteringVoice* g_master = nullptr;

    bool InitOutput()
    {
        using CreateFn = HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);
        HMODULE m = LoadLibraryA("xaudio2_9.dll");
        if (!m) m = LoadLibraryA("xaudio2_8.dll");
        auto create = m ? (CreateFn)GetProcAddress(m, "XAudio2Create") : nullptr;
        if (!create || FAILED(create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) return false;
        if (FAILED(g_xa->CreateMasteringVoice(&g_master, 2, 48000, 0, nullptr, nullptr, AudioCategory_GameMedia))) return false;
        return true;
    }

    // plays one decoded song; returns when it ends or the player skips it
    void PlaySong(const Song& s, bool& paused)
    {
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT; wf.nChannels = WORD(s.channels); wf.nSamplesPerSec = s.rate;
        wf.wBitsPerSample = 32; wf.nBlockAlign = WORD(4 * s.channels); wf.nAvgBytesPerSec = s.rate * wf.nBlockAlign;
        IXAudio2SourceVoice* voice = nullptr;
        if (FAILED(g_xa->CreateSourceVoice(&voice, &wf, 0, 2.0f, nullptr, nullptr, nullptr))) { Log("[music] no voice for this song\n"); return; }
        voice->SetVolume(g_level);
        voice->Start(0, 0);
        const size_t chunk = size_t(s.rate) / 2 * s.channels;   // 0.5 s per buffer
        size_t pos = 0;
        float lastLevel = g_level;
        for (;;)
        {
            int c = g_command.exchange(0);
            if (c == 1) { voice->Stop(0); voice->FlushSourceBuffers(); break; }
            if (c == 2) { paused = !paused; if (paused) voice->Stop(0); else voice->Start(0, 0); }
            if (g_level != lastLevel) { lastLevel = g_level; voice->SetVolume(lastLevel); }
            XAUDIO2_VOICE_STATE vs{};
            voice->GetState(&vs, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (pos >= s.pcm.size() && vs.BuffersQueued == 0) break;
            if (pos < s.pcm.size() && vs.BuffersQueued < 3)
            {
                size_t n = std::min(chunk, s.pcm.size() - pos);
                XAUDIO2_BUFFER b{};
                b.AudioBytes = UINT32(n * 4); b.pAudioData = reinterpret_cast<const BYTE*>(s.pcm.data() + pos);
                if (pos + n >= s.pcm.size()) b.Flags = XAUDIO2_END_OF_STREAM;
                voice->SubmitSourceBuffer(&b, nullptr);
                pos += n;
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        voice->DestroyVoice();   // waits for the voice to stop using the song's samples
    }

    // MCI fallback for files Media Foundation can't decode
    void PlayWithMci(const fs::path& song, bool& paused)
    {
        std::wstring cmd = L"open \"" + fs::absolute(song).wstring() + L"\" type mpegvideo alias nbashsong";
        if (mciSendStringW(cmd.c_str(), nullptr, 0, nullptr) != 0)
        {
            cmd = L"open \"" + fs::absolute(song).wstring() + L"\" alias nbashsong";
            if (mciSendStringW(cmd.c_str(), nullptr, 0, nullptr) != 0) { Log("[music] cannot play %ls\n", song.filename().c_str()); return; }
        }
        auto setVol = [] { wchar_t v[64]; swprintf(v, 64, L"setaudio nbashsong volume to %d", int(g_level * 1000)); mciSendStringW(v, nullptr, 0, nullptr); };
        setVol();
        mciSendStringW(L"play nbashsong", nullptr, 0, nullptr);
        auto started = std::chrono::steady_clock::now();
        float lastLevel = g_level;
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            int c = g_command.exchange(0);
            if (c == 1) break;
            if (c == 2) { paused = !paused; mciSendStringW(paused ? L"pause nbashsong" : L"resume nbashsong", nullptr, 0, nullptr); }
            if (g_level != lastLevel) { lastLevel = g_level; setVol(); }
            wchar_t mode[64] = {};
            mciSendStringW(L"status nbashsong mode", mode, 64, nullptr);
            if (!paused && wcscmp(mode, L"stopped") == 0 && std::chrono::steady_clock::now() - started > std::chrono::milliseconds(500)) break;
        }
        mciSendStringW(L"close nbashsong", nullptr, 0, nullptr);
    }

    void Player()
    {
        const auto& lib = Library();
        if (lib.empty()) { Log("[music] no songs found\n"); return; }
        Log("[music] %zu songs - they replace the game's soundtrack\n", lib.size());
        // 16 Nov (user: "have the music not start playing until you get into the main menu"): wait for the game's own first song
        // request - it comes when the front end / main menu starts (after the saves load). Safety net: 120 s.
        {
            auto t0 = std::chrono::steady_clock::now();
            while (g_request.load() < 0 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(120)) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            Log("[music] %s - music starts now\n", g_request.load() >= 0 ? "main menu reached" : "no menu song request after 120 s");
            g_command = 0;      // the request's "end the current song" is for nothing yet: don't let it skip the first song
        }
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)) && InitOutput();
        if (!mf) Log("[music] Media Foundation / XAudio2 not available - plain playback, no levelling\n");
        std::mt19937 rng{uint32_t(GetTickCount())};
        std::vector<int> order(lib.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = int(i);
        std::shuffle(order.begin(), order.end(), rng);
        size_t pos = 0;
        bool paused = false;
        // background decoder for the next song (a plain thread + hand-off)
        struct Pending { std::mutex m; std::condition_variable cv; bool done = false; Song song; int index = -1; };
        auto decodeAsync = [&](int i) {
            auto pend = std::make_shared<Pending>();
            pend->index = i;
            fs::path p = lib[size_t(i)].path;
            std::thread([pend, p] {
                Song s = Decode(p);
                std::lock_guard lk(pend->m); pend->song = std::move(s); pend->done = true; pend->cv.notify_all();
            }).detach();
            return pend;
        };
        auto take = [](const std::shared_ptr<Pending>& pend) {
            std::unique_lock lk(pend->m); pend->cv.wait(lk, [&] { return pend->done; }); return std::move(pend->song);
        };
        auto nextShuffled = [&]() {
            if (pos >= order.size())
            {
                int last = order.back();
                std::shuffle(order.begin(), order.end(), rng);
                if (order.size() > 1 && order[0] == last) std::swap(order[0], order[1]);   // no repeat at the seam
                pos = 0;
            }
            return order[pos++];
        };
        int upcoming = nextShuffled();
        std::shared_ptr<Pending> next;
        if (mf) next = decodeAsync(upcoming);
        for (;;)
        {
            int idx = upcoming;
            int req = g_request.exchange(-1);
            if (req >= 0 && req < int(lib.size())) idx = req;        // the game started this song slot
            const Entry& e = lib[size_t(idx)];
            {
                std::lock_guard lk(g_lock);
                g_nowPlaying = e.path.stem().wstring();
                g_title = e.info.title; g_artist = e.info.artist; g_album = e.info.album;
            }
            auto started = std::chrono::steady_clock::now();
            bool played = false;
            if (mf)
            {
                Song s;
                if (next && next->index == idx) s = take(next);
                else { if (next) take(next); s = Decode(e.path); }   // asked for another song: decode it now
                upcoming = nextShuffled();
                if (upcoming == idx && lib.size() > 1) upcoming = nextShuffled();
                next = decodeAsync(upcoming);
                if (s.ok)
                {
                    Log("[music] playing %ls (loudness %.1f dB, levelled by %+.1f dB)\n", e.path.filename().c_str(), s.loudnessDb, 20 * std::log10(s.gain));
                    PlaySong(s, paused);
                    played = true;
                }
                else Log("[music] Media Foundation can't decode %ls - using the plain player\n", e.path.filename().c_str());
            }
            else upcoming = nextShuffled();
            if (!played)
            {
                Log("[music] playing %ls (plain player)\n", e.path.filename().c_str());
                PlayWithMci(e.path, paused);
            }
            if (g_request.load() < 0 && std::chrono::steady_clock::now() - started < std::chrono::seconds(2))
                std::this_thread::sleep_for(std::chrono::seconds(2));   // a song that ends at once: don't spin
            paused = false;
        }
    }
}

// The game started one of its song slots; the patched songs.bin holds your song's title there,
// so play the song with that title now.
void MusicRequestTitle(const std::string& title)
{
    const auto& lib = Library();
    for (size_t i = 0; i < lib.size(); i++)
        if (Trunc(lib[i].info.title) == title)
        {
            bool same; { std::lock_guard lk(g_lock); same = Trunc(g_title) == title; }
            if (same) return;                      // already playing it
            Log("[music] game started song slot \"%s\"\n", title.c_str());
            g_request = int(i);
            g_command = 1;                         // end the current song
            return;
        }
}
bool MusicIsOurTitle(const std::string& title)
{
    for (auto& e : Library()) if (Trunc(e.info.title) == title) return true;
    return false;
}

// songs.bin with your songs in EA's song slots (type 1 = song list, type 2 = the same songs'
// in-game streams, matched by their original title). Written next to the original.
bool MusicWriteSongsBin(const std::string& in, const std::string& out)
{
    const auto& lib = Library();
    if (lib.empty()) return false;
    FILE* f = fopen(in.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> d;
    uint8_t buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
    fclose(f);
    const size_t R = 288;
    auto type = [&](size_t r) { const uint8_t* p = &d[r * R + 264]; return uint32_t(p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]); };
    auto str = [&](size_t r, size_t off) { return std::string(reinterpret_cast<const char*>(&d[r * R + off])).substr(0, 63); };
    auto put = [&](size_t r, size_t off, const std::string& v) {
        memset(&d[r * R + off], 0, 64); memcpy(&d[r * R + off], v.data(), std::min<size_t>(v.size(), 63));
    };
    std::vector<std::pair<std::string, size_t>> byTitle;   // original title -> your song
    size_t k = 0, written = 0;
    for (size_t r = 0; r < d.size() / R; r++)
        if (type(r) == 1)
        {
            size_t song = k++ % lib.size();
            byTitle.push_back({ str(r, 0), song });
            put(r, 0, lib[song].info.title); put(r, 64, lib[song].info.artist); put(r, 128, lib[song].info.album);
            written++;
        }
    k = 0;
    for (size_t r = 0; r < d.size() / R; r++)
        if (type(r) == 2)
        {
            std::string t = str(r, 0);
            size_t song = k++ % lib.size();
            for (auto& [orig, s2] : byTitle) if (orig == t) { song = s2; break; }
            put(r, 0, lib[song].info.title); put(r, 64, lib[song].info.artist); put(r, 128, lib[song].info.album);
            written++;
        }
    FILE* o = fopen(out.c_str(), "wb");
    if (!o) return false;
    fwrite(d.data(), 1, d.size(), o);
    fclose(o);
    Log("[music] song list: %zu of the game's song slots now hold your %zu songs\n", written, lib.size());
    return true;
}

// true when your songs replace the game's soundtrack (checked by the game's music volume)
bool MusicReplacesGameMusic()
{
    static const bool r = [] {
        if (getenv("NBASH_NOMUSIC") || getenv("NBASH_GAMEMUSIC")) return false;
        return !Library().empty();
    }();
    return r;
}

void MusicStart()
{
    if (getenv("NBASH_NOMUSIC") || g_started.exchange(true)) return;
    if (const char* v = getenv("NBASH_MUSIC_VOLUME")) g_level = std::clamp(atoi(v), 0, 1000) / 1000.0f;
    std::thread(Player).detach();
}
void MusicNext() { g_command = 1; }
void MusicPause() { g_command = 2; }
void MusicLouder(int dir)
{
    float v = std::clamp(g_level + dir * 0.05f, 0.0f, 1.0f);
    g_level = v;
    Log("[music] music level %d%%\n", int(v * 100 + 0.5f));
}
int MusicLevelPercent() { return int(g_level * 100 + 0.5f); }
std::wstring MusicNowPlaying() { std::lock_guard lk(g_lock); return g_nowPlaying; }
bool MusicCurrentInfo(std::string& title, std::string& artist, std::string& album)
{
    std::lock_guard lk(g_lock);
    if (g_title.empty()) return false;
    title = g_title; artist = g_artist; album = g_album;
    return true;
}
#else
#include <string>
bool MusicReplacesGameMusic() { return false; }
void MusicStart() {}
void MusicNext() {}
void MusicPause() {}
void MusicLouder(int) {}
int MusicLevelPercent() { return 0; }
std::wstring MusicNowPlaying() { return {}; }
bool MusicCurrentInfo(std::string&, std::string&, std::string&) { return false; }
void MusicRequestTitle(const std::string&) {}
bool MusicIsOurTitle(const std::string&) { return false; }
bool MusicWriteSongsBin(const std::string&, const std::string&) { return false; }
#endif
