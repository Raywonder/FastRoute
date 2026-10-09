#include "audio.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <vector>

extern "C" {
#include <ogg/ogg.h>
#include <vorbis/vorbisenc.h>
#include <opus.h>
#include <speex/speex_resampler.h>
}

// ---------------------------------------------------------------------------
// Ogg Vorbis and Opus recording
//
// Windows has no encoder for either, the same gap the stream block runs into
// from the other direction. The libraries are in the build for that, so they
// serve here too and a recording can be made in the same formats a stream can
// be played in.
//
// Both write an Ogg stream by hand: libogg does the framing, and what goes
// into the pages is the codec's business.
// ---------------------------------------------------------------------------

namespace {

// A serial number identifies a logical stream inside an Ogg file. There is
// only ever one here, so anything unique to this file will do.
static int NextSerial()
{
    static long counter = 0;
    return (int)(GetTickCount() ^ (DWORD)(++counter * 2654435761u));
}

class OggWriterBase : public Recorder
{
public:
    void Write(const float* buf, size_t frames) override
    {
        if (!m_running) return;
        {
            std::lock_guard<std::mutex> g(m_lock);
            m_queue.insert(m_queue.end(), buf, buf + frames * kChannels);
        }
        m_wake.notify_one();
    }

    void Stop() override
    {
        if (!m_running) return;
        m_running = false;
        m_wake.notify_all();
        if (m_thread.joinable()) m_thread.join();
        Finish();
        if (m_f) { fclose(m_f); m_f = nullptr; }
    }

    std::wstring Path() const override { return m_path; }

protected:
    bool OpenFile(const std::wstring& path, std::wstring* err)
    {
        m_path = path;
        if (_wfopen_s(&m_f, path.c_str(), L"wb") != 0 || !m_f)
        {
            if (err) *err = L"Could not create " + path;
            return false;
        }
        return true;
    }

    void RunWriter()
    {
        m_running = true;
        m_thread = std::thread([this] {
            std::vector<float> chunk;
            for (;;)
            {
                {
                    std::unique_lock<std::mutex> g(m_lock);
                    m_wake.wait(g, [this] { return !m_queue.empty() || !m_running; });
                    if (m_queue.empty() && !m_running) break;
                    chunk.assign(m_queue.begin(), m_queue.end());
                    m_queue.clear();
                }
                if (!chunk.empty()) Encode(chunk.data(), chunk.size() / kChannels);
            }
        });
    }

    void WritePage(const ogg_page& og)
    {
        if (!m_f) return;
        fwrite(og.header, 1, (size_t)og.header_len, m_f);
        fwrite(og.body, 1, (size_t)og.body_len, m_f);
    }

    virtual void Encode(const float* buf, size_t frames) = 0;
    virtual void Finish() = 0;

    std::wstring m_path;
    FILE* m_f = nullptr;
    int m_rate = 48000;

private:
    std::deque<float> m_queue;
    std::mutex m_lock;
    std::condition_variable m_wake;
    std::thread m_thread;
    std::atomic<bool> m_running{ false };
};

// ---------------------------------------------------------------------------
// Vorbis
// ---------------------------------------------------------------------------
class VorbisRecorder : public OggWriterBase
{
public:
    ~VorbisRecorder() override { Stop(); Release(); }

    bool Start(const std::wstring& path, int rate, int kbps, std::wstring* err) override
    {
        if (!OpenFile(path, err)) return false;
        m_rate = rate;

        vorbis_info_init(&m_vi);
        // Managed to a nominal bitrate rather than a quality setting, so the
        // number in the dialog means the same thing it does for MP3 and Opus.
        int rc = vorbis_encode_init(&m_vi, kChannels, rate, -1, kbps * 1000, -1);
        if (rc != 0)
        {
            if (err) *err = FormatW(
                L"The Vorbis encoder would not take %d kbps at %d Hz stereo (%d). "
                L"Try a different bitrate.", kbps, rate, rc);
            return false;
        }
        m_viUp = true;

        vorbis_comment_init(&m_vc);
        vorbis_comment_add_tag(&m_vc, "ENCODER", "FastRoute");
        vorbis_analysis_init(&m_vd, &m_vi);
        vorbis_block_init(&m_vd, &m_vb);
        m_dspUp = true;

        if (ogg_stream_init(&m_os, NextSerial()) != 0)
        {
            if (err) *err = L"The Ogg stream could not be started.";
            return false;
        }
        m_osUp = true;

        ogg_packet head, tags, code;
        vorbis_analysis_headerout(&m_vd, &m_vc, &head, &tags, &code);
        ogg_stream_packetin(&m_os, &head);
        ogg_stream_packetin(&m_os, &tags);
        ogg_stream_packetin(&m_os, &code);

        // The headers are flushed onto pages of their own: a decoder is
        // entitled to expect the identification header alone on the first.
        ogg_page og;
        while (ogg_stream_flush(&m_os, &og)) WritePage(og);

        RunWriter();
        return true;
    }

protected:
    void Encode(const float* buf, size_t frames) override
    {
        if (!m_dspUp || frames == 0) return;

        float** dst = vorbis_analysis_buffer(&m_vd, (int)frames);
        for (size_t i = 0; i < frames; ++i)
            for (int c = 0; c < kChannels; ++c)
                dst[c][i] = buf[i * kChannels + c];
        vorbis_analysis_wrote(&m_vd, (int)frames);
        Drain();
    }

    void Finish() override
    {
        if (m_dspUp)
        {
            // A zero length write is how Vorbis is told the stream has ended,
            // which is what marks the last page as the last.
            vorbis_analysis_wrote(&m_vd, 0);
            Drain();
        }
        Release();
    }

private:
    void Drain()
    {
        ogg_packet op;
        ogg_page og;
        while (vorbis_analysis_blockout(&m_vd, &m_vb) == 1)
        {
            vorbis_analysis(&m_vb, nullptr);
            vorbis_bitrate_addblock(&m_vb);
            while (vorbis_bitrate_flushpacket(&m_vd, &op))
            {
                ogg_stream_packetin(&m_os, &op);
                while (ogg_stream_pageout(&m_os, &og)) WritePage(og);
            }
        }
    }

    void Release()
    {
        if (m_dspUp) { vorbis_block_clear(&m_vb); vorbis_dsp_clear(&m_vd); m_dspUp = false; }
        if (m_osUp)  { ogg_stream_clear(&m_os); m_osUp = false; }
        if (m_viUp)  { vorbis_comment_clear(&m_vc); vorbis_info_clear(&m_vi); m_viUp = false; }
    }

    vorbis_info m_vi{};
    vorbis_comment m_vc{};
    vorbis_dsp_state m_vd{};
    vorbis_block m_vb{};
    ogg_stream_state m_os{};
    bool m_viUp = false, m_dspUp = false, m_osUp = false;
};

// ---------------------------------------------------------------------------
// Opus
//
// Opus only encodes at 48, 24, 16, 12 or 8 kHz, so a session at 44.1 is
// resampled on the way in. Its frames are a fixed number of samples, so the
// incoming audio is gathered into 20 ms blocks before it goes anywhere.
// ---------------------------------------------------------------------------
class OpusRecorder : public OggWriterBase
{
public:
    ~OpusRecorder() override { Stop(); Release(); }

    bool Start(const std::wstring& path, int rate, int kbps, std::wstring* err) override
    {
        if (!OpenFile(path, err)) return false;
        m_rate = rate;

        if (rate != kOpusRate)
        {
            int rc = 0;
            m_res = speex_resampler_init((spx_uint32_t)kChannels, (spx_uint32_t)rate,
                                         (spx_uint32_t)kOpusRate, 5, &rc);
            if (!m_res)
            {
                if (err) *err = FormatW(L"%d Hz could not be converted to the "
                                        L"48000 Hz Opus works at.", rate);
                return false;
            }
        }

        int rc = 0;
        m_enc = opus_encoder_create(kOpusRate, kChannels, OPUS_APPLICATION_AUDIO, &rc);
        if (!m_enc || rc != OPUS_OK)
        {
            if (err) *err = FormatW(L"The Opus encoder could not be started (%d).", rc);
            return false;
        }
        opus_encoder_ctl(m_enc, OPUS_SET_BITRATE(kbps * 1000));

        // The encoder's own lookahead is what a decoder has to throw away at
        // the start, and it goes in the header so the decoder knows.
        opus_int32 lookahead = 0;
        opus_encoder_ctl(m_enc, OPUS_GET_LOOKAHEAD(&lookahead));
        m_preSkip = (int)lookahead;

        if (ogg_stream_init(&m_os, NextSerial()) != 0)
        {
            if (err) *err = L"The Ogg stream could not be started.";
            return false;
        }
        m_osUp = true;
        WriteHeaders();

        m_block.reserve((size_t)kFrameSamples * kChannels);
        RunWriter();
        return true;
    }

protected:
    void Encode(const float* buf, size_t frames) override
    {
        if (!m_enc || frames == 0) return;

        const float* src = buf;
        size_t n = frames;
        if (m_res)
        {
            size_t room = (size_t)((double)frames * kOpusRate / m_rate) + 64;
            m_resampled.resize(room * kChannels);
            spx_uint32_t inLen = (spx_uint32_t)frames;
            spx_uint32_t outLen = (spx_uint32_t)room;
            speex_resampler_process_interleaved_float(m_res, buf, &inLen,
                                                      m_resampled.data(), &outLen);
            src = m_resampled.data();
            n = outLen;
        }

        m_block.insert(m_block.end(), src, src + n * kChannels);
        while (m_block.size() >= (size_t)kFrameSamples * kChannels)
        {
            EncodeOneFrame(m_block.data());
            m_block.erase(m_block.begin(),
                          m_block.begin() + (ptrdiff_t)kFrameSamples * kChannels);
        }
    }

    void Finish() override
    {
        if (m_enc && !m_block.empty())
        {
            // The tail is padded out to a whole frame; the granule position
            // still says how much of it is real, so a decoder drops the rest.
            size_t real = m_block.size() / kChannels;
            m_block.resize((size_t)kFrameSamples * kChannels, 0.0f);
            EncodeOneFrame(m_block.data(), (int)real);
            m_block.clear();
        }
        if (m_osUp)
        {
            ogg_page og;
            while (ogg_stream_flush(&m_os, &og)) WritePage(og);
        }
        Release();
    }

private:
    static const int kOpusRate = 48000;
    static const int kFrameSamples = 960;      // 20 ms at 48 kHz

    void Put(std::vector<unsigned char>& v, const char* s, size_t n)
    {
        v.insert(v.end(), s, s + n);
    }
    void Put16(std::vector<unsigned char>& v, unsigned x)
    {
        v.push_back((unsigned char)(x & 0xFF));
        v.push_back((unsigned char)((x >> 8) & 0xFF));
    }
    void Put32(std::vector<unsigned char>& v, unsigned x)
    {
        for (int i = 0; i < 4; ++i) v.push_back((unsigned char)((x >> (8 * i)) & 0xFF));
    }

    void WriteHeaders()
    {
        std::vector<unsigned char> head;
        Put(head, "OpusHead", 8);
        head.push_back(1);                       // version
        head.push_back((unsigned char)kChannels);
        Put16(head, (unsigned)m_preSkip);
        Put32(head, (unsigned)m_rate);           // the original rate, for information
        Put16(head, 0);                          // output gain
        head.push_back(0);                       // channel mapping family 0

        std::vector<unsigned char> tags;
        Put(tags, "OpusTags", 8);
        const char* vendor = "FastRoute";
        Put32(tags, (unsigned)strlen(vendor));
        Put(tags, vendor, strlen(vendor));
        Put32(tags, 0);                          // no comments

        ogg_packet op{};
        op.packet = head.data();
        op.bytes = (long)head.size();
        op.b_o_s = 1;
        op.granulepos = 0;
        op.packetno = m_packetNo++;
        ogg_stream_packetin(&m_os, &op);

        ogg_page og;
        while (ogg_stream_flush(&m_os, &og)) WritePage(og);

        ogg_packet t{};
        t.packet = tags.data();
        t.bytes = (long)tags.size();
        t.granulepos = 0;
        t.packetno = m_packetNo++;
        ogg_stream_packetin(&m_os, &t);
        while (ogg_stream_flush(&m_os, &og)) WritePage(og);
    }

    // `real` is how many of the frame's samples are actual audio, which is all
    // of them except in the last frame of the file.
    void EncodeOneFrame(const float* pcm, int real = kFrameSamples)
    {
        m_packet.resize(4000);
        int bytes = opus_encode_float(m_enc, pcm, kFrameSamples,
                                      m_packet.data(), (opus_int32)m_packet.size());
        if (bytes < 0) return;

        m_granule += real;

        ogg_packet op{};
        op.packet = m_packet.data();
        op.bytes = bytes;
        op.granulepos = m_granule + m_preSkip;
        op.packetno = m_packetNo++;
        op.e_o_s = 0;
        ogg_stream_packetin(&m_os, &op);

        ogg_page og;
        while (ogg_stream_pageout(&m_os, &og)) WritePage(og);
    }

    void Release()
    {
        if (m_enc) { opus_encoder_destroy(m_enc); m_enc = nullptr; }
        if (m_res) { speex_resampler_destroy(m_res); m_res = nullptr; }
        if (m_osUp) { ogg_stream_clear(&m_os); m_osUp = false; }
    }

    ::OpusEncoder* m_enc = nullptr;
    SpeexResamplerState* m_res = nullptr;
    ogg_stream_state m_os{};
    bool m_osUp = false;
    int m_preSkip = 0;
    long long m_granule = 0;
    long long m_packetNo = 0;
    std::vector<float> m_block, m_resampled;
    std::vector<unsigned char> m_packet;
};

}   // namespace

std::unique_ptr<Recorder> MakeVorbisRecorder()
{
    return std::unique_ptr<Recorder>(new VorbisRecorder());
}

std::unique_ptr<Recorder> MakeOpusRecorder()
{
    return std::unique_ptr<Recorder>(new OpusRecorder());
}
