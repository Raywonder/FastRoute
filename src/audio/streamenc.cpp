#include "audio.h"
#include "aacenc.h"
#include "resample.h"
#include "streamenc.h"

#include <mutex>

void AdtsHeader(uint8_t h[7], size_t len, int sampleRate, int channels)
{
    static const int rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                 22050, 16000, 12000, 11025, 8000, 7350 };
    int idx = 3;
    for (int i = 0; i < 13; ++i) if (rates[i] == sampleRate) { idx = i; break; }
    const size_t full = len + 7;
    const int profile = 1;   // AAC-LC, as object type minus one
    h[0] = 0xFF;
    h[1] = 0xF1;             // MPEG-4, layer 0, no CRC
    h[2] = (uint8_t)((profile << 6) | (idx << 2) | ((channels >> 2) & 1));
    h[3] = (uint8_t)(((channels & 3) << 6) | ((full >> 11) & 3));
    h[4] = (uint8_t)((full >> 3) & 0xFF);
    h[5] = (uint8_t)(((full & 7) << 5) | 0x1F);
    h[6] = 0xFC;
}

namespace {

const int kOutRate = 48000;

// AAC (as ADTS) or MP3 from Windows' own encoders, at 48 kHz.
class MfStreamEncoder : public StreamEncoder
{
public:
    explicit MfStreamEncoder(bool mp3) : m_mp3(mp3) {}
    ~MfStreamEncoder() override { Stop(); }

    bool Start(int rate, int kbps, const std::string&, std::wstring* err) override
    {
        if (!m_conv.Prepare(rate, kOutRate))
        {
            if (err) *err = L"The session's sample rate could not be converted to 48 kHz.";
            return false;
        }
        return m_mp3 ? m_enc.StartMp3(kOutRate, kbps, err)
                     : m_enc.Start(kOutRate, kbps, err);
    }

    bool Encode(const float* buf, size_t frames, std::wstring* err) override
    {
        m_conv.Run(buf, frames, m_at48);
        // MP3 and AAC both buffer inside the encoder, so anything goes in;
        // a few hundred frames at a time keeps the latency down.
        const size_t n = m_at48.size() / kChannels;
        if (n == 0) return true;
        m_pcm.resize(n * kChannels);
        for (size_t i = 0; i < n * kChannels; ++i)
        {
            float v = m_at48[i];
            if (v > 1.0f) v = 1.0f;
            else if (v < -1.0f) v = -1.0f;
            m_pcm[i] = (int16_t)(v * 32767.0f);
        }
        m_at48.clear();
        m_frames.clear();
        if (!m_enc.Encode(m_pcm.data(), n, &m_frames, err)) return false;
        for (const auto& f : m_frames)
        {
            if (!m_mp3)
            {
                uint8_t h[7];
                AdtsHeader(h, f.size(), kOutRate, kChannels);
                m_out.insert(m_out.end(), h, h + 7);
            }
            m_out.insert(m_out.end(), f.begin(), f.end());
        }
        return true;
    }

    void Take(std::vector<uint8_t>* out) override
    {
        out->insert(out->end(), m_out.begin(), m_out.end());
        m_out.clear();
    }

    void Stop() override { m_enc.Stop(); }
    const char* ContentType() const override { return m_mp3 ? "audio/mpeg" : "audio/aac"; }
    int Kbps() const override { return m_enc.Kbps(); }

private:
    bool m_mp3;
    AacEncoder m_enc;
    Converter m_conv;
    std::vector<float> m_at48;
    std::vector<int16_t> m_pcm;
    std::vector<std::vector<uint8_t>> m_frames;
    std::vector<uint8_t> m_out;
};

// Opus or Vorbis in Ogg, through the Recorder's own writers. Those run their
// encoding on a thread of their own, so the pages arrive under a lock.
class OggStreamEncoder : public StreamEncoder
{
public:
    explicit OggStreamEncoder(bool opus) : m_opus(opus) {}
    ~OggStreamEncoder() override { Stop(); }

    bool Start(int rate, int kbps, const std::string& title, std::wstring* err) override
    {
        m_kbps = kbps;
        m_rec = MakeOggStreamEncoder(m_opus, title, [this](const unsigned char* p, size_t n) {
            std::lock_guard<std::mutex> g(m_lock);
            m_out.insert(m_out.end(), p, p + n);
        });
        return m_rec->Start(std::wstring(), rate, kbps, err);
    }

    bool Encode(const float* buf, size_t frames, std::wstring*) override
    {
        if (m_rec) m_rec->Write(buf, frames);
        return true;
    }

    void Take(std::vector<uint8_t>* out) override
    {
        std::lock_guard<std::mutex> g(m_lock);
        out->insert(out->end(), m_out.begin(), m_out.end());
        m_out.clear();
    }

    void Stop() override
    {
        if (m_rec) { m_rec->Stop(); m_rec.reset(); }
    }
    const char* ContentType() const override { return "audio/ogg"; }
    int Kbps() const override { return m_kbps; }

private:
    bool m_opus;
    int m_kbps = 128;
    std::unique_ptr<Recorder> m_rec;
    std::mutex m_lock;
    std::vector<uint8_t> m_out;
};

}   // namespace

std::unique_ptr<StreamEncoder> MakeStreamEncoder(int format)
{
    switch (format)
    {
    case SF_MP3:    return std::unique_ptr<StreamEncoder>(new MfStreamEncoder(true));
    case SF_OPUS:   return std::unique_ptr<StreamEncoder>(new OggStreamEncoder(true));
    case SF_VORBIS: return std::unique_ptr<StreamEncoder>(new OggStreamEncoder(false));
    default:        return std::unique_ptr<StreamEncoder>(new MfStreamEncoder(false));
    }
}
