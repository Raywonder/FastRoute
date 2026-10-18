// Stream Out self test: publishes a few seconds of a stereo tone to a real
// server with the same encoders and protocol code as the Stream Out block.
//
//   streamout_selftest rtmp URL KEY [seconds] [kbps]
//   streamout_selftest icecast|shoutcast1|shoutcast2 HOST:PORT MOUNT_OR_SID USER (- for none)
//                      FORMAT [seconds] [kbps] [title]
//
// FORMAT is aac, mp3, opus or vorbis. The source password is read from the
// environment variable FR_SELFTEST_PASS so it never appears on a command line.
//
// The left channel carries 440 Hz and the right 660 Hz, so a probe on the far
// end can tell a real stereo stream from a mono one copied to both sides.
// Exit code 0 means every byte was sent. Nothing secret is printed.
#include <winsock2.h>
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string>
#include <vector>

#include "audio/aacenc.h"
#include "audio/icecast.h"
#include "audio/rtmp.h"
#include "audio/streamenc.h"

static const int kRate = 48000;

static void Tone(std::vector<float>& buf, size_t frames, long long& n)
{
    buf.resize(frames * 2);
    for (size_t i = 0; i < frames; ++i, ++n)
    {
        double t = (double)n / kRate;
        buf[i * 2]     = (float)(sin(2 * 3.14159265358979 * 440 * t) * 0.25);
        buf[i * 2 + 1] = (float)(sin(2 * 3.14159265358979 * 660 * t) * 0.25);
    }
}

static void Pace(long long n, ULONGLONG start)
{
    long long due = n * 1000 / kRate;
    long long el = (long long)(GetTickCount64() - start);
    if (due > el) Sleep((DWORD)(due - el));
}

static std::string Narrow(const wchar_t* w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

static int Rtmp(int argc, wchar_t** argv)
{
    if (argc < 4) return 2;
    const int seconds = argc > 4 ? _wtoi(argv[4]) : 8;
    const int kbps = argc > 5 ? _wtoi(argv[5]) : 192;
    rtmp::Target t;
    std::wstring err;
    if (!rtmp::ParseTarget(argv[2], argv[3], &t, &err)) { fwprintf(stderr, L"target: %s\n", err.c_str()); return 3; }
    AacEncoder enc;
    if (!enc.Start(kRate, kbps, &err)) { fwprintf(stderr, L"encoder: %s\n", err.c_str()); return 4; }
    rtmp::Publisher pub;
    if (!pub.Connect(t, 10000, &err)) { fwprintf(stderr, L"connect: %s\n", err.c_str()); return 5; }
    wprintf(L"rtmp publishing, AAC %d kbps\n", enc.Kbps());
    pub.SendAudioMetadata(kRate, 2, enc.Kbps());
    pub.SendAacSequenceHeader(enc.Config().data(), enc.Config().size());

    std::vector<float> f;
    std::vector<int16_t> pcm(2048);
    std::vector<std::vector<uint8_t>> frames;
    long long n = 0, sent = 0;
    ULONGLONG start = GetTickCount64();
    while (n < (long long)seconds * kRate)
    {
        Tone(f, 1024, n);
        for (size_t i = 0; i < f.size(); ++i) pcm[i] = (int16_t)(f[i] * 32767);
        frames.clear();
        if (!enc.Encode(pcm.data(), 1024, &frames, &err)) { fwprintf(stderr, L"encode: %s\n", err.c_str()); return 6; }
        for (auto& fr : frames)
        {
            if (!pub.SendAacFrame(fr.data(), fr.size(), (uint32_t)(sent * 1024 * 1000 / kRate)))
            { fwprintf(stderr, L"send failed\n"); return 7; }
            ++sent;
        }
        if (!pub.Poll(&err)) { fwprintf(stderr, L"poll: %s\n", err.c_str()); return 8; }
        Pace(n, start);
    }
    wprintf(L"sent %lld AAC frames, %llu bytes\n", sent, pub.BytesSent());
    return 0;
}

static int Source(int argc, wchar_t** argv)
{
    if (argc < 6) return 2;
    std::wstring mode = argv[1];
    const int kind = mode == L"icecast" ? icecast::K_ICECAST
                   : mode == L"shoutcast2" ? icecast::K_SHOUTCAST2 : icecast::K_SHOUTCAST1;
    std::wstring fmtName = argv[5];
    const int fmt = fmtName == L"mp3" ? SF_MP3 : fmtName == L"opus" ? SF_OPUS
                  : fmtName == L"vorbis" ? SF_VORBIS : SF_AAC;
    const int seconds = argc > 6 ? _wtoi(argv[6]) : 8;
    const int kbps = argc > 7 ? _wtoi(argv[7]) : 128;
    const std::string title = argc > 8 ? Narrow(argv[8]) : std::string("FastRoute self test");

    icecast::Target t;
    std::wstring err;
    if (!icecast::ParseAddress(argv[2], kind == icecast::K_ICECAST ? argv[3] : L"", kind, &t, &err))
    { fwprintf(stderr, L"target: %s\n", err.c_str()); return 3; }
    if (kind == icecast::K_SHOUTCAST2) t.sid = _wtoi(argv[3]);
    t.user = Narrow(argv[4]);
    if (t.user == "-") t.user.clear();   // "-" is no user
    const char* pass = getenv("FR_SELFTEST_PASS");
    t.password = pass ? pass : "";
    t.name = "FastRoute self test";
    t.description = "A tone, 440 Hz left and 660 Hz right";
    t.genre = "Test";
    t.isPublic = false;

    std::unique_ptr<StreamEncoder> enc = MakeStreamEncoder(fmt);
    if (!enc->Start(kRate, kbps, title, &err)) { fwprintf(stderr, L"encoder: %s\n", err.c_str()); return 4; }
    t.contentType = enc->ContentType();
    t.kbps = enc->Kbps();

    icecast::Source src;
    if (!src.Connect(t, 10000, &err)) { fwprintf(stderr, L"connect: %s\n", err.c_str()); return 5; }
    wprintf(L"source connected, %S %d kbps\n", t.contentType.c_str(), t.kbps);

    std::vector<float> f;
    std::vector<uint8_t> bytes;
    long long n = 0;
    bool titled = false;
    ULONGLONG start = GetTickCount64();
    while (n < (long long)seconds * kRate)
    {
        Tone(f, 1024, n);
        if (!enc->Encode(f.data(), 1024, &err)) { fwprintf(stderr, L"encode: %s\n", err.c_str()); return 6; }
        bytes.clear();
        enc->Take(&bytes);
        if (!bytes.empty() && !src.Send(bytes.data(), bytes.size(), &err))
        { fwprintf(stderr, L"send: %s\n", err.c_str()); return 7; }
        if (!src.Poll(&err)) { fwprintf(stderr, L"poll: %s\n", err.c_str()); return 8; }
        if (!titled && n > kRate * 2 && fmt != SF_OPUS && fmt != SF_VORBIS)
        {
            titled = true;
            if (icecast::UpdateTitle(t, title, &err)) wprintf(L"title updated\n");
            else fwprintf(stderr, L"title: %s\n", err.c_str());
        }
        Pace(n, start);
    }
    wprintf(L"sent %llu bytes\n", src.BytesSent());
    enc->Stop();
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        fwprintf(stderr, L"usage: see the comment at the top of streamout_selftest.cpp\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::wstring mode = argv[1];
    int rc = mode == L"rtmp" ? Rtmp(argc, argv) : Source(argc, argv);
    if (rc == 2) fwprintf(stderr, L"bad arguments\n");
    CoUninitialize();
    return rc;
}
