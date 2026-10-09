#include "audio.h"
#include "util.h"

#include <winhttp.h>
#include <algorithm>
#include <vector>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

extern "C" {
#include <vorbis/vorbisfile.h>
#include <opusfile.h>
#include "resample.h"
}

// ---------------------------------------------------------------------------
// Internet audio streams
//
// Two decoders, because no one of them covers the formats people actually
// stream. Windows decodes MP3, AAC and (from Windows 10) FLAC through Media
// Foundation, which also does its own fetching, so those are handed to it
// whole. Ogg Vorbis and Opus it does not decode dependably - this machine has
// no Vorbis decoder at all and Windows 7 has neither - so those are fetched
// here over WinHTTP and decoded by libvorbis and libopus.
//
// Which one is used is decided by looking at the first bytes rather than the
// file extension: an Icecast mount point rarely has one, and a Content-Type
// header is only as honest as the server sending it.
// ---------------------------------------------------------------------------

namespace {

// A stream that has stopped and is waiting to be retried should notice being
// stopped promptly, so the wait is in small pieces.
static bool SleepUnlessStopped(std::atomic<bool>& stop, int ms)
{
    for (int left = ms; left > 0 && !stop.load(); left -= 100)
        Sleep(left < 100 ? left : 100);
    return !stop.load();
}

// ---------------------------------------------------------------------------
// HTTP, for the Ogg path
// ---------------------------------------------------------------------------
class HttpSource
{
public:
    ~HttpSource() { Close(); }

    bool Open(const std::wstring& url, std::wstring* err)
    {
        Close();

        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof(uc);
        wchar_t host[256] = { 0 }, path[2048] = { 0 };
        uc.lpszHostName = host;      uc.dwHostNameLength = 255;
        uc.lpszUrlPath  = path;      uc.dwUrlPathLength = 2047;
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc))
        {
            if (err) *err = L"That is not an address FastRoute can open: " + url;
            return false;
        }

        m_session = WinHttpOpen(L"FastRoute", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!m_session)   // the automatic proxy type is Windows 8.1 and later
            m_session = WinHttpOpen(L"FastRoute", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!m_session) { Fail(err, L"No HTTP session could be opened"); return false; }

        // A stalled stream must not wedge the thread; the read loop treats a
        // timeout as a dropped connection and reconnects.
        WinHttpSetTimeouts(m_session, 15000, 15000, 15000, 15000);

        m_conn = WinHttpConnect(m_session, host, uc.nPort, 0);
        if (!m_conn) { Fail(err, L"Could not reach " + std::wstring(host)); return false; }

        DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        m_req = WinHttpOpenRequest(m_conn, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!m_req) { Fail(err, L"The request could not be built"); return false; }

        // Shoutcast servers answer with metadata interleaved into the audio
        // unless asked not to. Not asking is how you get clicks every 8 KB.
        WinHttpAddRequestHeaders(m_req, L"Icy-MetaData: 0\r\n", (DWORD)-1,
                                 WINHTTP_ADDREQ_FLAG_ADD);

        if (!WinHttpSendRequest(m_req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(m_req, nullptr))
        {
            Fail(err, L"No answer from " + std::wstring(host));
            return false;
        }

        DWORD status = 0, len = sizeof(status);
        WinHttpQueryHeaders(m_req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            nullptr, &status, &len, nullptr);
        if (status && (status < 200 || status >= 300))
        {
            if (err) *err = FormatW(L"%s answered %lu rather than sending audio.",
                                    host, status);
            return false;
        }

        wchar_t type[256] = { 0 };
        len = sizeof(type);
        if (WinHttpQueryHeaders(m_req, WINHTTP_QUERY_CONTENT_TYPE, nullptr, type, &len, nullptr))
            m_type = type;
        return true;
    }

    void Close()
    {
        if (m_req) { WinHttpCloseHandle(m_req); m_req = nullptr; }
        if (m_conn) { WinHttpCloseHandle(m_conn); m_conn = nullptr; }
        if (m_session) { WinHttpCloseHandle(m_session); m_session = nullptr; }
        m_type.clear();
    }

    // Blocking. 0 means the stream ended, which for a live source means it
    // dropped; negative means it failed outright.
    int Read(void* buf, int n)
    {
        if (!m_req || n <= 0) return -1;
        DWORD got = 0;
        if (!WinHttpReadData(m_req, buf, (DWORD)n, &got)) return -1;
        return (int)got;
    }

    const std::wstring& ContentType() const { return m_type; }

private:
    void Fail(std::wstring* err, const std::wstring& what)
    {
        if (err) *err = FormatW(L"%s (error %lu).", what.c_str(), GetLastError());
    }

    HINTERNET m_session = nullptr, m_conn = nullptr, m_req = nullptr;
    std::wstring m_type;
};


// ---------------------------------------------------------------------------

// What a Read gave back. Positive is a frame count.
enum { kNothingYet = 0, kFailed = -1, kEnded = -2 };

class Decoder
{
public:
    virtual ~Decoder() {}
    // Fills `out` with interleaved stereo at the session rate and returns the
    // frame count, or one of the three above. Nothing-yet and ended are worth
    // telling apart: a live stream hands back nothing all the time, and
    // treating that as the end would reconnect every few milliseconds.
    virtual int Read(std::vector<float>& out) = 0;
};

// The read callback both vorbisfile and opusfile take has the same shape.
static int OggRead(void* src, unsigned char* buf, int n)
{
    int got = static_cast<HttpSource*>(src)->Read(buf, n);
    return got < 0 ? 0 : got;   // both libraries treat 0 as the end
}

class VorbisDecoder : public Decoder
{
public:
    VorbisDecoder(HttpSource* http, int rateOut) : m_http(http), m_rateOut(rateOut) {}

    ~VorbisDecoder() override { if (m_open) ov_clear(&m_vf); }

    bool Start(const std::vector<char>& sniff, std::wstring* err)
    {
        ov_callbacks cb{};
        cb.read_func = [](void* p, size_t sz, size_t n, void* src) -> size_t {
            return (size_t)OggRead(src, (unsigned char*)p, (int)(sz * n));
        };
        // No seek, tell or close: this is a stream, not a file.
        int rc = ov_open_callbacks(m_http, &m_vf, sniff.data(), (long)sniff.size(), cb);
        if (rc < 0)
        {
            if (err) *err = FormatW(L"The Ogg Vorbis stream could not be read (%d).", rc);
            return false;
        }
        m_open = true;

        vorbis_info* vi = ov_info(&m_vf, -1);
        if (!vi) { if (err) *err = L"The Ogg Vorbis stream has no audio in it."; return false; }
        m_channels = vi->channels;
        m_conv.Prepare(vi->rate, m_rateOut);
        return true;
    }

    int Read(std::vector<float>& out) override
    {
        float** pcm = nullptr;
        int bs = 0;
        long got = ov_read_float(&m_vf, &pcm, 4096, &bs);
        if (got == 0) return kEnded;
        // A hole is a gap in the stream, not the end of it: keep going.
        if (got < 0)  return (got == OV_HOLE) ? kNothingYet : kFailed;

        // Down to stereo: everything past the second channel is folded in, and
        // mono is doubled, so the bus always gets two.
        m_stereo.resize((size_t)got * kChannels);
        for (long i = 0; i < got; ++i)
        {
            float l = pcm[0][i];
            float r = (m_channels > 1) ? pcm[1][i] : l;
            for (int c = 2; c < m_channels; ++c)
            {
                float extra = pcm[c][i] * 0.5f;
                l += extra;
                r += extra;
            }
            m_stereo[(size_t)i * kChannels + 0] = l;
            m_stereo[(size_t)i * kChannels + 1] = r;
        }

        out.clear();
        m_conv.Run(m_stereo.data(), (size_t)got, out);
        return (int)(out.size() / kChannels);
    }

private:
    HttpSource* m_http;
    int m_rateOut;
    OggVorbis_File m_vf{};
    bool m_open = false;
    int m_channels = 2;
    std::vector<float> m_stereo;
    Converter m_conv;
};

class OpusDecoder : public Decoder
{
public:
    OpusDecoder(HttpSource* http, int rateOut) : m_http(http), m_rateOut(rateOut) {}

    ~OpusDecoder() override { if (m_of) op_free(m_of); }

    bool Start(const std::vector<char>& sniff, std::wstring* err)
    {
        OpusFileCallbacks cb{};
        cb.read = [](void* src, unsigned char* buf, int n) -> int {
            return OggRead(src, buf, n);
        };
        int rc = 0;
        m_of = op_open_callbacks(m_http, &cb,
                                 (const unsigned char*)sniff.data(), sniff.size(), &rc);
        if (!m_of)
        {
            if (err) *err = FormatW(L"The Opus stream could not be read (%d).", rc);
            return false;
        }
        // Opus is always 48 kHz by definition, whatever went into it.
        m_conv.Prepare(48000, m_rateOut);
        return true;
    }

    int Read(std::vector<float>& out) override
    {
        m_stereo.resize(4096 * kChannels);
        int got = op_read_float_stereo(m_of, m_stereo.data(), (int)m_stereo.size());
        if (got == 0) return kEnded;
        if (got < 0)  return (got == OP_HOLE) ? kNothingYet : kFailed;

        out.clear();
        m_conv.Run(m_stereo.data(), (size_t)got, out);
        return (int)(out.size() / kChannels);
    }

private:
    HttpSource* m_http;
    int m_rateOut;
    OggOpusFile* m_of = nullptr;
    std::vector<float> m_stereo;
    Converter m_conv;
};

// Everything Windows does decode: MP3, AAC, FLAC on Windows 10, WMA, and any
// container Media Foundation knows. It fetches the URL itself.
class MfDecoder : public Decoder
{
public:
    MfDecoder(std::wstring url, int rate) : m_url(std::move(url)), m_rate(rate) {}

    ~MfDecoder() override
    {
        if (m_reader) m_reader->Release();
        if (m_up) MFShutdown();
    }

    bool Start(std::wstring* err)
    {
        if (!MediaFoundationAvailable())
        {
            if (err) *err = L"This edition of Windows has no Media Foundation, so "
                            L"only Ogg Vorbis and Opus streams can be played.";
            return false;
        }
        if (FAILED(MFStartup(MF_VERSION)))
        {
            if (err) *err = L"Media Foundation could not start.";
            return false;
        }
        m_up = true;

        IMFAttributes* attrs = nullptr;
        if (SUCCEEDED(MFCreateAttributes(&attrs, 1)) && attrs)
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);

        HRESULT hr = MFCreateSourceReaderFromURL(m_url.c_str(), attrs, &m_reader);
        if (attrs) attrs->Release();
        if (FAILED(hr))
        {
            if (err) *err = FormatW(
                L"Windows could not open that stream (%s). If it is Ogg Vorbis or "
                L"Opus, FastRoute decodes those itself, so the address is probably "
                L"wrong or the server is not answering.", HrText(hr).c_str());
            return false;
        }

        m_reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        m_reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

        // Asking for plain PCM at the session's rate makes the reader insert
        // whatever decoder and resampler it needs behind the scenes.
        IMFMediaType* pcm = nullptr;
        if (SUCCEEDED(MFCreateMediaType(&pcm)) && pcm)
        {
            pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)m_rate);
            pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
            pcm->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kChannels * 2);
            pcm->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)(m_rate * kChannels * 2));
            pcm->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
            hr = m_reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                               nullptr, pcm);
            pcm->Release();
        }
        if (FAILED(hr))
        {
            if (err) *err = FormatW(
                L"Windows opened that stream but will not decode it to %d Hz stereo "
                L"(%s). It may be a format this Windows has no decoder for.",
                m_rate, HrText(hr).c_str());
            return false;
        }
        return true;
    }

    int Read(std::vector<float>& out) override
    {
        if (!m_reader) return -1;

        DWORD flags = 0;
        IMFSample* sample = nullptr;
        HRESULT hr = m_reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                          0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr)) { if (sample) sample->Release(); return kFailed; }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
        {
            if (sample) sample->Release();
            return kEnded;
        }
        if (!sample) { out.clear(); return kNothingYet; }

        IMFMediaBuffer* buf = nullptr;
        int frames = 0;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && buf)
        {
            BYTE* p = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(buf->Lock(&p, nullptr, &len)))
            {
                frames = (int)(len / (kChannels * 2));
                out.resize((size_t)frames * kChannels);
                const short* pcm = reinterpret_cast<const short*>(p);
                for (size_t i = 0; i < out.size(); ++i) out[i] = pcm[i] / 32768.0f;
                buf->Unlock();
            }
            buf->Release();
        }
        sample->Release();
        return frames;
    }

private:
    std::wstring m_url;
    int m_rate;
    IMFSourceReader* m_reader = nullptr;
    bool m_up = false;
};

// ---------------------------------------------------------------------------

class StreamCapture : public CaptureSource
{
public:
    StreamCapture(std::wstring url, int bufferMs, bool reconnect, int retrySeconds, int rate)
        : CaptureSource(rate, bufferMs), m_url(std::move(url)),
          m_reconnect(reconnect), m_retry(retrySeconds < 1 ? 1 : retrySeconds) {}

    ~StreamCapture() override { Stop(); }

    bool Start(std::wstring* err) override
    {
        m_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_stop = false;
        m_thread = std::thread([this] { ThreadMain(); });

        // Opening a network stream is not instant, and a session that says it
        // is running should be running. Wait, but not indefinitely.
        WaitForSingleObject(m_ready, 15000);
        if (!m_initOk)
        {
            if (err) *err = m_initErr.empty()
                ? FormatW(L"%s did not answer within fifteen seconds.", m_url.c_str())
                : m_initErr;
            Stop();
            return false;
        }
        return true;
    }

    void Stop() override
    {
        m_stop = true;
        if (m_thread.joinable()) m_thread.join();
        if (m_ready) { CloseHandle(m_ready); m_ready = nullptr; }
    }

private:
    void ThreadMain()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_ring.PrimeSilence(m_prime);

        bool first = true;
        while (!m_stop.load())
        {
            std::wstring err;
            HttpSource http;
            std::unique_ptr<Decoder> dec = Connect(http, &err);

            if (first)
            {
                // Only the first attempt decides whether the session starts.
                m_initOk = (dec != nullptr);
                m_initErr = err;
                SetEvent(m_ready);
                first = false;
                if (!dec && !m_reconnect) break;
            }
            else if (!dec)
            {
                LogError(FormatW(L"%s could not be reached: %s", m_url.c_str(),
                                 err.empty() ? L"no reason given." : err.c_str()));
            }

            if (dec) Pump(*dec);
            if (m_stop.load() || !m_reconnect) break;

            if (!SleepUnlessStopped(m_stop, m_retry * 1000)) break;
        }

        if (!m_initOk) SetEvent(m_ready);   // a failure with no retry still answers
        CoUninitialize();
    }

    // Reads the first bytes and picks the decoder from what is actually there.
    std::unique_ptr<Decoder> Connect(HttpSource& http, std::wstring* err)
    {
        // Media Foundation fetches for itself, so the probe is only worth doing
        // for the Ogg path; but the probe is what tells us which path it is.
        if (!http.Open(m_url, err)) return nullptr;

        std::vector<char> sniff;
        sniff.reserve(4096);
        char buf[1024];
        while (sniff.size() < 64)
        {
            int got = http.Read(buf, sizeof(buf));
            if (got <= 0) break;
            sniff.insert(sniff.end(), buf, buf + got);
        }

        const bool ogg = sniff.size() >= 4 && memcmp(sniff.data(), "OggS", 4) == 0;
        if (ogg)
        {
            // The codec is named in the first page: Vorbis identifies itself
            // with a 0x01 packet type, Opus with "OpusHead".
            const bool opus = sniff.end() !=
                std::search(sniff.begin(), sniff.end(),
                            "OpusHead", "OpusHead" + 8);
            if (opus)
            {
                std::unique_ptr<OpusDecoder> d(new OpusDecoder(&http, m_rate));
                if (!d->Start(sniff, err)) return nullptr;
                return std::unique_ptr<Decoder>(d.release());
            }
            std::unique_ptr<VorbisDecoder> d(new VorbisDecoder(&http, m_rate));
            if (!d->Start(sniff, err)) return nullptr;
            return std::unique_ptr<Decoder>(d.release());
        }

        // Not Ogg, so Windows can have it. Our own connection is dropped:
        // Media Foundation opens its own.
        http.Close();
        std::unique_ptr<MfDecoder> d(new MfDecoder(m_url, m_rate));
        if (!d->Start(err)) return nullptr;
        return std::unique_ptr<Decoder>(d.release());
    }

    void Pump(Decoder& dec)
    {
        std::vector<float> out;
        while (!m_stop.load())
        {
            int frames = dec.Read(out);
            if (frames == kFailed || frames == kEnded) return;   // caller reconnects
            if (frames == kNothingYet) { Sleep(5); continue; }

            NotePacket((unsigned)frames);

            // A live stream paces itself, but a file over HTTP arrives as fast
            // as the link allows and would fill the ring in seconds. So it is
            // written a slice at a time, waiting for the mixer to take what is
            // already there.
            //
            // Waiting at the working depth rather than the ceiling matters: at
            // the ceiling, every burst that reaches it is discarded before the
            // throttle ever engages. Slicing matters for the same reason - a
            // decoder can hand back a whole second at once, which is more than
            // the headroom, and the wait between whole buffers would be too
            // late to help.
            const size_t slice = m_prime / 4 ? m_prime / 4 : 1;
            size_t at = 0;
            while (at < (size_t)frames && !m_stop.load())
            {
                while (!m_stop.load() && m_ring.Count() > m_prime) Sleep(10);
                if (m_stop.load()) return;

                size_t take = (size_t)frames - at;
                if (take > slice) take = slice;
                m_ring.Write(out.data() + at * kChannels, take);
                at += take;
            }
        }
    }

    std::wstring m_url;
    bool m_reconnect;
    int m_retry;
    HANDLE m_ready = nullptr;
    bool m_initOk = false;
    std::wstring m_initErr;
};

}   // namespace

std::unique_ptr<CaptureSource> MakeStreamCapture(const std::wstring& url, int bufferMs,
                                                 bool reconnect, int retrySeconds,
                                                 int rate)
{
    if (bufferMs < 200) bufferMs = 200;
    if (bufferMs > 10000) bufferMs = 10000;
    return std::unique_ptr<CaptureSource>(
        new StreamCapture(url, bufferMs, reconnect, retrySeconds, rate));
}
