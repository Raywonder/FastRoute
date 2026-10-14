#include <winsock2.h>
#include <windows.h>
#include <objbase.h>

#include "audio.h"
#include "aacenc.h"
#include "icecast.h"
#include "resample.h"
#include "rtmp.h"
#include "streamenc.h"
#include "util.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

// ---------------------------------------------------------------------------
// Stream Out
//
// Puts the bus on a live streaming server. One block, four kinds of server:
//
//  * RTMP, which is what AAA Streamer and most streaming platforms take in:
//    AAC-LC at 48 kHz stereo, the format every HLS player in every browser
//    can play, which is what such a platform turns RTMP into.
//  * Icecast 2, in AAC, MP3, Ogg Opus or Ogg Vorbis.
//  * Shoutcast 1 and 2, in AAC or MP3.
//
// Shaped like Net Send: Render() hands it the bus and it clocks nothing. The
// render thread only queues; a worker thread resamples, encodes, connects and
// sends, because a TCP send can block for as long as the network likes and a
// late render callback is a dropout on every output in the session.
//
// It is live, so nothing is kept for later. While the connection is down the
// audio is dropped rather than queued, and when it comes back the stream picks
// up from now rather than playing the gap late. Reconnecting backs off from two
// seconds to a minute, quietly: each new reason is logged once, not each try,
// and the outage is announced once, not on every attempt.
// ---------------------------------------------------------------------------

namespace {

const int kOutRate = 48000;
const int kFrame = 1024;              // samples per AAC frame
const int kConnectTimeoutMs = 10000;
const int kBackoffFirstSec = 2;
const int kBackoffMaxSec = 60;
// While live, the most that may wait in the queue before the oldest is
// dropped: a connection this far behind is not going to catch up.
const int kMaxQueueSec = 4;

enum State { S_CONNECTING, S_LIVE, S_RETRYING, S_FAILED, S_STOPPED };

std::wstring WidenA(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string Utf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0,
                                nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n,
                                   nullptr, nullptr);
    return s;
}

bool IsOgg(int fmt) { return fmt == SF_OPUS || fmt == SF_VORBIS; }

class StreamOut : public NetSink
{
public:
    ~StreamOut() override { Stop(); }

    bool Start(const Block& b, int rate, std::wstring* err) override
    {
        Stop();
        m_server = b.outServer;
        m_format = b.outFormat;
        if (m_server == OS_RTMP)
        {
            m_format = SF_AAC;
            if (!rtmp::ParseTarget(b.outUrl, b.outKey, &m_rtmp, err)) return false;
            m_host = WidenA(m_rtmp.host);
        }
        else
        {
            if (m_server != OS_ICECAST && IsOgg(m_format))
            {
                if (err) *err = L"Shoutcast streams AAC or MP3. Choose one of those "
                                L"formats, or an Icecast server for Ogg.";
                return false;
            }
            const int kind = m_server == OS_ICECAST ? icecast::K_ICECAST
                           : m_server == OS_SHOUTCAST2 ? icecast::K_SHOUTCAST2
                                                       : icecast::K_SHOUTCAST1;
            if (!icecast::ParseAddress(b.outUrl, b.outMount, kind, &m_ice, err))
                return false;
            if (b.outKey.empty())
            {
                if (err) *err = L"Type the source password for the server.";
                return false;
            }
            m_ice.user = Utf8(b.outUser);
            m_ice.password = Utf8(b.outKey);
            m_ice.sid = b.outSid;
            m_ice.name = Utf8(b.outName);
            m_ice.description = Utf8(b.outDesc);
            m_ice.genre = Utf8(b.outGenre);
            m_ice.url = Utf8(b.outSiteUrl);
            m_ice.isPublic = b.outPublic;
            m_ice.sampleRate = kOutRate;
            m_host = WidenA(m_ice.host);
        }

        if ((m_format == SF_AAC || m_format == SF_MP3) && !MediaFoundationAvailable())
        {
            if (err) *err = L"AAC and MP3 are encoded by Media Foundation, which this "
                            L"edition of Windows does not have. Install the Media "
                            L"Feature Pack, or stream Ogg Opus to an Icecast server.";
            return false;
        }
        if (m_format == SF_MP3 && !AacEncoder::Mp3Available())
        {
            if (err) *err = L"Windows on this machine has no MP3 encoder. Choose AAC.";
            return false;
        }

        m_rate = rate > 0 ? rate : kDefaultRate;
        m_kbps = b.outKbps;
        m_reconnect = b.outReconnect;
        m_title = Utf8(b.outTitle);
        m_titleSent.clear();
        m_name = b.label.empty() ? std::wstring(L"Stream Out") : b.label;
        m_running = true;
        SetState(S_CONNECTING, L"");
        m_thread = std::thread([this] { Run(); });
        return true;
    }

    void Write(const float* buf, size_t frames) override
    {
        if (!m_running || !m_live) return;
        {
            std::lock_guard<std::mutex> g(m_lock);
            m_queue.insert(m_queue.end(), buf, buf + frames * kChannels);
            const size_t cap = (size_t)m_rate * kChannels * kMaxQueueSec;
            if (m_queue.size() > cap)
                m_queue.erase(m_queue.begin(),
                              m_queue.begin() + (ptrdiff_t)(m_queue.size() - cap));
        }
        m_wake.notify_one();
    }

    // The one thing that can change under a running stream: the title. The
    // worker sends it to the server on its next pass.
    void Configure(const Block& b) override
    {
        std::lock_guard<std::mutex> g(m_lock);
        m_title = Utf8(b.outTitle);
        m_wake.notify_one();
    }

    void Stop() override
    {
        if (!m_running && !m_thread.joinable()) return;
        m_running = false;
        m_wake.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }

    std::wstring StatusText() override
    {
        std::lock_guard<std::mutex> g(m_stateLock);
        switch (m_state)
        {
        case S_CONNECTING:
            return FormatW(L"%s: connecting to %s.", m_name.c_str(), m_host.c_str());
        case S_LIVE:
            return FormatW(L"%s: live on %s, %s %d kbps, %s sent.", m_name.c_str(),
                           m_host.c_str(), FormatName(), m_kbpsUsed,
                           SizeText(m_bytes.load()).c_str());
        case S_RETRYING:
            if (m_retryIn <= 0)
                return FormatW(L"%s: not connected. %s Trying again now.",
                               m_name.c_str(), m_detail.c_str());
            return FormatW(L"%s: not connected. %s Trying again in %d seconds.",
                           m_name.c_str(), m_detail.c_str(), m_retryIn);
        case S_FAILED:
            return FormatW(L"%s stopped: %s", m_name.c_str(), m_detail.c_str());
        default:
            return std::wstring();
        }
    }

    unsigned EventSerial(std::wstring* text) override
    {
        std::lock_guard<std::mutex> g(m_stateLock);
        if (text) *text = m_event;
        return m_serial;
    }

private:
    const wchar_t* FormatName() const
    {
        switch (m_format)
        {
        case SF_MP3:    return L"MP3";
        case SF_OPUS:   return L"Ogg Opus";
        case SF_VORBIS: return L"Ogg Vorbis";
        default:        return L"AAC";
        }
    }

    void SetState(State s, const std::wstring& detail, int retryIn = 0)
    {
        std::lock_guard<std::mutex> g(m_stateLock);
        const bool changed = s != m_state;
        m_state = s;
        m_detail = detail;
        m_retryIn = retryIn;
        if (!changed) return;

        // What a screen reader is told when this changes. Short, because it
        // is spoken over whatever the person is doing.
        switch (s)
        {
        case S_CONNECTING:
            m_event = FormatW(L"%s connecting.", m_name.c_str());
            break;
        case S_LIVE:
            m_event = FormatW(L"%s is live.", m_name.c_str());
            break;
        case S_RETRYING:
            m_event = FormatW(L"%s is not connected, reconnecting.", m_name.c_str());
            break;
        case S_FAILED:
            m_event = FormatW(L"%s stopped: %s", m_name.c_str(), detail.c_str());
            break;
        default:
            m_event.clear();
        }
        ++m_serial;
    }

    // Sleeps for the backoff, but wakes at once if the block is stopped.
    void Wait(int seconds)
    {
        for (int left = seconds; left > 0 && m_running; --left)
        {
            {
                std::lock_guard<std::mutex> g(m_stateLock);
                m_retryIn = left;
            }
            std::unique_lock<std::mutex> g(m_lock);
            m_wake.wait_for(g, std::chrono::seconds(1), [this] { return !m_running.load(); });
        }
        std::lock_guard<std::mutex> g(m_stateLock);
        m_retryIn = 0;
    }

    // Takes what the render thread has queued, waiting briefly for some.
    void TakeQueued(std::vector<float>& chunk)
    {
        std::unique_lock<std::mutex> g(m_lock);
        m_wake.wait_for(g, std::chrono::milliseconds(200), [this] {
            return !m_queue.empty() || !m_running.load();
        });
        chunk.assign(m_queue.begin(), m_queue.end());
        m_queue.clear();
    }

    // ---- One connection's worth of RTMP ------------------------------------
    // Returns when the connection is lost (true, reason in err) or the encoder
    // fails or the block stops (false).
    bool RunRtmp(AacEncoder& enc, Converter& conv, std::wstring& err)
    {
        rtmp::Publisher pub;
        if (!pub.Connect(m_rtmp, kConnectTimeoutMs, &err)) return true;
        Live();
        m_kbpsUsed = enc.Kbps();
        pub.SendAudioMetadata(kOutRate, kChannels, enc.Kbps());
        pub.SendAacSequenceHeader(enc.Config().data(), enc.Config().size());

        std::vector<float> chunk, at48;
        std::vector<int16_t> pcm;
        std::vector<std::vector<uint8_t>> frames;
        unsigned long long frameNo = 0;
        while (m_running)
        {
            TakeQueued(chunk);
            if (!chunk.empty()) conv.Run(chunk.data(), chunk.size() / kChannels, at48);

            // Whole AAC frames only, so every timestamp lands on a frame.
            const size_t whole = (at48.size() / kChannels / kFrame) * kFrame;
            if (whole > 0)
            {
                pcm.resize(whole * kChannels);
                for (size_t i = 0; i < whole * kChannels; ++i)
                {
                    float v = at48[i];
                    if (v > 1.0f) v = 1.0f;
                    else if (v < -1.0f) v = -1.0f;
                    pcm[i] = (int16_t)(v * 32767.0f);
                }
                at48.erase(at48.begin(), at48.begin() + (ptrdiff_t)(whole * kChannels));

                frames.clear();
                if (!enc.Encode(pcm.data(), whole, &frames, &err))
                {
                    Fail(L"stopped encoding", err);
                    return false;
                }
                for (const auto& f : frames)
                {
                    const uint32_t ts = (uint32_t)(frameNo * kFrame * 1000ULL / kOutRate);
                    if (!pub.SendAacFrame(f.data(), f.size(), ts))
                    {
                        err = L"Sending to the server failed.";
                        m_live = false;
                        return true;
                    }
                    ++frameNo;
                }
                m_bytes = pub.BytesSent();
            }
            if (!pub.Poll(&err)) { m_live = false; return true; }
        }
        m_live = false;
        return false;
    }

    // ---- One connection's worth of Icecast or Shoutcast --------------------
    bool RunSource(std::wstring& err)
    {
        // A fresh encoder per connection: an Ogg stream has to start with its
        // headers, and a listener joining a reconnected mount needs them.
        std::unique_ptr<StreamEncoder> enc = MakeStreamEncoder(m_format);
        std::string title;
        {
            std::lock_guard<std::mutex> g(m_lock);
            title = m_title;
        }
        if (!enc->Start(m_rate, m_kbps, title, &err))
        {
            Fail(L"could not start its encoder", err);
            return false;
        }
        m_ice.contentType = enc->ContentType();
        m_ice.kbps = enc->Kbps();

        icecast::Source src;
        if (!src.Connect(m_ice, kConnectTimeoutMs, &err)) { enc->Stop(); return true; }
        Live();
        m_kbpsUsed = enc->Kbps();
        m_titleSent = IsOgg(m_format) ? title : std::string();

        std::vector<float> chunk;
        std::vector<uint8_t> bytes;
        while (m_running)
        {
            TakeQueued(chunk);
            if (!chunk.empty() && !enc->Encode(chunk.data(), chunk.size() / kChannels, &err))
            {
                Fail(L"stopped encoding", err);
                enc->Stop();
                return false;
            }
            bytes.clear();
            enc->Take(&bytes);
            if (!bytes.empty() && !src.Send(bytes.data(), bytes.size(), &err))
            {
                m_live = false;
                enc->Stop();
                return true;
            }
            m_bytes = src.BytesSent();
            if (!src.Poll(&err)) { m_live = false; enc->Stop(); return true; }
            SendTitleIfChanged();
        }
        m_live = false;
        enc->Stop();
        return false;
    }

    // Ogg carries its title inside the stream, set when it starts, and
    // Icecast will not change it from outside; the other formats are told
    // through the server's admin page.
    void SendTitleIfChanged()
    {
        std::string want;
        {
            std::lock_guard<std::mutex> g(m_lock);
            want = m_title;
        }
        if (want == m_titleSent || IsOgg(m_format)) return;
        m_titleSent = want;
        if (want.empty()) return;
        std::wstring err;
        if (!icecast::UpdateTitle(m_ice, want, &err))
            LogError(FormatW(L"%s could not set the title: %s", m_name.c_str(), err.c_str()));
    }

    void Live()
    {
        {
            std::lock_guard<std::mutex> g(m_lock);
            m_queue.clear();   // live: start from now, not from the gap
        }
        m_live = true;
        m_bytes = 0;
        SetState(S_LIVE, L"");
    }

    void Fail(const wchar_t* what, const std::wstring& err)
    {
        LogError(FormatW(L"%s %s: %s", m_name.c_str(), what, err.c_str()));
        SetState(S_FAILED, err);
        m_running = false;
        m_live = false;
    }

    void Run()
    {
        const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        // RTMP keeps one encoder for the life of the block: its configuration
        // goes out with every connection, so nothing about it has to restart.
        AacEncoder aac;
        Converter conv;
        std::wstring err;
        if (m_server == OS_RTMP)
        {
            conv.Prepare(m_rate, kOutRate);
            if (!aac.Start(kOutRate, m_kbps, &err))
            {
                Fail(L"could not start its encoder", err);
                if (SUCCEEDED(hrCom)) CoUninitialize();
                return;
            }
        }

        int backoff = kBackoffFirstSec;
        std::wstring lastLogged;
        while (m_running)
        {
            err.clear();
            const unsigned long long before = GetTickCount64();
            const bool retry = m_server == OS_RTMP ? RunRtmp(aac, conv, err)
                                                   : RunSource(err);
            if (!retry || !m_running) break;

            // A connection that stayed up a while starts the backoff over.
            if (GetTickCount64() - before > 30000) backoff = kBackoffFirstSec;

            // Logged once per distinct reason, so a server that is down for an
            // hour is one line in the log rather than sixty.
            if (err != lastLogged)
            {
                LogError(FormatW(L"%s, %s: %s", m_name.c_str(), m_host.c_str(), err.c_str()));
                lastLogged = err;
            }
            if (!m_reconnect)
            {
                SetState(S_FAILED, err);
                break;
            }
            SetState(S_RETRYING, err, backoff);
            Wait(backoff);
            backoff = backoff * 2 > kBackoffMaxSec ? kBackoffMaxSec : backoff * 2;
        }
        m_live = false;
        aac.Stop();
        if (SUCCEEDED(hrCom)) CoUninitialize();
    }

    int m_server = OS_RTMP;
    int m_format = SF_AAC;
    rtmp::Target m_rtmp;
    icecast::Target m_ice;
    int m_rate = kDefaultRate;
    int m_kbps = 192;
    int m_kbpsUsed = 192;
    bool m_reconnect = true;
    std::wstring m_name, m_host;
    std::string m_title, m_titleSent;

    std::thread m_thread;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_live{ false };
    std::atomic<unsigned long long> m_bytes{ 0 };

    std::deque<float> m_queue;
    std::mutex m_lock;
    std::condition_variable m_wake;

    std::mutex m_stateLock;
    State m_state = S_STOPPED;
    std::wstring m_detail, m_event;
    int m_retryIn = 0;
    unsigned m_serial = 0;
};

}   // namespace

std::unique_ptr<NetSink> MakeStreamOutSink()
{
    return std::unique_ptr<NetSink>(new StreamOut());
}
