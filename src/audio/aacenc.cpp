#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>

#include "aacenc.h"
#include "rtmp.h"

#include <stdio.h>

namespace {

std::wstring HrMsg(const wchar_t* what, HRESULT hr)
{
    wchar_t buf[64];
    swprintf_s(buf, L" (0x%08lX).", (unsigned long)hr);
    return std::wstring(what) + buf;
}

}   // namespace

struct AacEncoder::Impl
{
    IMFTransform* mft = nullptr;
    bool mfUp = false;
    bool providesSamples = false;
    DWORD outSize = 0;
};

AacEncoder::AacEncoder() : m(new Impl()) {}
AacEncoder::~AacEncoder() { Stop(); delete m; }

int AacEncoder::NearestKbps(int kbps)
{
    static const int allowed[] = { 96, 128, 160, 192 };
    int best = 192, bestDiff = 1 << 30;
    for (int a : allowed)
    {
        int d = a > kbps ? a - kbps : kbps - a;
        if (d < bestDiff) { best = a; bestDiff = d; }
    }
    return best;
}

void AacEncoder::Stop()
{
    if (m->mft)
    {
        m->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        m->mft->Release();
        m->mft = nullptr;
    }
    if (m->mfUp) { MFShutdown(); m->mfUp = false; }
    m_inFrames = 0;
}

int AacEncoder::NearestMp3Kbps(int kbps)
{
    static const int allowed[] = { 96, 128, 160, 192, 224, 256, 320 };
    int best = 192, bestDiff = 1 << 30;
    for (int a : allowed)
    {
        int d = a > kbps ? a - kbps : kbps - a;
        if (d < bestDiff) { best = a; bestDiff = d; }
    }
    return best;
}

bool AacEncoder::Mp3Available()
{
    static int cached = -1;
    if (cached >= 0) return cached != 0;
    cached = 0;
    if (SUCCEEDED(MFStartup(MF_VERSION)))
    {
        MFT_REGISTER_TYPE_INFO outInfo = { MFMediaType_Audio, MFAudioFormat_MP3 };
        IMFActivate** acts = nullptr;
        UINT32 count = 0;
        if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER,
                                MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
                                    MFT_ENUM_FLAG_SORTANDFILTER,
                                nullptr, &outInfo, &acts, &count)) && acts)
        {
            cached = count > 0 ? 1 : 0;
            for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
            CoTaskMemFree(acts);
        }
        MFShutdown();
    }
    return cached != 0;
}

bool AacEncoder::StartCodec(int sampleRate, int kbps, bool mp3, std::wstring* err)
{
    Stop();
    m_rate = sampleRate;
    m_mp3 = mp3;
    m_kbps = mp3 ? NearestMp3Kbps(kbps) : NearestKbps(kbps);

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) { if (err) *err = HrMsg(L"Media Foundation could not start", hr); return false; }
    m->mfUp = true;

    MFT_REGISTER_TYPE_INFO outInfo = { MFMediaType_Audio,
                                       mp3 ? MFAudioFormat_MP3 : MFAudioFormat_AAC };
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    // No input filter: the MP3 encoder registers no input types, so asking
    // for PCM in would find nothing even where it is installed.
    hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER,
                   MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
                       MFT_ENUM_FLAG_SORTANDFILTER,
                   nullptr, &outInfo, &acts, &count);
    if (FAILED(hr) || count == 0)
    {
        if (acts) CoTaskMemFree(acts);
        if (err) *err = mp3
            ? L"Windows on this machine has no MP3 encoder. Choose AAC instead."
            : L"Windows on this machine has no AAC encoder. On an N "
              L"edition of Windows, install the Media Feature Pack.";
        return false;
    }
    hr = acts[0]->ActivateObject(IID_PPV_ARGS(&m->mft));
    for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    if (FAILED(hr) || !m->mft)
    {
        if (err) *err = HrMsg(L"The Windows AAC encoder could not be started", hr);
        return false;
    }

    IMFMediaType* in = nullptr;
    IMFMediaType* out = nullptr;
    MFCreateMediaType(&in);
    MFCreateMediaType(&out);
    if (!in || !out)
    {
        if (in) in->Release();
        if (out) out->Release();
        if (err) *err = L"Out of memory setting up the AAC encoder.";
        return false;
    }
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)sampleRate);
    in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)(sampleRate * 4));

    if (!mp3)
    {
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)sampleRate);
        out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)(m_kbps * 1000 / 8));
        out->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);                     // raw
        out->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29); // AAC-LC
    }
    else
    {
        // The MP3 encoder wants one of the types it offers, word for word,
        // so the closest bitrate at this rate in stereo is picked from them.
        // The input has to be set first for it to offer any.
        hr = m->mft->SetInputType(0, in, 0);
        IMFMediaType* best = nullptr;
        UINT32 bestDiff = 0xFFFFFFFF;
        for (DWORD i = 0; SUCCEEDED(hr); ++i)
        {
            IMFMediaType* t = nullptr;
            if (FAILED(m->mft->GetOutputAvailableType(0, i, &t)) || !t) break;
            UINT32 r = 0, c = 0, bps = 0;
            t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &r);
            t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &c);
            t->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bps);
            const UINT32 want = (UINT32)(m_kbps * 125);
            const UINT32 d = bps > want ? bps - want : want - bps;
            if (r == (UINT32)sampleRate && c == 2 && d < bestDiff)
            {
                if (best) best->Release();
                best = t;
                bestDiff = d;
            }
            else
                t->Release();
        }
        if (!best)
        {
            in->Release();
            out->Release();
            if (err) *err = HrMsg(L"The Windows MP3 encoder offers nothing for "
                                     L"stereo at this rate", hr);
            return false;
        }
        UINT32 bps = 0;
        best->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bps);
        m_kbps = (int)(bps / 125);
        out->Release();
        out = best;
    }

    // The documented order is input first; some builds of the encoder are
    // happier the other way round, so both are tried before giving up.
    hr = m->mft->SetInputType(0, in, 0);
    if (SUCCEEDED(hr)) hr = m->mft->SetOutputType(0, out, 0);
    if (FAILED(hr))
    {
        m->mft->SetInputType(0, nullptr, 0);
        hr = m->mft->SetOutputType(0, out, 0);
        if (SUCCEEDED(hr)) hr = m->mft->SetInputType(0, in, 0);
    }
    in->Release();
    if (FAILED(hr))
    {
        out->Release();
        if (err) *err = HrMsg(mp3 ? L"The Windows MP3 encoder would not take stereo "
                                    L"at this rate and bitrate"
                                  : L"The Windows AAC encoder would not take stereo at "
                                    L"this rate and bitrate", hr);
        return false;
    }

    // The configuration the encoder itself reports, when it says: it is the
    // tail of the HEAACWAVEINFO in the user data. Otherwise the standard two
    // bytes for AAC-LC, which is what was asked for.
    m_asc = rtmp::AacLcConfig(sampleRate, 2);
    if (!mp3)
    {
        IMFMediaType* cur = nullptr;
        if (SUCCEEDED(m->mft->GetOutputCurrentType(0, &cur)) && cur)
        {
            UINT32 len = 0;
            if (SUCCEEDED(cur->GetBlobSize(MF_MT_USER_DATA, &len)) && len >= 14 && len < 64)
            {
                std::vector<uint8_t> ud(len);
                if (SUCCEEDED(cur->GetBlob(MF_MT_USER_DATA, ud.data(), len, nullptr)))
                    m_asc.assign(ud.begin() + 12, ud.end());
            }
            cur->Release();
        }
    }
    out->Release();

    MFT_OUTPUT_STREAM_INFO osi = {};
    m->mft->GetOutputStreamInfo(0, &osi);
    m->providesSamples = (osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                         MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    m->outSize = osi.cbSize ? osi.cbSize : 8192;

    m->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool AacEncoder::Drain(std::vector<std::vector<uint8_t>>* frames, std::wstring* err)
{
    for (int guard = 0; guard < 1000; ++guard)
    {
        MFT_OUTPUT_DATA_BUFFER ob = {};
        IMFSample* sample = nullptr;
        IMFMediaBuffer* buf = nullptr;
        if (!m->providesSamples)
        {
            if (FAILED(MFCreateSample(&sample)) ||
                FAILED(MFCreateMemoryBuffer(m->outSize, &buf)))
            {
                if (sample) sample->Release();
                if (err) *err = L"Out of memory while encoding.";
                return false;
            }
            sample->AddBuffer(buf);
            ob.pSample = sample;
        }
        DWORD status = 0;
        HRESULT hr = m->mft->ProcessOutput(0, 1, &ob, &status);
        if (ob.pEvents) ob.pEvents->Release();

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
        {
            if (buf) buf->Release();
            if (sample) sample->Release();
            return true;
        }
        if (FAILED(hr))
        {
            if (buf) buf->Release();
            if (sample) sample->Release();
            if (ob.pSample && ob.pSample != sample) ob.pSample->Release();
            if (err) *err = HrMsg(L"The AAC encoder failed", hr);
            return false;
        }

        IMFSample* got = ob.pSample;
        IMFMediaBuffer* flat = nullptr;
        if (got && SUCCEEDED(got->ConvertToContiguousBuffer(&flat)) && flat)
        {
            BYTE* p = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(flat->Lock(&p, nullptr, &len)))
            {
                if (len > 0) frames->emplace_back(p, p + len);
                flat->Unlock();
            }
            flat->Release();
        }
        if (buf) buf->Release();
        if (sample) sample->Release();
        if (got && got != sample) got->Release();
    }
    return true;
}

bool AacEncoder::Encode(const int16_t* pcm, size_t sampleFrames,
                        std::vector<std::vector<uint8_t>>* frames, std::wstring* err)
{
    if (!m->mft || sampleFrames == 0) return m->mft != nullptr;

    const DWORD bytes = (DWORD)(sampleFrames * 4);
    IMFSample* sample = nullptr;
    IMFMediaBuffer* buf = nullptr;
    if (FAILED(MFCreateSample(&sample)) || FAILED(MFCreateMemoryBuffer(bytes, &buf)))
    {
        if (sample) sample->Release();
        if (err) *err = L"Out of memory while encoding.";
        return false;
    }
    BYTE* p = nullptr;
    if (SUCCEEDED(buf->Lock(&p, nullptr, nullptr)))
    {
        memcpy(p, pcm, bytes);
        buf->Unlock();
    }
    buf->SetCurrentLength(bytes);
    sample->AddBuffer(buf);
    buf->Release();
    sample->SetSampleTime(m_inFrames * 10000000LL / m_rate);
    sample->SetSampleDuration((LONGLONG)sampleFrames * 10000000LL / m_rate);
    m_inFrames += (long long)sampleFrames;

    HRESULT hr = m->mft->ProcessInput(0, sample, 0);
    if (hr == MF_E_NOTACCEPTING)
    {
        if (!Drain(frames, err)) { sample->Release(); return false; }
        hr = m->mft->ProcessInput(0, sample, 0);
    }
    sample->Release();
    if (FAILED(hr))
    {
        if (err) *err = HrMsg(L"The AAC encoder would not take audio", hr);
        return false;
    }
    return Drain(frames, err);
}
