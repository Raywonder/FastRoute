#include "audio.h"

// Signalsmith Stretch is built with its own warning settings in mind and this
// project is built with /W3; wrapping the include keeps our warnings ours.
#pragma warning(push, 0)
#include <signalsmith-stretch.h>
#pragma warning(pop)

#include <cmath>
#include <vector>

// ---------------------------------------------------------------------------
// Pitch shifter
//
// Moves the pitch without moving the speed, which is not something a filter
// can do: the signal has to be taken apart into overlapping windows, each
// window's spectrum moved up or down, and the pieces laid back down in the
// same places they came from. That is what Signalsmith Stretch does here, and
// the reason this block costs so much more latency than every other one.
//
// The cost is the window. The block cannot say anything about a sound until it
// has a whole window of it, so the latency is exactly one window - 120 ms at
// the best setting, which is a long time in a live path and is why the window
// is a setting rather than a constant. A shorter window is less delay and less
// to work with: it resolves low frequencies more coarsely, so a bass note gets
// grainy long before a voice does.
//
// At no shift at all the block hands the buffer straight back. It could run
// the shifter at a ratio of one, but that would be 120 ms of delay and a good
// deal of arithmetic to arrive at approximately the input, so a pitch shifter
// set to zero costs nothing at all and says so.
// ---------------------------------------------------------------------------

namespace {

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// How long a window each quality setting asks for, as a fraction of a second,
// and the hop between windows. The top one is Signalsmith's own presetDefault;
// the other two are the same shape, halved and quartered. Latency is exactly
// the window, so these are also 120, 60 and 30 ms.
struct Geometry { double block, interval; };

const Geometry kGeometry[kPitchQualityCount] = {
    { 0.12,  0.03   },   // PQ_BEST
    { 0.06,  0.015  },   // PQ_BALANCED
    { 0.03,  0.0075 },   // PQ_LOW_DELAY
};

// Frames handed to the shifter at a time. It accepts any length, but a fixed
// one means the scratch buffers are allocated once, in Configure, rather than
// growing on the render thread the first time a device asks for a long period.
const size_t kChunk = 512;

}  // namespace

class PitchFx : public Effect
{
public:
    bool Start(int rate, std::wstring* err) override
    {
        (void)err;
        m_rate = rate > 0 ? rate : kDefaultRate;
        // Nothing is allocated here. Configure always follows Start, and it is
        // Configure that knows which window was asked for.
        m_ready = false;
        m_quality = -1;
        return true;
    }

    void Configure(const Block& b) override
    {
        const int cents = Clamp(b.pitchSemitones, kPitchSemitonesMin, kPitchSemitonesMax) * 100
                        + Clamp(b.pitchCents, kPitchCentsMin, kPitchCentsMax);
        const int quality = Clamp(b.pitchQuality, 0, kPitchQualityCount - 1);

        const bool wasOff = m_off;
        m_off = (cents == 0);
        if (m_off) return;                  // nothing is built until it is needed

        if (quality != m_quality || !m_ready)
        {
            // Allocation, on the same lock the render thread takes. This is
            // the one moment it is safe, which is why the geometry is only
            // ever changed from here.
            const Geometry& g = kGeometry[quality];
            m_stretch.configure(kChannels,
                                (int)(m_rate * g.block),
                                (int)(m_rate * g.interval));
            m_stretch.reset();

            m_latency = (size_t)(m_stretch.inputLatency() + m_stretch.outputLatency());

            for (int c = 0; c < kChannels; ++c)
            {
                m_in[c].assign(kChunk, 0.0f);
                m_out[c].assign(kChunk, 0.0f);
                m_inPtr[c] = m_in[c].data();
                m_outPtr[c] = m_out[c].data();
            }

            // The dry path is delayed by exactly what the shifter delays by,
            // so that a block mixed part dry is a shifted voice against its own
            // undisturbed self rather than against a copy of itself 120 ms out
            // of step, which would comb filter rather than harmonise.
            m_delay.assign((m_latency + 1) * kChannels, 0.0f);
            m_pos = 0;
            m_quality = quality;
            m_ready = true;
        }
        else if (wasOff)
        {
            // Coming back from no shift at all. Everything the shifter and the
            // delay line are holding is from before it was turned off, and
            // playing that out would be a click on the way back in.
            m_stretch.reset();
            m_delay.assign(m_delay.size(), 0.0f);
            m_pos = 0;
        }

        m_stretch.setTransposeSemitones((float)cents / 100.0f);
    }

    // Zero while it is doing nothing, because then Process hands the buffer
    // back untouched and undelayed.
    int LatencyFrames() const override
    {
        return (m_off || !m_ready) ? 0 : (int)m_latency;
    }

    void Process(float* buf, size_t frames) override
    {
        if (m_off || !m_ready) return;

        const float wet = m_wet.load();
        const size_t span = m_latency + 1;

        for (size_t done = 0; done < frames; )
        {
            const size_t k = (frames - done) < kChunk ? (frames - done) : kChunk;
            float* at = buf + done * kChannels;

            for (size_t i = 0; i < k; ++i)
                for (int c = 0; c < kChannels; ++c)
                    m_in[c][i] = at[i * kChannels + c];

            // The shifter will not read and write the same memory, and the
            // dry is still needed after it has run, so this goes out of place.
            m_stretch.process(m_inPtr, (int)k, m_outPtr, (int)k);

            for (size_t i = 0; i < k; ++i)
            {
                for (int c = 0; c < kChannels; ++c)
                    m_delay[m_pos * kChannels + c] = m_in[c][i];
                m_pos = (m_pos + 1) % span;

                for (int c = 0; c < kChannels; ++c)
                {
                    const float dry = m_delay[m_pos * kChannels + c];
                    at[i * kChannels + c] = dry + (m_out[c][i] - dry) * wet;
                }
            }
            done += k;
        }
    }

private:
    signalsmith::stretch::SignalsmithStretch<float> m_stretch;

    int    m_rate = kDefaultRate;
    int    m_quality = -1;
    bool   m_ready = false;
    bool   m_off = true;
    size_t m_latency = 0;

    std::vector<float> m_in[kChannels], m_out[kChannels];
    float* m_inPtr[kChannels] = { nullptr, nullptr };
    float* m_outPtr[kChannels] = { nullptr, nullptr };

    // The dry path, delayed to meet the shifted one.
    std::vector<float> m_delay;
    size_t m_pos = 0;
};

std::unique_ptr<Effect> MakePitchShift()
{
    return std::unique_ptr<Effect>(new PitchFx());
}
