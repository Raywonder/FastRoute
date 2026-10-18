#pragma once
#include <stdint.h>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Encoders for the byte-stream servers (Icecast and Shoutcast)
//
// Each takes the session's float audio at the session's rate and hands back
// bytes ready to write down a source connection: ADTS framed AAC, MP3 frames,
// or Ogg pages of Opus or Vorbis. RTMP does not use these; it wants AAC frames
// one at a time with their timestamps, and drives AacEncoder directly.
//
// AAC and MP3 come from Windows (Media Foundation), so COM has to be
// initialised on the thread that starts and uses them. Opus and Vorbis are
// the same encoders the Recorder writes Ogg files with.
// ---------------------------------------------------------------------------

// Stored in session files, so the numbers are fixed once written.
enum StreamFormat
{
    SF_AAC = 0,
    SF_MP3 = 1,
    SF_OPUS = 2,
    SF_VORBIS = 3,
};
static const int kStreamFormatCount = 4;

class StreamEncoder
{
public:
    virtual ~StreamEncoder() {}
    // `rate` is what Encode will be given. The title, UTF-8, is only used by
    // the Ogg formats, which carry it in their own tags.
    virtual bool Start(int rate, int kbps, const std::string& title, std::wstring* err) = 0;
    virtual bool Encode(const float* buf, size_t frames, std::wstring* err) = 0;
    // Moves whatever is ready into `out`, appending.
    virtual void Take(std::vector<uint8_t>* out) = 0;
    virtual void Stop() = 0;
    virtual const char* ContentType() const = 0;
    // The bitrate actually in use, after the encoder rounded it.
    virtual int Kbps() const = 0;
};

std::unique_ptr<StreamEncoder> MakeStreamEncoder(int format);

// Seven bytes of ADTS header for one raw AAC-LC frame of `len` bytes.
void AdtsHeader(uint8_t out[7], size_t len, int sampleRate, int channels);
