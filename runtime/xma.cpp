// XMA audio decoder contexts. On the console these are 64-byte hardware contexts in
// physical memory; the kernel's XMA* functions edit their fields and a hardware decoder
// turns XMA packets (2 KB each, holding bit-packed frames of 512 samples) into 16-bit
// big-endian PCM in an output ring of 256-byte blocks.
//
// This is a port of Xenia's XmaContextNew (BSD; xenia-canary src/xenia/apu/
// xma_context_new.cc): packet/frame walking, split frames, loops, buffer swapping and the
// output ring follow it closely. Each frame is decoded by FFmpeg's "xmaframes" decoder
// (LGPL, from Xenia's FFmpeg fork, linked statically). Without FFmpeg (non-Windows
// builds) frames decode to silence, which keeps the game's streams running at the right
// rate. NBASH_XMA_SILENT=1 forces silence (for comparison).
#include "guest.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <memory>

#ifdef NBASH_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
}
#endif

namespace
{
    struct XmaData   // host-order view of the 16 big-endian dwords
    {
        uint32_t input_buffer_0_packet_count : 12; uint32_t loop_count : 8; uint32_t input_buffer_0_valid : 1;
        uint32_t input_buffer_1_valid : 1; uint32_t output_buffer_block_count : 5; uint32_t output_buffer_write_offset : 5;

        uint32_t input_buffer_1_packet_count : 12; uint32_t loop_subframe_start : 2; uint32_t loop_subframe_end : 3;
        uint32_t loop_subframe_skip : 3; uint32_t subframe_decode_count : 4; uint32_t output_buffer_padding : 3;
        uint32_t sample_rate : 2; uint32_t is_stereo : 1; uint32_t unk_dword_1_c : 1; uint32_t output_buffer_valid : 1;

        uint32_t input_buffer_read_offset : 26; uint32_t error_status : 5; uint32_t error_set : 1;
        uint32_t loop_start : 26; uint32_t parser_error_status : 5; uint32_t parser_error_set : 1;
        uint32_t loop_end : 26; uint32_t packet_metadata : 5; uint32_t current_buffer : 1;

        uint32_t input_buffer_0_ptr, input_buffer_1_ptr, output_buffer_ptr, work_buffer_ptr;

        uint32_t output_buffer_read_offset : 5; uint32_t : 25; uint32_t stop_when_done : 1; uint32_t interrupt_when_done : 1;
        uint32_t unk_dwords_10_15[6];

        void Load(uint32_t a) { auto* d = (uint32_t*)this; for (int i = 0; i < 16; i++) d[i] = R32(a + i * 4); }
        void Store(uint32_t a) const { auto* d = (const uint32_t*)this; for (int i = 0; i < 16; i++) W32(a + i * 4, d[i]); }

        bool IsInputBufferValid(uint32_t b) const { return b == 0 ? input_buffer_0_valid : input_buffer_1_valid; }
        bool IsCurrentInputBufferValid() const { return IsInputBufferValid(current_buffer); }
        bool IsAnyInputBufferValid() const { return input_buffer_0_valid || input_buffer_1_valid; }
        uint32_t GetInputBufferAddress(uint32_t b) const { return b == 0 ? input_buffer_0_ptr : input_buffer_1_ptr; }
        uint32_t GetCurrentInputBufferAddress() const { return GetInputBufferAddress(current_buffer); }
        uint32_t GetInputBufferPacketCount(uint32_t b) const { return b == 0 ? input_buffer_0_packet_count : input_buffer_1_packet_count; }
        uint32_t GetCurrentInputBufferPacketCount() const { return GetInputBufferPacketCount(current_buffer); }
        bool IsConsumeOnlyContext() const { return (input_buffer_0_packet_count | input_buffer_1_packet_count) == 0; }
    };
    static_assert(sizeof(XmaData) == 64, "XMA context is 64 bytes");

    constexpr uint32_t kContexts = 320;
    constexpr uint32_t kBytesPerPacket = 2048, kBytesPerPacketHeader = 4, kBytesPerPacketData = kBytesPerPacket - kBytesPerPacketHeader;
    constexpr uint32_t kBitsPerPacket = kBytesPerPacket * 8, kBitsPerPacketHeader = 32, kBitsPerFrameHeader = 15;
    constexpr uint32_t kSamplesPerFrame = 512, kBytesPerFrameChannel = kSamplesPerFrame * 2;
    constexpr uint32_t kOutputBytesPerBlock = 256;
    constexpr uint32_t kMaxFrameLength = 0x7FFF;
    constexpr uint32_t kMaxFrameSizeinBits = 0x4000 - kBitsPerPacketHeader;
    constexpr int kIdToSampleRate[4] = { 24000, 32000, 44100, 48000 };

    uint8_t* PhysPtr(uint32_t p) { return G2H(0xA0000000u + (p & 0x1FFFFFFF)); }
    uint32_t Phys(uint32_t a) { return a & 0x1FFFFFFF; }

    // ---- packet header helpers (xma_helpers.h)
    uint8_t PacketFrameCount(const uint8_t* p) { return p[0] >> 2; }
    bool PacketIsXma2(const uint8_t* p) { return (p[2] & 7) == 1; }
    uint8_t PacketSkipCount(const uint8_t* p) { return p[3]; }
    uint32_t PacketFrameOffset(const uint8_t* p) { return uint32_t(uint16_t(((p[0] & 3) << 13) | (p[1] << 5) | (p[2] >> 3))) + 32; }

    // ---- MSB-first bit reader (xenia/base/bit_stream)
    struct BitStream
    {
        const uint8_t* buf; size_t size_bits; size_t off = 0;
        BitStream(const uint8_t* b, size_t bits) : buf(b), size_bits(bits) {}
        void SetOffset(size_t o) { off = std::min(o, size_bits); }
        size_t BitsRemaining() const { return size_bits - off; }
        uint64_t Peek(size_t n) const
        {
            uint64_t v = 0;
            for (size_t i = 0; i < n; i++)
            {
                size_t b = off + i;
                uint32_t bit = b < size_bits ? (buf[b >> 3] >> (7 - (b & 7))) & 1 : 0;
                v = (v << 1) | bit;
            }
            return v;
        }
        void Advance(size_t n) { off = std::min(off + n, size_bits); }
        uint64_t Read(size_t n) { uint64_t v = Peek(n); Advance(n); return v; }
        // Copies num_bits starting at the current offset into dest, byte-aligned to the source
        // (so the data starts at bit (offset % 8) of dest[0]); returns that bit offset.
        size_t Copy(uint8_t* dest, size_t destBytes, size_t num_bits)
        {
            size_t start = off, lead = start & 7;
            size_t first = start >> 3, last = (start + num_bits + 7) >> 3;   // exclusive
            size_t srcBytes = (size_bits + 7) >> 3;
            size_t n = std::min({ last - first, destBytes, srcBytes > first ? srcBytes - first : 0 });
            memcpy(dest, buf + first, n);
            if (n) dest[0] &= uint8_t(0xFF >> lead);                         // clear bits before the frame
            size_t endBit = lead + num_bits;
            if ((endBit & 7) && (endBit >> 3) < n) dest[endBit >> 3] &= uint8_t(0xFF << (8 - (endBit & 7)));
            Advance(num_bits);
            return lead;
        }
    };

    // ---- output ring (xenia/base/ring_buffer: read == write means empty)
    struct Ring
    {
        uint8_t* buf; uint32_t cap, rd, wr;
        uint32_t WriteCount() const { return rd == wr ? cap : wr < rd ? rd - wr : (cap - wr) + rd; }
        bool Empty() const { return rd == wr; }
        void Write(const uint8_t* src, uint32_t n)
        {
            n = std::min(n, WriteCount());
            if (!n || !cap) return;
            if (wr + n < cap) { memcpy(buf + wr, src, n); wr += n; }
            else { uint32_t left = cap - wr; memcpy(buf + wr, src, left); memcpy(buf, src + left, n - left); wr = n - left; }
        }
    };

    struct PacketInfo
    {
        uint8_t frame_count = 0, current_frame = 0; uint32_t current_frame_size = 0;
        bool IsLastFrameInPacket() const { return frame_count == 0 || current_frame == frame_count - 1; }
    };

    bool g_silent = false;
    std::atomic<uint64_t> g_framesDecoded{0}, g_framesFailed{0};

    struct Context
    {
        bool allocated = false;
        std::atomic<bool> enabled{false};
        uint32_t guest = 0;

        std::array<uint8_t, kBytesPerPacketData * 2> input;
        std::array<uint8_t, 1 + 4096> frame;          // byte 0: padding info for the decoder
        std::array<uint8_t, kBytesPerFrameChannel * 2> raw;
        int32_t remainingBlocks = 0;                   // free blocks in the output ring
        uint8_t remainingSubframes = 0;               // decoded but not yet written
        uint8_t loopOutputLimit = 0;
        bool loopStartSkipPending = false;
#ifdef NBASH_HAVE_FFMPEG
        const AVCodec* codec = nullptr;
        AVCodecContext* av = nullptr;
        AVFrame* avFrame = nullptr;
        AVPacket* avPacket = nullptr;
#endif

        void Reset() { remainingSubframes = 0; loopOutputLimit = 0; loopStartSkipPending = false; }

        static void SwapInputBuffer(XmaData& d)
        {
            if (d.current_buffer == 0) d.input_buffer_0_valid = 0; else d.input_buffer_1_valid = 0;
            d.current_buffer ^= 1;
            d.input_buffer_read_offset = kBitsPerPacketHeader;
        }

        // ---------------------------------------------------------------- packets
        bool PacketHandle(const XmaData& d, uint32_t& buffer, uint32_t& packet, uint32_t count) const
        {
            if (packet >= count) { buffer ^= 1; packet -= count; }
            if (!d.IsInputBufferValid(buffer) || !d.GetInputBufferAddress(buffer)) return false;
            return packet < d.GetInputBufferPacketCount(buffer);
        }
        const uint8_t* NextPacket(const XmaData& d, uint32_t next, uint32_t count) const
        {
            uint32_t b = d.current_buffer, p = next;
            if (!PacketHandle(d, b, p, count)) return nullptr;
            return PhysPtr(d.GetInputBufferAddress(b)) + p * kBytesPerPacket;
        }
        static uint32_t NextPacketReadOffset(const uint8_t* buffer, uint32_t next, uint32_t count)
        {
            while (next < count)
            {
                const uint8_t* p = buffer + next * kBytesPerPacket;
                uint32_t fo = PacketFrameOffset(p);
                if (fo <= kMaxFrameSizeinBits) return next * kBitsPerPacket + fo;
                uint8_t skip = PacketSkipCount(p);
                if (skip == 0xFF) break;
                next += skip + 1;
            }
            return kBitsPerPacketHeader;
        }
        uint32_t NextPacketReadOffset(const XmaData& d, uint32_t next, uint32_t count) const
        {
            uint32_t b = d.current_buffer, p = next;
            if (!PacketHandle(d, b, p, count)) return kBitsPerPacketHeader;
            return NextPacketReadOffset(PhysPtr(d.GetInputBufferAddress(b)), p, d.GetInputBufferPacketCount(b));
        }

        static PacketInfo GetPacketInfo(const uint8_t* packet, uint32_t frame_offset)
        {
            PacketInfo info;
            uint32_t first = PacketFrameOffset(packet);
            BitStream s(packet, kBitsPerPacket);
            s.SetOffset(first);
            if (frame_offset < first) { info.current_frame = 0; info.current_frame_size = first - frame_offset; }
            for (;;)
            {
                if (s.BitsRemaining() < kBitsPerFrameHeader)
                {
                    if (s.BitsRemaining() > 0)
                    {
                        if (s.off == frame_offset) { info.current_frame = info.frame_count; info.current_frame_size = 0; }
                        info.frame_count++;
                    }
                    break;
                }
                uint64_t size = s.Peek(kBitsPerFrameHeader);
                if (size == 0 || size == kMaxFrameLength) break;
                if (s.off == frame_offset) { info.current_frame = info.frame_count; info.current_frame_size = uint32_t(size); }
                info.frame_count++;
                if (size > s.BitsRemaining()) break;
                s.Advance(size - 1);
                if (s.Read(1) == 0) break;
            }
            if (PacketIsXma2(packet))
            {
                uint8_t n = PacketFrameCount(packet);
                if (n > info.frame_count)
                {
                    if (info.current_frame_size == 0) info.current_frame = info.frame_count;
                    info.frame_count = n;
                }
            }
            return info;
        }

        // ---------------------------------------------------------------- decoding
        bool DecodeFrame(int rateId, bool stereo, uint32_t frameBits, uint32_t padStart)
        {
#ifdef NBASH_HAVE_FFMPEG
            if (g_silent) return false;
            if (!codec)
            {
                codec = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
                if (!codec) return false;
                avFrame = av_frame_alloc();
                avPacket = av_packet_alloc();
            }
            int rate = kIdToSampleRate[std::min(rateId, 3)];
            int ch = stereo ? 2 : 1;
            if (!av || av->sample_rate != rate || av->ch_layout.nb_channels != ch)
            {
                if (av) avcodec_free_context(&av);
                av = avcodec_alloc_context3(codec);
                av->sample_rate = rate;
                av_channel_layout_default(&av->ch_layout, ch);
                av->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
                if (avcodec_open2(av, codec, nullptr) < 0) { avcodec_free_context(&av); return false; }
            }
            int size = int(1 + (padStart + frameBits) / 8 + (((padStart + frameBits) % 8) ? 1 : 0));
            uint32_t padEnd = uint32_t(size * 8) - (8 + padStart + frameBits);
            frame[0] = uint8_t(((padStart & 7) << 5) | ((padEnd & 7) << 2));
            avPacket->data = frame.data();
            avPacket->size = size;
            if (avcodec_send_packet(av, avPacket) < 0) return false;
            if (avcodec_receive_frame(av, avFrame) < 0) return false;
            if (avFrame->nb_samples < int(kSamplesPerFrame) || !avFrame->data[0]) return false;
            // float planar -> interleaved big-endian int16, saturated
            auto* out = reinterpret_cast<uint16_t*>(raw.data());
            const float* c0 = reinterpret_cast<const float*>(avFrame->data[0]);
            const float* c1 = stereo && avFrame->data[1] ? reinterpret_cast<const float*>(avFrame->data[1]) : c0;
            uint32_t o = 0;
            for (uint32_t i = 0; i < kSamplesPerFrame; i++)
                for (int j = 0; j < ch; j++)
                {
                    float f = (j ? c1 : c0)[i];
                    f = f > 1.0f ? 1.0f : f < -1.0f ? -1.0f : f;
                    int v = int(lrintf(f * 32767.0f));
                    out[o++] = __builtin_bswap16(uint16_t(int16_t(v)));
                }
            return true;
#else
            (void)rateId; (void)stereo; (void)frameBits; (void)padStart;
            return false;
#endif
        }

        void UpdateLoopStatus(XmaData& d)
        {
            if (d.loop_count == 0) return;
            uint32_t ls = std::max(kBitsPerPacketHeader, uint32_t(d.loop_start));
            uint32_t le = std::max(kBitsPerPacketHeader, uint32_t(d.loop_end));
            if (d.input_buffer_read_offset != le) return;
            d.input_buffer_read_offset = ls;
            loopStartSkipPending = true;
            if (d.loop_count != 255) d.loop_count--;
        }

        void Decode(XmaData& d)
        {
            if (!d.IsAnyInputBufferValid()) return;
            if (remainingSubframes > 0) return;
            if (!d.IsCurrentInputBufferValid())
            {
                SwapInputBuffer(d);
                if (!d.IsCurrentInputBufferValid()) return;
            }
            uint8_t* cur = PhysPtr(d.GetCurrentInputBufferAddress());
            input.fill(0);

            bool isLoopEndFrame = false;
            if (d.loop_count > 0)
                isLoopEndFrame = d.input_buffer_read_offset == std::max(kBitsPerPacketHeader, uint32_t(d.loop_end));
            UpdateLoopStatus(d);
            if (!d.output_buffer_block_count) return;
            if (d.input_buffer_read_offset < kBitsPerPacketHeader) d.input_buffer_read_offset = kBitsPerPacketHeader;

            const uint32_t size = d.GetCurrentInputBufferPacketCount() * kBytesPerPacket;
            const uint32_t count = size / kBytesPerPacket;
            if (d.input_buffer_read_offset >= size * 8) { SwapInputBuffer(d); return; }   // Xenia: invalid packet index
            const uint32_t packetIndex = (d.input_buffer_read_offset >> 3) / kBytesPerPacket;

            uint8_t* packet = cur + packetIndex * kBytesPerPacket;
            const uint32_t firstFrame = PacketFrameOffset(packet);
            uint32_t rel = d.input_buffer_read_offset % kBitsPerPacket;
            if (rel < firstFrame)
            {
                d.input_buffer_read_offset = packetIndex * kBitsPerPacket + firstFrame;
                rel = firstFrame;
            }
            const uint8_t skip = PacketSkipCount(packet);
            if (skip == 0xFF)
            {
                uint32_t nextIdx = packetIndex + 1;
                uint32_t nextOff = NextPacketReadOffset(d, nextIdx, count);
                if (nextIdx >= count || nextOff == kBitsPerPacketHeader) SwapInputBuffer(d);
                d.input_buffer_read_offset = nextOff;
                return;
            }

            PacketInfo info = GetPacketInfo(packet, rel);
            const uint32_t nextPacketIndex = packetIndex + skip + 1;

            if (info.current_frame_size == 0)   // frame header split across the packet boundary
            {
                const uint8_t* np = NextPacket(d, nextPacketIndex, count);
                if (!np) { SwapInputBuffer(d); return; }
                memcpy(input.data(), packet + kBytesPerPacketHeader, kBytesPerPacketData);
                memcpy(input.data() + kBytesPerPacketData, np + kBytesPerPacketHeader, kBytesPerPacketData);
                BitStream combined(input.data(), (kBitsPerPacket - kBitsPerPacketHeader) * 2);
                combined.SetOffset(rel - kBitsPerPacketHeader);
                uint64_t fs = combined.Peek(kBitsPerFrameHeader);
                if (fs == kMaxFrameLength) { d.error_status = 4; return; }
                info.current_frame_size = uint32_t(fs);
            }

            BitStream stream(cur, size_t(packetIndex + 1) * kBitsPerPacket);
            stream.SetOffset(d.input_buffer_read_offset);
            const uint32_t bitsToCopy = std::min(uint32_t(stream.BitsRemaining()), info.current_frame_size);
            if (bitsToCopy == 0) { SwapInputBuffer(d); return; }

            if (info.IsLastFrameInPacket() && stream.BitsRemaining() < info.current_frame_size)
            {
                const uint8_t* np = NextPacket(d, nextPacketIndex, count);
                if (!np) { d.error_status = 4; return; }
                memcpy(input.data() + kBytesPerPacketData, np + kBytesPerPacketHeader, kBytesPerPacketData);
            }
            memcpy(input.data(), packet + kBytesPerPacketHeader, kBytesPerPacketData);

            stream = BitStream(input.data(), (kBitsPerPacket - kBitsPerPacketHeader) * 2);
            stream.SetOffset(rel - kBitsPerPacketHeader);
            frame.fill(0);
            const uint32_t padStart = uint32_t(stream.Copy(frame.data() + 1, frame.size() - 1, info.current_frame_size));

            raw.fill(0);
            if (DecodeFrame(d.sample_rate, d.is_stereo, info.current_frame_size, padStart)) g_framesDecoded++;
            else g_framesFailed++;          // keep the stream's timing: the frame plays as silence
            remainingSubframes = uint8_t(4 << d.is_stereo);
            loopOutputLimit = isLoopEndFrame ? uint8_t((d.loop_subframe_end + 1) << d.is_stereo) : 0;
            if (loopStartSkipPending)
            {
                uint8_t s = uint8_t(d.loop_subframe_skip << d.is_stereo);
                if (s < remainingSubframes) remainingSubframes -= s;
                loopStartSkipPending = false;
            }

            if (!info.IsLastFrameInPacket())
            {
                uint32_t nextFrame = (d.input_buffer_read_offset + bitsToCopy) % kBitsPerPacket;
                d.input_buffer_read_offset = packetIndex * kBitsPerPacket + nextFrame;
                return;
            }
            uint32_t nextOff = NextPacketReadOffset(d, nextPacketIndex, count);
            if (nextPacketIndex >= count || nextOff == kBitsPerPacketHeader) SwapInputBuffer(d);
            if (nextOff == kBitsPerPacketHeader && d.IsAnyInputBufferValid())
            {
                nextOff = PacketFrameOffset(PhysPtr(d.GetCurrentInputBufferAddress()));
                if (nextOff > kMaxFrameSizeinBits) { SwapInputBuffer(d); return; }
            }
            d.input_buffer_read_offset = nextOff;
        }

        void Consume(Ring& r, const XmaData& d)
        {
            if (!remainingSubframes) return;
            const uint8_t total = uint8_t((kBytesPerFrameChannel / kOutputBytesPerBlock) << d.is_stereo);
            if (loopOutputLimit > 0 && uint8_t(total - remainingSubframes) >= loopOutputLimit)
            {
                remainingBlocks -= d.output_buffer_padding;
                remainingSubframes = 0;
                loopOutputLimit = 0;
                return;
            }
            const int sdc = std::max(1u, uint32_t(d.subframe_decode_count));
            int toWrite = std::min(int(remainingSubframes), sdc);
            if (loopOutputLimit > 0) toWrite = std::min(toWrite, int(loopOutputLimit) - int(total - remainingSubframes));
            if (toWrite <= 0) { remainingSubframes = 0; return; }
            const int readOff = int(total) - int(remainingSubframes);
            r.Write(raw.data() + kOutputBytesPerBlock * readOff, uint32_t(toWrite) * kOutputBytesPerBlock);
            const int headroom = (remainingSubframes - toWrite == 0) ? int(d.output_buffer_padding) : 0;
            remainingBlocks -= toWrite + headroom;
            remainingSubframes = uint8_t(remainingSubframes - toWrite);
        }

        // Re-read the context and merge only decoder-owned fields (the game may have changed
        // others while we worked).
        void StoreMerged(const XmaData& d, const XmaData& initial)
        {
            XmaData f; f.Load(guest);
            f.loop_count = d.loop_count;
            f.output_buffer_write_offset = d.output_buffer_write_offset;
            if (initial.input_buffer_0_valid && !d.input_buffer_0_valid) f.input_buffer_0_valid = 0;
            if (initial.input_buffer_1_valid && !d.input_buffer_1_valid) f.input_buffer_1_valid = 0;
            if (initial.output_buffer_valid && !d.output_buffer_valid) f.output_buffer_valid = 0;
            f.input_buffer_read_offset = d.input_buffer_read_offset;
            f.error_status = d.error_status;
            f.current_buffer = d.current_buffer;
            f.output_buffer_read_offset = d.output_buffer_read_offset;
            f.Store(guest);
        }

        bool Work()
        {
            if (!allocated || !enabled) return false;
            enabled = false;
            XmaData d; d.Load(guest);
            const XmaData initial = d;
            if (!d.output_buffer_valid) return true;

            Ring r{ PhysPtr(d.output_buffer_ptr), d.output_buffer_block_count * kOutputBytesPerBlock,
                    d.output_buffer_read_offset * kOutputBytesPerBlock, d.output_buffer_write_offset * kOutputBytesPerBlock };
            remainingBlocks = int32_t(r.WriteCount() / kOutputBytesPerBlock);

            if (d.IsConsumeOnlyContext())
            {
                if (remainingSubframes == 0) return true;
                Consume(r, d);
                d.output_buffer_write_offset = r.wr / kOutputBytesPerBlock;
                if (r.Empty()) d.output_buffer_valid = 0;
                StoreMerged(d, initial);
                return true;
            }

            const int32_t minimum = int32_t(std::max(1u, uint32_t(d.subframe_decode_count))) + int32_t(d.output_buffer_padding);
            if (minimum > remainingBlocks) { StoreMerged(d, initial); return true; }

            for (int guard = 0; remainingBlocks >= minimum && guard < 8192; guard++)
            {
                const uint32_t preOff = d.input_buffer_read_offset;
                const uint8_t preRemaining = remainingSubframes;
                Decode(d);
                Consume(r, d);
                if ((!d.IsAnyInputBufferValid() || d.error_status == 4) && remainingSubframes == 0) break;
                if (preRemaining == 0 && d.input_buffer_read_offset == preOff && remainingSubframes == 0) break;
            }

            if (initial.IsAnyInputBufferValid())
                d.output_buffer_write_offset = r.wr / kOutputBytesPerBlock;
            else if (d.output_buffer_write_offset != d.output_buffer_read_offset)
            {
                d.output_buffer_write_offset = d.output_buffer_read_offset;
                d.output_buffer_valid = 0;
            }
            if (remainingBlocks == 0 && r.Empty()) d.output_buffer_valid = 0;
            StoreMerged(d, initial);
            return true;
        }
    };

    Context g_ctx[kContexts];
    uint32_t g_array = 0;                    // guest address of the context array
    std::mutex g_lock;
    std::condition_variable g_kick;
    std::atomic<bool> g_started{false};

    int IndexOf(uint32_t ptr)
    {
        if (!g_array || ptr < g_array) return -1;
        uint32_t i = (ptr - g_array) / 64;
        return i < kContexts ? int(i) : -1;
    }

    void DecoderThread()
    {
        auto lastLog = std::chrono::steady_clock::now();
        for (;;)
        {
            bool worked = false;
            {
                std::lock_guard lock(g_lock);
                for (uint32_t i = 0; i < kContexts; i++) worked |= g_ctx[i].Work();
            }
            auto now = std::chrono::steady_clock::now();
            if (now - lastLog > std::chrono::seconds(10))
            {
                lastLog = now;
                static uint64_t lastOk = 0, lastBad = 0;
                uint64_t ok = g_framesDecoded, bad = g_framesFailed;
                if (ok != lastOk || bad != lastBad)
                    Log("[xma] frames decoded %llu, failed %llu (last 10 s: %llu / %llu)\n",
                        (unsigned long long)ok, (unsigned long long)bad,
                        (unsigned long long)(ok - lastOk), (unsigned long long)(bad - lastBad));
                lastOk = ok; lastBad = bad;
            }
            if (!worked)
            {
                std::unique_lock lock(g_lock);
                g_kick.wait_for(lock, std::chrono::milliseconds(1));
            }
        }
    }

    void EnsureStarted()
    {
        bool expected = false;
        if (!g_started.compare_exchange_strong(expected, true)) return;
        g_silent = getenv("NBASH_XMA_SILENT") != nullptr;
        g_array = AllocPhysical(kContexts * 64, 0x1000);
        for (uint32_t i = 0; i < kContexts; i++) g_ctx[i].guest = g_array + i * 64;
#ifdef NBASH_HAVE_FFMPEG
        Log("[xma] decoder: FFmpeg xmaframes%s\n", g_silent ? " (forced silent)" : "");
#else
        Log("[xma] decoder: silent (built without FFmpeg)\n");
#endif
        std::thread(DecoderThread).detach();
    }
}

uint64_t XmaFramesDecoded() { return g_framesDecoded; }

IMPORT(XMACreateContext)
{
    TRACE_IMPORT();
    EnsureStarted();
    std::lock_guard lock(g_lock);
    for (uint32_t i = 0; i < kContexts; i++)
        if (!g_ctx[i].allocated)
        {
            g_ctx[i].allocated = true;
            g_ctx[i].enabled = false;
            g_ctx[i].Reset();
            memset(G2H(g_array + i * 64), 0, 64);
            W32(ARG(3).u32, g_array + i * 64);
            RETURN(0);
        }
    W32(ARG(3).u32, 0);
    RETURN(STATUS_NO_MEMORY);
}

IMPORT(XMAReleaseContext)
{
    TRACE_IMPORT();
    std::lock_guard lock(g_lock);
    int i = IndexOf(ARG(3).u32);
    if (i >= 0) { g_ctx[i].allocated = false; g_ctx[i].enabled = false; g_ctx[i].Reset(); memset(G2H(ARG(3).u32), 0, 64); }
    RETURN(0);
}

// XMAInitializeContext(context, XMA_CONTEXT_INIT*)
IMPORT(XMAInitializeContext)
{
    TRACE_IMPORT();
    uint32_t ptr = ARG(3).u32, init = ARG(4).u32;
    std::lock_guard lock(g_lock);
    XmaData d; memset(&d, 0, sizeof(d));
    d.input_buffer_0_ptr = R32(init + 0) ? Phys(R32(init + 0)) : 0;
    d.input_buffer_0_packet_count = R32(init + 4);
    d.input_buffer_1_ptr = R32(init + 8) ? Phys(R32(init + 8)) : 0;
    d.input_buffer_1_packet_count = R32(init + 12);
    d.input_buffer_read_offset = R32(init + 16);
    d.output_buffer_ptr = Phys(R32(init + 20));
    d.output_buffer_block_count = R32(init + 24);
    d.subframe_decode_count = R32(init + 32);
    d.is_stereo = R32(init + 36) >= 1;
    d.sample_rate = R32(init + 40);
    d.loop_start = R32(init + 44);
    d.loop_end = R32(init + 48);
    d.loop_count = R8(init + 52);
    d.loop_subframe_end = R8(init + 53);
    d.loop_subframe_skip = R8(init + 54);
    d.Store(ptr);
    int i = IndexOf(ptr);
    if (i >= 0) g_ctx[i].Reset();
    RETURN(0);
}

#define XMA_FIELD_OP(body) { std::lock_guard lock(g_lock); XmaData d; d.Load(ARG(3).u32); body; }
IMPORT(XMASetInputBuffer0)      { TRACE_IMPORT(); XMA_FIELD_OP(d.input_buffer_0_ptr = Phys(ARG(4).u32); d.input_buffer_0_packet_count = ARG(5).u32; d.Store(ARG(3).u32)); RETURN(0); }
IMPORT(XMASetInputBuffer1)      { TRACE_IMPORT(); XMA_FIELD_OP(d.input_buffer_1_ptr = Phys(ARG(4).u32); d.input_buffer_1_packet_count = ARG(5).u32; d.Store(ARG(3).u32)); RETURN(0); }
IMPORT(XMASetInputBuffer0Valid) { TRACE_IMPORT(); XMA_FIELD_OP(d.input_buffer_0_valid = 1; d.Store(ARG(3).u32)); RETURN(0); }
IMPORT(XMASetInputBuffer1Valid) { TRACE_IMPORT(); XMA_FIELD_OP(d.input_buffer_1_valid = 1; d.Store(ARG(3).u32)); RETURN(0); }
IMPORT(XMAIsInputBuffer0Valid)  { TRACE_IMPORT(); uint32_t v; XMA_FIELD_OP(v = d.input_buffer_0_valid); RETURN(v); }
IMPORT(XMAIsInputBuffer1Valid)  { TRACE_IMPORT(); uint32_t v; XMA_FIELD_OP(v = d.input_buffer_1_valid); RETURN(v); }
IMPORT(XMASetOutputBufferValid) { TRACE_IMPORT(); XMA_FIELD_OP(d.output_buffer_valid = 1; d.Store(ARG(3).u32)); RETURN(0); }
IMPORT(XMAIsOutputBufferValid)  { TRACE_IMPORT(); uint32_t v; XMA_FIELD_OP(v = d.output_buffer_valid); RETURN(v); }
IMPORT(XMAGetOutputBufferReadOffset)  { TRACE_IMPORT(); uint32_t v; XMA_FIELD_OP(v = d.output_buffer_read_offset); RETURN(v); }
IMPORT(XMASetOutputBufferReadOffset)  { TRACE_IMPORT(); XMA_FIELD_OP(d.output_buffer_read_offset = ARG(4).u32; d.Store(ARG(3).u32)); RETURN(0); }
IMPORT(XMAGetOutputBufferWriteOffset) { TRACE_IMPORT(); uint32_t v; XMA_FIELD_OP(v = d.output_buffer_write_offset); RETURN(v); }

IMPORT(XMAEnableContext)
{
    TRACE_IMPORT();
    int i = IndexOf(ARG(3).u32);
    if (i >= 0) { g_ctx[i].enabled = true; g_kick.notify_all(); }
    RETURN(0);
}

IMPORT(XMADisableContext)
{
    TRACE_IMPORT();
    std::lock_guard lock(g_lock);           // waits for any decode pass in progress
    int i = IndexOf(ARG(3).u32);
    if (i >= 0) g_ctx[i].enabled = false;
    RETURN(0);
}
