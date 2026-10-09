#include "audio.h"

#include <cmath>
#include <vector>

// ---------------------------------------------------------------------------
// Compressor and limiter
//
// Both are stereo linked: one gain is worked out from whichever channel is
// louder and applied to both. Two independent detectors would pull the stereo
// image sideways every time one channel got loud on its own, which on a voice
// panned slightly off centre is very audible and impossible to explain.
//
// Both also work in dB rather than in amplitude, because that is the unit the
// controls are in and the unit the maths is simplest in: a ratio is a straight
// multiplication of the amount by which the signal is over the threshold.
// ---------------------------------------------------------------------------

namespace {

const double kMinLevel = 1.0e-9;      // -180 dB, the floor for the log

double ToDb(double x) { return 20.0 * std::log10(x < kMinLevel ? kMinLevel : x); }
double FromDb(double db) { return std::pow(10.0, db / 20.0); }

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// One pole smoothing coefficient for a time constant in milliseconds. The
// filter reaches about 63% of the way in one time constant and 98% in four,
// which is what "attack" and "release" mean everywhere else as well.
double CoefFor(double ms, int rate)
{
    if (ms <= 0.0 || rate <= 0) return 0.0;   // 0 means "arrive immediately"
    return std::exp(-1.0 / (ms * 0.001 * (double)rate));
}

// ---------------------------------------------------------------------------
// Compressor
//
// Above the threshold, every dB in becomes 1/ratio dB out. The knee is a fixed
// 6 dB either side of the threshold, over which the ratio comes in gradually
// rather than switching on at a point - a hard knee is audible on speech as a
// sort of clicking on the consonants that cross the line.
// ---------------------------------------------------------------------------
class CompressorFx : public Effect
{
public:
    bool Start(int rate, std::wstring* err) override
    {
        (void)err;
        m_rate = rate > 0 ? rate : kDefaultRate;
        m_gr = 0.0;
        return true;
    }

    void Configure(const Block& b) override
    {
        m_threshDb = (double)Clamp(b.compThreshDb, kCompThreshMin, kCompThreshMax);
        m_ratio    = (double)Clamp(b.compRatio, kCompRatioMin, kCompRatioMax);
        m_attack   = CoefFor((double)Clamp(b.compAttackMs, kCompAttackMin, kCompAttackMax), m_rate);
        m_release  = CoefFor((double)Clamp(b.compReleaseMs, kCompReleaseMin, kCompReleaseMax), m_rate);

        // Automatic makeup restores the level a full scale peak would have had
        // before the compressor took hold of it, so a compressed signal is as
        // loud at the top as it was and louder everywhere below. That is what
        // the control is for; anything more is the Gain block's job.
        double makeup = (double)Clamp(b.compMakeupDb, 0, kCompMakeupMax);
        if (b.compAutoMakeup) makeup = AutoMakeupDb(m_threshDb, m_ratio);
        m_makeupDb = makeup;

        m_off = (m_ratio <= 1.0);
    }

    void Process(float* buf, size_t frames) override
    {
        if (m_off && m_makeupDb == 0.0) return;
        const float wet = m_wet.load();
        const double slope = 1.0 / m_ratio - 1.0;   // dB out per dB over, minus one

        for (size_t i = 0; i < frames; ++i)
        {
            const float dl = buf[i * kChannels + 0];
            const float dr = buf[i * kChannels + 1];

            const double peak = (std::fabs(dl) > std::fabs(dr))
                              ? std::fabs(dl) : std::fabs(dr);
            const double over = ToDb(peak) - m_threshDb;

            // Static curve, in dB of reduction (never positive).
            double target;
            if (over <= -kKneeDb * 0.5)        target = 0.0;
            else if (over >= kKneeDb * 0.5)    target = slope * over;
            else
            {
                const double d = over + kKneeDb * 0.5;
                target = slope * d * d / (2.0 * kKneeDb);
            }

            // More reduction is the attack; less is the release.
            const double c = (target < m_gr) ? m_attack : m_release;
            m_gr = target + (m_gr - target) * c;

            const float g = (float)FromDb(m_gr + m_makeupDb);
            buf[i * kChannels + 0] = wet * dl * g + (1.0f - wet) * dl;
            buf[i * kChannels + 1] = wet * dr * g + (1.0f - wet) * dr;
        }
    }

private:
    static const int kKneeDb = 6;

    int    m_rate = kDefaultRate;
    double m_threshDb = -20.0;
    double m_ratio = 4.0;
    double m_attack = 0.0, m_release = 0.0;
    double m_makeupDb = 0.0;
    double m_gr = 0.0;            // current reduction in dB, audio thread only
    bool   m_off = false;
};

// ---------------------------------------------------------------------------
// Limiter
//
// A limiter is a compressor at an infinite ratio, but the useful part is the
// lookahead: the audio is held back by a few milliseconds so the gain can
// already be down by the time a peak arrives, instead of catching up after it.
// Without that a limiter either lets the first moment of every transient
// through or has to distort it away.
//
// The peak of the whole lookahead window is what the gain is worked out from,
// tracked with a monotonic queue so it costs a constant amount per sample
// however long the window is.
//
// The dry signal is taken from the delay line too, not from the input, so the
// mix control stays a straight crossfade between two things that line up.
// ---------------------------------------------------------------------------
class LimiterFx : public Effect
{
public:
    bool Start(int rate, std::wstring* err) override
    {
        (void)err;
        m_rate = rate > 0 ? rate : kDefaultRate;
        return true;
    }

    void Configure(const Block& b) override
    {
        m_ceiling = FromDb((double)Clamp(b.limCeilingDb, kLimCeilingMin, kLimCeilingMax));
        m_release = CoefFor((double)Clamp(b.limReleaseMs, kLimReleaseMin, kLimReleaseMax), m_rate);

        const int ms = Clamp(b.limLookaheadMs, kLimLookaheadMin, kLimLookaheadMax);
        const size_t look = (size_t)((long long)m_rate * ms / 1000);

        // Four time constants inside the window, so the gain has arrived by
        // the time the peak it was worked out for reaches the output. With no
        // lookahead at all there is nowhere to ramp and the ceiling clamp
        // below is doing the work, which is honest: that is a clipper.
        m_attack = CoefFor((double)ms / 4.0, m_rate);

        if (look != m_look)
        {
            m_look = look;
            // Allocated here rather than in Process. Configure runs under the
            // same lock as the render, so this is the one safe moment.
            m_delay.assign((m_look + 1) * kChannels, 0.0f);
            // One slot more than the queue can ever hold, so a full queue is
            // never mistaken for an empty one.
            m_qSpan = m_look + 2;
            m_qVal.assign(m_qSpan, 0.0f);
            m_qPos.assign(m_qSpan, 0ull);
            m_head = m_tail = 0;
            m_pos = 0;
            m_n = 0;
            m_gain = 1.0;
        }
    }

    int LatencyFrames() const override { return m_delay.empty() ? 0 : (int)m_look; }

    void Process(float* buf, size_t frames) override
    {
        if (m_delay.empty()) return;
        const float wet = m_wet.load();
        const size_t span = m_look + 1;

        for (size_t i = 0; i < frames; ++i)
        {
            const float inL = buf[i * kChannels + 0];
            const float inR = buf[i * kChannels + 1];

            // What comes out now is what went in `m_look` frames ago. The new
            // frame is written first and the oldest read after it, which for a
            // buffer one longer than the delay is the slot in front. Reading
            // the same slot first would delay by the whole buffer instead -
            // one frame too many, and the peak being looked for would fall
            // outside the window it is looked for in.
            m_delay[m_pos * kChannels + 0] = inL;
            m_delay[m_pos * kChannels + 1] = inR;
            m_pos = (m_pos + 1) % span;
            const float dl = m_delay[m_pos * kChannels + 0];
            const float dr = m_delay[m_pos * kChannels + 1];

            // Running maximum over the window. Anything already in the queue
            // that this sample equals or beats can never be the maximum again,
            // because it leaves the window first.
            const float lvl = (std::fabs(inL) > std::fabs(inR))
                            ? std::fabs(inL) : std::fabs(inR);
            while (m_head != m_tail)
            {
                size_t back = (m_tail + m_qSpan - 1) % m_qSpan;
                if (m_qVal[back] > lvl) break;
                m_tail = back;
            }
            m_qVal[m_tail] = lvl;
            m_qPos[m_tail] = m_n;
            m_tail = (m_tail + 1) % m_qSpan;
            while (m_qPos[m_head] + m_look < m_n) m_head = (m_head + 1) % m_qSpan;
            const double peak = m_qVal[m_head];
            ++m_n;

            const double target = (peak > m_ceiling) ? (m_ceiling / peak) : 1.0;
            const double c = (target < m_gain) ? m_attack : m_release;
            m_gain = target + (m_gain - target) * c;

            float l = (float)(dl * m_gain);
            float r = (float)(dr * m_gain);

            // Backstop. The ramp is fast enough that this almost never fires,
            // but "almost never" is not what a ceiling means.
            const float ceil = (float)m_ceiling;
            if (l >  ceil) l =  ceil;
            if (l < -ceil) l = -ceil;
            if (r >  ceil) r =  ceil;
            if (r < -ceil) r = -ceil;

            buf[i * kChannels + 0] = wet * l + (1.0f - wet) * dl;
            buf[i * kChannels + 1] = wet * r + (1.0f - wet) * dr;
        }
    }

private:
    int    m_rate = kDefaultRate;
    double m_ceiling = 1.0;
    double m_attack = 0.0, m_release = 0.0;
    double m_gain = 1.0;

    size_t m_look = (size_t)-1;        // forces the first Configure to allocate
    std::vector<float> m_delay;
    size_t m_pos = 0;

    // Monotonic queue of candidate maxima, as a ring of the same span.
    std::vector<float> m_qVal;
    std::vector<unsigned long long> m_qPos;
    size_t m_qSpan = 0;
    size_t m_head = 0, m_tail = 0;
    unsigned long long m_n = 0;
};

// ---------------------------------------------------------------------------
// Noise gate
//
// The opposite end of the same idea as the compressor: below a threshold, turn
// the signal down rather than up. What it is for is the hiss and hum between
// the words, and the neighbour's lawnmower in the gaps.
//
// Two details are what separate a usable gate from one that chatters. The
// first is hysteresis: it opens at the threshold but does not shut until the
// signal has fallen a few dB below it, so a sound sitting exactly on the line
// does not switch the gate on and off at the syllable rate. The second is the
// hold, which keeps it open for a while after the sound stops - without one, a
// gate closes on the tail of every word and the speech sounds clipped short.
//
// The cut is a setting rather than silence. A gate that shuts completely is
// very obvious; one that drops the background 20 dB often sounds like the
// noise was simply never there.
// ---------------------------------------------------------------------------
class GateFx : public Effect
{
public:
    bool Start(int rate, std::wstring* err) override
    {
        (void)err;
        m_rate = rate > 0 ? rate : kDefaultRate;
        // Starts open: a gate that begins shut swallows the first word while
        // it works out that there is something there.
        m_open = true;
        m_gainDb = 0.0;
        m_held = 0;
        return true;
    }

    void Configure(const Block& b) override
    {
        m_threshDb = (double)Clamp(b.gateThreshDb, kGateThreshMin, kGateThreshMax);
        m_rangeDb  = (double)Clamp(b.gateRangeDb, kGateRangeMin, kGateRangeMax);
        m_attack   = CoefFor((double)Clamp(b.gateAttackMs, kGateAttackMin, kGateAttackMax), m_rate);
        m_release  = CoefFor((double)Clamp(b.gateReleaseMs, kGateReleaseMin, kGateReleaseMax), m_rate);
        m_holdFrames = (size_t)((long long)m_rate *
                                Clamp(b.gateHoldMs, kGateHoldMin, kGateHoldMax) / 1000);
        m_off = (m_rangeDb <= 0.0);

        const size_t look = (size_t)((long long)m_rate *
            Clamp(b.gateLookaheadMs, kGateLookaheadMin, kGateLookaheadMax) / 1000);

        if (look != m_look || m_delay.empty())
        {
            m_look = look;
            // Allocated here rather than in Process. Configure runs under the
            // same lock as the render, so this is the one safe moment.
            m_delay.assign((m_look + 1) * kChannels, 0.0f);
            // One slot more than the queue can ever hold, so a full queue is
            // never mistaken for an empty one.
            m_qSpan = m_look + 2;
            m_qVal.assign(m_qSpan, 0.0f);
            m_qPos.assign(m_qSpan, 0ull);
            m_head = m_tail = 0;
            m_pos = 0;
            m_n = 0;
        }
    }

    // Zero while the gate is doing nothing, because then Process hands the
    // buffer back untouched and undelayed, and a block that claimed latency it
    // was not adding would pull every dry path in the session out of step.
    int LatencyFrames() const override
    {
        return (m_off || m_delay.empty()) ? 0 : (int)m_look;
    }

    void Process(float* buf, size_t frames) override
    {
        if (m_off || m_delay.empty()) return;
        const float wet = m_wet.load();
        const double closeDb = m_threshDb - kHysteresisDb;
        const size_t span = m_look + 1;

        for (size_t i = 0; i < frames; ++i)
        {
            const float inL = buf[i * kChannels + 0];
            const float inR = buf[i * kChannels + 1];

            // What comes out now is what went in `m_look` frames ago. The new
            // frame is written first and the oldest read after it, which for a
            // buffer one longer than the delay is the slot in front. With no
            // lookahead the span is one, the write and the read are the same
            // slot, and this is the passthrough it was before.
            m_delay[m_pos * kChannels + 0] = inL;
            m_delay[m_pos * kChannels + 1] = inR;
            m_pos = (m_pos + 1) % span;
            const float dl = m_delay[m_pos * kChannels + 0];
            const float dr = m_delay[m_pos * kChannels + 1];

            // The detector runs on the undelayed signal, over a running
            // maximum of the window, so the gate opens as soon as anything
            // loud enough is anywhere in the next `m_look` frames rather than
            // when it finally arrives. Anything already in the queue that this
            // sample equals or beats can never be the maximum again, because
            // it leaves the window first.
            const float lvl = (std::fabs(inL) > std::fabs(inR))
                            ? std::fabs(inL) : std::fabs(inR);
            while (m_head != m_tail)
            {
                size_t back = (m_tail + m_qSpan - 1) % m_qSpan;
                if (m_qVal[back] > lvl) break;
                m_tail = back;
            }
            m_qVal[m_tail] = lvl;
            m_qPos[m_tail] = m_n;
            m_tail = (m_tail + 1) % m_qSpan;
            while (m_qPos[m_head] + m_look < m_n) m_head = (m_head + 1) % m_qSpan;
            const double peak = m_qVal[m_head];
            ++m_n;

            const double level = ToDb(peak);

            if (level >= m_threshDb)
            {
                m_open = true;
                m_held = 0;
            }
            else if (level < closeDb)
            {
                // Below the lower line. The hold has to run out before the
                // gate actually starts closing.
                if (m_held < m_holdFrames) ++m_held;
                else m_open = false;
            }
            // Between the two lines nothing changes, which is the hysteresis.

            const double target = m_open ? 0.0 : -m_rangeDb;
            const double c = (target > m_gainDb) ? m_attack : m_release;
            m_gainDb = target + (m_gainDb - target) * c;

            const float g = (float)FromDb(m_gainDb);
            buf[i * kChannels + 0] = wet * dl * g + (1.0f - wet) * dl;
            buf[i * kChannels + 1] = wet * dr * g + (1.0f - wet) * dr;
        }
    }

private:
    static const int kHysteresisDb = 3;

    int    m_rate = kDefaultRate;
    double m_threshDb = -40.0;
    double m_rangeDb = 60.0;
    double m_attack = 0.0, m_release = 0.0;
    double m_gainDb = 0.0;
    size_t m_holdFrames = 0, m_held = 0;
    bool   m_open = true;
    bool   m_off = false;

    // The lookahead: a delay line for the audio, and a monotonic queue giving
    // the running maximum over the window for the detector.
    size_t m_look = 0;
    std::vector<float> m_delay;
    size_t m_pos = 0;
    std::vector<float> m_qVal;
    std::vector<unsigned long long> m_qPos;
    size_t m_qSpan = 0, m_head = 0, m_tail = 0;
    unsigned long long m_n = 0;
};

}   // namespace

std::unique_ptr<Effect> MakeCompressor() { return std::unique_ptr<Effect>(new CompressorFx()); }
std::unique_ptr<Effect> MakeLimiter()    { return std::unique_ptr<Effect>(new LimiterFx()); }
std::unique_ptr<Effect> MakeNoiseGate()  { return std::unique_ptr<Effect>(new GateFx()); }
