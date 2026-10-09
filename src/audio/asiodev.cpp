#include "asiodev.h"
#include "util.h"

// The SDK's host helpers. These are the BSD licensed part of it; the interface
// itself, in common/, is the part that makes FastRoute GPLv3.
#include <asiodrivers.h>
#include <asio.h>

// Declared in asiodrivers.cpp and in no header, so it is declared here. It
// has to be this rather than an AsioDrivers of our own: it is what creates
// the SDK's global asioDrivers pointer, and ASIOExit dereferences that
// pointer unconditionally. Loading a driver through a private instance left
// the global null, so the first successful close took the process down with
// it - and only a successful one, because a driver that fails ASIOInit has
// theAsioDriver zeroed by ASIOInit itself and never reaches the dereference.
extern bool loadAsioDriver(char* name);

#include <atomic>
#include <cmath>
#include <mutex>

// ---------------------------------------------------------------------------
// Enumeration
//
// AsioDriverList reads the registry and nothing else, which is what makes this
// safe to call from a dialog: no driver is loaded, so nothing takes the audio
// hardware away from whatever is using it. Loading a driver to ask it anything
// is a different matter entirely and only happens in Open below.
// ---------------------------------------------------------------------------
namespace {

// The SDK is all narrow strings, and a driver name is whatever the
// manufacturer registered - which on a non-English machine may not be ASCII.
std::wstring Widen(const char* s)
{
    if (!s || !*s) return std::wstring();
    const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring out((size_t)(n - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, &out[0], n);
    return out;
}

std::string Narrow(const std::wstring& s)
{
    if (s.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_ACP, 0, s.c_str(), -1, nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string out((size_t)(n - 1), '\0');
    WideCharToMultiByte(CP_ACP, 0, s.c_str(), -1, &out[0], n, nullptr, nullptr);
    return out;
}

}  // namespace

namespace {
// Not inside Dev: it outlives any one open driver, and it is set before the
// first one is opened.
HWND g_host = nullptr;
}

void SetAsioHostWindow(HWND host) { g_host = host; }

std::vector<std::wstring> EnumAsioDrivers()
{
    std::vector<std::wstring> out;

    AsioDriverList list;
    const long count = list.asioGetNumDev();
    for (long i = 0; i < count; ++i)
    {
        char name[128] = {};
        if (list.asioGetDriverName((int)i, name, (int)sizeof(name) - 1) != 0)
            continue;
        const std::wstring w = Widen(name);
        if (!w.empty()) out.push_back(w);
    }
    return out;
}

bool AsioDriverExists(const std::wstring& name)
{
    if (name.empty()) return false;
    for (const std::wstring& d : EnumAsioDrivers())
        if (_wcsicmp(d.c_str(), name.c_str()) == 0) return true;
    return false;
}

// ---------------------------------------------------------------------------
// The device
//
// One per process, because the SDK keeps one global driver pointer and the
// callbacks it hands the driver are plain C function pointers with nowhere to
// put a `this`. Both of those point the same way: a singleton, and a driver
// either open or not.
// ---------------------------------------------------------------------------
namespace {

// Everything the open driver is, in one place so that the callback can read it
// without locking. The callback runs on the driver's own real-time thread; it
// may not take a lock that the UI thread holds, so what it needs is written
// before Start and not touched again until after Stop.
struct Dev
{
    bool loaded = false;
    bool buffers = false;
    bool running = false;
    std::wstring name;

    long inChannels = 0, outChannels = 0;
    long bufferFrames = 0;
    double rate = 0.0;
    long inLatency = 0, outLatency = 0;
    bool outputReady = false;        // the driver wants ASIOOutputReady calls

    std::vector<std::wstring> inNames, outNames;
    std::vector<long> inTypes, outTypes;

    // Which device channels the stereo bus maps to. -1 for "none", which is
    // what a driver with no inputs leaves the input pair as.
    int inPair[kChannels] = { -1, -1 };
    int outPair[kChannels] = { -1, -1 };

    // The buffers the driver gave us, two per prepared channel. Only the
    // chosen pairs are prepared: a thirty-two channel interface would
    // otherwise have sixty-four buffers churning for two channels of use.
    std::vector<ASIOBufferInfo> infos;
    // Where in `infos` each half of each pair landed.
    int inIndex[kChannels] = { -1, -1 };
    int outIndex[kChannels] = { -1, -1 };

    AsioDevice::Callback cb;
    std::vector<float> scratchIn, scratchOut;
    std::atomic<unsigned> overloads{ 0 };
    std::atomic<bool> resetWanted{ false };

    std::mutex lock;       // guards opening and closing, never the callback
};

Dev g_dev;

const wchar_t* ErrText(ASIOError e)
{
    switch (e)
    {
    case ASE_OK:               return L"no error";
    case ASE_SUCCESS:          return L"done";
    case ASE_NotPresent:        return L"the hardware is not there";
    case ASE_HWMalfunction:     return L"the hardware is not working";
    case ASE_InvalidParameter:  return L"it did not accept that";
    case ASE_InvalidMode:       return L"not in that mode";
    case ASE_SPNotAdvancing:    return L"the driver has stalled";
    case ASE_NoClock:           return L"there is no clock";
    case ASE_NoMemory:          return L"the driver ran out of memory";
    default:                    return L"an unknown error";
    }
}

// --- the driver's samples, and ours ----------------------------------------
//
// ASIO hands over whatever the hardware uses, which on a PC is almost always
// 32 bit integers but is allowed to be any of a dozen things. Only the ones
// that actually turn up are handled, and anything else is refused when the
// driver is opened rather than producing noise when it is started.
bool TypeSupported(long t)
{
    switch (t)
    {
    case ASIOSTInt16LSB:
    case ASIOSTInt24LSB:
    case ASIOSTInt32LSB:
    case ASIOSTFloat32LSB:
    case ASIOSTFloat64LSB:
    case ASIOSTInt32LSB16:
    case ASIOSTInt32LSB18:
    case ASIOSTInt32LSB20:
    case ASIOSTInt32LSB24:
        return true;
    default:
        return false;
    }
}

const wchar_t* TypeName(long t)
{
    switch (t)
    {
    case ASIOSTInt16LSB:    return L"16 bit";
    case ASIOSTInt24LSB:    return L"24 bit packed";
    case ASIOSTInt32LSB:    return L"32 bit";
    case ASIOSTFloat32LSB:  return L"32 bit float";
    case ASIOSTFloat64LSB:  return L"64 bit float";
    case ASIOSTInt32LSB16:  return L"16 bit in 32";
    case ASIOSTInt32LSB18:  return L"18 bit in 32";
    case ASIOSTInt32LSB20:  return L"20 bit in 32";
    case ASIOSTInt32LSB24:  return L"24 bit in 32";
    default:                return L"an unsupported format";
    }
}

// Scale for the integer types, as the largest positive value.
double FullScale(long t)
{
    switch (t)
    {
    case ASIOSTInt16LSB:    return 32767.0;
    case ASIOSTInt24LSB:    return 8388607.0;
    case ASIOSTInt32LSB:    return 2147483647.0;
    case ASIOSTInt32LSB16:  return 32767.0;
    case ASIOSTInt32LSB18:  return 131071.0;
    case ASIOSTInt32LSB20:  return 524287.0;
    case ASIOSTInt32LSB24:  return 8388607.0;
    default:                return 1.0;
    }
}

float Clamp1(float v) { return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v); }

// One channel of the driver's buffer into one channel of an interleaved
// stereo float block.
void ToFloat(long type, const void* src, float* dst, int frames, int stride)
{
    const double scale = 1.0 / FullScale(type);
    switch (type)
    {
    case ASIOSTInt16LSB:
    {
        const short* p = static_cast<const short*>(src);
        for (int i = 0; i < frames; ++i) dst[i * stride] = (float)(p[i] * scale);
        break;
    }
    case ASIOSTInt24LSB:
    {
        const unsigned char* p = static_cast<const unsigned char*>(src);
        for (int i = 0; i < frames; ++i)
        {
            int v = p[i * 3] | (p[i * 3 + 1] << 8) | (p[i * 3 + 2] << 16);
            if (v & 0x800000) v -= 0x1000000;
            dst[i * stride] = (float)(v * scale);
        }
        break;
    }
    case ASIOSTInt32LSB:
    case ASIOSTInt32LSB16:
    case ASIOSTInt32LSB18:
    case ASIOSTInt32LSB20:
    case ASIOSTInt32LSB24:
    {
        // The xx variants are right aligned in a 32 bit word, so the scale
        // above is the one that matters and the value is read whole.
        const int* p = static_cast<const int*>(src);
        for (int i = 0; i < frames; ++i) dst[i * stride] = (float)(p[i] * scale);
        break;
    }
    case ASIOSTFloat32LSB:
    {
        const float* p = static_cast<const float*>(src);
        for (int i = 0; i < frames; ++i) dst[i * stride] = p[i];
        break;
    }
    case ASIOSTFloat64LSB:
    {
        const double* p = static_cast<const double*>(src);
        for (int i = 0; i < frames; ++i) dst[i * stride] = (float)p[i];
        break;
    }
    default:
        for (int i = 0; i < frames; ++i) dst[i * stride] = 0.0f;
        break;
    }
}

void FromFloat(long type, const float* src, void* dst, int frames, int stride)
{
    const double scale = FullScale(type);
    switch (type)
    {
    case ASIOSTInt16LSB:
    {
        short* p = static_cast<short*>(dst);
        for (int i = 0; i < frames; ++i)
            p[i] = (short)lrint(Clamp1(src[i * stride]) * scale);
        break;
    }
    case ASIOSTInt24LSB:
    {
        unsigned char* p = static_cast<unsigned char*>(dst);
        for (int i = 0; i < frames; ++i)
        {
            const int v = (int)lrint(Clamp1(src[i * stride]) * scale);
            p[i * 3 + 0] = (unsigned char)(v & 0xFF);
            p[i * 3 + 1] = (unsigned char)((v >> 8) & 0xFF);
            p[i * 3 + 2] = (unsigned char)((v >> 16) & 0xFF);
        }
        break;
    }
    case ASIOSTInt32LSB:
    case ASIOSTInt32LSB16:
    case ASIOSTInt32LSB18:
    case ASIOSTInt32LSB20:
    case ASIOSTInt32LSB24:
    {
        int* p = static_cast<int*>(dst);
        for (int i = 0; i < frames; ++i)
            p[i] = (int)llrint((double)Clamp1(src[i * stride]) * scale);
        break;
    }
    case ASIOSTFloat32LSB:
    {
        float* p = static_cast<float*>(dst);
        for (int i = 0; i < frames; ++i) p[i] = src[i * stride];
        break;
    }
    case ASIOSTFloat64LSB:
    {
        double* p = static_cast<double*>(dst);
        for (int i = 0; i < frames; ++i) p[i] = src[i * stride];
        break;
    }
    default:
        break;
    }
}

// --- what the driver calls -------------------------------------------------

void OnBufferSwitch(long index, ASIOBool /*processNow*/)
{
    Dev& d = g_dev;
    if (!d.running || !d.cb) return;
    const int frames = (int)d.bufferFrames;

    // Input first, into interleaved stereo. A driver with no inputs, or a
    // block that wants none, leaves this as null and the callback is told so.
    float* in = nullptr;
    if (d.inIndex[0] >= 0)
    {
        in = d.scratchIn.data();
        for (int c = 0; c < kChannels; ++c)
        {
            const int at = d.inIndex[c] >= 0 ? d.inIndex[c] : d.inIndex[0];
            const long type = d.inTypes[(size_t)d.infos[(size_t)at].channelNum];
            ToFloat(type, d.infos[(size_t)at].buffers[index], in + c, frames,
                    kChannels);
        }
    }

    float* out = d.outIndex[0] >= 0 ? d.scratchOut.data() : nullptr;
    if (out) memset(out, 0, (size_t)frames * kChannels * sizeof(float));

    d.cb(in, out, frames);

    if (out)
    {
        for (int c = 0; c < kChannels; ++c)
        {
            const int at = d.outIndex[c];
            if (at < 0) continue;
            const long type = d.outTypes[(size_t)d.infos[(size_t)at].channelNum];
            FromFloat(type, out + c, d.infos[(size_t)at].buffers[index], frames,
                      kChannels);
        }
    }

    // Some drivers want telling that the output is ready; the ones that do
    // said so when the buffers were created.
    if (d.outputReady) ASIOOutputReady();
}

ASIOTime* OnBufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool processNow)
{
    OnBufferSwitch(index, processNow);
    return params;
}

void OnSampleRateChanged(ASIOSampleRate rate)
{
    // The hardware was reclocked underneath us - a digital input changing, as
    // a rule. The session was built for the old rate, so this is noted and the
    // graph is asked to rebuild rather than carrying on at the wrong speed.
    g_dev.rate = rate;
    g_dev.resetWanted.store(true);
}

long OnMessage(long selector, long value, void* /*message*/, double* /*opt*/)
{
    switch (selector)
    {
    case kAsioSelectorSupported:
        switch (value)
        {
        case kAsioEngineVersion:
        case kAsioResetRequest:
        case kAsioResyncRequest:
        case kAsioLatenciesChanged:
        case kAsioSupportsTimeInfo:
        case kAsioOverload:
            return 1;
        default:
            return 0;
        }
    case kAsioEngineVersion:
        return 2;
    case kAsioResetRequest:
    case kAsioResyncRequest:
    case kAsioLatenciesChanged:
        // All three mean "what you were told is no longer true". Rebuilding
        // from inside the driver's own callback is not allowed, so it is
        // recorded and the session rebuilds on its own thread.
        g_dev.resetWanted.store(true);
        return 1;
    case kAsioSupportsTimeInfo:
        return 1;
    case kAsioOverload:
        g_dev.overloads.fetch_add(1);
        return 1;
    default:
        return 0;
    }
}

ASIOCallbacks g_callbacks = {
    OnBufferSwitch,
    OnSampleRateChanged,
    OnMessage,
    OnBufferSwitchTimeInfo,
};

// Defined further down, beside the rest of the teardown, but needed by Open
// to unwind a half-finished one.
void CloseLocked();

}  // namespace

// ---------------------------------------------------------------------------

AsioDevice& AsioDevice::Instance()
{
    static AsioDevice one;
    return one;
}

bool AsioDevice::Open() const { return g_dev.loaded; }
std::wstring AsioDevice::Name() const { return g_dev.name; }
int AsioDevice::Rate() const { return (int)(g_dev.rate + 0.5); }
int AsioDevice::BufferFrames() const { return (int)g_dev.bufferFrames; }
int AsioDevice::InputChannels() const { return (int)g_dev.inChannels; }
int AsioDevice::OutputChannels() const { return (int)g_dev.outChannels; }
bool AsioDevice::Running() const { return g_dev.running; }
unsigned AsioDevice::Overloads() const { return g_dev.overloads.load(); }

std::vector<std::wstring> AsioDevice::ChannelNames(bool input) const
{
    return input ? g_dev.inNames : g_dev.outNames;
}

void AsioDevice::SetInputPair(int left, int right)
{
    g_dev.inPair[0] = left;
    g_dev.inPair[1] = right;
}

void AsioDevice::SetOutputPair(int left, int right)
{
    g_dev.outPair[0] = left;
    g_dev.outPair[1] = right;
}

bool AsioDevice::Open(const std::wstring& driver, int wantRate, std::wstring* err)
{
    std::lock_guard<std::mutex> g(g_dev.lock);

    if (g_dev.loaded)
    {
        if (_wcsicmp(g_dev.name.c_str(), driver.c_str()) == 0) return true;
        if (err) *err = FormatW(
            L"%s is already open, and an ASIO driver can only be used by one "
            L"thing at a time. A session can have ASIO blocks for one device, "
            L"not for two.", g_dev.name.c_str());
        return false;
    }

    std::string narrow = Narrow(driver);
    if (narrow.empty() || !loadAsioDriver(&narrow[0]))
    {
        if (err) *err = FormatW(
            L"%s could not be loaded. It may have been uninstalled, or it may "
            L"be in use by another program - most ASIO drivers allow only one.",
            driver.c_str());
        return false;
    }
    g_dev.loaded = true;
    g_dev.name = driver;

    ASIODriverInfo info = {};
    info.asioVersion = 2;
    info.sysRef = g_host;
    ASIOError e = ASIOInit(&info);
    if (e != ASE_OK)
    {
        if (err) *err = FormatW(L"%s would not start: %s.", driver.c_str(),
                                ErrText(e));
        CloseLocked();
        return false;
    }

    if ((e = ASIOGetChannels(&g_dev.inChannels, &g_dev.outChannels)) != ASE_OK)
    {
        if (err) *err = FormatW(L"%s would not say how many channels it has: %s.",
                                driver.c_str(), ErrText(e));
        CloseLocked();
        return false;
    }

    // 1024 is far past any real interface and well short of a number that
    // would do damage as a buffer size.
    const long kMaxChannels = 1024;
    if (g_dev.inChannels < 0 || g_dev.outChannels < 0 ||
        g_dev.inChannels > kMaxChannels || g_dev.outChannels > kMaxChannels)
    {
        if (err) *err = FormatW(
            L"%s reports %ld inputs and %ld outputs, which cannot be right. "
            L"FastRoute will not use it rather than act on that.",
            driver.c_str(), g_dev.inChannels, g_dev.outChannels);
        CloseLocked();
        return false;
    }
    if (g_dev.inChannels == 0 && g_dev.outChannels == 0)
    {
        if (err) *err = FormatW(
            L"%s reports no inputs and no outputs, so there is nothing to "
            L"use it for. It may need its hardware connected.", driver.c_str());
        CloseLocked();
        return false;
    }

    // The rate is asked for, not insisted on. A driver locked to an external
    // clock will refuse, and then what it is running at is what the session
    // runs at - which is the right way round: the hardware's clock wins.
    if (wantRate > 0 && ASIOCanSampleRate((ASIOSampleRate)wantRate) == ASE_OK)
        ASIOSetSampleRate((ASIOSampleRate)wantRate);
    ASIOSampleRate got = 0.0;
    if (ASIOGetSampleRate(&got) != ASE_OK || got < 4000.0)
    {
        if (err) *err = FormatW(L"%s would not say what rate it is running at.",
                                driver.c_str());
        CloseLocked();
        return false;
    }
    g_dev.rate = got;

    long minSize = 0, maxSize = 0, preferred = 0, granularity = 0;
    if (ASIOGetBufferSize(&minSize, &maxSize, &preferred, &granularity) != ASE_OK ||
        preferred <= 0)
    {
        if (err) *err = FormatW(L"%s would not say what buffer size it wants.",
                                driver.c_str());
        CloseLocked();
        return false;
    }
    // A buffer this big is not a setting, it is a wrong answer, and it would
    // be believed twice over: once for the driver's own buffers and again for
    // the scratch the callback converts through.
    if (preferred > 1 << 20)
    {
        if (err) *err = FormatW(
            L"%s asks for a buffer of %ld frames, which cannot be right. "
            L"FastRoute will not use it rather than act on that.",
            driver.c_str(), preferred);
        CloseLocked();
        return false;
    }
    // Otherwise the driver's own preference, always. Buffer size is an ASIO
    // control panel setting and belongs there, where other hosts leave it.
    g_dev.bufferFrames = preferred;

    // Channel names and formats, which also settles whether the formats are
    // ones this can convert.
    g_dev.inNames.clear();
    g_dev.outNames.clear();
    g_dev.inTypes.assign((size_t)g_dev.inChannels, 0);
    g_dev.outTypes.assign((size_t)g_dev.outChannels, 0);

    for (int dir = 0; dir < 2; ++dir)
    {
        const bool input = (dir == 0);
        const long count = input ? g_dev.inChannels : g_dev.outChannels;
        for (long i = 0; i < count; ++i)
        {
            ASIOChannelInfo ci = {};
            ci.channel = i;
            ci.isInput = input ? ASIOTrue : ASIOFalse;
            if (ASIOGetChannelInfo(&ci) != ASE_OK) continue;

            std::wstring name = Widen(ci.name);
            if (name.empty()) name = FormatW(L"%ld", i + 1);
            if (input)  { g_dev.inNames.push_back(name);  g_dev.inTypes[(size_t)i] = ci.type; }
            else        { g_dev.outNames.push_back(name); g_dev.outTypes[(size_t)i] = ci.type; }

            if (!TypeSupported(ci.type))
            {
                if (err) *err = FormatW(
                    L"%s presents its %s channels in %s, which FastRoute "
                    L"cannot convert. Most drivers offer 32 bit integer or "
                    L"float in their control panel.",
                    driver.c_str(), input ? L"input" : L"output",
                    TypeName(ci.type));
                Close();
                return false;
            }
        }
    }

    ASIOGetLatencies(&g_dev.inLatency, &g_dev.outLatency);
    g_dev.overloads.store(0);
    g_dev.resetWanted.store(false);
    return true;
}

bool AsioDevice::Start(Callback cb, std::wstring* err)
{
    std::lock_guard<std::mutex> g(g_dev.lock);
    if (!g_dev.loaded)
    {
        if (err) *err = L"No ASIO driver is open.";
        return false;
    }
    if (g_dev.running) return true;

    // Only the channels actually used are prepared. A thirty-two channel
    // interface would otherwise have sixty-four buffers being filled and
    // emptied for the two that are wanted.
    g_dev.infos.clear();
    for (int c = 0; c < kChannels; ++c) { g_dev.inIndex[c] = -1; g_dev.outIndex[c] = -1; }

    for (int c = 0; c < kChannels; ++c)
    {
        const int ch = g_dev.inPair[c];
        if (ch < 0 || ch >= (int)g_dev.inChannels) continue;
        ASIOBufferInfo bi = {};
        bi.isInput = ASIOTrue;
        bi.channelNum = ch;
        g_dev.inIndex[c] = (int)g_dev.infos.size();
        g_dev.infos.push_back(bi);
    }
    for (int c = 0; c < kChannels; ++c)
    {
        const int ch = g_dev.outPair[c];
        if (ch < 0 || ch >= (int)g_dev.outChannels) continue;
        ASIOBufferInfo bi = {};
        bi.isInput = ASIOFalse;
        bi.channelNum = ch;
        g_dev.outIndex[c] = (int)g_dev.infos.size();
        g_dev.infos.push_back(bi);
    }

    if (g_dev.infos.empty())
    {
        if (err) *err = FormatW(L"No channels of %s were chosen.",
                                g_dev.name.c_str());
        return false;
    }

    const ASIOError e = ASIOCreateBuffers(g_dev.infos.data(),
                                          (long)g_dev.infos.size(),
                                          g_dev.bufferFrames, &g_callbacks);
    if (e != ASE_OK)
    {
        if (err) *err = FormatW(L"%s would not prepare its buffers: %s.",
                                g_dev.name.c_str(), ErrText(e));
        return false;
    }
    g_dev.buffers = true;
    g_dev.outputReady = (ASIOOutputReady() == ASE_OK);

    // Everything the callback reads is in place before it can run.
    g_dev.scratchIn.assign((size_t)g_dev.bufferFrames * kChannels, 0.0f);
    g_dev.scratchOut.assign((size_t)g_dev.bufferFrames * kChannels, 0.0f);
    g_dev.cb = std::move(cb);
    g_dev.running = true;

    const ASIOError s = ASIOStart();
    if (s != ASE_OK)
    {
        g_dev.running = false;
        g_dev.cb = nullptr;
        ASIODisposeBuffers();
        g_dev.buffers = false;
        if (err) *err = FormatW(L"%s would not start: %s.", g_dev.name.c_str(),
                                ErrText(s));
        return false;
    }
    return true;
}

void AsioDevice::Stop()
{
    std::lock_guard<std::mutex> g(g_dev.lock);
    if (!g_dev.running) return;
    g_dev.running = false;        // the callback stops doing anything first
    ASIOStop();
    if (g_dev.buffers) { ASIODisposeBuffers(); g_dev.buffers = false; }
    g_dev.cb = nullptr;
}

namespace {

// The teardown itself, which assumes the caller holds g_dev.lock. Open calls
// it on a failed open, with the lock already in hand.
void CloseLocked()
{
    if (g_dev.running)
    {
        g_dev.running = false;
        ASIOStop();
        if (g_dev.buffers) { ASIODisposeBuffers(); g_dev.buffers = false; }
        g_dev.cb = nullptr;
    }
    if (g_dev.loaded)
    {
        // ASIOExit releases the driver through the global asioDrivers, so
        // removing it again here would be a second release of the same thing.
        ASIOExit();
        g_dev.loaded = false;
    }
    g_dev.name.clear();
    g_dev.inChannels = g_dev.outChannels = 0;
    g_dev.bufferFrames = 0;
    g_dev.rate = 0.0;
    g_dev.inNames.clear();
    g_dev.outNames.clear();
    g_dev.inTypes.clear();
    g_dev.outTypes.clear();
    g_dev.infos.clear();
}

}  // namespace

void AsioDevice::Close()
{
    std::lock_guard<std::mutex> g(g_dev.lock);
    CloseLocked();
}

void AsioDevice::ShowControlPanel(HWND /*parent*/)
{
    // ASIOControlPanel takes no parent: the driver puts up its own window
    // wherever it likes, and there is nothing here that can change that.
    if (g_dev.loaded) ASIOControlPanel();
}
