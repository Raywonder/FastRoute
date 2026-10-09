#include "audio.h"
#include "util.h"

#include <algorithm>
#include <cmath>

extern "C" {
#include <speex/speex_preprocess.h>
#include <speex/speex_resampler.h>
#include <rnnoise.h>
}

// ---------------------------------------------------------------------------
// Frame based mono effect
//
// The bus arrives in whatever block size the output endpoint asks for, which
// is not the frame size these denoisers want. This buffers up to a frame,
// runs the algorithm once per channel, and hands the result back through a
// small FIFO. The dry signal is blended inside the frame, where it is still
// aligned with the wet one, so the mix control cannot comb filter.
// ---------------------------------------------------------------------------
class FrameFx : public Effect
{
public:
    int LatencyFrames() const override { return m_n; }

    void Process(float* buf, size_t frames) override
    {
        const float wet = m_wet.load();

        for (size_t i = 0; i < frames; ++i)
        {
            for (int c = 0; c < kChannels; ++c) m_in[c].push_back(buf[i * kChannels + c]);

            if ((int)m_in[0].size() >= m_n)
            {
                for (int c = 0; c < kChannels; ++c)
                {
                    m_dry.assign(m_in[c].begin(), m_in[c].begin() + m_n);
                    Frame(m_in[c].data(), c);
                    for (int k = 0; k < m_n; ++k)
                        m_out[c].push_back(wet * m_in[c][k] + (1.0f - wet) * m_dry[k]);
                    m_in[c].erase(m_in[c].begin(), m_in[c].begin() + m_n);
                }
            }

            for (int c = 0; c < kChannels; ++c)
            {
                if (m_pos[c] < m_out[c].size())
                    buf[i * kChannels + c] = m_out[c][m_pos[c]++];
                else
                    buf[i * kChannels + c] = 0.0f;   // only before the pipe fills
            }
        }

        // Reclaim the consumed part of each output FIFO.
        for (int c = 0; c < kChannels; ++c)
        {
            if (m_pos[c] > 0 && m_pos[c] >= m_out[c].size())
            {
                m_out[c].clear();
                m_pos[c] = 0;
            }
            else if (m_pos[c] > (size_t)m_n * 4)
            {
                m_out[c].erase(m_out[c].begin(), m_out[c].begin() + m_pos[c]);
                m_pos[c] = 0;
            }
        }
    }

protected:
    // Processes one frame of `m_n` mono samples in place, for channel `c`.
    virtual void Frame(float* mono, int c) = 0;

    void SetFrame(int n)
    {
        m_n = n;
        m_dry.resize(n);
        for (int c = 0; c < kChannels; ++c)
        {
            m_in[c].clear();
            m_in[c].reserve((size_t)n * 4);
            // Prime with one frame of silence: that is the algorithmic latency,
            // and paying it up front means the FIFO never runs dry later.
            m_out[c].assign(n, 0.0f);
            m_pos[c] = 0;
        }
    }

    int m_n = 480;

private:
    std::vector<float> m_in[kChannels];
    std::vector<float> m_out[kChannels];
    size_t m_pos[kChannels] = { 0, 0 };
    std::vector<float> m_dry;
};

// ---------------------------------------------------------------------------
// SpeexDSP preprocessor
//
// Works at any sample rate, and exposes how hard to suppress, so this is the
// one that covers endpoints RNNoise cannot take.
// ---------------------------------------------------------------------------
class SpeexDenoise : public FrameFx
{
public:
    ~SpeexDenoise() override
    {
        for (int c = 0; c < kChannels; ++c)
            if (m_st[c]) speex_preprocess_state_destroy(m_st[c]);
    }

    bool Start(int rate, std::wstring* err) override
    {
        SetFrame(rate / 100);          // 10 ms frames
        m_scratch.resize(m_n);

        for (int c = 0; c < kChannels; ++c)
        {
            m_st[c] = speex_preprocess_state_init(m_n, rate);
            if (!m_st[c])
            {
                if (err) *err = L"The Speex denoiser could not be initialised.";
                return false;
            }
            int on = 1, off = 0;
            speex_preprocess_ctl(m_st[c], SPEEX_PREPROCESS_SET_DENOISE, &on);
            // Denoise only: the preprocessor also offers gain control, voice
            // detection and dereverb, none of which belong in a routing block.
            speex_preprocess_ctl(m_st[c], SPEEX_PREPROCESS_SET_AGC, &off);
            speex_preprocess_ctl(m_st[c], SPEEX_PREPROCESS_SET_VAD, &off);
            speex_preprocess_ctl(m_st[c], SPEEX_PREPROCESS_SET_DEREVERB, &off);
            ApplySuppress(c);
        }
        return true;
    }

    void Configure(const Block& b) override
    {
        m_suppress = b.suppressDb;
        for (int c = 0; c < kChannels; ++c) if (m_st[c]) ApplySuppress(c);
    }

protected:
    void Frame(float* mono, int c) override
    {
        if (!m_st[c]) return;

        for (int i = 0; i < m_n; ++i)
        {
            float v = mono[i] * 32767.0f;
            if (v > 32767.0f) v = 32767.0f;
            else if (v < -32768.0f) v = -32768.0f;
            m_scratch[i] = (spx_int16_t)lrintf(v);
        }
        speex_preprocess_run(m_st[c], m_scratch.data());
        for (int i = 0; i < m_n; ++i) mono[i] = m_scratch[i] / 32767.0f;
    }

private:
    void ApplySuppress(int c)
    {
        // The control takes a negative number of dB; the UI shows it positive.
        int db = -std::abs(m_suppress.load());
        speex_preprocess_ctl(m_st[c], SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &db);
    }

    SpeexPreprocessState* m_st[kChannels] = { nullptr, nullptr };
    std::vector<spx_int16_t> m_scratch;
    std::atomic<int> m_suppress{ 20 };
};

// ---------------------------------------------------------------------------
// RNNoise
//
// Fixed at 48 kHz and 480 sample frames, and it wants samples on the 16 bit
// scale rather than normalised.
//
// A session runs at its master output endpoint's rate, which is whatever
// Windows has that device set to, and 44.1 kHz is ordinary. This used to
// refuse to start there and tell you to go and change the device, which made
// the block unusable on a lot of perfectly normal machines for a reason that
// has nothing to do with what the block is for. So the bus is converted to
// 48 kHz around the model and back again, with the resampler that already
// comes with SpeexDSP.
//
// It is not free. The conversion has a group delay of its own on top of the
// model's 10 ms frame, and there is a little silence primed into the output
// so that the odd sample of jitter between the two converters can never leave
// the FIFO empty. All of that is reported by LatencyFrames, which is what
// keeps the rest of the graph aligned and what the session window shows.
//
// A session already at 48 kHz gets none of it: the converters are never
// created and the audio takes exactly the path it always took.
// ---------------------------------------------------------------------------
class RnnDenoise : public FrameFx
{
public:
    ~RnnDenoise() override
    {
        for (int c = 0; c < kChannels; ++c)
            if (m_st[c]) rnnoise_destroy(m_st[c]);
        if (m_up)   speex_resampler_destroy(m_up);
        if (m_down) speex_resampler_destroy(m_down);
    }

    bool Start(int rate, std::wstring* err) override
    {
        if (rate <= 0)
        {
            if (err) *err = L"RNNoise was started with no sample rate.";
            return false;
        }
        m_rate = rate;

        SetFrame(rnnoise_get_frame_size());
        m_scratch.resize(m_n);
        for (int c = 0; c < kChannels; ++c)
        {
            m_st[c] = rnnoise_create(nullptr);   // built in model
            if (!m_st[c])
            {
                if (err) *err = L"RNNoise could not be initialised.";
                return false;
            }
        }

        if (rate == kRnnRate) return true;

        int upErr = 0, downErr = 0;
        m_up   = speex_resampler_init(kChannels, (spx_uint32_t)rate,
                                      kRnnRate, kQuality, &upErr);
        m_down = speex_resampler_init(kChannels, kRnnRate,
                                      (spx_uint32_t)rate, kQuality, &downErr);
        if (!m_up || !m_down)
        {
            if (err) *err = FormatW(
                L"RNNoise runs at 48000 Hz, this session is running at %d Hz, and "
                L"the converter between the two could not be created. Use the "
                L"Speex denoiser here, or set the output device to 48000 Hz in "
                L"Windows sound settings.", rate);
            return false;
        }

        // What the pair of converters swallows before either of them starts
        // handing samples back, measured in the session's own frames.
        m_convDelay = (size_t)(speex_resampler_get_output_latency(m_up) *
                               (double)rate / kRnnRate) +
                      (size_t)speex_resampler_get_output_latency(m_down);

        // Silence ahead of the first real sample. It has to cover that swallow,
        // or the first call would find the FIFO short and hand back zeros, and
        // a little more for the sample of jitter the two phases can differ by
        // afterwards. Everything put in here is latency, which is why there is
        // not more of it.
        m_prime = m_convDelay + kSlack;
        m_tail.assign(m_prime * kChannels, 0.0f);
        m_taken = 0;
        return true;
    }

    int LatencyFrames() const override
    {
        if (!m_up) return FrameFx::LatencyFrames();
        // Three things, and leaving any of them out would put every other
        // block's dry path out of step with this one: the model's frame, which
        // is 480 samples of 48 kHz whatever the session is doing and so has to
        // be quoted in the session's own frames; the group delay of the two
        // converters; and the silence primed in front of the FIFO.
        return (int)((double)m_n * m_rate / kRnnRate) +
               (int)m_convDelay + (int)m_prime;
    }

    void Process(float* buf, size_t frames) override
    {
        if (!m_up) { FrameFx::Process(buf, frames); return; }
        if (frames == 0) return;

        // Up to 48 kHz. The headroom past the ratio is what lets the converter
        // run out of input rather than out of room, which is what makes it
        // consume the whole buffer instead of leaving some behind.
        const size_t cap = (size_t)((double)frames * kRnnRate / m_rate) + 32;
        if (m_at48.size() < cap * kChannels) m_at48.resize(cap * kChannels);

        spx_uint32_t in = (spx_uint32_t)frames, out = (spx_uint32_t)cap;
        speex_resampler_process_interleaved_float(m_up, buf, &in,
                                                  m_at48.data(), &out);

        FrameFx::Process(m_at48.data(), out);

        const size_t room = (size_t)((double)out * m_rate / kRnnRate) + 32;
        if (m_atRate.size() < room * kChannels) m_atRate.resize(room * kChannels);

        spx_uint32_t back = out, done = (spx_uint32_t)room;
        speex_resampler_process_interleaved_float(m_down, m_at48.data(), &back,
                                                  m_atRate.data(), &done);
        m_tail.insert(m_tail.end(), m_atRate.begin(),
                      m_atRate.begin() + (size_t)done * kChannels);

        // Hand back exactly what was asked for, which the priming silence is
        // what makes possible on the very first call.
        const size_t have = m_tail.size() / kChannels - m_taken;
        const size_t take = frames < have ? frames : have;
        for (size_t i = 0; i < take * kChannels; ++i)
            buf[i] = m_tail[m_taken * kChannels + i];
        for (size_t i = take * kChannels; i < frames * kChannels; ++i)
            buf[i] = 0.0f;
        m_taken += take;

        // Reclaim what has been handed out, in one move rather than per call.
        if (m_taken > (size_t)m_rate / 10)
        {
            m_tail.erase(m_tail.begin(), m_tail.begin() + m_taken * kChannels);
            m_taken = 0;
        }
    }

protected:
    void Frame(float* mono, int c) override
    {
        if (!m_st[c]) return;
        for (int i = 0; i < m_n; ++i) m_scratch[i] = mono[i] * 32768.0f;
        rnnoise_process_frame(m_st[c], m_scratch.data(), m_scratch.data());
        for (int i = 0; i < m_n; ++i) mono[i] = m_scratch[i] / 32768.0f;
    }

private:
    static const spx_uint32_t kRnnRate = 48000;
    // SPEEX_RESAMPLER_QUALITY_DESKTOP. This runs in the render callback, and
    // the band it has to get right is speech, which the model has already had
    // its way with by the time anybody hears it.
    static const int kQuality = SPEEX_RESAMPLER_QUALITY_DESKTOP;
    static const size_t kSlack = 64;

    DenoiseState* m_st[kChannels] = { nullptr, nullptr };
    std::vector<float> m_scratch;

    int m_rate = (int)kRnnRate;
    SpeexResamplerState* m_up = nullptr;     // session rate -> 48 kHz
    SpeexResamplerState* m_down = nullptr;   // 48 kHz -> session rate
    std::vector<float> m_at48;               // scratch, interleaved
    std::vector<float> m_atRate;             // scratch, interleaved
    std::vector<float> m_tail;               // what is waiting to be handed out
    size_t m_taken = 0;                      // how much of it already has been
    size_t m_convDelay = 0;
    size_t m_prime = 0;
};

std::unique_ptr<Effect> MakeSpeexDenoise() { return std::unique_ptr<Effect>(new SpeexDenoise()); }
std::unique_ptr<Effect> MakeRnnDenoise()   { return std::unique_ptr<Effect>(new RnnDenoise()); }
