#pragma once
#include "audio.h"

// ---------------------------------------------------------------------------
// ASIO
//
// ASIO is a different shape from WASAPI in three ways that decide how it has
// to be fitted in here, and none of them are details.
//
// It owns the clock. An ASIO driver does not hand over audio when asked; it
// calls the host when it is ready, with one buffer of input and one to fill
// with output, at a size the driver chooses. That is the whole reason ASIO is
// low latency, and it means an ASIO device wants to be the thing driving the
// graph rather than something hanging off it.
//
// Input and output are one callback. A driver presents both halves of the
// interface together, so an ASIO input and an ASIO output on the same device
// are not two independent things - they are two views of one callback. That is
// a gift rather than a nuisance: audio in and audio out share a clock exactly,
// so there is no drift between them and nothing to resample.
//
// And there is one driver per process. The SDK keeps a single global pointer
// to the driver it has loaded, and most drivers allow one host anyway, so a
// session can host one ASIO device and no more. Asking for a second is
// refused with a reason rather than quietly ignored.
//
// Only enumeration is safe to do idly: it reads the registry. Everything else
// opens the driver, which takes the audio hardware away from whatever else is
// using it.
// ---------------------------------------------------------------------------

// The window a driver is given when it starts. On Windows an ASIO driver is
// handed a window handle and some of them use it - as the owner of a dialog
// they put up, as a rule - so passing nothing is asking for trouble from a
// driver that does not check. Set once, when there is a main window to give.
void SetAsioHostWindow(HWND host);

// Every ASIO driver registered on this machine, by the name it registered
// under. Reads the registry only: no driver is loaded and no device opened.
std::vector<std::wstring> EnumAsioDrivers();

// Whether the name is one of them. Cheap, and the honest thing to check before
// telling somebody their driver has gone.
bool AsioDriverExists(const std::wstring& name);

// ---------------------------------------------------------------------------
// The one ASIO device this process can have open
// ---------------------------------------------------------------------------
class AsioDevice
{
public:
    // There is one of these, because the SDK has one global driver.
    static AsioDevice& Instance();

    // Loads the driver and prepares its buffers. `wantRate` is a request: the
    // driver may refuse it, and what it settled on is in Rate() afterwards.
    // Fails with a reason if another driver is already open.
    bool Open(const std::wstring& driver, int wantRate, std::wstring* err);
    void Close();

    bool Open() const;
    std::wstring Name() const;

    // What the driver decided. The buffer size is the period the callback
    // arrives at, and the session's own period when ASIO is driving.
    int Rate() const;
    int BufferFrames() const;
    int InputChannels() const;
    int OutputChannels() const;

    // The driver's own names for its channels - "Analog 1", "ADAT 3" - which
    // is what a dialog should offer rather than bare numbers.
    std::vector<std::wstring> ChannelNames(bool input) const;

    // Which two channels of the device the stereo bus is. Set before Start,
    // and ignored for a direction the driver has none of.
    void SetInputPair(int left, int right);
    void SetOutputPair(int left, int right);

    // The callback. `in` is the device's chosen input pair as interleaved
    // stereo, or null when the driver has no inputs; `out` is the output pair
    // to fill, or null when it has no outputs. Called from the driver's own
    // thread, which is real time: no allocation, no locks that anything else
    // holds for long, and no waiting.
    using Callback = std::function<void(const float* in, float* out, int frames)>;

    bool Start(Callback cb, std::wstring* err);
    void Stop();
    bool Running() const;

    // How many times the driver reported that it lost its place, which is the
    // ASIO equivalent of a dropout.
    unsigned Overloads() const;

    // The driver's own settings window. Buffer size and clock source live
    // there rather than here: that is where ASIO puts them, and every other
    // host sends people to the same place.
    void ShowControlPanel(HWND parent);

private:
    AsioDevice() {}
    AsioDevice(const AsioDevice&) = delete;
    AsioDevice& operator=(const AsioDevice&) = delete;
};
