#pragma once
#include <stdint.h>
#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// A minimal RTMP publisher
//
// Enough of RTMP to push one audio stream to a server such as MediaMTX, nginx
// rtmp or a streaming platform's ingest: the plain handshake, connect,
// createStream, publish, a metadata message and FLV audio tags. Nothing is
// played back and nothing but audio is sent, so the parts of RTMP that exist
// for those are left out.
//
// It owns one TCP connection and does no threading of its own. The Stream Out
// block drives it from its worker thread, and the self test drives it from
// main(), which is why it knows nothing about blocks or the engine.
// ---------------------------------------------------------------------------
namespace rtmp {

// Where to publish. A URL such as rtmp://host:1935/live plus a stream key, or
// the whole thing with the key as its last path segment and no key given.
struct Target
{
    std::string host;
    int port = 1935;
    std::string app;      // "live"
    std::string tcUrl;    // rtmp://host:port/app, without the key
    std::string key;      // the stream name published to
};

// False, with a reason, for anything that is not rtmp:// or has no key. The
// reason never contains the key.
bool ParseTarget(const std::wstring& url, const std::wstring& key, Target* t,
                 std::wstring* err);

class Publisher
{
public:
    Publisher();
    ~Publisher();
    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;

    // Opens the connection and goes all the way to publishing. Blocks for at
    // most about timeoutMs. On failure the connection is closed again and the
    // reason is in err, worded for a person.
    bool Connect(const Target& t, int timeoutMs, std::wstring* err);
    void Close();
    bool Connected() const { return m_sock != ~(uintptr_t)0; }

    // What a player needs to know before the first frame: an onMetaData that
    // says this is AAC audio only, so the server does not wait for video.
    bool SendAudioMetadata(int sampleRate, int channels, int kbps);
    // The AAC AudioSpecificConfig, sent once before any frame.
    bool SendAacSequenceHeader(const uint8_t* asc, size_t n);
    // One raw AAC frame, timestamped in milliseconds from the stream start.
    bool SendAacFrame(const uint8_t* data, size_t n, uint32_t tsMs);

    // Reads whatever the server has sent without waiting, answering pings.
    // False once the connection has gone, with the reason in err.
    bool Poll(std::wstring* err);

    // Bytes of audio and protocol sent since Connect, for the status line.
    unsigned long long BytesSent() const { return m_sent; }

    struct Amf;   // a decoded AMF0 value, used by the reply parser

private:
    struct InStream
    {
        uint32_t ts = 0, len = 0, msid = 0;
        uint8_t type = 0;
        bool extTs = false;
        std::vector<uint8_t> buf;   // the message being assembled
    };

    bool SendAll(const uint8_t* p, size_t n, std::wstring* err);
    bool RecvAll(uint8_t* p, size_t n, std::wstring* err);
    bool SendMessage(int csid, uint8_t type, uint32_t msid, uint32_t ts,
                     const std::vector<uint8_t>& payload, std::wstring* err);
    bool SendCommand(int csid, uint32_t msid, const std::vector<uint8_t>& amf,
                     std::wstring* err);
    // Reads one complete message. False on a dead connection or the deadline.
    bool ReadMessage(uint8_t* type, uint32_t* msid, std::vector<uint8_t>* body,
                     unsigned long long deadline, std::wstring* err);
    // Handles protocol control and pings; true if the message was one of them.
    bool HandleControl(uint8_t type, const std::vector<uint8_t>& body);
    // Waits for the reply to a numbered command, handling everything else.
    bool WaitResult(double txn, std::vector<Amf>* reply, unsigned long long deadline,
                    std::wstring* err);
    bool WaitPublishStart(unsigned long long deadline, std::wstring* err);
    bool WaitReadable(unsigned long long deadline);

    uintptr_t m_sock;
    uint32_t m_inChunk = 128;
    uint32_t m_outChunk = 128;
    uint32_t m_streamId = 1;
    std::map<int, InStream> m_in;
    unsigned long long m_sent = 0;
    unsigned long long m_received = 0, m_ackedAt = 0;
    uint32_t m_ackWindow = 0;
};

// ---------------------------------------------------------------------------
// AMF0, exposed for the self test
// ---------------------------------------------------------------------------
void AmfString(std::vector<uint8_t>& o, const std::string& s);
void AmfNumber(std::vector<uint8_t>& o, double v);
void AmfBool(std::vector<uint8_t>& o, bool v);
void AmfNull(std::vector<uint8_t>& o);

// The two bytes of AudioSpecificConfig for AAC-LC at a rate and channel count.
std::vector<uint8_t> AacLcConfig(int sampleRate, int channels);

}   // namespace rtmp
