// Audio output clock. XAudio's render driver client is called by the console every audio
// frame (256 samples at 48 kHz, ~5.33 ms) to produce the next frame of mixed sound; game
// timing (movie playback among it) follows these callbacks. Each submitted frame (6 channels x
// 256 float samples, big-endian, one channel after another) is mixed down to stereo and played
// through XAudio2 (NBASH_NOSOUND=1 = silent, NBASH_VOLUME=0..2, default 1).
#include "guest.h"
#ifdef _WIN32
#include <windows.h>
#include <xaudio2.h>
#endif
#include <chrono>
#include <thread>
#include <atomic>

int AudioQueuedBuffers();   // host output queue depth, -1 = no output (pace by the clock)
void AudioLevelReport();    // 10 s level / limiter line

namespace
{
    struct Client { uint32_t callback = 0, arg = 0; std::atomic<bool> active{false}; };
    Client g_clients[8];
    std::atomic<uint64_t> g_framesSubmitted{0};
    std::atomic<uint64_t> g_dry{0}, g_droppedOut{0};   // output ran empty / frames dropped

    void ClientThread(int index)
    {
        Client& c = g_clients[index];
        GuestThread* t = CreateGuestThreadState(0x10000);
        t_thread = t;
        uint32_t stack = AllocVirtual(0x40000, 0x1000);
        auto* ctx = new PPCContext();
        InitContext(*ctx, t, stack + 0x40000);
        auto next = std::chrono::steady_clock::now();
        const auto frame = std::chrono::nanoseconds(1000000000LL * 256 / 48000);
        static const int target = [] { const char* v = getenv("NBASH_AUDIO_BUFFERS"); int n = v ? atoi(v) : 8; return n < 2 ? 2 : n > 40 ? 40 : n; }();
        while (c.active)
        {
            // Even pacing: one callback every 5.33 ms (game timing follows these), nudged at most
            // 3% faster or slower to keep ~target frames queued at the sound card. Never bursts:
            // back-to-back calls make the game's mixer output silence, and if the game stops
            // submitting sound (replays) the clock must still run at real time.
            int q = index == 0 ? AudioQueuedBuffers() : -1;
            double adj = 1.0;
            if (q >= 0) { adj = 1.0 + (q - target) * 0.006; adj = adj < 0.97 ? 0.97 : adj > 1.03 ? 1.03 : adj; }
            next += std::chrono::nanoseconds(int64_t(1000000000.0 * 256 / 48000 * adj));
            std::this_thread::sleep_until(next);
            if (std::chrono::steady_clock::now() - next > std::chrono::milliseconds(100))
                next = std::chrono::steady_clock::now();              // fell behind: don't burst
            ctx->r3.u64 = c.arg;
            auto t0 = std::chrono::steady_clock::now();
            GuestCall(*ctx, c.callback);
            if (index == 0)
            {
                // timing report every 10 s: how long the game's mixer takes per 5.3 ms frame
                static double sum = 0, mx = 0; static uint64_t n = 0, slow = 0;
                static auto last = std::chrono::steady_clock::now();
                auto t1 = std::chrono::steady_clock::now();
                double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                sum += ms; n++; if (ms > mx) mx = ms; if (ms > 5.0) slow++;
                if (t1 - last > std::chrono::seconds(10))
                {
                    Log("[audio] 10 s: %llu frames (%.0f/s, need 187.5), mixer avg %.2f ms max %.1f ms, slow %llu, output ran dry %llu, dropped %llu, queued %d\n",
                        (unsigned long long)n, n / std::chrono::duration<double>(t1 - last).count(), sum / n, mx,
                        (unsigned long long)slow, (unsigned long long)g_dry.exchange(0), (unsigned long long)g_droppedOut.exchange(0), AudioQueuedBuffers());
                    AudioLevelReport();
                    sum = 0; mx = 0; n = 0; slow = 0; last = t1;
                }
            }
        }
    }
}

IMPORT(XAudioRegisterRenderDriverClient)
{
    TRACE_IMPORT();
    uint32_t cbPtr = ARG(3).u32, driverPtr = ARG(4).u32;
    if (!cbPtr || !R32(cbPtr)) RETURN(0x80070057);                     // E_INVALIDARG
    for (int i = 0; i < 8; i++)
    {
        if (g_clients[i].active) continue;
        g_clients[i].callback = R32(cbPtr);
        g_clients[i].arg = R32(cbPtr + 4);
        g_clients[i].active = true;
        W32(driverPtr, 0x41550000 | i);
        Log("[audio] render client %d: callback %08X arg %08X\n", i, g_clients[i].callback, g_clients[i].arg);
        std::thread(ClientThread, i).detach();
        RETURN(0);
    }
    RETURN(0x8007000E);                                                 // E_OUTOFMEMORY
}

IMPORT(XAudioUnregisterRenderDriverClient)
{
    TRACE_IMPORT();
    uint32_t i = ARG(3).u32 & 0xFFFF;
    if (i < 8) g_clients[i].active = false;
    RETURN(0);
}

uint64_t AudioFramesSubmitted() { return g_framesSubmitted; }

namespace
{
    // NBASH_AUDIODUMP=file.wav: also write everything the game outputs (stereo 16-bit 48 kHz)
    FILE* g_dump = nullptr;
    uint32_t g_dumpBytes = 0;
    void DumpHeader()
    {
        auto w32 = [](uint32_t v) { fwrite(&v, 4, 1, g_dump); };
        auto w16 = [](uint16_t v) { fwrite(&v, 2, 1, g_dump); };
        fseek(g_dump, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, g_dump); w32(36 + g_dumpBytes); fwrite("WAVEfmt ", 1, 8, g_dump);
        w32(16); w16(1); w16(2); w32(48000); w32(48000 * 4); w16(4); w16(16);
        fwrite("data", 1, 4, g_dump); w32(g_dumpBytes);
        fseek(g_dump, 0, SEEK_END);
    }
    void DumpFrame(uint32_t samples)
    {
        static int state = 0;
        if (state == 0)
        {
            const char* p = getenv("NBASH_AUDIODUMP");
            state = p && (g_dump = fopen(p, "wb")) ? 1 : 2;
            if (state == 1) DumpHeader();
        }
        if (state != 1 || !samples) return;
        auto in = [&](int ch, int i) { uint32_t b = R32(samples + uint32_t(ch * 256 + i) * 4); float f; memcpy(&f, &b, 4); return f; };
        int16_t out[512];
        for (int i = 0; i < 256; i++)
        {
            float c = in(2, i) * 0.7071f;
            float l = in(0, i) + c + in(4, i) * 0.7071f, r = in(1, i) + c + in(5, i) * 0.7071f;
            l = l > 1 ? 1 : l < -1 ? -1 : l; r = r > 1 ? 1 : r < -1 ? -1 : r;
            out[i * 2] = int16_t(l * 32767); out[i * 2 + 1] = int16_t(r * 32767);
        }
        fwrite(out, 2, 512, g_dump);
        g_dumpBytes += 2048;
        if ((g_dumpBytes & 0x3FFFF) == 0) { DumpHeader(); fflush(g_dump); }
    }
}


#ifdef _WIN32
namespace
{
    // host sound output: XAudio2 source voice, 48 kHz stereo float
    IXAudio2* g_xa = nullptr;
    IXAudio2MasteringVoice* g_master = nullptr;
    IXAudio2SourceVoice* g_voice = nullptr;
    constexpr int kBufs = 64;
    float g_bufs[kBufs][256 * 6];
    // 14 Nov (audio polish): the game mixes 5.1 (crowd / court ambience largely in the surround channels).
    //  - a 5.1 / 7.1 / spatial (Windows Sonic, Dolby Atmos for headphones) output now gets all 6 channels as they are (real surround);
    //  - stereo gets a proper downmix (front + 0.707 centre + 0.707 surround + LFE) through a smooth peak LIMITER instead of hard clipping
    //    at full scale (the old way crackled on loud moments: crowd + music + dunk could sum to ~2.5x full scale).
    int g_outCh = 2;                     // channels sent to the sound device (2 or 6)
    float g_limGain = 1.0f;              // limiter gain (1 = untouched)
    uint64_t g_limFrames = 0;            // frames where the limiter worked (status line)
    float g_peak = 0.0f;                 // loudest sample before the limiter in the last 10 s
    int g_next = 0;
    std::atomic<int> g_state{0};   // 0 not tried, 1 ready, 2 failed / off
    uint64_t g_dropped = 0;
    std::atomic<uint64_t> g_loudFrames{0};   // frames that were not silent (for the status line)

    bool InitOutput()
    {
        if (getenv("NBASH_NOSOUND")) return false;
        Log("[audio] starting sound output\n");
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        using CreateFn = HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);
        HMODULE m = LoadLibraryA("xaudio2_9.dll");
        if (!m) m = LoadLibraryA("xaudio2_8.dll");
        auto create = m ? (CreateFn)GetProcAddress(m, "XAudio2Create") : nullptr;
        Log("[audio] XAudio2 library %s\n", create ? "found" : "missing");
        if (!create || FAILED(create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) { Log("[audio] XAudio2 not available - no sound\n"); return false; }
        Log("[audio] XAudio2 created\n");
        // the device's own channel count (XAUDIO2_DEFAULT_CHANNELS): 6+ = surround output; NBASH_STEREO=1 forces the stereo downmix
        if (FAILED(g_xa->CreateMasteringVoice(&g_master, XAUDIO2_DEFAULT_CHANNELS, 48000, 0, nullptr, nullptr, AudioCategory_GameEffects)) &&
            FAILED(g_xa->CreateMasteringVoice(&g_master, 2, 48000, 0, nullptr, nullptr, AudioCategory_GameEffects))) { Log("[audio] no sound device\n"); return false; }
        XAUDIO2_VOICE_DETAILS md{}; g_master->GetVoiceDetails(&md);
        g_outCh = (md.InputChannels >= 6 && !getenv("NBASH_STEREO")) ? 6 : 2;
        WAVEFORMATEXTENSIBLE wx{};
        wx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; wx.Format.nChannels = WORD(g_outCh); wx.Format.nSamplesPerSec = 48000;
        wx.Format.wBitsPerSample = 32; wx.Format.nBlockAlign = WORD(4 * g_outCh); wx.Format.nAvgBytesPerSec = 48000 * 4 * g_outCh;
        wx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX); wx.Samples.wValidBitsPerSample = 32;
        wx.dwChannelMask = g_outCh == 6 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT)
                                        : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
        static const GUID kFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };   // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
        wx.SubFormat = kFloat;
        if (FAILED(g_xa->CreateSourceVoice(&g_voice, &wx.Format, 0, 2.0f, nullptr, nullptr, nullptr)))
        {
            if (g_outCh == 6) { Log("[audio] surround voice refused - stereo\n"); g_outCh = 2; wx.Format.nChannels = 2; wx.Format.nBlockAlign = 8; wx.Format.nAvgBytesPerSec = 48000 * 8; wx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT; }
            if (FAILED(g_xa->CreateSourceVoice(&g_voice, &wx.Format, 0, 2.0f, nullptr, nullptr, nullptr))) { Log("[audio] could not create a sound voice\n"); return false; }
        }
        Log("[audio] sound device: %u channels -> game sound sent as %s\n", unsigned(md.InputChannels), g_outCh == 6 ? "5.1 surround (as the game mixes it)" : "stereo (downmix + limiter)");
        const char* v = getenv("NBASH_VOLUME");
        g_voice->SetVolume(v ? float(atof(v)) : 1.0f);
        g_voice->Start(0, 0);
        Log("[audio] sound output on (XAudio2, 48 kHz stereo)\n");
        return true;
    }

    void PlayFrame(uint32_t samples)
    {
        DumpFrame(samples);
        int st = g_state.load();
        if (st == 0) { st = InitOutput() ? 1 : 2; g_state = st; }
        if (st != 1 || !samples) return;
        XAUDIO2_VOICE_STATE vs{};
        g_voice->GetState(&vs, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (vs.BuffersQueued == 0 && g_framesSubmitted > 400) g_dry++;
        if (vs.BuffersQueued > 48) { g_dropped++; g_droppedOut++; return; }   // safety only; the clock keeps ~6 queued
        // 6 channels (front L, front R, centre, LFE, surround L, surround R) x 256 samples
        auto in = [&](int ch, int i) { uint32_t b = R32(samples + uint32_t(ch * 256 + i) * 4); float f; memcpy(&f, &b, 4); return f; };
        float* out = g_bufs[g_next];
        bool loud = false, limited = false;
        // smooth peak limiter: ceiling just under full scale, instant attack (gain drops to fit the peak), ~80 ms release - no hard clipping
        const float ceil_ = 0.95f, rel = 1.0f - 1.0f / (0.080f * 48000.0f);
        const int nc = g_outCh;
        for (int i = 0; i < 256; i++)
        {
            float v[6];
            if (nc == 6) { for (int ch = 0; ch < 6; ch++) v[ch] = in(ch, i); }
            else
            {
                float c = in(2, i) * 0.7071f, lfe = in(3, i) * 0.5f;
                v[0] = in(0, i) + c + in(4, i) * 0.7071f + lfe;
                v[1] = in(1, i) + c + in(5, i) * 0.7071f + lfe;
            }
            float pk = 0.0f; for (int ch = 0; ch < nc; ch++) { float a = v[ch] < 0 ? -v[ch] : v[ch]; if (a > pk) pk = a; }
            if (pk > g_peak) g_peak = pk;
            if (pk * g_limGain > ceil_) { g_limGain = ceil_ / pk; limited = true; }
            else g_limGain = 1.0f - (1.0f - g_limGain) * rel;
            for (int ch = 0; ch < nc; ch++)
            {
                float o = v[ch] * g_limGain; o = o > 1.0f ? 1.0f : o < -1.0f ? -1.0f : o;
                out[i * nc + ch] = o;
                if (o > 0.001f || o < -0.001f) loud = true;
            }
        }
        if (loud) g_loudFrames++;
        if (limited) g_limFrames++;
        XAUDIO2_BUFFER b{};
        b.AudioBytes = 256 * nc * 4; b.pAudioData = (const BYTE*)out;
        g_voice->SubmitSourceBuffer(&b, nullptr);
        g_next = (g_next + 1) % kBufs;
    }
}
#else
namespace { void PlayFrame(uint32_t samples) { DumpFrame(samples); } }
#endif

#ifdef _WIN32
void AudioLevelReport()
{
    if (g_state.load() != 1) return;
    Log("[audio] level: loudest %.2f x full scale, limiter worked in %llu frames (of ~1875)%s\n", g_peak, (unsigned long long)g_limFrames, g_outCh == 6 ? ", 5.1 output" : "");
    g_peak = 0.0f; g_limFrames = 0;
}
int AudioQueuedBuffers()
{
    if (g_state.load() != 1 || !g_voice) return -1;
    XAUDIO2_VOICE_STATE vs{};
    g_voice->GetState(&vs, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    return int(vs.BuffersQueued);
}
#else
void AudioLevelReport() {}
int AudioQueuedBuffers() { return -1; }
#endif

IMPORT(XAudioSubmitRenderDriverFrame)
{
    g_framesSubmitted++;
    PlayFrame(ARG(4).u32);
    RETURN(0);
}

// Voice category volumes (music, effects, ...). The mixer asks every pass which categories
// changed, then reads the changed ones. Both must write their output: an unwritten mask was
// whatever sat on the caller's stack, and "changed" categories then got garbage volumes,
// which sometimes stopped a movie's sound track from playing (and with it the movie).
IMPORT(XAudioGetVoiceCategoryVolumeChangeMask)
{
    TRACE_IMPORT();
    // first call: both categories "changed" so the mixer reads their volumes once (1.0)
    static std::atomic<bool> first{true};
    if (ARG(4).u32) W32(ARG(4).u32, first.exchange(false) ? 3 : 0);
    RETURN(0);
}
// Category 0 = the game's sound, 1 = its soundtrack (the console's "background music", which it
// silences when you play your own music). With songs in the music folder the soundtrack is muted.
bool MusicReplacesGameMusic();   // music.cpp
IMPORT(XAudioGetVoiceCategoryVolume)
{
    TRACE_IMPORT();
    uint32_t cat = ARG(3).u32;
    static const float soundtrack = [] {
        bool mute = MusicReplacesGameMusic();
        Log("[audio] game soundtrack %s\n", mute ? "muted (your music replaces it)" : "on");
        return mute ? 0.0f : 1.0f;
    }();
    float one = cat == 1 ? soundtrack : 1.0f; uint32_t bits; memcpy(&bits, &one, 4);
    if (ARG(4).u32) W32(ARG(4).u32, bits);
    RETURN(0);
}

// XMsgInProcessCall(app, message, buffer, length): system "apps". The game asks the media
// player (app 0xFA) who controls music playback before it plays its own soundtrack;
// answer "the game" (0), not locked - as Xenia does.
IMPORT(XMsgInProcessCall)
{
    TRACE_IMPORT();
    uint32_t app = ARG(3).u32, msg = ARG(4).u32, buf = ARG(5).u32;
    if (app == 0xFA && msg == 0x0007001B && buf)                 // XMPGetPlaybackController
    {
        if (uint32_t p = R32(buf + 4)) W32(p, 0);
        if (uint32_t p = R32(buf + 8)) W32(p, 0);
        RETURN(0);
    }
    static int logged = 0;
    if (logged++ < 10) Log("[xmsg] InProcessCall app %X message %08X - not handled\n", app, msg);
    RETURN(0);
}

#ifdef _WIN32
uint64_t AudioFramesWithSound() { return g_loudFrames; }
#else
uint64_t AudioFramesWithSound() { return 0; }
#endif
