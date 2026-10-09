#include "audio.h"
#include "asiodev.h"
#include "../model.h"
#include "../util.h"

#include <mutex>

// ---------------------------------------------------------------------------
// ASIO, as the graph sees it
//
// An ASIO driver presents both halves of an interface in one callback, so an
// ASIO input block and an ASIO output block on the same driver are not two
// devices - they are two views of one callback with one clock. That is worth
// arranging properly, because it is the whole reason to prefer ASIO: audio in
// and audio out share a clock exactly, so there is nothing to resample and
// nothing to drift.
//
// The hub below is what they share. Whichever of the two is built first opens
// the driver; the other joins it. Neither owns it, so neither closing order
// is wrong.
//
// Mixing ASIO with WASAPI needs no special arrangement at all. The engine
// already lets the first output own the clock and runs the rest in ring mode,
// which is how two WASAPI outputs have always worked; an ASIO renderer is
// simply another output. So an ASIO input feeding a WASAPI output, or a WASAPI
// input feeding an ASIO output, both work, and go through the same ring and
// drift correction as any other pair of unrelated clocks. Only ASIO to ASIO on
// one driver escapes that, and it escapes it completely.
//
// What does not work is two different ASIO drivers at once. The SDK keeps one
// global driver pointer, and most drivers permit one host anyway.
//
// The sample rate is the one place ASIO is less forgiving than WASAPI. A
// shared-mode WASAPI endpoint resamples to its own mix format for nothing, so
// every WASAPI block lands at the session rate whatever the hardware runs at.
// ASIO does not: it hands over the hardware's rate and that is the end of the
// matter. So the driver is asked to change to the session's rate, and if it
// will not - because it is locked to an external clock, as a rule - that is
// said plainly rather than played back at the wrong speed.
// ---------------------------------------------------------------------------

namespace {

// How much the input side holds. An ASIO input read by an ASIO output on the
// same driver needs none of this, because they are the same callback - but it
// costs one buffer and it is what makes the input also usable by a WASAPI
// output, which is a different clock.
const int kInputBufferMs = 20;

class AsioCapture;

// Defined below, with AsioCapture itself, which needs the hub above.
void PushCapture(AsioCapture* c, const float* in, size_t frames);

// What the two ends share.
struct Hub
{
    std::mutex lock;

    std::wstring driver;
    bool prepared = false;      // the driver is open and has been asked
    bool started = false;       // the callback is running

    bool haveOut = false, haveIn = false;
    int outL = 0, outR = 1;
    int inL = 0, inR = 1;

    // Set when the ASIO renderer is the master clock: the callback pulls the
    // mix straight through, with nothing buffered between it and the hardware.
    Renderer::SourceFn source;

    // Set when it is not, and is fed by whoever is.
    Ring* outRing = nullptr;

    // Where the input half puts what it read, for the input block to collect.
    AsioCapture* capture = nullptr;

    int rate = 0;
};

Hub g_hub;

// Nothing is holding the driver any more, so let it go: a driver left open is
// one no other program can use.
void ReleaseLocked()
{
    if (g_hub.started) { AsioDevice::Instance().Stop(); g_hub.started = false; }
    if (g_hub.prepared) { AsioDevice::Instance().Close(); g_hub.prepared = false; }
    g_hub.driver.clear();
    g_hub.haveOut = g_hub.haveIn = false;
    g_hub.source = nullptr;
    g_hub.outRing = nullptr;
    g_hub.capture = nullptr;
    g_hub.rate = 0;
}

// Opens the driver, or confirms the open one is the right one. The caller
// holds the lock.
bool PrepareLocked(const std::wstring& driver, int wantRate, std::wstring* err)
{
    if (g_hub.prepared)
    {
        if (_wcsicmp(g_hub.driver.c_str(), driver.c_str()) == 0) return true;
        if (err) *err = FormatW(
            L"This session uses two different ASIO drivers, %s and %s. ASIO "
            L"allows one driver at a time, so the blocks in one session have "
            L"to be on the same interface.",
            g_hub.driver.c_str(), driver.c_str());
        return false;
    }

    if (!AsioDevice::Instance().Open(driver, wantRate, err)) return false;
    g_hub.prepared = true;
    g_hub.driver = driver;
    g_hub.rate = AsioDevice::Instance().Rate();
    return true;
}

// Starts the callback once both ends have said what they want. Idempotent,
// because either end may be the one that gets here first.
bool StartLocked(std::wstring* err)
{
    if (g_hub.started) return true;

    AsioDevice& dev = AsioDevice::Instance();

    // A direction with no block gets no buffers prepared, which is what makes
    // an input-only or output-only use of the driver cost nothing extra.
    if (g_hub.haveIn) dev.SetInputPair(g_hub.inL, g_hub.inR);
    else              dev.SetInputPair(-1, -1);
    if (g_hub.haveOut) dev.SetOutputPair(g_hub.outL, g_hub.outR);
    else               dev.SetOutputPair(-1, -1);

    // The callback. It runs on the driver's thread, which is real time: it
    // allocates nothing, waits for nothing, and takes no lock. Everything it
    // reads out of the hub was written before this call.
    if (!dev.Start([](const float* in, float* out, int frames) {
            Hub& h = g_hub;

            if (in && h.capture) PushCapture(h.capture, in, (size_t)frames);

            if (out)
            {
                if (h.source) h.source(out, (size_t)frames);
                else if (h.outRing) h.outRing->Read(out, (size_t)frames);
            }
        }, err))
        return false;

    g_hub.started = true;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// The input end
//
// A ring, like every other capture, so that the rest of the graph cannot tell
// an ASIO input from a microphone. When an ASIO output on the same driver is
// clocking the session the ring is read and written in the same callback and
// never grows past a buffer; when a WASAPI output is clocking it, the ring and
// its drift correction are doing real work, because those are two clocks.
// ---------------------------------------------------------------------------
namespace {

class AsioCapture : public CaptureSource
{
public:
    AsioCapture(const Block& b, int rate)
        : CaptureSource(rate, kInputBufferMs),
          m_driver(b.deviceId), m_left(b.asioInLeft), m_right(b.asioInRight)
    {
    }

    ~AsioCapture() override { Stop(); }

    bool Start(std::wstring* err) override
    {
        std::lock_guard<std::mutex> g(g_hub.lock);

        if (!AsioDriverExists(m_driver))
        {
            if (err) *err = FormatW(
                L"The ASIO driver %s is not installed on this machine.",
                m_driver.c_str());
            return false;
        }
        if (!PrepareLocked(m_driver, m_rate, err)) return false;

        AsioDevice& dev = AsioDevice::Instance();
        if (dev.InputChannels() <= 0)
        {
            if (err) *err = FormatW(
                L"%s has no inputs, so there is nothing for an ASIO input "
                L"block to record.", m_driver.c_str());
            return false;
        }
        if (!RateAgrees(err)) return false;

        g_hub.haveIn = true;
        g_hub.inL = Clamp(m_left, dev.InputChannels());
        g_hub.inR = Clamp(m_right, dev.InputChannels());
        g_hub.capture = this;

        m_packetFrames.store((unsigned)dev.BufferFrames());
        return StartLocked(err);
    }

    void Stop() override
    {
        std::lock_guard<std::mutex> g(g_hub.lock);
        if (g_hub.capture != this) return;
        g_hub.capture = nullptr;
        g_hub.haveIn = false;
        // The output end may still be using the driver, so it is only let go
        // when neither end is.
        if (!g_hub.haveOut) ReleaseLocked();
    }

    std::wstring TargetName() const override
    {
        AsioDevice& dev = AsioDevice::Instance();
        const std::vector<std::wstring> names = dev.ChannelNames(true);
        if (names.empty()) return std::wstring();
        const int l = Clamp(m_left, (int)names.size());
        const int r = Clamp(m_right, (int)names.size());
        if (l == r) return names[(size_t)l];
        return names[(size_t)l] + L" and " + names[(size_t)r];
    }

    // Called from the driver's thread.
    void Push(const float* in, size_t frames) { m_ring.Write(in, frames); }

private:
    static int Clamp(int ch, int count)
    {
        if (count <= 0) return 0;
        return (ch < 0) ? 0 : (ch >= count ? count - 1 : ch);
    }

    // The driver may have refused the session's rate - it is locked to a
    // digital input, as a rule. Saying so is the only honest option: playing
    // 48 kHz audio through a 44.1 kHz clock is not a small error, it is
    // everything most of a semitone flat and steadily falling behind.
    bool RateAgrees(std::wstring* err) const
    {
        const int got = AsioDevice::Instance().Rate();
        if (got == m_rate) return true;
        if (err) *err = FormatW(
            L"%s is running at %d Hz and this session is running at %d Hz, and "
            L"the driver would not change. Set %d Hz in the driver's own "
            L"control panel, or let the ASIO device be the first output block "
            L"in the session so that the session follows it instead.",
            m_driver.c_str(), got, m_rate, m_rate);
        return false;
    }

    std::wstring m_driver;
    int m_left, m_right;
};

void PushCapture(AsioCapture* c, const float* in, size_t frames)
{
    c->Push(in, frames);
}

// ---------------------------------------------------------------------------
// The output end
// ---------------------------------------------------------------------------
class AsioRenderer : public Renderer
{
public:
    ~AsioRenderer() override { Stop(); }

    void Configure(const Block& b) override
    {
        m_left = b.asioOutLeft;
        m_right = b.asioOutRight;
    }

    bool Start(const std::wstring& driver, int rate, SourceFn source,
               std::wstring* err) override
    {
        std::lock_guard<std::mutex> g(g_hub.lock);

        if (!AsioDriverExists(driver))
        {
            if (err) *err = FormatW(
                L"The ASIO driver %s is not installed on this machine.",
                driver.c_str());
            return false;
        }
        if (!PrepareLocked(driver, rate, err)) return false;

        AsioDevice& dev = AsioDevice::Instance();
        if (dev.OutputChannels() <= 0)
        {
            if (err) *err = FormatW(
                L"%s has no outputs, so there is nothing for an ASIO output "
                L"block to play to.", driver.c_str());
            return false;
        }

        // The master is allowed to disagree about the rate, because it is the
        // master: the session follows the hardware. Anything else is not.
        const int got = dev.Rate();
        if (!source && got != rate)
        {
            if (err) *err = FormatW(
                L"%s is running at %d Hz and this session is running at %d Hz, "
                L"and the driver would not change. Set %d Hz in the driver's "
                L"own control panel, or move the ASIO block to the top of the "
                L"outputs so the session follows it instead.",
                driver.c_str(), got, rate, rate);
            return false;
        }

        m_driver = driver;
        m_rate = got;
        g_hub.haveOut = true;
        g_hub.outL = Clamp(m_left, dev.OutputChannels());
        g_hub.outR = Clamp(m_right, dev.OutputChannels());

        if (source)
        {
            g_hub.source = std::move(source);
            g_hub.outRing = nullptr;
        }
        else
        {
            // Ring mode: fed by whichever output owns the clock. Primed with
            // silence so the first read is a cushion rather than a gap.
            g_hub.source = nullptr;
            m_ring.reset(new Ring((size_t)RingFramesFor(got),
                                  (size_t)MaxBacklogFor(got),
                                  (size_t)TargetBacklogFor(got)));
            m_ring->PrimeSilence((size_t)PrimeFramesFor(got));
            g_hub.outRing = m_ring.get();
        }

        m_started = true;
        return StartLocked(err);
    }

    void Stop() override
    {
        std::lock_guard<std::mutex> g(g_hub.lock);
        if (!m_started) return;
        m_started = false;
        g_hub.haveOut = false;
        g_hub.source = nullptr;
        g_hub.outRing = nullptr;
        if (!g_hub.haveIn) ReleaseLocked();
        m_ring.reset();
    }

    void Write(const float* src, size_t frames) override
    {
        if (m_ring) m_ring->Write(src, frames);
    }

    unsigned BufferFrames() const override
    {
        return (unsigned)AsioDevice::Instance().BufferFrames();
    }

    unsigned PeriodFrames() const override
    {
        // ASIO has one number where WASAPI has two: the buffer the driver
        // chose is also the period it calls at.
        return (unsigned)AsioDevice::Instance().BufferFrames();
    }

private:
    static int Clamp(int ch, int count)
    {
        if (count <= 0) return 0;
        return (ch < 0) ? 0 : (ch >= count ? count - 1 : ch);
    }

    std::wstring m_driver;
    int m_left = 0, m_right = 1;
    int m_rate = 0;
    bool m_started = false;
    std::unique_ptr<Ring> m_ring;
};

}  // namespace

std::unique_ptr<Renderer> MakeAsioRenderer()
{
    return std::unique_ptr<Renderer>(new AsioRenderer());
}

std::unique_ptr<CaptureSource> MakeAsioCapture(const Block& b, int rate)
{
    return std::unique_ptr<CaptureSource>(new AsioCapture(b, rate));
}

bool AsioQueryRate(const std::wstring& driver, int* rate)
{
    std::lock_guard<std::mutex> g(g_hub.lock);
    if (!AsioDriverExists(driver)) return false;
    // No rate is asked for: what is wanted is whatever the hardware is doing,
    // because this is the question asked when ASIO is to be the clock.
    if (!PrepareLocked(driver, 0, nullptr)) return false;
    const int got = AsioDevice::Instance().Rate();
    if (got < 4000) return false;
    if (rate) *rate = got;
    return true;
}

void AsioReleaseIfIdle()
{
    std::lock_guard<std::mutex> g(g_hub.lock);
    if (!g_hub.haveIn && !g_hub.haveOut) ReleaseLocked();
}

void AsioQueryDriver(const std::wstring& driver, AsioFacts* out)
{
    if (!out) return;
    *out = AsioFacts();

    std::lock_guard<std::mutex> g(g_hub.lock);
    if (!AsioDriverExists(driver))
    {
        out->why = FormatW(L"%s is not installed on this machine.",
                           driver.c_str());
        return;
    }

    // Whether this leaves the driver open afterwards depends on whether
    // anything was using it before: a running session must not have its
    // driver closed underneath it by a dialog merely looking.
    const bool wasInUse = (g_hub.haveIn || g_hub.haveOut);

    std::wstring err;
    if (!PrepareLocked(driver, 0, &err))
    {
        out->why = err.empty() ? FormatW(L"%s could not be opened.", driver.c_str())
                               : err;
        return;
    }

    AsioDevice& dev = AsioDevice::Instance();
    out->ok = true;
    out->rate = dev.Rate();
    out->bufferFrames = dev.BufferFrames();
    out->inNames = dev.ChannelNames(true);
    out->outNames = dev.ChannelNames(false);

    if (!wasInUse) ReleaseLocked();
}

void AsioShowControlPanel(const std::wstring& driver, HWND parent)
{
    std::lock_guard<std::mutex> g(g_hub.lock);
    if (!AsioDriverExists(driver)) return;

    const bool wasInUse = (g_hub.haveIn || g_hub.haveOut);
    if (!PrepareLocked(driver, 0, nullptr)) return;

    AsioDevice::Instance().ShowControlPanel(parent);

    // The panel is the driver's own window and it may well be modal to
    // itself, so by the time this returns the person has finished with it.
    if (!wasInUse) ReleaseLocked();
}
