#include "audio.h"

#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------------------
// Tone, graphic EQ and channel mixing
//
// All three are sample-by-sample and add no latency, so the dry signal stays
// aligned with the wet one and the mix control is a straight crossfade. That
// matters more than it sounds: the engine bypasses an effect by winding its
// mix to dry rather than skipping it, which keeps the filter state running and
// makes enable and disable click free.
// ---------------------------------------------------------------------------

namespace {

// Direct form I biquad with one set of state per channel. The coefficients are
// only written from Configure(), which the engine calls holding the same lock
// Render() takes, so the audio thread never sees a half-updated filter.
struct Biquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double x1[kChannels] = { 0.0 }, x2[kChannels] = { 0.0 };
    double y1[kChannels] = { 0.0 }, y2[kChannels] = { 0.0 };
    bool   flat = true;          // 0 dB, or a centre frequency past Nyquist

    float Run(float in, int c)
    {
        if (flat) return in;
        const double x = in;
        double y = b0 * x + b1 * x1[c] + b2 * x2[c] - a1 * y1[c] - a2 * y2[c];
        // A biquad fed digital silence rings down towards denormals, which on
        // x86 cost hundreds of cycles each. This threshold sits below -400 dB,
        // so flushing it to zero is inaudible by an enormous margin.
        if (y > -1e-25 && y < 1e-25) y = 0.0;
        x2[c] = x1[c]; x1[c] = x;
        y2[c] = y1[c]; y1[c] = y;
        return (float)y;
    }
};

enum class Shape { Peak, LowShelf, HighShelf, LowPass, HighPass };

// Audio EQ Cookbook, Robert Bristow-Johnson. `shape` picks the response;
// `qOrS` is Q for a peak and the shelf slope S for the two shelves.
void Design(Biquad& q, Shape shape, double f0, double qOrS, double gainDb, int rate)
{
    q.flat = true;
    if (rate <= 0) return;
    // A peak or a shelf at 0 dB is a straight wire, and saying so here is what
    // lets the callers skip it per sample. A low or high pass is not: it does
    // its work regardless of any gain, so it is never flat on that account.
    const bool usesGain = (shape == Shape::Peak || shape == Shape::LowShelf ||
                           shape == Shape::HighShelf);
    if (usesGain && gainDb == 0.0) return;
    // A filter centred at or above Nyquist has no band left to work on. This
    // is what drops the 16 kHz slider on a 32 kHz endpoint, rather than
    // letting it fold back as something audible and wrong.
    if (f0 <= 0.0 || f0 >= rate * 0.49) return;

    const double A  = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * M_PI * f0 / (double)rate;
    const double cs = std::cos(w0), sn = std::sin(w0);
    // The two shelves take a slope rather than a Q, which is a different
    // formula; everything else here is an ordinary Q.
    const bool shelf = (shape == Shape::LowShelf || shape == Shape::HighShelf);
    const double alpha = shelf
        ? sn / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / qOrS - 1.0) + 2.0)
        : sn / (2.0 * qOrS);
    const double sqA2 = 2.0 * std::sqrt(A) * alpha;

    double b0, b1, b2, a0, a1, a2;
    switch (shape)
    {
    case Shape::LowShelf:
        b0 =        A * ((A + 1.0) - (A - 1.0) * cs + sqA2);
        b1 =  2.0 * A * ((A - 1.0) - (A + 1.0) * cs);
        b2 =        A * ((A + 1.0) - (A - 1.0) * cs - sqA2);
        a0 =             (A + 1.0) + (A - 1.0) * cs + sqA2;
        a1 =     -2.0 * ((A - 1.0) + (A + 1.0) * cs);
        a2 =             (A + 1.0) + (A - 1.0) * cs - sqA2;
        break;
    case Shape::HighShelf:
        b0 =        A * ((A + 1.0) + (A - 1.0) * cs + sqA2);
        b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cs);
        b2 =        A * ((A + 1.0) + (A - 1.0) * cs - sqA2);
        a0 =             (A + 1.0) - (A - 1.0) * cs + sqA2;
        a1 =      2.0 * ((A - 1.0) - (A + 1.0) * cs);
        a2 =             (A + 1.0) - (A - 1.0) * cs - sqA2;
        break;
    case Shape::LowPass:
        b0 = (1.0 - cs) / 2.0;
        b1 =  1.0 - cs;
        b2 = (1.0 - cs) / 2.0;
        a0 =  1.0 + alpha;
        a1 = -2.0 * cs;
        a2 =  1.0 - alpha;
        break;
    case Shape::HighPass:
        b0 =  (1.0 + cs) / 2.0;
        b1 = -(1.0 + cs);
        b2 =  (1.0 + cs) / 2.0;
        a0 =   1.0 + alpha;
        a1 =  -2.0 * cs;
        a2 =   1.0 - alpha;
        break;
    default:
        b0 =  1.0 + alpha * A;
        b1 = -2.0 * cs;
        b2 =  1.0 - alpha * A;
        a0 =  1.0 + alpha / A;
        a1 = -2.0 * cs;
        a2 =  1.0 - alpha / A;
        break;
    }
    if (a0 == 0.0) return;

    q.b0 = b0 / a0; q.b1 = b1 / a0; q.b2 = b2 / a0;
    q.a1 = a1 / a0; q.a2 = a2 / a0;
    q.flat = false;
}

int ClampDb(int db, int limit)
{
    if (db < -limit) return -limit;
    if (db >  limit) return  limit;
    return db;
}

// ---------------------------------------------------------------------------
// Bass and treble: one low shelf and one high shelf.
//
// The corner frequencies are the ones a hi-fi tone control uses, which is the
// point of having this block at all when there is a ten band EQ next to it:
// two controls that do the obvious thing.
// ---------------------------------------------------------------------------
class ToneFx : public Effect
{
public:
    bool Start(int rate, std::wstring* err) override
    {
        (void)err;
        m_rate = rate;
        return true;
    }

    void Configure(const Block& b) override
    {
        Design(m_low,  Shape::LowShelf,   100.0, 0.9, ClampDb(b.bassDb, 15),   m_rate);
        Design(m_high, Shape::HighShelf, 4000.0, 0.9, ClampDb(b.trebleDb, 15), m_rate);
    }

    void Process(float* buf, size_t frames) override
    {
        if (m_low.flat && m_high.flat) return;
        const float wet = m_wet.load();

        for (size_t i = 0; i < frames; ++i)
            for (int c = 0; c < kChannels; ++c)
            {
                const float dry = buf[i * kChannels + c];
                const float v = m_high.Run(m_low.Run(dry, c), c);
                buf[i * kChannels + c] = wet * v + (1.0f - wet) * dry;
            }
    }

private:
    Biquad m_low, m_high;
    int m_rate = kDefaultRate;
};

// ---------------------------------------------------------------------------
// Ten band graphic equaliser, one peaking filter per octave.
// ---------------------------------------------------------------------------
class Eq10Fx : public Effect
{
public:
    bool Start(int rate, std::wstring* err) override
    {
        (void)err;
        m_rate = rate;
        return true;
    }

    void Configure(const Block& b) override
    {
        // Q 1.5 is the flattest compromise for octave spaced bands in series.
        // Wider and the skirts of neighbouring bands pile up; narrower and the
        // response dips between the centres. Swept across 0.8 to 4.0, with
        // every slider at +6 dB, 1.5 is where the worst deviation bottoms out
        // at about 2.5 dB. It does not reach zero: like any graphic EQ built
        // as a cascade, pushing every band up by G raises the overall level by
        // rather more than G. A Gain block below the EQ is the way to trim it.
        for (int i = 0; i < kEqBands; ++i)
            Design(m_band[i], Shape::Peak, (double)EqBandHz()[i], 1.5,
                   ClampDb(b.eqDb[i], 12), m_rate);

        // Only the bands actually in use are walked per sample, so a flat EQ
        // costs one comparison per buffer.
        m_active = 0;
        for (int i = 0; i < kEqBands; ++i)
            if (!m_band[i].flat) m_order[m_active++] = i;
    }

    void Process(float* buf, size_t frames) override
    {
        if (m_active == 0) return;
        const float wet = m_wet.load();

        for (size_t i = 0; i < frames; ++i)
            for (int c = 0; c < kChannels; ++c)
            {
                const float dry = buf[i * kChannels + c];
                float v = dry;
                for (int k = 0; k < m_active; ++k) v = m_band[m_order[k]].Run(v, c);
                buf[i * kChannels + c] = wet * v + (1.0f - wet) * dry;
            }
    }

private:
    Biquad m_band[kEqBands];
    int m_order[kEqBands] = { 0 };   // indices of the bands that are not flat
    int m_active = 0;
    int m_rate = kDefaultRate;
};

// ---------------------------------------------------------------------------
// Four band parametric equaliser
//
// Where the graphic EQ has ten fixed bands with a level each, this has four
// bands that are each a whole filter: a shape, a frequency, a gain and a Q.
// That is what a parametric EQ is for - taking out one ring at 3.2 kHz, or
// rolling off everything below 80 Hz, neither of which a fixed grid can do.
//
// The shapes are the ones a mixing desk offers. Peak and the two shelves use
// the gain; the two pass filters do not, and are never flat on account of it.
// ---------------------------------------------------------------------------
class ParaEqFx : public Effect
{
public:
    bool Start(int rate, std::wstring* err) override
    {
        (void)err;
        m_rate = rate;
        return true;
    }

    void Configure(const Block& b) override
    {
        m_active = 0;
        for (int i = 0; i < kParaBands; ++i)
        {
            const int type = b.paraType[i];
            if (type == PT_OFF) { m_band[i].flat = true; continue; }

            Shape shape = Shape::Peak;
            switch (type)
            {
            case PT_LOW_SHELF:  shape = Shape::LowShelf;  break;
            case PT_HIGH_SHELF: shape = Shape::HighShelf; break;
            case PT_LOW_PASS:   shape = Shape::LowPass;   break;
            case PT_HIGH_PASS:  shape = Shape::HighPass;  break;
            default:            shape = Shape::Peak;      break;
            }

            int hz = b.paraHz[i];
            if (hz < kParaHzMin) hz = kParaHzMin;
            if (hz > kParaHzMax) hz = kParaHzMax;

            int q = b.paraQ[i];
            if (q < kParaQMin) q = kParaQMin;
            if (q > kParaQMax) q = kParaQMax;

            // The shelves take a slope rather than a Q, and a slope above 1
            // makes the response ring rather than steepen, so it is held
            // there. The dialog still shows the Q, because that is the number
            // the band was set with.
            double qOrS = (double)q / 10.0;
            if (shape == Shape::LowShelf || shape == Shape::HighShelf)
                qOrS = qOrS > 1.0 ? 1.0 : qOrS;

            Design(m_band[i], shape, (double)hz, qOrS,
                   ClampDb(b.paraGainDb[i], 18), m_rate);
        }

        for (int i = 0; i < kParaBands; ++i)
            if (!m_band[i].flat) m_order[m_active++] = i;
    }

    void Process(float* buf, size_t frames) override
    {
        if (m_active == 0) return;
        const float wet = m_wet.load();

        for (size_t i = 0; i < frames; ++i)
            for (int c = 0; c < kChannels; ++c)
            {
                const float dry = buf[i * kChannels + c];
                float v = dry;
                for (int k = 0; k < m_active; ++k) v = m_band[m_order[k]].Run(v, c);
                buf[i * kChannels + c] = wet * v + (1.0f - wet) * dry;
            }
    }

private:
    Biquad m_band[kParaBands];
    int m_order[kParaBands] = { 0 };
    int m_active = 0;
    int m_rate = kDefaultRate;
};

// ---------------------------------------------------------------------------
// Channel mixer: per channel volume and pan, then stereo width, channel
// reversal and centre cancellation.
//
// Two stages, in that order. First each incoming channel is given a level and
// a place in the stereo field, which is a 2x2 matrix - how much of each input
// reaches each output. Then width and centre cancel shape whatever image that
// produced, as one mid/side decomposition: width scales the side signal, 0%
// collapsing to mono, 100% untouched, 200% doubling the difference between
// the channels; centre cancel drops the mid entirely, which is what removes
// anything panned dead centre - usually the lead vocal - and leaves the two
// channels out of phase with each other.
//
// Volume and pan go first because they describe the source and width and
// centre describe the result. Panning both channels to the middle and then
// cancelling the centre leaves silence, which is the arithmetic agreeing with
// what the two controls say they do.
//
// The pan law is constant power: half the power to each side at the centre,
// full scale at either extreme. That means the default - each channel at 0 dB
// on its own side - multiplies by exactly one and this stage is the identity,
// so a mixer block that has only ever had its width touched is bit for bit
// what it was before any of this existed.
// ---------------------------------------------------------------------------
class ChannelFx : public Effect
{
public:
    bool Start(int rate, std::wstring* err) override
    {
        (void)rate; (void)err;
        return true;
    }

    void Configure(const Block& b) override
    {
        int w = b.widthPct;
        if (w < 0) w = 0;
        if (w > 200) w = 200;
        m_width.store((float)w / 100.0f);
        m_swap.store(b.swapChannels);
        m_centre.store(b.centreCancel);

        // Folded into the matrix here rather than per frame, so however the
        // controls are set Process stays at two multiply-adds a frame.
        for (int c = 0; c < kMixChannels; ++c)
        {
            int db = b.chanGainDb[c];
            if (db < kChanGainMin) db = kChanGainMin;
            if (db > kChanGainMax) db = kChanGainMax;
            const float g = db <= kChanGainMin
                ? 0.0f                       // the bottom of the slider is off
                : (float)std::pow(10.0, db / 20.0);

            int p = b.chanPan[c];
            if (p < kChanPanMin) p = kChanPanMin;
            if (p > kChanPanMax) p = kChanPanMax;

            // The extremes are written down rather than computed: cos(pi/2) is
            // not zero in floating point, and a channel panned hard over that
            // still leaks a hundred-millionth of itself into the other side is
            // not hard over, and is not the identity either.
            float toL, toR;
            if (p <= kChanPanMin)      { toL = 1.0f; toR = 0.0f; }
            else if (p >= kChanPanMax) { toL = 0.0f; toR = 1.0f; }
            else
            {
                const double a = ((p - kChanPanMin) /
                                  (double)(kChanPanMax - kChanPanMin)) * (M_PI / 2.0);
                toL = (float)std::cos(a);
                toR = (float)std::sin(a);
            }

            m_toL[c].store(g * toL);
            m_toR[c].store(g * toR);
        }
    }

    void Process(float* buf, size_t frames) override
    {
        const float width = m_width.load();
        const bool swap = m_swap.load(), cut = m_centre.load();
        const float lToL = m_toL[0].load(), lToR = m_toR[0].load();
        const float rToL = m_toL[1].load(), rToR = m_toR[1].load();

        // The identity, which is what most of these blocks are: the matrix
        // compares exactly because the extremes were written down rather than
        // arrived at by trigonometry.
        if (!swap && !cut && width == 1.0f &&
            lToL == 1.0f && lToR == 0.0f && rToL == 0.0f && rToR == 1.0f)
            return;

        const float wet = m_wet.load();

        for (size_t i = 0; i < frames; ++i)
        {
            const float dl = buf[i * kChannels + 0];
            const float dr = buf[i * kChannels + 1];

            const float il = swap ? dr : dl;
            const float ir = swap ? dl : dr;

            float l = il * lToL + ir * rToL;
            float r = il * lToR + ir * rToR;

            const float mid  = cut ? 0.0f : (l + r) * 0.5f;
            const float side = (l - r) * 0.5f * width;

            l = mid + side;
            r = mid - side;

            buf[i * kChannels + 0] = wet * l + (1.0f - wet) * dl;
            buf[i * kChannels + 1] = wet * r + (1.0f - wet) * dr;
        }
    }

private:
    std::atomic<float> m_width{ 1.0f };
    std::atomic<bool>  m_swap{ false };
    std::atomic<bool>  m_centre{ false };
    // What each input channel contributes to each output channel, volume and
    // pan already multiplied together. Index is the input channel.
    std::atomic<float> m_toL[kMixChannels] = { { 1.0f }, { 0.0f } };
    std::atomic<float> m_toR[kMixChannels] = { { 0.0f }, { 1.0f } };
};

}   // namespace

std::unique_ptr<Effect> MakeToneControl()  { return std::unique_ptr<Effect>(new ToneFx()); }
std::unique_ptr<Effect> MakeEqualiser()    { return std::unique_ptr<Effect>(new Eq10Fx()); }
std::unique_ptr<Effect> MakeParametricEq(){ return std::unique_ptr<Effect>(new ParaEqFx()); }
std::unique_ptr<Effect> MakeChannelMixer() { return std::unique_ptr<Effect>(new ChannelFx()); }

std::unique_ptr<Effect> MakeEffectFor(const std::wstring& typeId)
{
    if (typeId == BT_FX_SPEEX)    return MakeSpeexDenoise();
    if (typeId == BT_FX_RNNOISE)  return MakeRnnDenoise();
    if (typeId == BT_FX_TONE)     return MakeToneControl();
    if (typeId == BT_FX_EQ10)     return MakeEqualiser();
    if (typeId == BT_FX_PARA)     return MakeParametricEq();
    if (typeId == BT_FX_CHANNELS) return MakeChannelMixer();
    if (typeId == BT_FX_COMP)     return MakeCompressor();
    if (typeId == BT_FX_LIMIT)    return MakeLimiter();
    if (typeId == BT_FX_GATE)     return MakeNoiseGate();
    if (typeId == BT_FX_PITCH)    return MakePitchShift();
    if (typeId == BT_FX_NORM)     return MakeNormaliser();
    if (typeId == BT_FX_CLIPMON)  return MakeClipMonitor();
    if (typeId == BT_FX_BUFFER)   return MakeBuffer();
    if (typeId == BT_FX_PLUGIN)   return MakePluginEffect();
    return nullptr;
}
