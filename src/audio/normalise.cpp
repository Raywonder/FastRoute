#include "audio.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Normaliser and clipping monitor
//
// Two effects that both watch the level rather than shape it, and are easier
// to read next to each other than apart.
// ---------------------------------------------------------------------------

namespace {

const double kMinLevel = 1.0e-9;      // -180 dB, the floor for the log

double FromDb(double db) { return std::pow(10.0, db / 20.0); }
int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

double CoefFor(double ms, int rate)
{
    if (ms <= 0.0 || rate <= 0) return 0.0;
    return std::exp(-1.0 / (ms * 0.001 * (double)rate));
}

// ---------------------------------------------------------------------------
// Normaliser
//
// Normalising a file is a two pass job: find the loudest point, then scale the
// whole thing so that point lands on the target. A live stream has no second
// pass and no end to find the loudest point of, so this is the running form of
// the same idea - the gain follows the signal instead of being worked out once.
//
// The lookahead is what makes that bearable. The gain applied to a sample is
// worked out from the loudest sample within the window *ahead* of it, so the
// level is already down by the time a loud passage arrives rather than being
// caught out by it. That is also exactly the latency it costs, which is why
// the control is in milliseconds and says so.
//
// Two things stop it being unusable:
//
//  * The maximum gain. Room tone at -70 dB would otherwise be worth 67 dB of
//    boost on the way to a -3 dB target, and a pause between sentences would
//    arrive louder than the sentences. Capping the boost is what keeps quiet
//    quiet.
//  * The release. Coming down is immediate, because a peak that is not caught
//    is a peak that clips. Going back up is slow, so the gain does not audibly
//    pump between words. They are deliberately asymmetric.
// ---------------------------------------------------------------------------
class NormaliseFx : public Effect
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
        m_target  = FromDb((double)Clamp(b.normTargetDb, kNormTargetMin, kNormTargetMax));
        m_maxGain = FromDb((double)Clamp(b.normMaxGainDb, kNormMaxGainMin, kNormMaxGainMax));
        m_release = CoefFor((double)Clamp(b.normReleaseMs, kNormReleaseMin, kNormReleaseMax),
                            m_rate);

        const int ms = Clamp(b.normLookaheadMs, kNormLookaheadMin, kNormLookaheadMax);
        const size_t look = (size_t)((long long)m_rate * ms / 1000);

        // Four time constants inside the window, so a reduction has arrived by
        // the time the peak it was worked out for reaches the output.
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

            // Write the new frame, step on, then read the oldest - the slot in
            // front of the one just written. Reading first would delay by the
            // whole buffer instead, one frame too many, and put the peak being
            // looked for outside the window it is looked for in.
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

            // Silence is left alone rather than boosted to the ceiling. Without
            // this the gap between two sentences would be filled with whatever
            // the noise floor is, brought up to the target.
            double target = 1.0;
            if (peak > kMinLevel)
            {
                target = m_target / peak;
                if (target > m_maxGain) target = m_maxGain;
                if (target < 0.0) target = 0.0;
            }
            else
            {
                target = m_gain;      // hold, do not climb, through silence
            }

            // Down at once, up slowly. A peak that is not caught in time is a
            // peak that clips; a gain that rises quickly is one that pumps.
            const double c = (target < m_gain) ? m_attack : m_release;
            m_gain = target + (m_gain - target) * c;

            const float l = (float)(dl * m_gain);
            const float r = (float)(dr * m_gain);

            buf[i * kChannels + 0] = wet * l + (1.0f - wet) * dl;
            buf[i * kChannels + 1] = wet * r + (1.0f - wet) * dr;
        }
    }

private:
    int    m_rate = kDefaultRate;
    double m_target = 1.0;
    double m_maxGain = 1.0;
    double m_attack = 0.0, m_release = 0.0;
    double m_gain = 1.0;

    size_t m_look = (size_t)-1;        // forces the first Configure to allocate
    std::vector<float> m_delay;
    size_t m_pos = 0;

    std::vector<float> m_qVal;
    std::vector<unsigned long long> m_qPos;
    size_t m_qSpan = 0;
    size_t m_head = 0, m_tail = 0;
    unsigned long long m_n = 0;
};

// ---------------------------------------------------------------------------
// Clipping monitor
//
// This one does not touch the audio at all. Process reads the buffer and
// writes nothing back, so the block can sit anywhere in a chain without being
// part of it - which is the point. It is a meter you do not have to be looking
// at, for the case where the first sign of trouble is otherwise a ruined
// recording found afterwards.
//
// The beep goes to the default output device through Beep(), deliberately not
// into the session's own bus. Nobody downstream hears it: not the recording,
// not the stream, not the other end of a call. The one exception is a session
// capturing system audio or capturing this process, which would pick the beep
// up like any other sound on the machine - so a monitor in a session like that
// is worth thinking about twice.
//
// Beep() blocks for its whole duration, which on the audio thread would be an
// underrun every time it fired. So the audio thread only ever sets a flag, and
// a thread of this effect's own does the beeping. That thread polls rather
// than being signalled, because signalling it would mean a system call from
// the render callback to save a few milliseconds nobody is waiting on.
// ---------------------------------------------------------------------------
const int kPollMs = 50;
const int kBeepHz = 1200;    // above the hotkey tones, which mean something else
const int kBeepMs = 120;

class ClipMonitorFx : public Effect
{
public:
    ~ClipMonitorFx() override { StopWorker(); }

    bool Start(int rate, std::wstring* err) override
    {
        (void)err;
        m_rate = rate > 0 ? rate : kDefaultRate;
        m_stop = false;
        m_thread = std::thread([this] { Worker(); });
        return true;
    }

    void Configure(const Block& b) override
    {
        m_thresh = (float)FromDb((double)Clamp(b.clipThreshDb, kClipThreshMin, kClipThreshMax));
        m_intervalMs.store(Clamp(b.clipIntervalS, kClipIntervalMin, kClipIntervalMax) * 1000);
    }

    // Reads the bus and writes nothing back. A monitor that altered the audio
    // would not be a monitor.
    void Process(float* buf, size_t frames) override
    {
        const float t = m_thresh;
        const size_t samples = frames * kChannels;
        for (size_t i = 0; i < samples; ++i)
        {
            const float v = buf[i] < 0.0f ? -buf[i] : buf[i];
            if (v >= t)
            {
                m_seen.store(true, std::memory_order_relaxed);
                m_count.fetch_add(1, std::memory_order_relaxed);
                return;          // one is enough; the rest of the buffer is the same news
            }
        }
    }

    // Bypassing a monitor has to mean it stops watching, not that it carries on
    // beeping silently in the background. Render winds the mix to dry rather
    // than skipping an effect, so dry is what "off" looks like from in here.
    void SetMix(float wet) override
    {
        Effect::SetMix(wet);
        m_watching.store(wet > 0.0f);
    }

private:
    void StopWorker()
    {
        m_stop = true;
        if (m_thread.joinable()) m_thread.join();
    }

    void Worker()
    {
        auto last = std::chrono::steady_clock::now() -
                    std::chrono::milliseconds(kClipIntervalMax * 1000);
        while (!m_stop.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
            if (m_stop.load()) break;
            if (!m_seen.exchange(false)) continue;
            if (!m_watching.load()) continue;

            const auto now = std::chrono::steady_clock::now();
            const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   now - last).count();
            if (since < m_intervalMs.load()) continue;
            last = now;
            Beep(kBeepHz, kBeepMs);
        }
    }

    int   m_rate = kDefaultRate;
    float m_thresh = 1.0f;
    std::atomic<int>  m_intervalMs{ 2000 };
    std::atomic<bool> m_seen{ false };
    std::atomic<bool> m_watching{ true };
    std::atomic<unsigned long long> m_count{ 0 };
    std::atomic<bool> m_stop{ false };
    std::thread m_thread;
};

}  // namespace

std::unique_ptr<Effect> MakeNormaliser()
{
    return std::unique_ptr<Effect>(new NormaliseFx());
}

std::unique_ptr<Effect> MakeClipMonitor()
{
    return std::unique_ptr<Effect>(new ClipMonitorFx());
}
