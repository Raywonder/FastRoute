#pragma once
#include <stdint.h>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// AAC-LC encoding with the encoder Windows ships
//
// The Recorder's AAC goes through a sink writer, which wants a file at the
// other end. A live stream wants the frames themselves, one at a time, so this
// drives the same Media Foundation encoder directly as a transform.
//
// Raw AAC out (no ADTS headers): RTMP carries the configuration once, in the
// sequence header, and each frame after that bare.
//
// The caller must have COM initialised on the calling thread, and must only
// use this once MediaFoundationAvailable() has said the DLLs are there.
// ---------------------------------------------------------------------------
class AacEncoder
{
public:
    AacEncoder();
    ~AacEncoder();
    AacEncoder(const AacEncoder&) = delete;
    AacEncoder& operator=(const AacEncoder&) = delete;

    // Stereo 16 bit PCM in at the given rate (44100 or 48000). The Windows
    // encoder only takes 96, 128, 160 or 192 kbps; anything else is rounded
    // to the nearest of those.
    bool Start(int sampleRate, int kbps, std::wstring* err) { return StartCodec(sampleRate, kbps, false, err); }
    // The same transform route for MP3, which Windows 8 and later ship an
    // encoder for. Its frames are 1152 samples and come out ready to send.
    bool StartMp3(int sampleRate, int kbps, std::wstring* err) { return StartCodec(sampleRate, kbps, true, err); }
    bool StartCodec(int sampleRate, int kbps, bool mp3, std::wstring* err);
    // Whether this machine has an MP3 encoder at all. Probed once.
    static bool Mp3Available();
    void Stop();

    // Interleaved stereo. Every complete AAC frame the encoder has ready is
    // appended to `frames`, each one 1024 samples of audio.
    bool Encode(const int16_t* pcm, size_t sampleFrames,
                std::vector<std::vector<uint8_t>>* frames, std::wstring* err);

    // The AudioSpecificConfig for the RTMP sequence header.
    const std::vector<uint8_t>& Config() const { return m_asc; }
    int Kbps() const { return m_kbps; }

    static int NearestKbps(int kbps);
    static int NearestMp3Kbps(int kbps);

private:
    bool Drain(std::vector<std::vector<uint8_t>>* frames, std::wstring* err);

    struct Impl;
    Impl* m;
    std::vector<uint8_t> m_asc;
    int m_rate = 48000;
    int m_kbps = 192;
    bool m_mp3 = false;
    long long m_inFrames = 0;
};
