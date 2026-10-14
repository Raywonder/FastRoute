#pragma once
#include <stdint.h>
#include <string>

// ---------------------------------------------------------------------------
// Icecast and Shoutcast sources
//
// The other half of Stream Out: where RTMP is a message protocol, these are a
// login followed by the encoded bytes, written down a socket for as long as
// the stream lasts.
//
//  * Icecast 2: an HTTP PUT to the mount with Basic authentication, which is
//    what Icecast 2.4 and later expect. A server that does not know PUT gets
//    the older SOURCE method instead, on a second connection.
//  * Shoutcast 1: the password alone on the port after the listening one, an
//    "OK2" back, then the icy- headers and the audio.
//  * Shoutcast 2: the same legacy login, which DNAS 2 still accepts, with the
//    stream id after the password ("password:#2", or "user:password:#2").
//
// Title updates go to the server's admin page on a connection of their own,
// because none of these protocols carries one inside the audio.
// ---------------------------------------------------------------------------
namespace icecast {

enum Kind { K_ICECAST = 1, K_SHOUTCAST1 = 2, K_SHOUTCAST2 = 3 };

struct Target
{
    int kind = K_ICECAST;
    std::string host;
    int port = 8000;
    std::string mount;          // "/live", Icecast only
    std::string user;           // "source" when empty, Icecast and Shoutcast 2
    std::string password;
    int sid = 1;                // Shoutcast 2
    std::string contentType;    // "audio/ogg", "audio/aac", "audio/mpeg"
    int kbps = 128;
    int sampleRate = 48000;
    // What listeners and directories see. UTF-8.
    std::string name, description, genre, url;
    bool isPublic = false;
};

// Fills in the host, port and (for Icecast) the mount from what was typed:
// "host:port", "http://host:port" or "http://host:port/mount". The rest of the
// target is left as it is. False, with a reason, for anything unusable.
bool ParseAddress(const std::wstring& address, const std::wstring& mount, int kind,
                  Target* t, std::wstring* err);

class Source
{
public:
    Source();
    ~Source();
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;

    bool Connect(const Target& t, int timeoutMs, std::wstring* err);
    bool Send(const uint8_t* p, size_t n, std::wstring* err);
    // Notices a server that has closed the connection, without waiting.
    bool Poll(std::wstring* err);
    void Close();
    unsigned long long BytesSent() const { return m_sent; }

private:
    bool Open(const Target& t, int port, unsigned long long deadline, std::wstring* err);
    bool ReadLine(std::string* line, unsigned long long deadline);
    uintptr_t m_sock;
    unsigned long long m_sent = 0;
};

// Sets the "now playing" title on the server, blocking for at most a few
// seconds. UTF-8. False, with a reason, if the server refused it.
bool UpdateTitle(const Target& t, const std::string& title, std::wstring* err);

std::string Base64(const std::string& s);

}   // namespace icecast
