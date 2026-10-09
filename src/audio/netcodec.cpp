#include "netcodec.h"
#include "resample.h"
#include "util.h"

#include <opus.h>
#include <FLAC/stream_encoder.h>
#include <FLAC/stream_decoder.h>

#include <cmath>
#include <cstring>
#include <vector>

namespace net {

namespace {

// --- float to integer and back ---------------------------------------------
//
// 16 and 24 bit go out as signed integers scaled by the largest positive
// value, and are clamped first: the bus is float and can legitimately exceed
// one, and an unclamped conversion wraps a loud peak into the opposite sign,
// which is the loudest possible noise rather than the quietest.

float Clamp1(float v)
{
    return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
}

void Put16(unsigned char*& p, int v)
{
    *p++ = (unsigned char)(v & 0xFF);
    *p++ = (unsigned char)((v >> 8) & 0xFF);
}

void Put24(unsigned char*& p, int v)
{
    *p++ = (unsigned char)(v & 0xFF);
    *p++ = (unsigned char)((v >> 8) & 0xFF);
    *p++ = (unsigned char)((v >> 16) & 0xFF);
}

int Get16(const unsigned char*& p)
{
    const int v = (int)(short)(p[0] | (p[1] << 8));
    p += 2;
    return v;
}

int Get24(const unsigned char*& p)
{
    int v = p[0] | (p[1] << 8) | (p[2] << 16);
    if (v & 0x800000) v -= 0x1000000;          // sign extend
    p += 3;
    return v;
}

// --- a FIFO that never allocates while it is being used --------------------
//
// Sized once, up front, big enough for a long device period plus a packet.
// The render thread pushes into it, so growing it there is not an option.
class Fifo
{
public:
    void Reset(size_t frames)
    {
        m_buf.assign(frames * kChannels, 0.0f);
        m_head = m_tail = m_count = 0;
    }

    size_t Frames() const { return m_count; }
    size_t Room() const { return (m_buf.size() / kChannels) - m_count; }

    // Whatever does not fit is dropped, and dropping is reported: a FIFO that
    // silently overran would show up as audio that drifts without explanation.
    size_t Push(const float* in, size_t frames)
    {
        const size_t cap = m_buf.size() / kChannels;
        if (cap == 0) return 0;
        const size_t take = frames > Room() ? Room() : frames;
        for (size_t i = 0; i < take; ++i)
        {
            float* dst = &m_buf[m_head * kChannels];
            for (int c = 0; c < kChannels; ++c) dst[c] = in[i * kChannels + c];
            m_head = (m_head + 1) % cap;
        }
        m_count += take;
        return take;
    }

    bool Pop(float* out, size_t frames)
    {
        if (m_count < frames) return false;
        const size_t cap = m_buf.size() / kChannels;
        for (size_t i = 0; i < frames; ++i)
        {
            const float* src = &m_buf[m_tail * kChannels];
            for (int c = 0; c < kChannels; ++c) out[i * kChannels + c] = src[c];
            m_tail = (m_tail + 1) % cap;
        }
        m_count -= frames;
        return true;
    }

private:
    std::vector<float> m_buf;
    size_t m_head = 0, m_tail = 0, m_count = 0;
};

// How much room a FIFO needs: a generous device period, plus a packet, plus
// slack. 200 ms covers the longest period any endpoint here asks for.
size_t FifoFramesFor(int rate, int packetFrames)
{
    const size_t period = (size_t)rate / 5;
    return period + (size_t)packetFrames * 4 + 64;
}

// --- PCM --------------------------------------------------------------------

class PcmEncoder : public Encoder
{
public:
    PcmEncoder(int codec, int rate, int frames)
        : m_codec(codec), m_rate(rate), m_frames(frames)
    {
        m_fifo.Reset(FifoFramesFor(rate, frames));
        m_scratch.assign((size_t)frames * kChannels, 0.0f);
    }

    void Push(const float* in, int frames) override
    {
        m_fifo.Push(in, (size_t)frames);
    }

    int Pull(unsigned char* out, int maxBytes, int* frames) override
    {
        const int need = m_frames * PcmBytesPerFrame(m_codec);
        if (need > maxBytes) return -1;
        if (!m_fifo.Pop(m_scratch.data(), (size_t)m_frames)) return 0;
        PcmFromFloat(m_codec, m_scratch.data(), m_frames, out);
        if (frames) *frames = m_frames;
        return need;
    }

    int WireRate() const override { return m_rate; }
    int WireFramesPerPacket() const override { return m_frames; }

private:
    int m_codec, m_rate, m_frames;
    Fifo m_fifo;
    std::vector<float> m_scratch;
};

class PcmDecoder : public Decoder
{
public:
    PcmDecoder(int codec, int rate) : m_codec(codec), m_rate(rate) {}

    bool Decode(const unsigned char* in, int bytes, int frames,
                std::vector<float>& out) override
    {
        const int per = PcmBytesPerFrame(m_codec);
        if (per <= 0) return false;
        // The header's frame count and the payload length have to agree. They
        // come from the same sender, but not necessarily a trustworthy one.
        if (frames <= 0 || bytes < frames * per) return false;
        const size_t at = out.size();
        out.resize(at + (size_t)frames * kChannels);
        PcmToFloat(m_codec, in, frames, out.data() + at);
        return true;
    }

    bool Conceal(int frames, std::vector<float>& out) override
    {
        if (frames <= 0) return false;
        out.insert(out.end(), (size_t)frames * kChannels, 0.0f);
        return true;
    }

    int WireRate() const override { return m_rate; }

private:
    int m_codec, m_rate;
};

// --- Opus -------------------------------------------------------------------
//
// Opus is the reason the wire rate is a question. It works at 8, 12, 16, 24 or
// 48 kHz and nothing else, and it insists on being handed one of six exact
// frame lengths. So the session's audio is converted to 48 kHz on the way in
// and the packet says 48000, whatever the session is actually running at.

const int kOpusRate = 48000;

// 20 ms is the usual compromise: shorter costs proportionally more header and
// more CPU, longer costs latency and loses more audio per lost packet.
int OpusFramesFor(int packetMs)
{
    // The only lengths Opus will take, in milliseconds times two so that 2.5
    // can be in the list.
    const int halves[] = { 5, 10, 20, 40, 80, 120 };
    int want = packetMs * 2;
    int best = 40;                              // 20 ms
    int bestGap = 1 << 30;
    for (int h : halves)
    {
        const int gap = h > want ? h - want : want - h;
        if (gap < bestGap) { bestGap = gap; best = h; }
    }
    return kOpusRate * best / 2000;
}

class OpusNetEncoder : public Encoder
{
public:
    ~OpusNetEncoder() override
    {
        if (m_enc) opus_encoder_destroy(m_enc);
    }

    bool Open(int rate, int frames48, const OpusOptions& opt, std::wstring* err)
    {
        m_rate = rate;
        m_frames48 = frames48;

        int e = 0;
        // Audio rather than voice as the application: this carries whatever
        // the session is carrying, and the signal hint below is the narrower
        // way to tell it which that is.
        m_enc = opus_encoder_create(kOpusRate, kChannels, OPUS_APPLICATION_AUDIO, &e);
        if (!m_enc || e != OPUS_OK)
        {
            if (err) *err = FormatW(L"The Opus encoder could not be created (error %d).", e);
            return false;
        }
        opus_encoder_ctl(m_enc, OPUS_SET_BITRATE(
            opt.kbps > 0 ? opt.kbps * 1000 : 128000));

        // Forward error correction, and the loss figure that sizes it. Opus
        // puts a low rate copy of each frame inside the next packet, so a lost
        // one can be rebuilt from its successor rather than guessed at - but
        // only if the receiver asks for it, which is what DecodeFec below is
        // for. Zero turns the whole thing off and stops paying for it.
        opus_encoder_ctl(m_enc, OPUS_SET_INBAND_FEC(opt.lossPct > 0 ? 1 : 0));
        opus_encoder_ctl(m_enc, OPUS_SET_PACKET_LOSS_PERC(opt.lossPct));

        opus_encoder_ctl(m_enc, OPUS_SET_COMPLEXITY(opt.complexity));
        opus_encoder_ctl(m_enc, OPUS_SET_SIGNAL(
            opt.signal == NOS_MUSIC ? OPUS_SIGNAL_MUSIC :
            opt.signal == NOS_VOICE ? OPUS_SIGNAL_VOICE : OPUS_AUTO));
        // Constant bitrate makes every packet the same size, which is easier
        // to plan a congested link around even though it is worse per bit.
        opus_encoder_ctl(m_enc, OPUS_SET_VBR(opt.rateMode == NOR_CBR ? 0 : 1));
        opus_encoder_ctl(m_enc, OPUS_SET_VBR_CONSTRAINT(
            opt.rateMode == NOR_CONSTRAINED ? 1 : 0));
        // Discontinuous transmission sends almost nothing while the input is
        // silent. It saves a great deal on a mostly quiet microphone and is
        // wrong for anything the far end is measuring, since the silence
        // stops arriving rather than arriving as silence.
        opus_encoder_ctl(m_enc, OPUS_SET_DTX(opt.dtx ? 1 : 0));

        if (!m_conv.Prepare(rate, kOpusRate))
        {
            if (err) *err = L"The rate converter for Opus could not be created.";
            return false;
        }
        m_fifo.Reset(FifoFramesFor(kOpusRate, frames48));
        m_scratch.assign((size_t)frames48 * kChannels, 0.0f);
        m_converted.reserve(FifoFramesFor(kOpusRate, frames48) * kChannels);
        return true;
    }

    void Push(const float* in, int frames) override
    {
        m_converted.clear();
        m_conv.Run(in, (size_t)frames, m_converted);
        m_fifo.Push(m_converted.data(), m_converted.size() / kChannels);
    }

    int Pull(unsigned char* out, int maxBytes, int* frames) override
    {
        if (!m_enc) return -1;
        if (!m_fifo.Pop(m_scratch.data(), (size_t)m_frames48)) return 0;
        const int n = opus_encode_float(m_enc, m_scratch.data(), m_frames48,
                                        out, maxBytes);
        if (n < 0) return -1;
        if (frames) *frames = m_frames48;
        return n;
    }

    int WireRate() const override { return kOpusRate; }
    int WireFramesPerPacket() const override { return m_frames48; }

    int LatencyFrames() const override
    {
        // Opus's own lookahead, plus the rate converter in front of it, both
        // measured at the session rate rather than at 48 kHz.
        opus_int32 look = 0;
        if (m_enc) opus_encoder_ctl(m_enc, OPUS_GET_LOOKAHEAD(&look));
        const int opusAtSession = (int)((double)look * m_rate / kOpusRate);
        return opusAtSession + m_conv.LatencyFrames();
    }

private:
    OpusEncoder* m_enc = nullptr;
    int m_rate = kDefaultRate;
    int m_frames48 = 960;
    Converter m_conv;
    Fifo m_fifo;
    std::vector<float> m_scratch;
    std::vector<float> m_converted;
};

class OpusNetDecoder : public Decoder
{
public:
    ~OpusNetDecoder() override
    {
        if (m_dec) opus_decoder_destroy(m_dec);
    }

    bool Open(std::wstring* err)
    {
        int e = 0;
        m_dec = opus_decoder_create(kOpusRate, kChannels, &e);
        if (!m_dec || e != OPUS_OK)
        {
            if (err) *err = FormatW(L"The Opus decoder could not be created (error %d).", e);
            return false;
        }
        return true;
    }

    bool Decode(const unsigned char* in, int bytes, int frames,
                std::vector<float>& out) override
    {
        if (!m_dec || bytes <= 0) return false;
        // What the header claims is a hint. Opus knows how long its own packet
        // is, so the buffer is sized for the longest frame it can hold rather
        // than for whatever a stranger said.
        (void)frames;
        const int room = kOpusRate * 120 / 1000;         // 120 ms, the maximum
        const size_t at = out.size();
        out.resize(at + (size_t)room * kChannels);
        const int got = opus_decode_float(m_dec, in, bytes, out.data() + at, room, 0);
        if (got < 0)
        {
            out.resize(at);
            return false;
        }
        out.resize(at + (size_t)got * kChannels);
        m_lastFrames = got;
        return true;
    }

    bool DecodeFec(const unsigned char* in, int bytes, int frames,
                   std::vector<float>& out) override
    {
        if (!m_dec || !in || bytes <= 0 || frames <= 0) return false;

        // The packet that arrived carries a low rate copy of the one before
        // it, and decoding it with the FEC flag asks for that copy rather than
        // for its own audio. It is only there if the sender was asked to put
        // it there, and only for one packet back, so this fails often and
        // harmlessly - Conceal picks up whatever it cannot do.
        const size_t at = out.size();
        out.resize(at + (size_t)frames * kChannels);
        const int got = opus_decode_float(m_dec, in, bytes, out.data() + at,
                                          frames, 1);
        if (got != frames)
        {
            out.resize(at);
            return false;
        }
        return true;
    }

    bool Conceal(int frames, std::vector<float>& out) override
    {
        if (!m_dec) return false;
        // Opus can make a fair guess at a packet it never got from the shape
        // of the ones before it, which is the one real advantage a lossy codec
        // has over a lossless one on a lossy link.
        int want = frames > 0 ? frames : m_lastFrames;
        if (want <= 0) want = kOpusRate / 50;
        const size_t at = out.size();
        out.resize(at + (size_t)want * kChannels);
        const int got = opus_decode_float(m_dec, nullptr, 0, out.data() + at, want, 0);
        if (got < 0)
        {
            // Not even a guess: silence, which at least has the right length.
            for (size_t i = at; i < out.size(); ++i) out[i] = 0.0f;
            return true;
        }
        out.resize(at + (size_t)got * kChannels);
        return true;
    }

    int WireRate() const override { return kOpusRate; }

private:
    OpusDecoder* m_dec = nullptr;
    int m_lastFrames = 0;
};

// --- FLAC -------------------------------------------------------------------
//
// Lossless and about a third smaller than the PCM it came from, which on a
// busy wired network is the difference between fitting and not. Sixteen bit,
// deliberately: FLAC is integer only, and at 16 bits a frame is small enough
// that one fits in one datagram with room to spare even in the worst case
// where the audio does not compress at all. Anybody who wants more depth than
// that has 24 bit PCM a line above it in the list.
//
// The awkward part of putting FLAC in packets is that a FLAC stream begins
// with a header describing it, and a receiver that joined late or lost a
// packet has not got it. What saves it is that FLAC frames are independent of
// each other - there is no prediction across a frame boundary - so one frame
// per packet is a thing that can be decoded on its own. The sender therefore
// throws its stream header away and sends only frames, and the receiver builds
// the identical header locally from what the packet header already tells it
// and puts it in front of each frame before decoding. Nothing describing the
// stream goes over the wire twice, and every packet stands alone.

const int kFlacBps = 16;

// A frame of this many samples cannot exceed a datagram even uncompressed:
// 16 bit stereo is four bytes a frame, and the slack covers the frame header
// and the subframe headers in the pathological case where FLAC gives up and
// stores the block verbatim.
int FlacFramesCap()
{
    return (kMaxPayloadBytes - 64) / (2 * kChannels);
}

class FlacEncoder : public Encoder
{
public:
    ~FlacEncoder() override
    {
        if (m_enc)
        {
            FLAC__stream_encoder_finish(m_enc);
            FLAC__stream_encoder_delete(m_enc);
        }
    }

    bool Open(int rate, int frames, std::wstring* err)
    {
        m_rate = rate;
        m_frames = frames;

        m_enc = FLAC__stream_encoder_new();
        if (!m_enc)
        {
            if (err) *err = L"The FLAC encoder could not be created.";
            return false;
        }
        FLAC__stream_encoder_set_channels(m_enc, kChannels);
        FLAC__stream_encoder_set_bits_per_sample(m_enc, kFlacBps);
        FLAC__stream_encoder_set_sample_rate(m_enc, (unsigned)rate);
        FLAC__stream_encoder_set_blocksize(m_enc, (unsigned)frames);
        // The subset is what makes a frame decodable without the stream it
        // came from, which is the whole basis of sending one per packet.
        FLAC__stream_encoder_set_streamable_subset(m_enc, true);
        // Level 5 is FLAC's own default. Higher levels cost real time here for
        // a few percent of size, and this runs on every packet.
        FLAC__stream_encoder_set_compression_level(m_enc, 5);
        // Verification decodes everything a second time to compare it. Useful
        // when writing an archive, pointless and expensive on a live link.
        FLAC__stream_encoder_set_verify(m_enc, false);

        if (FLAC__stream_encoder_init_stream(m_enc, WriteCb, nullptr, nullptr,
                                             nullptr, this) !=
            FLAC__STREAM_ENCODER_INIT_STATUS_OK)
        {
            if (err) *err = FormatW(L"The FLAC encoder would not start (%s).",
                                    L"init_stream failed");
            return false;
        }

        m_fifo.Reset(FifoFramesFor(rate, frames));
        m_block.assign((size_t)frames * kChannels, 0.0f);
        m_ints.assign((size_t)frames * kChannels, 0);
        return true;
    }

    void Push(const float* in, int frames) override
    {
        m_fifo.Push(in, (size_t)frames);
    }

    int Pull(unsigned char* out, int maxBytes, int* frames) override
    {
        // Anything already encoded and waiting goes first.
        if (!m_ready.empty())
            return Take(out, maxBytes, frames);

        if (!m_fifo.Pop(m_block.data(), (size_t)m_frames)) return 0;

        const int n = m_frames * kChannels;
        for (int i = 0; i < n; ++i)
            m_ints[(size_t)i] = (FLAC__int32)lrintf(Clamp1(m_block[(size_t)i]) *
                                                    32767.0f);
        if (!FLAC__stream_encoder_process_interleaved(m_enc, m_ints.data(),
                                                      (unsigned)m_frames))
            return -1;
        if (m_ready.empty()) return 0;
        return Take(out, maxBytes, frames);
    }

    int WireRate() const override { return m_rate; }
    int WireFramesPerPacket() const override { return m_frames; }

private:
    struct Packet { std::vector<unsigned char> bytes; int frames; };

    int Take(unsigned char* out, int maxBytes, int* frames)
    {
        Packet p = std::move(m_ready.front());
        m_ready.erase(m_ready.begin());
        if ((int)p.bytes.size() > maxBytes) return -1;
        memcpy(out, p.bytes.data(), p.bytes.size());
        if (frames) *frames = p.frames;
        return (int)p.bytes.size();
    }

    static FLAC__StreamEncoderWriteStatus WriteCb(
        const FLAC__StreamEncoder*, const FLAC__byte buffer[], size_t bytes,
        unsigned samples, unsigned, void* client)
    {
        FlacEncoder* self = static_cast<FlacEncoder*>(client);
        // Zero samples means this is the stream header rather than audio. It
        // is thrown away: the receiver builds the same thing for itself, so
        // sending it would be describing the stream twice.
        if (samples == 0) return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;

        Packet p;
        p.bytes.assign(buffer, buffer + bytes);
        p.frames = (int)samples;
        self->m_ready.push_back(std::move(p));
        return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
    }

    FLAC__StreamEncoder* m_enc = nullptr;
    int m_rate = kDefaultRate;
    int m_frames = 0;
    Fifo m_fifo;
    std::vector<float> m_block;
    std::vector<FLAC__int32> m_ints;
    std::vector<Packet> m_ready;
};

// The 42 bytes a FLAC stream starts with: the magic, one metadata block
// header, and a STREAMINFO. Everything in it that this cannot know is written
// as the "unknown" value the format defines for it - frame sizes, total
// length and the MD5 are all legitimately zero.
void BuildFlacHeader(int rate, int channels, int bps, int blocksize,
                     std::vector<unsigned char>& out)
{
    out.assign(42, 0);
    unsigned char* p = out.data();
    p[0] = 'f'; p[1] = 'L'; p[2] = 'a'; p[3] = 'C';
    p[4] = 0x80;                     // last metadata block, type 0 (STREAMINFO)
    p[5] = 0; p[6] = 0; p[7] = 34;   // its length

    unsigned char* s = p + 8;
    s[0] = (unsigned char)((blocksize >> 8) & 0xFF);   // min blocksize
    s[1] = (unsigned char)(blocksize & 0xFF);
    s[2] = (unsigned char)((blocksize >> 8) & 0xFF);   // max blocksize
    s[3] = (unsigned char)(blocksize & 0xFF);
    // Bytes 4 to 9 are the min and max frame size, zero for unknown.
    // Then 20 bits of rate, 3 of channels less one, 5 of depth less one and
    // 36 of total samples, packed together across bytes 10 to 17.
    const unsigned r = (unsigned)rate;
    s[10] = (unsigned char)((r >> 12) & 0xFF);
    s[11] = (unsigned char)((r >> 4) & 0xFF);
    s[12] = (unsigned char)(((r & 0x0F) << 4) |
                            (((unsigned)(channels - 1) & 0x07) << 1) |
                            ((((unsigned)bps - 1) >> 4) & 0x01));
    s[13] = (unsigned char)((((unsigned)bps - 1) & 0x0F) << 4);
    // The remaining 36 bits of total samples stay zero, which means unknown,
    // and so does the MD5 in bytes 18 to 33.
}

class FlacDecoderImpl : public Decoder
{
public:
    ~FlacDecoderImpl() override
    {
        if (m_dec)
        {
            FLAC__stream_decoder_finish(m_dec);
            FLAC__stream_decoder_delete(m_dec);
        }
    }

    bool Open(int rate, std::wstring* err)
    {
        m_rate = rate;
        m_dec = FLAC__stream_decoder_new();
        if (!m_dec)
        {
            if (err) *err = L"The FLAC decoder could not be created.";
            return false;
        }
        // Nothing here has an MD5 to check against: the header this builds
        // says the stream's checksum is unknown, which it is.
        FLAC__stream_decoder_set_md5_checking(m_dec, false);
        if (FLAC__stream_decoder_init_stream(m_dec, ReadCb, nullptr, nullptr,
                                             nullptr, nullptr, WriteCb,
                                             nullptr, ErrorCb, this) !=
            FLAC__STREAM_DECODER_INIT_STATUS_OK)
        {
            if (err) *err = L"The FLAC decoder would not start.";
            return false;
        }
        return true;
    }

    bool Decode(const unsigned char* in, int bytes, int frames,
                std::vector<float>& out) override
    {
        if (!m_dec || bytes <= 0 || frames <= 0) return false;

        // One packet, one frame, one little stream of its own: the header this
        // builds, then the frame, and then the end. Reading it that way means
        // the decoder never has to look ahead into a packet that has not
        // arrived, which is what makes a lost packet survivable.
        if (frames != m_blocksize)
        {
            m_blocksize = frames;
            BuildFlacHeader(m_rate, kChannels, kFlacBps, frames, m_header);
        }
        m_in.assign(m_header.begin(), m_header.end());
        m_in.insert(m_in.end(), in, in + bytes);
        m_at = 0;
        m_out = &out;
        m_failed = false;
        const size_t before = out.size();

        FLAC__stream_decoder_reset(m_dec);
        FLAC__stream_decoder_process_until_end_of_stream(m_dec);
        m_out = nullptr;

        return !m_failed && out.size() > before;
    }

    bool Conceal(int frames, std::vector<float>& out) override
    {
        if (frames <= 0) return false;
        // Nothing to guess with: FLAC frames carry no history, so a frame that
        // never arrived leaves a hole of exactly the right length.
        out.insert(out.end(), (size_t)frames * kChannels, 0.0f);
        return true;
    }

    int WireRate() const override { return m_rate; }

private:
    static FLAC__StreamDecoderReadStatus ReadCb(const FLAC__StreamDecoder*,
                                                FLAC__byte buffer[],
                                                size_t* bytes, void* client)
    {
        FlacDecoderImpl* self = static_cast<FlacDecoderImpl*>(client);
        const size_t left = self->m_in.size() - self->m_at;
        if (left == 0)
        {
            *bytes = 0;
            return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
        }
        const size_t take = (*bytes < left) ? *bytes : left;
        memcpy(buffer, self->m_in.data() + self->m_at, take);
        self->m_at += take;
        *bytes = take;
        return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    }

    static FLAC__StreamDecoderWriteStatus WriteCb(const FLAC__StreamDecoder*,
                                                  const FLAC__Frame* frame,
                                                  const FLAC__int32* const buf[],
                                                  void* client)
    {
        FlacDecoderImpl* self = static_cast<FlacDecoderImpl*>(client);
        if (!self->m_out) return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;

        const unsigned n = frame->header.blocksize;
        const unsigned chans = frame->header.channels;
        const size_t at = self->m_out->size();
        self->m_out->resize(at + (size_t)n * kChannels);
        float* dst = self->m_out->data() + at;

        for (unsigned i = 0; i < n; ++i)
            for (int c = 0; c < kChannels; ++c)
            {
                const unsigned src = (chans == 1) ? 0
                                   : ((unsigned)c < chans ? (unsigned)c : chans - 1);
                dst[(size_t)i * kChannels + c] = (float)buf[src][i] / 32767.0f;
            }
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }

    static void ErrorCb(const FLAC__StreamDecoder*,
                        FLAC__StreamDecoderErrorStatus, void* client)
    {
        static_cast<FlacDecoderImpl*>(client)->m_failed = true;
    }

    FLAC__StreamDecoder* m_dec = nullptr;
    int m_rate = kDefaultRate;
    int m_blocksize = 0;
    std::vector<unsigned char> m_header;
    std::vector<unsigned char> m_in;
    size_t m_at = 0;
    std::vector<float>* m_out = nullptr;
    bool m_failed = false;
};

}  // namespace

// ---------------------------------------------------------------------------
// PCM, in the open
// ---------------------------------------------------------------------------

int PcmBytesPerFrame(int codec)
{
    switch (codec)
    {
    case NC_PCM16:     return 2 * kChannels;
    case NC_PCM24:     return 3 * kChannels;
    case NC_PCM_FLOAT: return 4 * kChannels;
    default:           return 0;
    }
}

void PcmFromFloat(int codec, const float* in, int frames, unsigned char* out)
{
    const int n = frames * kChannels;
    unsigned char* p = out;
    switch (codec)
    {
    case NC_PCM16:
        for (int i = 0; i < n; ++i)
            Put16(p, (int)lrintf(Clamp1(in[i]) * 32767.0f));
        break;
    case NC_PCM24:
        for (int i = 0; i < n; ++i)
            Put24(p, (int)lrintf(Clamp1(in[i]) * 8388607.0f));
        break;
    case NC_PCM_FLOAT:
        // Not clamped: the point of sending float is that it is the bus
        // exactly as it stands, headroom above one included, and the far end
        // can decide what to do about that.
        memcpy(out, in, (size_t)n * sizeof(float));
        break;
    default:
        break;
    }
}

void PcmToFloat(int codec, const unsigned char* in, int frames, float* out)
{
    const int n = frames * kChannels;
    const unsigned char* p = in;
    switch (codec)
    {
    case NC_PCM16:
        for (int i = 0; i < n; ++i) out[i] = (float)Get16(p) / 32767.0f;
        break;
    case NC_PCM24:
        for (int i = 0; i < n; ++i) out[i] = (float)Get24(p) / 8388607.0f;
        break;
    case NC_PCM_FLOAT:
        memcpy(out, in, (size_t)n * sizeof(float));
        break;
    default:
        for (int i = 0; i < n; ++i) out[i] = 0.0f;
        break;
    }
}

int VbanBytesPerSample(int datatype)
{
    switch (datatype)
    {
    case vban::DATATYPE_BYTE8:   return 1;
    case vban::DATATYPE_INT16:   return 2;
    case vban::DATATYPE_INT24:   return 3;
    case vban::DATATYPE_INT32:   return 4;
    case vban::DATATYPE_FLOAT32: return 4;
    case vban::DATATYPE_FLOAT64: return 8;
    default:                     return 0;     // 12 and 10 bit are not handled
    }
}

bool VbanToFloat(int datatype, const unsigned char* in, int frames, int channels,
                 std::vector<float>& out)
{
    const int per = VbanBytesPerSample(datatype);
    if (per <= 0 || frames <= 0 || channels <= 0) return false;

    const size_t at = out.size();
    out.resize(at + (size_t)frames * kChannels);
    float* dst = out.data() + at;

    for (int f = 0; f < frames; ++f)
    {
        // A VBAN stream can be any number of channels. Mono is sent to both
        // sides, anything wider is cut down to the first two: a stereo bus has
        // nowhere to put channel seven, and dropping it is at least obvious.
        for (int c = 0; c < kChannels; ++c)
        {
            const int src = (channels == 1) ? 0 : (c < channels ? c : channels - 1);
            const unsigned char* s = in + ((size_t)f * channels + src) * per;
            float v = 0.0f;
            switch (datatype)
            {
            case vban::DATATYPE_BYTE8:
                v = ((float)s[0] - 128.0f) / 127.0f;
                break;
            case vban::DATATYPE_INT16:
            {
                const short q = (short)(s[0] | (s[1] << 8));
                v = (float)q / 32767.0f;
                break;
            }
            case vban::DATATYPE_INT24:
            {
                int q = s[0] | (s[1] << 8) | (s[2] << 16);
                if (q & 0x800000) q -= 0x1000000;
                v = (float)q / 8388607.0f;
                break;
            }
            case vban::DATATYPE_INT32:
            {
                int q = 0;
                memcpy(&q, s, 4);
                v = (float)((double)q / 2147483647.0);
                break;
            }
            case vban::DATATYPE_FLOAT32:
                memcpy(&v, s, 4);
                break;
            case vban::DATATYPE_FLOAT64:
            {
                double d = 0.0;
                memcpy(&d, s, 8);
                v = (float)d;
                break;
            }
            default:
                break;
            }
            dst[(size_t)f * kChannels + c] = v;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Packet sizing
// ---------------------------------------------------------------------------

int FramesPerPacketFor(int codec, int rate, int packetMs)
{
    if (rate <= 0) rate = kDefaultRate;
    if (packetMs < 1) packetMs = 1;

    if (codec == NC_OPUS) return OpusFramesFor(packetMs);

    const int per = PcmBytesPerFrame(codec);
    int frames = rate * packetMs / 1000;
    if (frames < 1) frames = 1;
    if (per > 0)
    {
        // One datagram, always. A payload that needed fragmenting would be one
        // the network could lose in halves, and half a packet of PCM is not
        // half as good - it is a gap in the middle of the audio.
        const int cap = kMaxPayloadBytes / per;
        if (frames > cap) frames = cap;
    }
    else if (codec == NC_FLAC)
    {
        // Sized for the worst case rather than the usual one. FLAC normally
        // gets 16 bit stereo down to about two thirds, but a block it cannot
        // predict is stored verbatim at the full four bytes a frame, and a
        // packet that only fits when the audio happens to be compressible is
        // a packet that fails on applause.
        const int cap = FlacFramesCap();
        if (frames > cap) frames = cap;
    }
    return frames;
}

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------

std::unique_ptr<Encoder> MakeEncoder(int codec, int rate, int packetMs,
                                     const OpusOptions& opus, std::wstring* err)
{
    if (rate <= 0) rate = kDefaultRate;

    if (codec == NC_PCM16 || codec == NC_PCM24 || codec == NC_PCM_FLOAT)
    {
        const int frames = FramesPerPacketFor(codec, rate, packetMs);
        return std::unique_ptr<Encoder>(new PcmEncoder(codec, rate, frames));
    }
    if (codec == NC_OPUS)
    {
        std::unique_ptr<OpusNetEncoder> e(new OpusNetEncoder());
        if (!e->Open(rate, OpusFramesFor(packetMs), opus, err)) return nullptr;
        return std::unique_ptr<Encoder>(e.release());
    }
    if (codec == NC_FLAC)
    {
        std::unique_ptr<FlacEncoder> e(new FlacEncoder());
        if (!e->Open(rate, FramesPerPacketFor(NC_FLAC, rate, packetMs), err))
            return nullptr;
        return std::unique_ptr<Encoder>(e.release());
    }
    if (err) *err = FormatW(L"%s cannot be sent.", NetCodecName(codec));
    return nullptr;
}

std::unique_ptr<Decoder> MakeDecoder(int codec, int rate, std::wstring* err)
{
    if (rate <= 0) rate = kDefaultRate;

    if (codec == NC_PCM16 || codec == NC_PCM24 || codec == NC_PCM_FLOAT)
        return std::unique_ptr<Decoder>(new PcmDecoder(codec, rate));
    if (codec == NC_OPUS)
    {
        std::unique_ptr<OpusNetDecoder> d(new OpusNetDecoder());
        if (!d->Open(err)) return nullptr;
        return std::unique_ptr<Decoder>(d.release());
    }
    if (codec == NC_FLAC)
    {
        std::unique_ptr<FlacDecoderImpl> d(new FlacDecoderImpl());
        if (!d->Open(rate, err)) return nullptr;
        return std::unique_ptr<Decoder>(d.release());
    }
    if (err) *err = FormatW(L"%s cannot be received.", NetCodecName(codec));
    return nullptr;
}

}  // namespace net
