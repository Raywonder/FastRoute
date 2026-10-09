#include "net.h"
#include "util.h"

// winsock2.h has to come before anything that pulls in the old winsock.h.
// WIN32_LEAN_AND_MEAN is set for the whole project, which keeps windows.h from
// doing that, so this include is safe wherever it lands.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

namespace net {

// ---------------------------------------------------------------------------
// Winsock
//
// Counted rather than started once at boot, because a FastRoute with no net
// blocks has no business having a Winsock instance at all.
// ---------------------------------------------------------------------------
namespace {

std::mutex g_wsLock;
int g_wsCount = 0;

std::wstring SockError(const wchar_t* what)
{
    const int e = WSAGetLastError();
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, (DWORD)e,
                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   (LPWSTR)&msg, 0, nullptr);
    std::wstring text = msg ? msg : L"";
    if (msg) LocalFree(msg);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' ||
                             text.back() == L' '))
        text.pop_back();
    if (text.empty()) return FormatW(L"%s failed (error %d).", what, e);
    return FormatW(L"%s failed: %s (error %d)", what, text.c_str(), e);
}

}  // namespace

bool Startup(std::wstring* err)
{
    std::lock_guard<std::mutex> lock(g_wsLock);
    if (g_wsCount > 0) { ++g_wsCount; return true; }

    WSADATA data = {};
    const int r = WSAStartup(MAKEWORD(2, 2), &data);
    if (r != 0)
    {
        if (err) *err = FormatW(L"Windows networking could not be started "
                                L"(WSAStartup returned %d).", r);
        return false;
    }
    g_wsCount = 1;
    return true;
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g_wsLock);
    if (g_wsCount <= 0) return;
    if (--g_wsCount == 0) WSACleanup();
}

// ---------------------------------------------------------------------------
// Addresses
// ---------------------------------------------------------------------------

std::wstring AddrText(uint32_t v4)
{
    return FormatW(L"%u.%u.%u.%u", (v4 >> 24) & 0xFF, (v4 >> 16) & 0xFF,
                   (v4 >> 8) & 0xFF, v4 & 0xFF);
}

std::wstring AddrText(uint32_t v4, uint16_t port)
{
    return FormatW(L"%s:%u", AddrText(v4).c_str(), (unsigned)port);
}

bool IsMulticast(uint32_t v4)
{
    return (v4 & 0xF0000000u) == 0xE0000000u;
}

bool ParseHost(const std::wstring& text, uint16_t defPort,
               uint32_t* v4, uint16_t* port, std::wstring* err)
{
    if (v4) *v4 = 0;
    if (port) *port = defPort;

    std::wstring host = text;
    while (!host.empty() && host.front() == L' ') host.erase(host.begin());
    while (!host.empty() && host.back() == L' ') host.pop_back();
    if (host.empty())
    {
        if (err) *err = L"No address.";
        return false;
    }

    // One colon at most: this is IPv4, so a colon can only be the port.
    const size_t colon = host.find(L':');
    if (colon != std::wstring::npos)
    {
        const std::wstring tail = host.substr(colon + 1);
        host.erase(colon);
        const int n = _wtoi(tail.c_str());
        if (n <= 0 || n > 65535)
        {
            if (err) *err = FormatW(L"\"%s\" is not a port number between 1 and "
                                    L"65535.", tail.c_str());
            return false;
        }
        if (port) *port = (uint16_t)n;
    }

    if (!Startup(err)) return false;

    // Numeric first, so that the common case needs no resolver at all.
    IN_ADDR num = {};
    if (InetPtonW(AF_INET, host.c_str(), &num) == 1)
    {
        if (v4) *v4 = ntohl(num.S_un.S_addr);
        Shutdown();
        return true;
    }

    ADDRINFOW hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    PADDRINFOW list = nullptr;
    const int r = GetAddrInfoW(host.c_str(), nullptr, &hints, &list);
    if (r != 0 || !list)
    {
        if (list) FreeAddrInfoW(list);
        Shutdown();
        if (err) *err = FormatW(L"\"%s\" could not be found on the network.",
                                host.c_str());
        return false;
    }
    const sockaddr_in* sa = reinterpret_cast<const sockaddr_in*>(list->ai_addr);
    if (v4) *v4 = ntohl(sa->sin_addr.S_un.S_addr);
    FreeAddrInfoW(list);
    Shutdown();
    return true;
}

std::vector<uint32_t> LocalAddresses()
{
    std::vector<uint32_t> out;
    std::wstring ignored;
    if (!Startup(&ignored)) return out;

    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf(size);
    ULONG r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST |
                                   GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER |
                                   GAA_FLAG_SKIP_FRIENDLY_NAME,
                                   nullptr,
                                   reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()),
                                   &size);
    if (r == ERROR_BUFFER_OVERFLOW)
    {
        buf.resize(size);
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST |
                                 GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER |
                                 GAA_FLAG_SKIP_FRIENDLY_NAME,
                                 nullptr,
                                 reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()),
                                 &size);
    }
    if (r == NO_ERROR)
    {
        for (IP_ADAPTER_ADDRESSES* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
             a; a = a->Next)
        {
            if (a->OperStatus != IfOperStatusUp) continue;
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next)
            {
                if (!u->Address.lpSockaddr) continue;
                if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
                const sockaddr_in* sa =
                    reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
                const uint32_t v = ntohl(sa->sin_addr.S_un.S_addr);
                if (v == 0) continue;
                out.push_back(v);
            }
        }
    }
    Shutdown();
    return out;
}

std::wstring HostName()
{
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(name, &n)) return name;
    return L"this pc";
}

// ---------------------------------------------------------------------------
// Udp
// ---------------------------------------------------------------------------

Udp::~Udp()
{
    Close();
}

bool Udp::Open(std::wstring* err)
{
    Close();
    if (!Startup(err)) return false;
    m_winsock = true;

    const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
    {
        if (err) *err = SockError(L"Opening a network socket");
        Shutdown();
        m_winsock = false;
        return false;
    }
    m_s = (uintptr_t)s;
    return true;
}

bool Udp::Bind(uint16_t port, bool shared, std::wstring* err)
{
    if (!Valid()) { if (err) *err = L"The socket is not open."; return false; }

    if (shared)
    {
        // Two sessions on this machine may both want to listen to the same
        // group, and so may another program. Without this the second one
        // fails to bind and the first one silently wins.
        BOOL on = TRUE;
        setsockopt((SOCKET)m_s, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&on), sizeof(on));
    }

    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.S_un.S_addr = INADDR_ANY;
    a.sin_port = htons(port);
    if (bind((SOCKET)m_s, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0)
    {
        if (err) *err = SockError(FormatW(L"Listening on port %u", (unsigned)port).c_str());
        return false;
    }
    return true;
}

bool Udp::JoinGroup(uint32_t group, std::wstring* err)
{
    if (!Valid()) { if (err) *err = L"The socket is not open."; return false; }
    if (!IsMulticast(group))
    {
        if (err) *err = FormatW(L"%s is not a multicast address.",
                                AddrText(group).c_str());
        return false;
    }

    // Every interface, not whichever one the routing table prefers. A machine
    // with Wi-Fi and Ethernet both up is the normal case, not the odd one.
    std::vector<uint32_t> ifs = LocalAddresses();
    ifs.push_back(0);                       // and whatever the stack chooses
    int joined = 0;
    std::wstring last;
    for (uint32_t iface : ifs)
    {
        ip_mreq r = {};
        r.imr_multiaddr.S_un.S_addr = htonl(group);
        r.imr_interface.S_un.S_addr = htonl(iface);
        if (setsockopt((SOCKET)m_s, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       reinterpret_cast<const char*>(&r), sizeof(r)) == 0)
        {
            ++joined;
            m_joined.push_back(iface);
        }
        else
        {
            last = SockError(FormatW(L"Joining %s", AddrText(group).c_str()).c_str());
        }
    }
    if (joined == 0)
    {
        if (err) *err = last.empty()
            ? FormatW(L"Could not join %s on any network interface.",
                      AddrText(group).c_str())
            : last;
        return false;
    }
    return true;
}

bool Udp::SetMulticastTtl(int ttl)
{
    if (!Valid()) return false;
    const DWORD v = (DWORD)(ttl < 0 ? 0 : (ttl > 255 ? 255 : ttl));
    // IP_MULTICAST_TTL only. IP_TTL governs a packet sent to a named host and
    // is a different question with a different answer: one hop is exactly
    // right for keeping a group on the local subnet and exactly wrong for
    // reaching a machine anywhere else, which the first router would drop.
    return setsockopt((SOCKET)m_s, IPPROTO_IP, IP_MULTICAST_TTL,
                      reinterpret_cast<const char*>(&v), sizeof(v)) == 0;
}

int Udp::MulticastTtl() const
{
    if (!Valid()) return -1;
    DWORD v = 0;
    int n = sizeof(v);
    if (getsockopt((SOCKET)m_s, IPPROTO_IP, IP_MULTICAST_TTL,
                   reinterpret_cast<char*>(&v), &n) != 0)
        return -1;
    return (int)v;
}

int Udp::UnicastTtl() const
{
    if (!Valid()) return -1;
    DWORD v = 0;
    int n = sizeof(v);
    if (getsockopt((SOCKET)m_s, IPPROTO_IP, IP_TTL,
                   reinterpret_cast<char*>(&v), &n) != 0)
        return -1;
    return (int)v;
}

bool Udp::SetMulticastLoopback(bool on)
{
    if (!Valid()) return false;
    const DWORD v = on ? 1 : 0;
    return setsockopt((SOCKET)m_s, IPPROTO_IP, IP_MULTICAST_LOOP,
                      reinterpret_cast<const char*>(&v), sizeof(v)) == 0;
}

bool Udp::SetBroadcast(bool on)
{
    if (!Valid()) return false;
    const BOOL v = on ? TRUE : FALSE;
    return setsockopt((SOCKET)m_s, SOL_SOCKET, SO_BROADCAST,
                      reinterpret_cast<const char*>(&v), sizeof(v)) == 0;
}

bool Udp::SetSendBufferBytes(int bytes)
{
    if (!Valid()) return false;
    return setsockopt((SOCKET)m_s, SOL_SOCKET, SO_SNDBUF,
                      reinterpret_cast<const char*>(&bytes), sizeof(bytes)) == 0;
}

bool Udp::SetRecvBufferBytes(int bytes)
{
    if (!Valid()) return false;
    return setsockopt((SOCKET)m_s, SOL_SOCKET, SO_RCVBUF,
                      reinterpret_cast<const char*>(&bytes), sizeof(bytes)) == 0;
}

uint16_t Udp::LocalPort() const
{
    if (!Valid()) return 0;
    sockaddr_in a = {};
    int n = sizeof(a);
    if (getsockname((SOCKET)m_s, reinterpret_cast<sockaddr*>(&a), &n) != 0) return 0;
    return ntohs(a.sin_port);
}

int Udp::Send(const void* data, int len, uint32_t v4, uint16_t port)
{
    if (!Valid()) return -1;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.S_un.S_addr = htonl(v4);
    a.sin_port = htons(port);
    return sendto((SOCKET)m_s, reinterpret_cast<const char*>(data), len, 0,
                  reinterpret_cast<const sockaddr*>(&a), sizeof(a));
}

int Udp::Recv(void* data, int len, int timeoutMs, uint32_t* fromV4, uint16_t* fromPort)
{
    if (!Valid()) return -1;

    if (timeoutMs >= 0)
    {
        fd_set r;
        FD_ZERO(&r);
        FD_SET((SOCKET)m_s, &r);
        timeval t;
        t.tv_sec = timeoutMs / 1000;
        t.tv_usec = (timeoutMs % 1000) * 1000;
        const int ready = select(0, &r, nullptr, nullptr, &t);
        if (ready == 0) return 0;
        if (ready < 0) return -1;
    }

    sockaddr_in a = {};
    int n = sizeof(a);
    const int got = recvfrom((SOCKET)m_s, reinterpret_cast<char*>(data), len, 0,
                             reinterpret_cast<sockaddr*>(&a), &n);
    if (got < 0)
    {
        // A datagram bigger than the buffer, or an ICMP port-unreachable from
        // an earlier send, both land here. Neither is fatal to the socket.
        const int e = WSAGetLastError();
        if (e == WSAEMSGSIZE || e == WSAECONNRESET) return 0;
        return -1;
    }
    if (fromV4) *fromV4 = ntohl(a.sin_addr.S_un.S_addr);
    if (fromPort) *fromPort = ntohs(a.sin_port);
    return got;
}

void Udp::Close()
{
    if (m_s != kInvalid)
    {
        closesocket((SOCKET)m_s);
        m_s = kInvalid;
    }
    m_joined.clear();
    if (m_winsock)
    {
        Shutdown();
        m_winsock = false;
    }
}

// ---------------------------------------------------------------------------
// Reading and writing the wire
//
// By hand, a byte at a time, rather than by casting a packed struct over the
// buffer. A wire format is a promise about bytes, and the compiler's idea of
// how to lay out a struct is not part of that promise.
// ---------------------------------------------------------------------------
namespace {

void Put8(unsigned char*& p, uint8_t v) { *p++ = v; }

void Put16(unsigned char*& p, uint16_t v)
{
    *p++ = (unsigned char)(v & 0xFF);
    *p++ = (unsigned char)((v >> 8) & 0xFF);
}

void Put32(unsigned char*& p, uint32_t v)
{
    for (int i = 0; i < 4; ++i) *p++ = (unsigned char)((v >> (8 * i)) & 0xFF);
}

void Put64(unsigned char*& p, uint64_t v)
{
    for (int i = 0; i < 8; ++i) *p++ = (unsigned char)((v >> (8 * i)) & 0xFF);
}

uint8_t Get8(const unsigned char*& p) { return *p++; }

uint16_t Get16(const unsigned char*& p)
{
    const uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
    p += 2;
    return v;
}

uint32_t Get32(const unsigned char*& p)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= (uint32_t)p[i] << (8 * i);
    p += 4;
    return v;
}

uint64_t Get64(const unsigned char*& p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    p += 8;
    return v;
}

// Names go on the wire as ASCII. Anything outside it becomes a question mark
// rather than being dropped, so a stream called something this cannot spell
// still has a name of the right length and is still told apart from its
// neighbour.
void PutText(unsigned char*& p, int n, const std::wstring& s, bool utf8)
{
    unsigned char* start = p;
    memset(p, 0, (size_t)n);
    if (utf8)
    {
        const int got = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1,
                                            reinterpret_cast<char*>(p), n,
                                            nullptr, nullptr);
        if (got <= 0) memset(p, 0, (size_t)n);
        start[n - 1] = 0;
    }
    else
    {
        int at = 0;
        for (wchar_t c : s)
        {
            if (at >= n - 1) break;
            start[at++] = (c >= 32 && c < 127) ? (unsigned char)c : (unsigned char)'?';
        }
    }
    p += n;
}

std::wstring GetText(const unsigned char*& p, int n, bool utf8)
{
    const char* s = reinterpret_cast<const char*>(p);
    int len = 0;
    while (len < n && s[len]) ++len;
    std::wstring out;
    if (len > 0)
    {
        if (utf8)
        {
            const int need = MultiByteToWideChar(CP_UTF8, 0, s, len, nullptr, 0);
            if (need > 0)
            {
                out.resize((size_t)need);
                MultiByteToWideChar(CP_UTF8, 0, s, len, &out[0], need);
            }
        }
        else
        {
            for (int i = 0; i < len; ++i) out.push_back((wchar_t)(unsigned char)s[i]);
        }
    }
    p += n;
    return out;
}

const unsigned char kBeaconMagic[4]    = { 'F', 'R', 'N', 'B' };
const unsigned char kDataMagic[4]      = { 'F', 'R', 'N', 'D' };
const unsigned char kSubscribeMagic[4] = { 'F', 'R', 'N', 'S' };
const uint8_t kVersion = 1;

}  // namespace

const wchar_t* NetCodecName(int c)
{
    switch (c)
    {
    case NC_PCM24:      return L"24 bit PCM, lossless";
    case NC_PCM_FLOAT:  return L"32 bit float PCM, lossless";
    case NC_FLAC:       return L"FLAC, lossless and compressed";
    case NC_OPUS:       return L"Opus, lossy and small";
    default:            return L"16 bit PCM, lossless";
    }
}

bool NetCodecIsLossless(int c)
{
    return c != NC_OPUS;
}

int NetCodecKbps(int c, int rate, int opusKbps)
{
    if (rate <= 0) rate = kDefaultRate;
    switch (c)
    {
    case NC_PCM16:     return rate * kChannels * 16 / 1000;
    case NC_PCM24:     return rate * kChannels * 24 / 1000;
    case NC_PCM_FLOAT: return rate * kChannels * 32 / 1000;
    // Music lands around 60% of the PCM it came from and speech a good deal
    // lower, so this is the honest upper half of the range rather than a
    // best case nobody would hit.
    case NC_FLAC:      return rate * kChannels * 16 * 7 / 10 / 1000;
    case NC_OPUS:      return opusKbps > 0 ? opusKbps : 96;
    default:           return rate * kChannels * 16 / 1000;
    }
}

void PackBeacon(const Beacon& b, unsigned char out[kBeaconBytes])
{
    memset(out, 0, kBeaconBytes);
    unsigned char* p = out;
    memcpy(p, kBeaconMagic, 4);
    p += 4;
    Put8(p, kVersion);
    Put8(p, b.codec);
    Put8(p, b.channels);
    Put8(p, 0);                       // reserved, must be zero
    Put32(p, b.streamId);
    Put32(p, b.group);
    Put16(p, b.port);
    Put16(p, b.packetFrames);
    Put32(p, b.rate);
    PutText(p, 32, b.name, false);
    PutText(p, 32, b.host, false);
    // 96 bytes: 4 + 4 + 4 + 4 + 2 + 2 + 4 + 32 + 32 = 88, and eight spare at
    // the end for whatever version two needs to say.
}

bool ParseBeacon(const unsigned char* p, int len, Beacon* out)
{
    if (!p || !out || len < kBeaconBytes) return false;
    if (memcmp(p, kBeaconMagic, 4) != 0) return false;
    const unsigned char* q = p + 4;
    if (Get8(q) != kVersion) return false;

    Beacon b;
    b.codec = Get8(q);
    b.channels = Get8(q);
    Get8(q);                          // reserved
    b.streamId = Get32(q);
    b.group = Get32(q);
    b.port = Get16(q);
    b.packetFrames = Get16(q);
    b.rate = Get32(q);
    b.name = GetText(q, 32, false);
    b.host = GetText(q, 32, false);

    if (b.codec >= kNetCodecCount) return false;
    if (b.channels == 0 || b.channels > 8) return false;
    if (b.rate < 4000 || b.rate > 768000) return false;
    *out = b;
    return true;
}


void PackSubscribe(const Subscribe& s, unsigned char out[kSubscribeBytes])
{
    memset(out, 0, kSubscribeBytes);
    unsigned char* p = out;
    memcpy(p, kSubscribeMagic, 4);
    p += 4;
    Put8(p, kVersion);
    Put8(p, (uint8_t)(s.wantBeacon ? 1 : 0));
    Put16(p, 0);                      // reserved
    Put32(p, s.wantStreamId);
    PutText(p, 32, s.name, false);
    // 4 + 4 + 4 + 32 = 44, and four spare at the end.
}

bool ParseSubscribe(const unsigned char* p, int len, Subscribe* out)
{
    if (!p || !out || len < kSubscribeBytes) return false;
    if (memcmp(p, kSubscribeMagic, 4) != 0) return false;
    const unsigned char* q = p + 4;
    if (Get8(q) != kVersion) return false;

    Subscribe s;
    s.wantBeacon = (Get8(q) != 0);
    Get16(q);                         // reserved
    s.wantStreamId = Get32(q);
    s.name = GetText(q, 32, false);
    *out = s;
    return true;
}

void PackDataHeader(const DataHeader& h, unsigned char out[kDataHeaderBytes])
{
    memset(out, 0, kDataHeaderBytes);
    unsigned char* p = out;
    memcpy(p, kDataMagic, 4);
    p += 4;
    Put8(p, kVersion);
    Put8(p, h.codec);
    Put8(p, h.channels);
    Put8(p, 0);                       // reserved
    Put32(p, h.rate);
    Put32(p, h.streamId);
    Put32(p, h.seq);
    Put64(p, h.frames);
    Put16(p, h.frameCount);
    Put16(p, h.payloadBytes);
    // 4 + 4 + 4 + 4 + 4 + 8 + 2 + 2 = 32.
}

bool ParseDataHeader(const unsigned char* p, int len, DataHeader* out)
{
    if (!p || !out || len < kDataHeaderBytes) return false;
    if (memcmp(p, kDataMagic, 4) != 0) return false;
    const unsigned char* q = p + 4;
    if (Get8(q) != kVersion) return false;

    DataHeader h;
    h.codec = Get8(q);
    h.channels = Get8(q);
    Get8(q);
    h.rate = Get32(q);
    h.streamId = Get32(q);
    h.seq = Get32(q);
    h.frames = Get64(q);
    h.frameCount = Get16(q);
    h.payloadBytes = Get16(q);

    if (h.codec >= kNetCodecCount) return false;
    if (h.channels == 0 || h.channels > 8) return false;
    if (h.payloadBytes > kMaxPayloadBytes) return false;
    if (len < kDataHeaderBytes + (int)h.payloadBytes) return false;
    *out = h;
    return true;
}

// ---------------------------------------------------------------------------
// VBAN
// ---------------------------------------------------------------------------
namespace vban {

namespace {
const int kRates[kRateCount] = {
    6000, 12000, 24000, 48000, 96000, 192000, 384000,
    8000, 16000, 32000, 64000, 128000, 256000, 512000,
    11025, 22050, 44100, 88200, 176400, 352800, 705600,
};
}  // namespace

int RateIndex(int rate)
{
    for (int i = 0; i < kRateCount; ++i) if (kRates[i] == rate) return i;
    return -1;
}

int RateAt(int index)
{
    return (index >= 0 && index < kRateCount) ? kRates[index] : 0;
}

void PackHeader(const Header& h, unsigned char out[kHeaderBytes])
{
    memset(out, 0, kHeaderBytes);
    unsigned char* p = out;
    *p++ = 'V';
    *p++ = 'B';
    *p++ = 'A';
    *p++ = 'N';
    // The sub protocol lives in the top three bits of the same byte as the
    // rate index, which is why a service packet's "rate" is not a rate.
    *p++ = (unsigned char)((h.rateIndex & kRateMask) | (h.protocol & kProtocolMask));
    // Both counts go on the wire one less than they are, so that a byte can
    // say 256.
    *p++ = (unsigned char)((h.frames   ? h.frames   - 1 : 0) & 0xFF);
    *p++ = (unsigned char)((h.channels ? h.channels - 1 : 0) & 0xFF);
    *p++ = (unsigned char)((h.datatype & kDatatypeMask) | (h.codec & kCodecMask));
    PutText(p, kNameBytes, h.name, false);
    Put32(p, h.counter);
}

bool ParseHeader(const unsigned char* p, int len, Header* out)
{
    if (!p || !out || len < kHeaderBytes) return false;
    if (p[0] != 'V' || p[1] != 'B' || p[2] != 'A' || p[3] != 'N') return false;

    const unsigned char* q = p + 4;
    Header h;
    const uint8_t sr = Get8(q);
    h.protocol = (uint8_t)(sr & kProtocolMask);
    h.rateIndex = (uint8_t)(sr & kRateMask);
    h.frames = (uint16_t)(Get8(q) + 1);
    h.channels = (uint16_t)(Get8(q) + 1);
    const uint8_t bit = Get8(q);
    h.datatype = (uint8_t)(bit & kDatatypeMask);
    h.codec = (uint8_t)(bit & kCodecMask);
    h.name = GetText(q, kNameBytes, false);
    h.counter = Get32(q);
    *out = h;
    return true;
}

void PackPing0(const Ping0& p0, unsigned char out[kPing0Bytes])
{
    memset(out, 0, kPing0Bytes);
    unsigned char* p = out;
    Put32(p, p0.bitType);
    Put32(p, p0.bitFeature);
    Put32(p, p0.bitFeatureEx);
    Put32(p, p0.preferredRate);
    Put32(p, p0.minRate);
    Put32(p, p0.maxRate);
    Put32(p, p0.colorRgb);
    for (int i = 0; i < 4; ++i) Put8(p, p0.version[i]);
    p += 8;                                // GPS_Position
    p += 8;                                // USER_Position
    PutText(p, 8, L"EN", false);           // LangCode_ascii
    p += 8;                                // reserved_ascii
    p += 64;                               // reservedEx
    p += 32;                               // DistantIP_ascii
    Put16(p, 0);                           // DistantPort
    Put16(p, 0);                           // DistantReserved
    PutText(p, 64, p0.deviceName, false);
    PutText(p, 64, p0.manufacturer, false);
    PutText(p, 64, p0.application, false);
    PutText(p, 64, p0.hostName, false);
    PutText(p, 128, p0.userName, true);
    PutText(p, 128, p0.userComment, true);
}

bool ParsePing0(const unsigned char* p, int len, Ping0* out)
{
    if (!p || !out || len < kPing0Bytes) return false;
    const unsigned char* q = p;
    Ping0 r;
    r.bitType = Get32(q);
    r.bitFeature = Get32(q);
    r.bitFeatureEx = Get32(q);
    r.preferredRate = Get32(q);
    r.minRate = Get32(q);
    r.maxRate = Get32(q);
    r.colorRgb = Get32(q);
    for (int i = 0; i < 4; ++i) r.version[i] = Get8(q);
    q += 8 + 8 + 8 + 8;                    // GPS, USER, LangCode, reserved
    q += 64;                               // reservedEx
    q += 32;                               // DistantIP
    q += 2 + 2;                            // DistantPort, DistantReserved
    r.deviceName   = GetText(q, 64, false);
    r.manufacturer = GetText(q, 64, false);
    r.application  = GetText(q, 64, false);
    r.hostName     = GetText(q, 64, false);
    r.userName     = GetText(q, 128, true);
    r.userComment  = GetText(q, 128, true);
    *out = r;
    return true;
}

Ping0 OurPing0()
{
    Ping0 p;
    // Both directions, and several streams of each: a session can hold as many
    // net blocks as it likes.
    p.bitType = PING_TYPE_RECEPTORSPOT | PING_TYPE_TRANSMITTERSPOT |
                PING_TYPE_MATRIX;
    p.bitFeature = PING_FEATURE_AUDIO;
    p.preferredRate = kDefaultRate;
    p.minRate = 11025;
    p.maxRate = 192000;
    p.version[0] = 0;
    p.version[1] = 5;
    p.deviceName = L"FastRoute";
    p.manufacturer = L"FastRoute";
    p.application = L"FastRoute";
    p.hostName = HostName();
    return p;
}

}  // namespace vban

// ---------------------------------------------------------------------------
// Peer
// ---------------------------------------------------------------------------

std::wstring Peer::Label() const
{
    std::wstring who = name.empty() ? L"unnamed stream" : name;
    if (!host.empty()) who += FormatW(L" on %s", host.c_str());

    std::wstring what;
    if (vbanHost)
    {
        // A VBAN host answers a ping with what it is, not with what it is
        // sending, so there is no format to report until audio turns up.
        what = L"VBAN host";
    }
    else
    {
        what = FormatW(L"%s, %d Hz %s", NetCodecName(codec), rate,
                       channels == 1 ? L"mono" : L"stereo");
    }
    return FormatW(L"%s - %s, %s", who.c_str(), what.c_str(),
                   AddrText(from).c_str());
}

int Peer::SecondsAgo() const
{
    const unsigned long long now = GetTickCount64();
    return (now > lastSeen) ? (int)((now - lastSeen) / 1000) : 0;
}

// ---------------------------------------------------------------------------
// Listener
//
// One per process in practice, though nothing here insists on it: each one
// holds its own socket on the beacon group. It listens for FastRoute beacons,
// and every few seconds sends a VBAN identification ping to the broadcast
// address so that VBAN hosts answer with their own details - VBAN is found by
// asking, where FastRoute is found by listening.
// ---------------------------------------------------------------------------

struct ListenerState
{
    mutable std::mutex lock;
    std::vector<Peer> peers;
    std::wstring err;

    std::atomic<bool> stop{ false };
    std::atomic<bool> poke{ false };
    std::thread thread;
};

namespace {

const int kForgetMs = 30000;        // unheard for this long and it is gone
const int kPingEveryMs = 5000;

void Remember(ListenerState* st, const Peer& p)
{
    std::lock_guard<std::mutex> lock(st->lock);
    for (Peer& e : st->peers)
    {
        // A FastRoute stream stays itself for as long as its id holds. A VBAN
        // host is identified by where it answered from, because it has no
        // stream id to give: what it announces is a machine, not a stream.
        const bool same = p.vbanHost ? (e.vbanHost && e.from == p.from)
                                     : (!e.vbanHost && e.streamId == p.streamId);
        if (!same) continue;
        const std::wstring kept = p.name.empty() ? e.name : p.name;
        e = p;
        e.name = kept;
        return;
    }
    st->peers.push_back(p);
}

void Forget(ListenerState* st)
{
    const unsigned long long now = GetTickCount64();
    std::lock_guard<std::mutex> lock(st->lock);
    st->peers.erase(std::remove_if(st->peers.begin(), st->peers.end(),
        [now](const Peer& p) {
            return now > p.lastSeen &&
                   (now - p.lastSeen) > (unsigned long long)kForgetMs;
        }), st->peers.end());
}

// Where an identification request goes. VBAN has no group of its own, so the
// question is put to the broadcast address of every interface this machine
// has, plus the all-subnets address for anything those missed. A host that
// ignores broadcasts does not answer, and can then only be reached by having
// its address typed in.
std::vector<uint32_t> PingTargets()
{
    std::vector<uint32_t> out;
    out.push_back(0xFFFFFFFFu);                 // 255.255.255.255
    for (uint32_t a : LocalAddresses())
    {
        // Without the adapter's prefix length this cannot be exact, so it
        // assumes the /24 that nearly every home and small office network
        // uses. Anything else is still reached by the address above.
        const uint32_t b = (a & 0xFFFFFF00u) | 0xFFu;
        if (std::find(out.begin(), out.end(), b) == out.end()) out.push_back(b);
    }
    return out;
}

void SendPings(Udp& sock, uint32_t& ident)
{
    unsigned char packet[vban::kHeaderBytes + vban::kPing0Bytes];

    vban::Header h;
    h.protocol = vban::PROTOCOL_SERVICE;
    h.rateIndex = 0;
    // For a service packet these two are not counts: VB-Audio reuses them as
    // the service type and the function within it. PackHeader writes a count
    // one less than it is given, so these go in one more than wanted.
    h.frames = vban::SERVICE_IDENTIFICATION + 1;
    h.channels = vban::FNCT_PING0 + 1;
    h.datatype = 0;
    h.codec = 0;
    h.name = L"FastRoute";
    h.counter = ident++;
    vban::PackHeader(h, packet);

    // A host only answers a request whose PING0 is present, even when every
    // field of it is zero, so ours goes with it saying what we are.
    const vban::Ping0 me = vban::OurPing0();
    vban::PackPing0(me, packet + vban::kHeaderBytes);

    for (uint32_t to : PingTargets())
        sock.Send(packet, (int)sizeof(packet), to, vban::kPort);
}

void TakeReply(ListenerState* st, const unsigned char* buf, int got, uint32_t from)
{
    vban::Header h;
    if (!vban::ParseHeader(buf, got, &h)) return;
    if (h.protocol != vban::PROTOCOL_SERVICE) return;

    // The same reuse of the count fields, read back. Both arrive one more than
    // the wire said, so the service type and the function are one less again.
    const int service = (int)h.frames - 1;
    const int fnct = (int)h.channels - 1;
    if (service != vban::SERVICE_IDENTIFICATION) return;
    if ((fnct & vban::FNCT_REPLY) == 0) return;      // our own request, echoed

    Peer p;
    p.vbanHost = true;
    p.from = from;
    p.port = vban::kPort;
    p.lastSeen = GetTickCount64();
    p.rate = 0;
    p.channels = 2;

    vban::Ping0 ping;
    if (got >= vban::kHeaderBytes + vban::kPing0Bytes &&
        vban::ParsePing0(buf + vban::kHeaderBytes, got - vban::kHeaderBytes, &ping))
    {
        p.name = !ping.deviceName.empty() ? ping.deviceName : ping.application;
        p.host = !ping.hostName.empty() ? ping.hostName : AddrText(from);
        if (ping.preferredRate >= 4000 && ping.preferredRate <= 768000)
            p.rate = (int)ping.preferredRate;
    }
    if (p.name.empty()) p.name = L"VBAN host";
    if (p.host.empty()) p.host = AddrText(from);
    Remember(st, p);
}

void ListenThread(ListenerState* st)
{
    std::wstring err;

    Udp beacon;
    if (!beacon.Open(&err) ||
        !beacon.Bind(kBeaconPort, true, &err) ||
        !beacon.JoinGroup(kBeaconGroup, &err))
    {
        std::lock_guard<std::mutex> lock(st->lock);
        st->err = err;
        return;
    }
    beacon.SetRecvBufferBytes(64 * 1024);

    // A second socket, on whatever port it is given, for the VBAN
    // conversation: a reply comes back to where the request came from, so
    // asking from here keeps it clear of any block that owns port 6980.
    Udp ping;
    const bool canPing = ping.Open(nullptr) && ping.Bind(0, false, nullptr) &&
                         ping.SetBroadcast(true);

    uint32_t ident = 1;
    unsigned long long lastPing = 0;
    unsigned char buf[2048];

    while (!st->stop.load())
    {
        uint32_t from = 0;
        uint16_t port = 0;
        const int got = beacon.Recv(buf, sizeof(buf), 100, &from, &port);
        if (got >= kBeaconBytes)
        {
            Beacon b;
            if (ParseBeacon(buf, got, &b))
            {
                Peer p;
                p.streamId = b.streamId;
                p.from = from;
                p.group = b.group;
                p.port = b.port;
                p.codec = b.codec;
                p.rate = (int)b.rate;
                p.channels = b.channels;
                p.packetFrames = b.packetFrames;
                p.name = b.name;
                p.host = b.host;
                p.lastSeen = GetTickCount64();
                Remember(st, p);
            }
        }

        if (canPing)
        {
            // Whatever has answered since the last pass. Nothing blocks here:
            // the waiting is done on the beacon socket above.
            for (int i = 0; i < 16; ++i)
            {
                uint32_t rf = 0;
                const int r = ping.Recv(buf, sizeof(buf), 0, &rf, nullptr);
                if (r <= 0) break;
                TakeReply(st, buf, r, rf);
            }

            const unsigned long long now = GetTickCount64();
            if (st->poke.exchange(false) ||
                now - lastPing > (unsigned long long)kPingEveryMs)
            {
                lastPing = now;
                SendPings(ping, ident);
            }
        }

        Forget(st);
    }
}

}  // namespace

Listener::Listener()
    : m_st(new ListenerState())
{
    ListenerState* st = m_st.get();
    st->thread = std::thread([st] { ListenThread(st); });
}

Listener::~Listener()
{
    if (!m_st) return;
    m_st->stop.store(true);
    if (m_st->thread.joinable()) m_st->thread.join();
}

std::vector<Peer> Listener::Peers() const
{
    std::vector<Peer> out;
    {
        std::lock_guard<std::mutex> lock(m_st->lock);
        out = m_st->peers;
    }
    // A stable order rather than the order they happened to arrive in: a list
    // that reshuffles itself under somebody reading it by arrow key is worse
    // than one that is not sorted at all. FastRoute senders first, because
    // those are the ones with a format to report.
    std::sort(out.begin(), out.end(), [](const Peer& a, const Peer& b) {
        if (a.vbanHost != b.vbanHost) return !a.vbanHost;
        if (a.host != b.host) return a.host < b.host;
        if (a.name != b.name) return a.name < b.name;
        return a.streamId < b.streamId;
    });
    return out;
}

bool Listener::Find(uint32_t streamId, const std::wstring& name, Peer* out) const
{
    std::lock_guard<std::mutex> lock(m_st->lock);
    // By id first. A sender that has been restarted comes back with a new one,
    // so the name is the fallback: it is what somebody chose, and it is what
    // they will expect to still work tomorrow.
    for (const Peer& p : m_st->peers)
        if (streamId && p.streamId == streamId) { if (out) *out = p; return true; }
    if (!name.empty())
        for (const Peer& p : m_st->peers)
            if (p.name == name) { if (out) *out = p; return true; }
    return false;
}

void Listener::Poke()
{
    m_st->poke.store(true);
}

std::wstring Listener::Error() const
{
    std::lock_guard<std::mutex> lock(m_st->lock);
    return m_st->err;
}

// ---------------------------------------------------------------------------
// What a block's stored answers come to
// ---------------------------------------------------------------------------

uint32_t StreamIdFor(const std::wstring& host, const std::wstring& name)
{
    // FNV-1a over the machine and the stream name. Any hash would do; what
    // matters is that it is the same hash tomorrow, so a receiver that saved
    // an id yesterday still recognises the sender it chose.
    uint32_t h = 2166136261u;
    auto feed = [&h](const std::wstring& s) {
        for (wchar_t c : s)
        {
            const wchar_t lower = (c >= L'A' && c <= L'Z') ? (wchar_t)(c + 32) : c;
            h ^= (uint32_t)(lower & 0xFF);
            h *= 16777619u;
            h ^= (uint32_t)((lower >> 8) & 0xFF);
            h *= 16777619u;
        }
    };
    feed(host);
    feed(L"/");
    feed(name);
    // Zero means "none" everywhere else, so it must not mean a stream here.
    return h ? h : 1u;
}

namespace {

int ClampTo(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

std::wstring Trimmed(const std::wstring& s, size_t max)
{
    std::wstring out = s;
    while (!out.empty() && out.front() == L' ') out.erase(out.begin());
    while (!out.empty() && out.back() == L' ') out.pop_back();
    if (out.size() > max) out.resize(max);
    return out;
}

}  // namespace

bool Resolve(const Block& b, bool sending, Settings* out, std::wstring* err)
{
    if (!out) return false;
    Settings s;

    s.protocol = ClampTo(b.netProtocol, 0, kNetProtocolCount - 1);
    const bool isVban = (s.protocol == NP_VBAN);

    // VBAN's stream name is 16 characters and is how a receiver tells one
    // stream from another, so it is not decoration there the way it is here.
    s.name = Trimmed(b.netStreamName,
                     isVban ? (size_t)vban::kNameBytes - 1 : (size_t)kNetNameMax);

    s.port = (uint16_t)(b.netPort > 0 && b.netPort <= 65535
                        ? b.netPort
                        : (isVban ? vban::kPort : kDataPort));

    s.bufferMs = ClampTo(b.netBufferMs, kNetBufferMsMin, kNetBufferMsMax);
    s.packetMs = ClampTo(b.netPacketMs, kNetPacketMsMin, kNetPacketMsMax);
    s.opus.kbps = ClampTo(b.netOpusKbps, kNetOpusKbpsMin, kNetOpusKbpsMax);
    s.opus.lossPct = ClampTo(b.netOpusLossPct, kNetOpusLossMin, kNetOpusLossMax);
    s.opus.complexity = ClampTo(b.netOpusComplexity, kNetOpusComplexityMin,
                                kNetOpusComplexityMax);
    s.opus.signal = ClampTo(b.netOpusSignal, 0, kNetOpusSignalCount - 1);
    s.opus.rateMode = ClampTo(b.netOpusRateMode, 0, kNetOpusRateModeCount - 1);
    s.opus.dtx = b.netOpusDtx;
    s.ttl = ClampTo(b.netTtl, kNetTtlMin, kNetTtlMax);

    // VBAN carries PCM and two of VB-Audio's own codecs, neither of which is
    // implemented here. Quietly sending 16 bit when Opus was asked for would
    // be worse than saying so, which the dialog and the summary both do.
    s.codec = isVban ? NC_PCM16 : ClampTo(b.netCodec, 0, kNetCodecCount - 1);

    const std::wstring addr = Trimmed(b.netAddress, 255);

    if (sending)
    {
        s.mode = ClampTo(b.netMode, 0, kNetModeCount - 1);

        if (s.mode == NM_UNICAST)
        {
            // An address here is optional, and usually left blank. A receiver
            // pointed at this machine asks for the audio and the sender sends
            // it to whoever asked, which is the way round anybody expects:
            // start the sender, then go to the other machine and name this
            // one. An address is for pushing at a receiver that cannot ask -
            // one that is not running yet, or behind something that will not
            // let it out.
            if (addr.empty())
            {
                s.addr = 0;
            }
            else
            {
                uint16_t port = s.port;
                if (!ParseHost(addr, s.port, &s.addr, &port, err)) return false;
                s.port = port;
            }
        }
        else
        {
            // A group was named, or the default one is used. VBAN has no group
            // of its own, so a VBAN block set to broadcast uses ours, which
            // works between copies of FastRoute but is not where another VBAN
            // program would look.
            if (addr.empty())
            {
                s.addr = kDataGroup;
            }
            else
            {
                uint16_t port = s.port;
                if (!ParseHost(addr, s.port, &s.addr, &port, err)) return false;
                s.port = port;
                if (!IsMulticast(s.addr))
                {
                    if (err) *err = FormatW(
                        L"%s is a single machine, not a group. Either switch "
                        L"this block to send to one host, or give it a group "
                        L"address between 224.0.0.0 and 239.255.255.255.",
                        AddrText(s.addr).c_str());
                    return false;
                }
            }
        }

        if (s.name.empty())
        {
            if (err) *err = L"This Net Send block has no stream name. A "
                            L"receiver finds it by that name, so it needs one.";
            return false;
        }
        s.streamId = StreamIdFor(HostName(), s.name);
    }
    else
    {
        // Receiving. An address here is the group to join; a host address is
        // not a thing to join, so it is taken as "accept from this one" and
        // the group stays the default.
        if (addr.empty())
        {
            s.addr = isVban ? 0u : kDataGroup;
        }
        else
        {
            uint16_t port = s.port;
            if (!ParseHost(addr, s.port, &s.addr, &port, err)) return false;
            s.port = port;
        }
        s.streamId = b.netSourceId;
    }

    *out = s;
    return true;
}

}  // namespace net
