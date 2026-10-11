#include "audio.h"
#include "util.h"

#include <shlwapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>

// ---------------------------------------------------------------------------
// Paths and templates
// ---------------------------------------------------------------------------

const wchar_t* RecFormatExt(RecFormat fmt)
{
    switch (fmt)
    {
    case RecFormat::Flac: return L"flac";
    case RecFormat::Mp3:  return L"mp3";
    case RecFormat::Aac:  return L"m4a";   // the container the sink writer makes
    case RecFormat::Ogg:  return L"ogg";
    case RecFormat::Opus: return L"opus";
    default:              return L"wav";
    }
}

const wchar_t* RecFormatShortName(RecFormat fmt)
{
    switch (fmt)
    {
    case RecFormat::Flac: return L"FLAC";
    case RecFormat::Mp3:  return L"MP3";
    case RecFormat::Aac:  return L"AAC";
    case RecFormat::Ogg:  return L"Ogg Vorbis";
    case RecFormat::Opus: return L"Opus";
    default:              return L"WAV";
    }
}

bool RecFormatUsesBitrate(RecFormat fmt)
{
    return fmt == RecFormat::Mp3 || fmt == RecFormat::Aac ||
           fmt == RecFormat::Ogg || fmt == RecFormat::Opus;
}

const wchar_t* RecFormatName(RecFormat fmt)
{
    switch (fmt)
    {
    case RecFormat::Flac: return L"FLAC (lossless)";
    case RecFormat::Mp3:  return L"MP3 (lossy)";
    case RecFormat::Aac:  return L"AAC in an M4A file (lossy)";
    case RecFormat::Ogg:  return L"Ogg Vorbis (lossy)";
    case RecFormat::Opus: return L"Opus (lossy)";
    default:              return L"WAV (uncompressed)";
    }
}

static std::wstring SanitiseName(const std::wstring& in)
{
    std::wstring out;
    for (wchar_t c : in)
    {
        if (wcschr(L"\\/:*?\"<>|", c)) out += L'-';
        else if (c >= L' ')            out += c;
    }
    // Trailing dots and spaces are legal to type but not to create.
    while (!out.empty() && (out.back() == L'.' || out.back() == L' ')) out.pop_back();
    return out.empty() ? std::wstring(L"recording") : out;
}

static void ReplaceAll(std::wstring& s, const std::wstring& from, const std::wstring& to)
{
    if (from.empty()) return;
    size_t at = 0;
    while ((at = s.find(from, at)) != std::wstring::npos)
    {
        s.replace(at, from.size(), to);
        at += to.size();
    }
}

std::wstring ResolveRecordingPath(const std::wstring& folder,
                                  const std::wstring& nameTemplate,
                                  const std::wstring& sessionName,
                                  RecFormat fmt)
{
    SYSTEMTIME t;
    GetLocalTime(&t);

    std::wstring date = FormatW(L"%04d-%02d-%02d", t.wYear, t.wMonth, t.wDay);
    std::wstring time = FormatW(L"%02d-%02d-%02d", t.wHour, t.wMinute, t.wSecond);

    std::wstring name = nameTemplate.empty() ? std::wstring(L"{session} {date} {time}")
                                             : nameTemplate;
    ReplaceAll(name, L"{session}",  sessionName);
    ReplaceAll(name, L"{date}",     date);
    ReplaceAll(name, L"{time}",     time);
    ReplaceAll(name, L"{datetime}", date + L" " + time);
    name = SanitiseName(name);

    // Worked out in one place, because Ctrl+Shift+O has to open the same
    // folder this is about to write into.
    std::wstring dir = ResolveRecordFolder(folder);
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);

    const wchar_t* ext = RecFormatExt(fmt);

    // {n} is the collision escape hatch. Whether or not the template uses it,
    // never overwrite a file that is already there.
    std::wstring path;
    for (int n = 1; n < 10000; ++n)
    {
        std::wstring candidate = name;
        if (candidate.find(L"{n}") != std::wstring::npos)
            ReplaceAll(candidate, L"{n}", FormatW(L"%03d", n));
        else if (n > 1)
            candidate += FormatW(L" (%d)", n);

        path = dir + L"\\" + candidate + L"." + ext;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) break;
    }
    return path;
}

// ---------------------------------------------------------------------------
// Writer thread
//
// Encoding must never happen on the mixer's thread: the file system and the
// Media Foundation encoders both block. Audio is queued and a writer thread
// drains it. The queue is deliberately unbounded - dropping a recording to
// save a few megabytes of RAM would be the wrong trade.
// ---------------------------------------------------------------------------
class RecorderBase : public Recorder
{
public:
    void Write(const float* buf, size_t frames) override
    {
        if (!m_running) return;
        {
            std::lock_guard<std::mutex> g(m_lock);
            m_queue.insert(m_queue.end(), buf, buf + frames * kChannels);
        }
        m_wake.notify_one();
    }

    void Stop() override
    {
        if (!m_running) return;
        m_running = false;
        m_wake.notify_all();
        if (m_thread.joinable()) m_thread.join();
        Close();
    }

    std::wstring Path() const override { return m_path; }

protected:
    void RunWriter()
    {
        m_running = true;
        m_thread = std::thread([this] {
            std::vector<float> chunk;
            for (;;)
            {
                {
                    std::unique_lock<std::mutex> g(m_lock);
                    m_wake.wait(g, [this] { return !m_queue.empty() || !m_running; });
                    if (m_queue.empty() && !m_running) break;
                    chunk.assign(m_queue.begin(), m_queue.end());
                    m_queue.clear();
                }
                if (!chunk.empty()) Encode(chunk.data(), chunk.size() / kChannels);
            }
        });
    }

    // Converts the interleaved float bus to interleaved 16 bit PCM.
    static void ToPcm16(const float* src, size_t frames, std::vector<short>& dst)
    {
        dst.resize(frames * kChannels);
        for (size_t i = 0; i < frames * kChannels; ++i)
        {
            float v = src[i];
            if (v > 1.0f) v = 1.0f;
            else if (v < -1.0f) v = -1.0f;
            dst[i] = (short)lrintf(v * 32767.0f);
        }
    }

    virtual void Encode(const float* buf, size_t frames) = 0;
    virtual void Close() = 0;

    std::wstring m_path;
    int m_rate = 48000;

private:
    std::deque<float> m_queue;
    std::mutex m_lock;
    std::condition_variable m_wake;
    std::thread m_thread;
    std::atomic<bool> m_running{ false };
};

// ---------------------------------------------------------------------------
// WAV
//
// Written directly rather than through Media Foundation: it is a header and
// a stream of samples, and doing it here means the size fields can be patched
// on close even if the encoder path is unavailable.
// ---------------------------------------------------------------------------
class WavRecorder : public RecorderBase
{
public:
    ~WavRecorder() override { Stop(); }

    bool Start(const std::wstring& path, int rate, int, std::wstring* err) override
    {
        m_path = path;
        m_rate = rate;

        if (_wfopen_s(&m_f, path.c_str(), L"wb") != 0 || !m_f)
        {
            if (err) *err = L"Could not create " + path;
            return false;
        }

        const int bytesPerFrame = kChannels * 2;
        unsigned char hdr[44] = { 0 };
        memcpy(hdr + 0, "RIFF", 4);
        memcpy(hdr + 8, "WAVEfmt ", 8);
        Put32(hdr + 16, 16);                       // fmt chunk size
        Put16(hdr + 20, 1);                        // PCM
        Put16(hdr + 22, (unsigned short)kChannels);
        Put32(hdr + 24, (unsigned)rate);
        Put32(hdr + 28, (unsigned)(rate * bytesPerFrame));
        Put16(hdr + 32, (unsigned short)bytesPerFrame);
        Put16(hdr + 34, 16);                       // bits per sample
        memcpy(hdr + 36, "data", 4);
        fwrite(hdr, 1, sizeof(hdr), m_f);

        RunWriter();
        return true;
    }

protected:
    void Encode(const float* buf, size_t frames) override
    {
        if (!m_f) return;
        ToPcm16(buf, frames, m_pcm);
        fwrite(m_pcm.data(), sizeof(short), m_pcm.size(), m_f);
        m_bytes += m_pcm.size() * sizeof(short);
    }

    void Close() override
    {
        if (!m_f) return;
        // Patch the two sizes now that the length is known.
        unsigned char v[4];
        Put32(v, (unsigned)(36 + m_bytes));
        fseek(m_f, 4, SEEK_SET);
        fwrite(v, 1, 4, m_f);
        Put32(v, (unsigned)m_bytes);
        fseek(m_f, 40, SEEK_SET);
        fwrite(v, 1, 4, m_f);
        fclose(m_f);
        m_f = nullptr;
    }

private:
    static void Put16(unsigned char* p, unsigned short v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
    static void Put32(unsigned char* p, unsigned v)
    {
        p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
    }

    FILE* m_f = nullptr;
    size_t m_bytes = 0;
    std::vector<short> m_pcm;
};

// ---------------------------------------------------------------------------
// FLAC and MP3, through Media Foundation
//
// Windows ships both encoders (a FLAC encoder MFT and an MP3 ACM wrapper MFT),
// so neither format needs a third party library. The sink writer picks the
// file container from the extension.
// ---------------------------------------------------------------------------
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

class MfRecorder : public RecorderBase
{
public:
    explicit MfRecorder(RecFormat fmt) : m_fmt(fmt) {}
    ~MfRecorder() override { Stop(); }

    bool Start(const std::wstring& path, int rate, int kbps, std::wstring* err) override
    {
        m_path = path;
        m_rate = rate;

        HRESULT hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) { Fail(err, L"Media Foundation could not start", hr); return false; }
        m_mfUp = true;

        const GUID sub = (m_fmt == RecFormat::Flac) ? MFAudioFormat_FLAC
                       : (m_fmt == RecFormat::Aac)  ? MFAudioFormat_AAC
                                                    : MFAudioFormat_MP3;
        IMFMediaType* out = PickOutputType(sub, rate, kbps);
        if (!out)
        {
            if (err) *err = FormatW(
                L"Windows has no %s encoder that accepts %d Hz stereo. Record to "
                L"WAV instead, or set the output device to 44100 or 48000 Hz.",
                RecFormatName(m_fmt), rate);
            return false;
        }

        hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, nullptr, &m_writer);
        if (FAILED(hr)) { out->Release(); Fail(err, L"Could not create " + path, hr); return false; }

        hr = m_writer->AddStream(out, &m_stream);
        out->Release();
        if (FAILED(hr)) { Fail(err, L"The encoder rejected the output format", hr); return false; }

        IMFMediaType* in = nullptr;
        if (SUCCEEDED(MFCreateMediaType(&in)) && in)
        {
            in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)rate);
            in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
            in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kChannels * 2);
            in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)(rate * kChannels * 2));
            in->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
            hr = m_writer->SetInputMediaType(m_stream, in, nullptr);
            in->Release();
        }
        if (FAILED(hr)) { Fail(err, L"The encoder rejected 16 bit PCM input", hr); return false; }

        hr = m_writer->BeginWriting();
        if (FAILED(hr)) { Fail(err, L"The encoder could not start", hr); return false; }

        RunWriter();
        return true;
    }

protected:
    void Encode(const float* buf, size_t frames) override
    {
        if (!m_writer || frames == 0) return;
        ToPcm16(buf, frames, m_pcm);

        const DWORD bytes = (DWORD)(m_pcm.size() * sizeof(short));
        IMFMediaBuffer* mb = nullptr;
        if (FAILED(MFCreateMemoryBuffer(bytes, &mb)) || !mb) return;

        BYTE* p = nullptr;
        DWORD maxLen = 0;
        if (SUCCEEDED(mb->Lock(&p, &maxLen, nullptr)))
        {
            memcpy(p, m_pcm.data(), bytes);
            mb->Unlock();
            mb->SetCurrentLength(bytes);

            IMFSample* smp = nullptr;
            if (SUCCEEDED(MFCreateSample(&smp)) && smp)
            {
                smp->AddBuffer(mb);
                smp->SetSampleTime(m_time);
                LONGLONG dur = (LONGLONG)frames * 10000000 / m_rate;
                smp->SetSampleDuration(dur);
                m_time += dur;
                m_writer->WriteSample(m_stream, smp);
                smp->Release();
            }
        }
        mb->Release();
    }

    void Close() override
    {
        if (m_writer)
        {
            m_writer->Finalize();
            m_writer->Release();
            m_writer = nullptr;
        }
        if (m_mfUp) { MFShutdown(); m_mfUp = false; }
    }

private:
    void Fail(std::wstring* err, const std::wstring& what, HRESULT hr)
    {
        if (err) *err = what + L" (" + HrText(hr) + L").";
    }

    // Picks the encoder output type matching our rate and channel count. For
    // FLAC the bit depth has to match the input type exactly, and for MP3 we
    // take whichever advertised bitrate is closest to the one asked for.
    static IMFMediaType* PickOutputType(const GUID& sub, int rate, int kbps)
    {
        IMFCollection* col = nullptr;
        if (FAILED(MFTranscodeGetAudioOutputAvailableTypes(sub, MFT_ENUM_FLAG_ALL,
                                                           nullptr, &col)) || !col)
            return nullptr;

        DWORD count = 0;
        col->GetElementCount(&count);

        const UINT32 wantBytes = (UINT32)(kbps * 1000 / 8);
        IMFMediaType* best = nullptr;
        UINT32 bestDelta = 0xFFFFFFFF;

        for (DWORD i = 0; i < count; ++i)
        {
            IUnknown* unk = nullptr;
            if (FAILED(col->GetElement(i, &unk)) || !unk) continue;

            IMFMediaType* t = nullptr;
            if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&t))) && t)
            {
                UINT32 sr = 0, ch = 0, bits = 0, bps = 0;
                t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
                t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
                t->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
                t->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bps);

                bool ok = (sr == (UINT32)rate) && (ch == (UINT32)kChannels) &&
                          (bits == 0 || bits == 16);
                if (ok)
                {
                    UINT32 d = (bps > wantBytes) ? bps - wantBytes : wantBytes - bps;
                    if (d < bestDelta)
                    {
                        bestDelta = d;
                        if (best) best->Release();
                        best = t;
                        best->AddRef();
                    }
                }
                t->Release();
            }
            unk->Release();
        }
        col->Release();
        return best;
    }

    RecFormat m_fmt;
    IMFSinkWriter* m_writer = nullptr;
    DWORD m_stream = 0;
    LONGLONG m_time = 0;
    bool m_mfUp = false;
    std::vector<short> m_pcm;
};

// Media Foundation is delay loaded, so nothing here touches it until this
// says the DLLs are there. Windows N and KN editions ship without them until
// the Media Feature Pack is installed, and a delay loaded import that cannot
// be resolved raises rather than returning an error.
bool MediaFoundationAvailable()
{
    static bool ok = LoadLibraryW(L"mfplat.dll") != nullptr &&
                     LoadLibraryW(L"mfreadwrite.dll") != nullptr &&
                     LoadLibraryW(L"mf.dll") != nullptr;
    return ok;
}

// Whether Windows here can encode a format at all. Probed once: enumerating
// the encoder MFTs is not free, and the answer cannot change while we run.
bool RecFormatAvailable(RecFormat fmt)
{
    // Written here rather than by Windows, so these work everywhere.
    if (fmt == RecFormat::Wav || fmt == RecFormat::Ogg || fmt == RecFormat::Opus)
        return true;
    if (!MediaFoundationAvailable()) return false;

    static int cached[kRecFormatCount] = { 1, -1, -1, -1, 1, 1 };
    int idx = (int)fmt;
    if (cached[idx] >= 0) return cached[idx] != 0;

    bool ok = false;
    if (SUCCEEDED(MFStartup(MF_VERSION)))
    {
        const GUID sub = (fmt == RecFormat::Flac) ? MFAudioFormat_FLAC
                       : (fmt == RecFormat::Aac)  ? MFAudioFormat_AAC
                                                  : MFAudioFormat_MP3;
        IMFCollection* col = nullptr;
        if (SUCCEEDED(MFTranscodeGetAudioOutputAvailableTypes(sub, MFT_ENUM_FLAG_ALL,
                                                              nullptr, &col)) && col)
        {
            DWORD count = 0;
            col->GetElementCount(&count);
            ok = count > 0;
            col->Release();
        }
        MFShutdown();
    }
    cached[idx] = ok ? 1 : 0;
    return ok;
}

std::unique_ptr<Recorder> MakeRecorder(RecFormat fmt)
{
    if (fmt == RecFormat::Wav)  return std::unique_ptr<Recorder>(new WavRecorder());
    if (fmt == RecFormat::Ogg)  return MakeVorbisRecorder();
    if (fmt == RecFormat::Opus) return MakeOpusRecorder();
    return std::unique_ptr<Recorder>(new MfRecorder(fmt));
}

// ---------------------------------------------------------------------------
// Recorder block output: silence gate and file splitting
// ---------------------------------------------------------------------------

RecordSettings RecordSettingsFor(const Block& b, const std::wstring& sessionName)
{
    RecordSettings s;
    s.folder = b.folder;
    s.nameTemplate = b.nameTemplate;
    s.sessionName = sessionName;
    // Against kRecFormatCount, not against 2. It was 2 when WAV, FLAC and MP3
    // were the only formats, and it stayed 2 when AAC, Ogg Vorbis and Opus
    // were added - so choosing one of those three silently recorded a WAV
    // file, under whatever name the template gave it.
    s.fmt = (RecFormat)((b.recFormat >= 0 && b.recFormat < kRecFormatCount)
                            ? b.recFormat : 0);
    s.kbps = b.bitrateKbps;
    s.skipSilence = b.skipSilence;
    s.silenceDb = b.silenceDb;
    s.holdSeconds = b.silenceHoldSec;
    s.splitMode = b.splitMode;
    s.splitMinutes = b.splitMinutes;
    s.splitMb = b.splitMb;
    return s;
}

// The gate measures over short blocks rather than per sample, so a single
// sample either way cannot open or shut it.
static const int kMeterMs = 20;
// Held back while the gate is shut and flushed when it opens, so the attack
// that opened it is in the file rather than clipped off the front of it.
static const int kPreRollMs = 300;
// RIFF stores its sizes in 32 bits, so a WAV cannot exceed 4 GB. This is the
// ceiling a WAV is rotated at whatever the split setting says, because the
// alternative is a file that silently stops being readable at the end.
static const unsigned long long kWavCeilingBytes = 4000ull * 1000ull * 1000ull;

bool RecordSink::Start(const RecordSettings& s, int rate, std::wstring* err)
{
    m_set = s;
    m_rate = rate;
    m_open = !s.skipSilence;   // with no gate the file starts immediately
    m_running = true;

    // The first file is opened on the sink thread like every later one, so
    // that every Media Foundation call for this recording happens on the one
    // thread. Start() waits for the answer so it can still be reported.
    m_thread = std::thread([this] { Worker(); });

    std::unique_lock<std::mutex> g(m_openLock);
    m_openCv.wait(g, [this] { return m_openDone; });
    if (!m_openOk)
    {
        g.unlock();
        m_running = false;
        m_wake.notify_all();
        if (m_thread.joinable()) m_thread.join();
        if (err) *err = m_openErr;
        return false;
    }
    return true;
}

void RecordSink::Write(const float* buf, size_t frames)
{
    if (!m_running) return;
    {
        std::lock_guard<std::mutex> g(m_lock);
        m_queue.insert(m_queue.end(), buf, buf + frames * kChannels);
    }
    m_wake.notify_one();
}

void RecordSink::Stop()
{
    if (!m_running.exchange(false)) return;
    m_wake.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void RecordSink::Configure(const RecordSettings& s)
{
    std::lock_guard<std::mutex> g(m_setLock);
    m_set = s;
}

RecordSettings RecordSink::Snapshot()
{
    std::lock_guard<std::mutex> g(m_setLock);
    return m_set;
}

std::wstring RecordSink::Path()
{
    std::lock_guard<std::mutex> g(m_setLock);
    return m_file ? m_file->Path() : std::wstring();
}

void RecordSink::Worker()
{
    // Media Foundation's sink writer wants COM up on the calling thread, and
    // this thread is the only one that ever touches it.
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    std::wstring err;
    bool ok = Open(&err);
    {
        std::lock_guard<std::mutex> g(m_openLock);
        m_openDone = true;
        m_openOk = ok;
        m_openErr = err;
    }
    m_openCv.notify_all();

    if (ok)
    {
        std::vector<float> chunk;
        for (;;)
        {
            {
                std::unique_lock<std::mutex> g(m_lock);
                m_wake.wait(g, [this] { return !m_queue.empty() || !m_running; });
                if (m_queue.empty() && !m_running) break;
                chunk.assign(m_queue.begin(), m_queue.end());
                m_queue.clear();
            }
            if (!chunk.empty()) Consume(chunk.data(), chunk.size() / kChannels);
        }

        // Whatever is left in the metering block goes out if the gate is open;
        // a partial block is not worth measuring.
        if (m_open && !m_acc.empty())
            Emit(m_acc.data(), m_acc.size() / kChannels);
        CloseFile();
    }

    if (SUCCEEDED(hrCom)) CoUninitialize();
}

bool RecordSink::Open(std::wstring* err)
{
    RecordSettings s = Snapshot();

    if (!RecFormatAvailable(s.fmt))
    {
        if (err) *err = FormatW(
            L"%s cannot encode %s, so this block cannot record in that format. "
            L"Choose WAV, Ogg Vorbis or Opus, which FastRoute writes itself.",
            WindowsVersionText().c_str(), RecFormatName(s.fmt));
        return false;
    }

    std::wstring path = ResolveRecordingPath(s.folder, s.nameTemplate, s.sessionName, s.fmt);

    std::unique_ptr<Recorder> r = MakeRecorder(s.fmt);
    if (!r || !r->Start(path, m_rate, s.kbps, err)) return false;

    m_file = std::move(r);
    m_fileFrames = 0;
    m_sinceSizeCheck = 0;
    m_files.fetch_add(1);
    return true;
}

void RecordSink::CloseFile()
{
    if (!m_file) return;
    m_file->Stop();
    m_file.reset();
}

bool RecordSink::Rotate()
{
    CloseFile();
    std::wstring err;
    if (Open(&err))
        return true;

    m_dead = true;
    LogError(L"The recording could not be continued in a new file, so it has "
             L"stopped: " + (err.empty() ? std::wstring(L"the file could not be created.") : err));
    return false;
}

// Runs the gate over `frames` and passes whatever survives to Emit.
void RecordSink::Consume(const float* buf, size_t frames)
{
    if (m_dead) return;

    RecordSettings s = Snapshot();

    if (!s.skipSilence)
    {
        // The gate is off, so anything held back from when it was on has to go
        // out first, in order, before this block.
        if (!m_open)
        {
            m_open = true;
            m_quietFrames = 0;
        }
        if (!m_acc.empty())
        {
            std::vector<float> pending;
            pending.swap(m_acc);
            Emit(pending.data(), pending.size() / kChannels);
        }
        if (!m_preRoll.empty())
        {
            std::vector<float> pre(m_preRoll.begin(), m_preRoll.end());
            m_preRoll.clear();
            Emit(pre.data(), pre.size() / kChannels);
        }
        m_skipping.store(false);
        Emit(buf, frames);
        return;
    }

    const size_t block = (size_t)(m_rate * kMeterMs / 1000) * kChannels;
    const size_t preRollMax = (size_t)(m_rate * kPreRollMs / 1000) * kChannels;
    const size_t holdFrames = (size_t)(s.holdSeconds > 0 ? s.holdSeconds : 0) *
                              (size_t)m_rate;
    // The threshold is an amplitude once it is off the dial.
    const float threshold = (float)pow(10.0, (double)s.silenceDb / 20.0);

    m_acc.insert(m_acc.end(), buf, buf + frames * kChannels);

    size_t at = 0;
    while (m_acc.size() - at >= block)
    {
        const float* p = m_acc.data() + at;

        double sum = 0.0;
        for (size_t i = 0; i < block; ++i) sum += (double)p[i] * (double)p[i];
        const float rms = (float)sqrt(sum / (double)block);

        if (rms >= threshold)
        {
            if (!m_open)
            {
                // Opening: the pre-roll is the run-up to this block, so it
                // goes into the file ahead of it.
                m_open = true;
                if (!m_preRoll.empty())
                {
                    std::vector<float> pre(m_preRoll.begin(), m_preRoll.end());
                    m_preRoll.clear();
                    Emit(pre.data(), pre.size() / kChannels);
                }
            }
            m_quietFrames = 0;
        }
        else if (m_open && s.holdSeconds != kHoldAlways)
        {
            // A hold of "always" means the gate only ever opens: the threshold
            // decides when the recording starts and nothing after that.
            m_quietFrames += block / kChannels;
            if (m_quietFrames >= holdFrames) m_open = false;
        }

        if (m_open)
        {
            Emit(p, block / kChannels);
        }
        else
        {
            m_preRoll.insert(m_preRoll.end(), p, p + block);
            while (m_preRoll.size() > preRollMax)
                m_preRoll.erase(m_preRoll.begin(),
                                m_preRoll.begin() + (m_preRoll.size() - preRollMax));
        }
        at += block;
        if (m_dead) break;
    }

    if (at) m_acc.erase(m_acc.begin(), m_acc.begin() + at);
    m_skipping.store(!m_open);
}

// How big the current file is. A WAV is 16 bit PCM at a known rate, so its
// size follows from the frame count exactly and with no lag - which matters,
// because the file on disk trails whatever the writer thread has flushed. An
// encoded file can only be measured, so that one is asked of the file system.
unsigned long long RecordSink::CurrentBytes(const RecordSettings& s)
{
    unsigned long long bytes = 0;
    if (s.fmt == RecFormat::Wav)
    {
        bytes = 44ull + m_fileFrames * (unsigned long long)(kChannels * 2);
    }
    else
    {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (m_file && GetFileAttributesExW(m_file->Path().c_str(),
                                           GetFileExInfoStandard, &fad))
            bytes = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    }
    // Published for the status line as a side effect of working it out, so
    // nothing has to hit the file system twice for the same number.
    m_bytes.store(bytes);
    return bytes;
}

// Rotates if the file has had enough bytes. False means recording has stopped.
bool RecordSink::CheckSize(const RecordSettings& s)
{
    const bool wantSize = (s.splitMode == 2);
    const bool wav = (s.fmt == RecFormat::Wav);
    if (!wantSize && !wav) return true;

    unsigned long long bytes = CurrentBytes(s);

    if (wantSize)
    {
        unsigned long long limit =
            (unsigned long long)(s.splitMb > 0 ? s.splitMb : 1) * 1024ull * 1024ull;
        if (bytes >= limit) return Rotate();
    }
    if (wav && bytes >= kWavCeilingBytes)
    {
        LogError(L"A WAV recording reached 4 GB, which is as large as the format "
                 L"can describe, so it has been continued in a new file.");
        return Rotate();
    }
    return true;
}

// Writes to the current file, splitting it where the settings say to.
void RecordSink::Emit(const float* buf, size_t frames)
{
    // Written in slices rather than whole buffers. A split boundary has to be
    // honoured wherever it falls, and audio does not always arrive in real
    // time - a recording catching up after a stall can hand over minutes at
    // once, and a whole-buffer write would sail straight past the limit.
    const size_t slice = (size_t)(m_rate / 4);

    while (frames > 0)
    {
        if (!m_file || m_dead) return;

        const RecordSettings s = Snapshot();
        const unsigned long long frameLimit = (s.splitMode == 1)
            ? (unsigned long long)(s.splitMinutes > 0 ? s.splitMinutes : 1) * 60ull *
              (unsigned long long)m_rate
            : 0;

        if (frameLimit && m_fileFrames >= frameLimit)
        {
            if (!Rotate()) return;
            continue;
        }

        size_t take = frames;
        if (frameLimit)
        {
            // A file asked to hold ten minutes holds ten minutes, not ten
            // minutes and however much the last buffer happened to carry.
            unsigned long long room = frameLimit - m_fileFrames;
            if (room < (unsigned long long)take) take = (size_t)room;
        }
        if (take > slice) take = slice;

        m_file->Write(buf, take);
        m_fileFrames += take;
        m_sinceSizeCheck += take;
        m_totalFrames.fetch_add(take);
        // WAV size follows from the frame count, so it can be kept current
        // here for nothing. The encoded formats are only measured when the
        // splitting asks, which is often enough for a status line.
        if (Snapshot().fmt == RecFormat::Wav)
            m_bytes.store(44ull + m_fileFrames * (unsigned long long)(kChannels * 2));
        buf += take * kChannels;
        frames -= take;

        if (frameLimit && m_fileFrames >= frameLimit)
        {
            if (!Rotate()) return;
            continue;
        }

        // An encoded file has to be measured on disk, so that is throttled;
        // a WAV is arithmetic and can be checked every slice.
        if (s.fmt == RecFormat::Wav || m_sinceSizeCheck >= (size_t)m_rate)
        {
            m_sinceSizeCheck = 0;
            if (!CheckSize(s)) return;
        }
    }
}
