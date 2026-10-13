#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "rtmp.h"

#include <string.h>

// ---------------------------------------------------------------------------
// RTMP publishing, from the client end
//
// The order of events is the one every encoder uses, which is also the order
// every server expects: handshake, raise the chunk size, connect to the
// application, create a stream, publish to it by key, then metadata and media.
// Each command is answered, and the answer is waited for, so that a refusal -
// a key the server does not want, an application that does not exist - comes
// back as a sentence rather than as a connection that silently goes nowhere.
//
// Outgoing messages always use a full header. That costs eleven bytes a frame
// that a cleverer writer would save, and buys a writer that cannot get its
// header compression wrong: at 192 kbps a frame is around five hundred bytes,
// so the saving would be two percent.
// ---------------------------------------------------------------------------

namespace rtmp {

namespace {

const uintptr_t kNoSocket = ~(uintptr_t)0;
const uint32_t kOurChunk = 4096;

// Chunk streams. Any numbers from 3 up would do; these are what Flash used,
// and some servers have been known to care.
const int kCsControl = 2;
const int kCsCommand = 3;
const int kCsAudio   = 4;
const int kCsData    = 5;

enum MsgType : uint8_t
{
    MT_SET_CHUNK = 1,
    MT_ABORT = 2,
    MT_ACK = 3,
    MT_USER_CONTROL = 4,
    MT_WINDOW_ACK = 5,
    MT_PEER_BW = 6,
    MT_AUDIO = 8,
    MT_DATA_AMF0 = 18,
    MT_CMD_AMF3 = 17,
    MT_CMD_AMF0 = 20,
};

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0,
                                nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n,
                                   nullptr, nullptr);
    return s;
}

void Put16(std::vector<uint8_t>& o, uint32_t v)
{
    o.push_back((uint8_t)(v >> 8)); o.push_back((uint8_t)v);
}
void Put24(std::vector<uint8_t>& o, uint32_t v)
{
    o.push_back((uint8_t)(v >> 16)); o.push_back((uint8_t)(v >> 8));
    o.push_back((uint8_t)v);
}
void Put32(std::vector<uint8_t>& o, uint32_t v)
{
    o.push_back((uint8_t)(v >> 24)); o.push_back((uint8_t)(v >> 16));
    o.push_back((uint8_t)(v >> 8)); o.push_back((uint8_t)v);
}
uint32_t Get24(const uint8_t* p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }
uint32_t Get32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

void AmfKey(std::vector<uint8_t>& o, const std::string& k)
{
    Put16(o, (uint32_t)k.size());
    o.insert(o.end(), k.begin(), k.end());
}
void AmfObjectEnd(std::vector<uint8_t>& o) { o.push_back(0); o.push_back(0); o.push_back(9); }

std::wstring SockErr(const wchar_t* what)
{
    int e = WSAGetLastError();
    if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK)
        return std::wstring(what) + L": the server stopped answering.";
    if (e == WSAECONNRESET || e == WSAECONNABORTED)
        return std::wstring(what) + L": the server closed the connection.";
    wchar_t num[32];
    wsprintfW(num, L" (network error %d).", e);
    return std::wstring(what) + num;
}

bool WinsockUp()
{
    static bool ok = [] {
        WSADATA wd;
        return WSAStartup(MAKEWORD(2, 2), &wd) == 0;
    }();
    return ok;
}

}   // namespace

// ---------------------------------------------------------------------------
// AMF0
// ---------------------------------------------------------------------------
void AmfString(std::vector<uint8_t>& o, const std::string& s)
{
    o.push_back(0x02);
    AmfKey(o, s);
}
void AmfNumber(std::vector<uint8_t>& o, double v)
{
    o.push_back(0x00);
    uint64_t bits;
    memcpy(&bits, &v, 8);
    for (int i = 7; i >= 0; --i) o.push_back((uint8_t)(bits >> (i * 8)));
}
void AmfBool(std::vector<uint8_t>& o, bool v) { o.push_back(0x01); o.push_back(v ? 1 : 0); }
void AmfNull(std::vector<uint8_t>& o) { o.push_back(0x05); }

// A decoded value, as much of one as the replies here need: commands are a
// name, a number and up to two objects, and what matters in the objects is a
// handful of strings such as "code" and "description".
struct Publisher::Amf
{
    int type = -1;
    double num = 0;
    bool b = false;
    std::string str;
    std::vector<std::pair<std::string, std::string>> props;

    std::string Prop(const char* k) const
    {
        for (const auto& kv : props) if (kv.first == k) return kv.second;
        return std::string();
    }
};

static bool AmfRead(const uint8_t* p, size_t n, size_t& at, Publisher::Amf* out, int depth);

static bool AmfReadProps(const uint8_t* p, size_t n, size_t& at, Publisher::Amf* out,
                         int depth)
{
    for (;;)
    {
        if (at + 3 > n) return false;
        uint32_t kl = ((uint32_t)p[at] << 8) | p[at + 1];
        if (kl == 0 && p[at + 2] == 0x09) { at += 3; return true; }
        at += 2;
        if (at + kl > n) return false;
        std::string key((const char*)p + at, kl);
        at += kl;
        Publisher::Amf v;
        if (!AmfRead(p, n, at, &v, depth + 1)) return false;
        if (!out) continue;
        std::string text;
        if (v.type == 0x02 || v.type == 0x0C) text = v.str;
        else if (v.type == 0x00)
        {
            char buf[64];
            sprintf_s(buf, "%.17g", v.num);
            text = buf;
        }
        else if (v.type == 0x01) text = v.b ? "true" : "false";
        out->props.emplace_back(key, text);
    }
}

static bool AmfRead(const uint8_t* p, size_t n, size_t& at, Publisher::Amf* out, int depth)
{
    if (depth > 16 || at >= n) return false;
    Publisher::Amf v;
    v.type = p[at++];
    switch (v.type)
    {
    case 0x00:
    {
        if (at + 8 > n) return false;
        uint64_t bits = 0;
        for (int i = 0; i < 8; ++i) bits = (bits << 8) | p[at + i];
        memcpy(&v.num, &bits, 8);
        at += 8;
        break;
    }
    case 0x01:
        if (at + 1 > n) return false;
        v.b = p[at++] != 0;
        break;
    case 0x02:
    {
        if (at + 2 > n) return false;
        uint32_t l = ((uint32_t)p[at] << 8) | p[at + 1];
        at += 2;
        if (at + l > n) return false;
        v.str.assign((const char*)p + at, l);
        at += l;
        break;
    }
    case 0x0C:
    {
        if (at + 4 > n) return false;
        uint32_t l = Get32(p + at);
        at += 4;
        if (at + l > n) return false;
        v.str.assign((const char*)p + at, l);
        at += l;
        break;
    }
    case 0x03:
        if (!AmfReadProps(p, n, at, &v, depth)) return false;
        break;
    case 0x08:
        if (at + 4 > n) return false;
        at += 4;   // the count is advisory; the end marker is what ends it
        if (!AmfReadProps(p, n, at, &v, depth)) return false;
        break;
    case 0x0A:
    {
        if (at + 4 > n) return false;
        uint32_t c = Get32(p + at);
        at += 4;
        for (uint32_t i = 0; i < c; ++i)
            if (!AmfRead(p, n, at, nullptr, depth + 1)) return false;
        break;
    }
    case 0x0B:
        if (at + 10 > n) return false;
        at += 10;
        break;
    case 0x05:
    case 0x06:
        break;
    default:
        return false;   // AMF3 switch, references and the rest: not used here
    }
    if (out) *out = v;
    return true;
}

std::vector<uint8_t> AacLcConfig(int sampleRate, int channels)
{
    static const int rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                 22050, 16000, 12000, 11025, 8000, 7350 };
    int idx = 3;
    for (int i = 0; i < 13; ++i) if (rates[i] == sampleRate) { idx = i; break; }
    const int objectType = 2;   // AAC-LC
    std::vector<uint8_t> asc(2);
    asc[0] = (uint8_t)((objectType << 3) | (idx >> 1));
    asc[1] = (uint8_t)(((idx & 1) << 7) | ((channels & 0x0F) << 3));
    return asc;
}

// ---------------------------------------------------------------------------
// Target
// ---------------------------------------------------------------------------
bool ParseTarget(const std::wstring& urlIn, const std::wstring& keyIn, Target* t,
                 std::wstring* err)
{
    std::string url = Narrow(urlIn);
    std::string key = Narrow(keyIn);
    while (!url.empty() && (url.back() == ' ' || url.back() == '/')) url.pop_back();
    while (!url.empty() && url.front() == ' ') url.erase(url.begin());
    while (!key.empty() && key.back() == ' ') key.pop_back();
    while (!key.empty() && key.front() == ' ') key.erase(key.begin());

    if (_strnicmp(url.c_str(), "rtmps://", 8) == 0)
    {
        if (err) *err = L"Secure RTMP (rtmps://) is not supported yet. Use an "
                        L"rtmp:// address.";
        return false;
    }
    if (_strnicmp(url.c_str(), "rtmp://", 7) != 0)
    {
        if (err) *err = L"The server address has to start with rtmp://, for example "
                        L"rtmp://aaastreamer.devinecreations.net:1935/live.";
        return false;
    }
    std::string rest = url.substr(7);
    size_t slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string path = slash == std::string::npos ? std::string() : rest.substr(slash + 1);

    Target out;
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos && hostport.find(']') == std::string::npos)
    {
        out.host = hostport.substr(0, colon);
        out.port = atoi(hostport.c_str() + colon + 1);
    }
    else
        out.host = hostport;
    if (out.host.size() > 2 && out.host.front() == '[' && out.host.back() == ']')
        out.host = out.host.substr(1, out.host.size() - 2);
    if (out.host.empty() || out.port <= 0 || out.port > 65535)
    {
        if (err) *err = L"The server address has no usable host name or port.";
        return false;
    }

    // With no key given, the last path segment is the key: that is how most
    // services print a "full" ingest address.
    if (key.empty())
    {
        size_t last = path.rfind('/');
        if (last == std::string::npos)
        {
            if (err) *err = L"There is no stream key. Type the key, or sign in and "
                            L"choose a stream.";
            return false;
        }
        key = path.substr(last + 1);
        path = path.substr(0, last);
    }
    if (path.empty())
    {
        if (err) *err = L"The server address has no application name after the "
                        L"host, such as /live.";
        return false;
    }
    if (key.empty())
    {
        if (err) *err = L"There is no stream key. Type the key, or sign in and "
                        L"choose a stream.";
        return false;
    }

    out.app = path;
    out.key = key;
    out.tcUrl = "rtmp://" + hostport + "/" + path;
    *t = out;
    return true;
}

// ---------------------------------------------------------------------------
// Publisher
// ---------------------------------------------------------------------------
Publisher::Publisher() : m_sock(kNoSocket) {}
Publisher::~Publisher() { Close(); }

void Publisher::Close()
{
    if (m_sock != kNoSocket)
    {
        shutdown((SOCKET)m_sock, SD_BOTH);
        closesocket((SOCKET)m_sock);
        m_sock = kNoSocket;
    }
    m_in.clear();
    m_inChunk = 128;
    m_outChunk = 128;
    m_ackWindow = 0;
    m_received = m_ackedAt = 0;
}

bool Publisher::WaitReadable(unsigned long long deadline)
{
    for (;;)
    {
        unsigned long long now = GetTickCount64();
        if (now >= deadline) return false;
        unsigned long long left = deadline - now;
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET((SOCKET)m_sock, &rs);
        timeval tv;
        tv.tv_sec = (long)(left / 1000);
        tv.tv_usec = (long)((left % 1000) * 1000);
        int r = select(0, &rs, nullptr, nullptr, &tv);
        if (r > 0) return true;
        if (r < 0) return false;
    }
}

bool Publisher::SendAll(const uint8_t* p, size_t n, std::wstring* err)
{
    while (n > 0)
    {
        int chunk = n > 65536 ? 65536 : (int)n;
        int s = send((SOCKET)m_sock, (const char*)p, chunk, 0);
        if (s <= 0)
        {
            if (err) *err = SockErr(L"Sending to the server failed");
            return false;
        }
        p += s;
        n -= (size_t)s;
        m_sent += (unsigned long long)s;
    }
    return true;
}

bool Publisher::RecvAll(uint8_t* p, size_t n, std::wstring* err)
{
    // Called with a deadline already set on the socket.
    while (n > 0)
    {
        int r = recv((SOCKET)m_sock, (char*)p, n > 65536 ? 65536 : (int)n, 0);
        if (r == 0)
        {
            if (err) *err = L"The server closed the connection.";
            return false;
        }
        if (r < 0)
        {
            if (err) *err = SockErr(L"Reading from the server failed");
            return false;
        }
        p += r;
        n -= (size_t)r;
        m_received += (unsigned long long)r;
    }
    return true;
}

bool Publisher::SendMessage(int csid, uint8_t type, uint32_t msid, uint32_t ts,
                            const std::vector<uint8_t>& payload, std::wstring* err)
{
    std::vector<uint8_t> o;
    o.reserve(payload.size() + 32 + payload.size() / m_outChunk * 5);
    const bool ext = ts >= 0xFFFFFF;

    o.push_back((uint8_t)(csid & 0x3F));                 // fmt 0
    Put24(o, ext ? 0xFFFFFF : ts);
    Put24(o, (uint32_t)payload.size());
    o.push_back(type);
    o.push_back((uint8_t)msid); o.push_back((uint8_t)(msid >> 8));   // little endian
    o.push_back((uint8_t)(msid >> 16)); o.push_back((uint8_t)(msid >> 24));
    if (ext) Put32(o, ts);

    size_t at = 0;
    for (;;)
    {
        size_t take = payload.size() - at;
        if (take > m_outChunk) take = m_outChunk;
        o.insert(o.end(), payload.begin() + (ptrdiff_t)at,
                 payload.begin() + (ptrdiff_t)(at + take));
        at += take;
        if (at >= payload.size()) break;
        o.push_back((uint8_t)(0xC0 | (csid & 0x3F)));     // fmt 3 continuation
        if (ext) Put32(o, ts);
    }
    return SendAll(o.data(), o.size(), err);
}

bool Publisher::SendCommand(int csid, uint32_t msid, const std::vector<uint8_t>& amf,
                            std::wstring* err)
{
    return SendMessage(csid, MT_CMD_AMF0, msid, 0, amf, err);
}

bool Publisher::ReadMessage(uint8_t* type, uint32_t* msid, std::vector<uint8_t>* body,
                            unsigned long long deadline, std::wstring* err)
{
    for (;;)
    {
        if (!WaitReadable(deadline))
        {
            if (err) *err = L"The server stopped answering.";
            return false;
        }
        uint8_t b0;
        if (!RecvAll(&b0, 1, err)) return false;
        const int fmt = b0 >> 6;
        int csid = b0 & 0x3F;
        if (csid == 0)
        {
            uint8_t e;
            if (!RecvAll(&e, 1, err)) return false;
            csid = 64 + e;
        }
        else if (csid == 1)
        {
            uint8_t e[2];
            if (!RecvAll(e, 2, err)) return false;
            csid = 64 + e[0] + e[1] * 256;
        }

        InStream& s = m_in[csid];
        const size_t hdrLen = fmt == 0 ? 11 : fmt == 1 ? 7 : fmt == 2 ? 3 : 0;
        uint8_t h[11];
        if (hdrLen && !RecvAll(h, hdrLen, err)) return false;

        uint32_t tsField = 0;
        if (fmt <= 2) tsField = Get24(h);
        if (fmt <= 1)
        {
            s.len = Get24(h + 3);
            s.type = h[6];
        }
        if (fmt == 0)
            s.msid = (uint32_t)h[7] | ((uint32_t)h[8] << 8) | ((uint32_t)h[9] << 16) |
                     ((uint32_t)h[10] << 24);
        if (fmt <= 2) s.extTs = tsField == 0xFFFFFF;
        if (s.extTs)
        {
            uint8_t e[4];
            if (!RecvAll(e, 4, err)) return false;
            if (fmt <= 2) tsField = Get32(e);
        }
        if (fmt == 0) s.ts = tsField;
        else if (fmt <= 2) s.ts += tsField;

        if (s.len > 16 * 1024 * 1024)
        {
            if (err) *err = L"The server sent a message too large to be real.";
            return false;
        }
        size_t want = s.len - s.buf.size();
        if (want > m_inChunk) want = m_inChunk;
        size_t at = s.buf.size();
        s.buf.resize(at + want);
        if (want && !RecvAll(s.buf.data() + at, want, err)) return false;

        if (m_ackWindow && m_received - m_ackedAt >= m_ackWindow)
        {
            std::vector<uint8_t> a;
            Put32(a, (uint32_t)m_received);
            m_ackedAt = m_received;
            if (!SendMessage(kCsControl, MT_ACK, 0, 0, a, err)) return false;
        }

        if (s.buf.size() >= s.len)
        {
            *type = s.type;
            *msid = s.msid;
            body->swap(s.buf);
            s.buf.clear();
            return true;
        }
    }
}

bool Publisher::HandleControl(uint8_t type, const std::vector<uint8_t>& body)
{
    switch (type)
    {
    case MT_SET_CHUNK:
        if (body.size() >= 4)
        {
            uint32_t c = Get32(body.data()) & 0x7FFFFFFF;
            if (c >= 1) m_inChunk = c;
        }
        return true;
    case MT_WINDOW_ACK:
        if (body.size() >= 4) m_ackWindow = Get32(body.data());
        return true;
    case MT_PEER_BW:
    case MT_ACK:
    case MT_ABORT:
        return true;
    case MT_USER_CONTROL:
        // A ping request is answered with the same timestamp, or the server
        // decides the client is gone.
        if (body.size() >= 6 && body[0] == 0 && body[1] == 6)
        {
            std::vector<uint8_t> pong;
            Put16(pong, 7);
            pong.insert(pong.end(), body.begin() + 2, body.begin() + 6);
            SendMessage(kCsControl, MT_USER_CONTROL, 0, 0, pong, nullptr);
        }
        return true;
    }
    return false;
}

bool Publisher::WaitResult(double txn, std::vector<Amf>* reply,
                           unsigned long long deadline, std::wstring* err)
{
    for (;;)
    {
        uint8_t type = 0;
        uint32_t msid = 0;
        std::vector<uint8_t> body;
        if (!ReadMessage(&type, &msid, &body, deadline, err)) return false;
        if (HandleControl(type, body)) continue;
        if (type != MT_CMD_AMF0 && type != MT_CMD_AMF3) continue;

        size_t at = (type == MT_CMD_AMF3 && !body.empty()) ? 1 : 0;
        std::vector<Amf> vals;
        while (at < body.size())
        {
            Amf v;
            if (!AmfRead(body.data(), body.size(), at, &v, 0)) break;
            vals.push_back(v);
        }
        if (vals.size() < 2 || vals[0].type != 0x02) continue;
        if (vals[1].type != 0x00 || vals[1].num != txn) continue;

        if (vals[0].str == "_error")
        {
            std::string why;
            for (const Amf& v : vals)
                if (!v.Prop("description").empty()) why = v.Prop("description");
            if (err) *err = L"The server refused the connection" +
                            (why.empty() ? std::wstring(L".") : L": " + Widen(why));
            return false;
        }
        if (reply) *reply = vals;
        return true;
    }
}

bool Publisher::WaitPublishStart(unsigned long long deadline, std::wstring* err)
{
    for (;;)
    {
        uint8_t type = 0;
        uint32_t msid = 0;
        std::vector<uint8_t> body;
        if (!ReadMessage(&type, &msid, &body, deadline, err)) return false;
        if (HandleControl(type, body)) continue;
        if (type != MT_CMD_AMF0 && type != MT_CMD_AMF3) continue;

        size_t at = (type == MT_CMD_AMF3 && !body.empty()) ? 1 : 0;
        std::vector<Amf> vals;
        while (at < body.size())
        {
            Amf v;
            if (!AmfRead(body.data(), body.size(), at, &v, 0)) break;
            vals.push_back(v);
        }
        if (vals.empty() || vals[0].type != 0x02) continue;
        if (vals[0].str != "onStatus" && vals[0].str != "_error") continue;

        std::string code, desc;
        for (const Amf& v : vals)
        {
            if (!v.Prop("code").empty()) code = v.Prop("code");
            if (!v.Prop("description").empty()) desc = v.Prop("description");
        }
        if (code == "NetStream.Publish.Start") return true;
        if (vals[0].str == "_error" || code.find("Publish") != std::string::npos ||
            code.find("Failed") != std::string::npos ||
            code.find("Rejected") != std::string::npos)
        {
            std::wstring why = Widen(desc.empty() ? code : desc);
            if (err) *err = L"The server would not take the stream" +
                            (why.empty() ? std::wstring(L".") : L": " + why);
            return false;
        }
    }
}

bool Publisher::Connect(const Target& t, int timeoutMs, std::wstring* err)
{
    Close();
    m_sent = 0;
    if (!WinsockUp())
    {
        if (err) *err = L"Windows networking could not be started.";
        return false;
    }
    const unsigned long long deadline = GetTickCount64() + (unsigned)timeoutMs;

    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    char port[16];
    sprintf_s(port, "%d", t.port);
    if (getaddrinfo(t.host.c_str(), port, &hints, &res) != 0 || !res)
    {
        if (err) *err = L"Could not find the server " + Widen(t.host) +
                        L". Check the address and the internet connection.";
        return false;
    }

    std::wstring lastErr;
    for (addrinfo* a = res; a; a = a->ai_next)
    {
        SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        // Non-blocking connect, so the deadline holds even when the address
        // is a black hole rather than a refusal.
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        int rc = connect(s, a->ai_addr, (int)a->ai_addrlen);
        bool ok = rc == 0;
        if (!ok && WSAGetLastError() == WSAEWOULDBLOCK)
        {
            unsigned long long now = GetTickCount64();
            unsigned long long left = now < deadline ? deadline - now : 0;
            fd_set ws, es;
            FD_ZERO(&ws); FD_SET(s, &ws);
            FD_ZERO(&es); FD_SET(s, &es);
            timeval tv;
            tv.tv_sec = (long)(left / 1000);
            tv.tv_usec = (long)((left % 1000) * 1000);
            if (select(0, nullptr, &ws, &es, &tv) > 0 && FD_ISSET(s, &ws)) ok = true;
        }
        if (!ok)
        {
            lastErr = L"Could not connect to " + Widen(t.host) + L" on port " +
                      Widen(port) + L".";
            closesocket(s);
            continue;
        }
        nb = 0;
        ioctlsocket(s, FIONBIO, &nb);
        BOOL one = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
        // A send that cannot get through for this long means the link is
        // gone, whatever TCP thinks; the block reconnects instead of waiting.
        DWORD sendMs = 10000;
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sendMs, sizeof(sendMs));
        // Reads only start once select says there is something to read, so
        // this only bounds a message the server stops sending halfway through.
        DWORD recvMs = 10000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recvMs, sizeof(recvMs));
        int sendBuf = 256 * 1024;
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&sendBuf, sizeof(sendBuf));
        m_sock = (uintptr_t)s;
        break;
    }
    freeaddrinfo(res);
    if (m_sock == kNoSocket)
    {
        if (err) *err = lastErr.empty() ? L"Could not connect to the server." : lastErr;
        return false;
    }

    // ---- Handshake: C0 C1, then S0 S1, then C2, then S2 --------------------
    {
        std::vector<uint8_t> c(1 + 1536, 0);
        c[0] = 3;
        uint32_t seed = GetTickCount() ^ 0x5DEECE66u;
        for (size_t i = 9; i < c.size(); ++i)
        {
            seed = seed * 1103515245u + 12345u;
            c[i] = (uint8_t)(seed >> 16);
        }
        if (!SendAll(c.data(), c.size(), err)) { Close(); return false; }

        std::vector<uint8_t> s01(1 + 1536);
        if (!WaitReadable(deadline) || !RecvAll(s01.data(), s01.size(), err))
        {
            if (err && err->empty()) *err = L"The server did not answer the handshake.";
            Close();
            return false;
        }
        if (s01[0] != 3)
        {
            if (err) *err = L"That server does not speak RTMP.";
            Close();
            return false;
        }
        // C2 echoes S1.
        if (!SendAll(s01.data() + 1, 1536, err)) { Close(); return false; }
        std::vector<uint8_t> s2(1536);
        if (!WaitReadable(deadline) || !RecvAll(s2.data(), s2.size(), err))
        {
            if (err && err->empty()) *err = L"The server did not finish the handshake.";
            Close();
            return false;
        }
    }

    // ---- Bigger chunks: fewer headers, and a frame goes in one piece --------
    {
        std::vector<uint8_t> p;
        Put32(p, kOurChunk);
        if (!SendMessage(kCsControl, MT_SET_CHUNK, 0, 0, p, err)) { Close(); return false; }
        m_outChunk = kOurChunk;
    }

    // ---- connect -----------------------------------------------------------
    {
        std::vector<uint8_t> a;
        AmfString(a, "connect");
        AmfNumber(a, 1);
        a.push_back(0x03);
        AmfKey(a, "app");      AmfString(a, t.app);
        AmfKey(a, "type");     AmfString(a, "nonprivate");
        AmfKey(a, "flashVer"); AmfString(a, "FMLE/3.0 (compatible; FastRoute)");
        AmfKey(a, "tcUrl");    AmfString(a, t.tcUrl);
        AmfObjectEnd(a);
        if (!SendCommand(kCsCommand, 0, a, err)) { Close(); return false; }
        if (!WaitResult(1, nullptr, deadline, err)) { Close(); return false; }
    }

    // ---- releaseStream, FCPublish, createStream -----------------------------
    // The first two are what encoders send and what some services require;
    // nothing waits for their answers, which not every server gives.
    {
        std::vector<uint8_t> a;
        AmfString(a, "releaseStream"); AmfNumber(a, 2); AmfNull(a); AmfString(a, t.key);
        if (!SendCommand(kCsCommand, 0, a, err)) { Close(); return false; }
        a.clear();
        AmfString(a, "FCPublish"); AmfNumber(a, 3); AmfNull(a); AmfString(a, t.key);
        if (!SendCommand(kCsCommand, 0, a, err)) { Close(); return false; }
        a.clear();
        AmfString(a, "createStream"); AmfNumber(a, 4); AmfNull(a);
        if (!SendCommand(kCsCommand, 0, a, err)) { Close(); return false; }

        std::vector<Amf> reply;
        if (!WaitResult(4, &reply, deadline, err)) { Close(); return false; }
        m_streamId = 1;
        for (size_t i = 2; i < reply.size(); ++i)
            if (reply[i].type == 0x00) { m_streamId = (uint32_t)reply[i].num; break; }
    }

    // ---- publish -------------------------------------------------------------
    {
        std::vector<uint8_t> a;
        AmfString(a, "publish"); AmfNumber(a, 5); AmfNull(a);
        AmfString(a, t.key); AmfString(a, "live");
        if (!SendCommand(kCsCommand, m_streamId, a, err)) { Close(); return false; }
        if (!WaitPublishStart(deadline, err)) { Close(); return false; }
    }
    return true;
}

bool Publisher::SendAudioMetadata(int sampleRate, int channels, int kbps)
{
    std::vector<uint8_t> a;
    AmfString(a, "@setDataFrame");
    AmfString(a, "onMetaData");
    a.push_back(0x08);
    Put32(a, 7);
    AmfKey(a, "duration");        AmfNumber(a, 0);
    AmfKey(a, "audiodatarate");   AmfNumber(a, kbps);
    AmfKey(a, "audiosamplerate"); AmfNumber(a, sampleRate);
    AmfKey(a, "audiosamplesize"); AmfNumber(a, 16);
    AmfKey(a, "stereo");          AmfBool(a, channels == 2);
    AmfKey(a, "audiocodecid");    AmfNumber(a, 10);   // AAC
    AmfKey(a, "encoder");         AmfString(a, "FastRoute");
    AmfObjectEnd(a);
    return SendMessage(kCsData, MT_DATA_AMF0, m_streamId, 0, a, nullptr);
}

bool Publisher::SendAacSequenceHeader(const uint8_t* asc, size_t n)
{
    std::vector<uint8_t> p;
    p.push_back(0xAF);   // AAC, 44 kHz flag (always, for AAC), 16 bit, stereo
    p.push_back(0x00);   // sequence header
    p.insert(p.end(), asc, asc + n);
    return SendMessage(kCsAudio, MT_AUDIO, m_streamId, 0, p, nullptr);
}

bool Publisher::SendAacFrame(const uint8_t* data, size_t n, uint32_t tsMs)
{
    std::vector<uint8_t> p;
    p.reserve(n + 2);
    p.push_back(0xAF);
    p.push_back(0x01);   // raw frame
    p.insert(p.end(), data, data + n);
    return SendMessage(kCsAudio, MT_AUDIO, m_streamId, tsMs, p, nullptr);
}

bool Publisher::Poll(std::wstring* err)
{
    if (m_sock == kNoSocket)
    {
        if (err) *err = L"Not connected.";
        return false;
    }
    for (int guard = 0; guard < 64; ++guard)
    {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET((SOCKET)m_sock, &rs);
        timeval tv = { 0, 0 };
        int r = select(0, &rs, nullptr, nullptr, &tv);
        if (r == 0) return true;
        if (r < 0)
        {
            if (err) *err = SockErr(L"The connection failed");
            return false;
        }
        uint8_t type = 0;
        uint32_t msid = 0;
        std::vector<uint8_t> body;
        if (!ReadMessage(&type, &msid, &body, GetTickCount64() + 5000, err)) return false;
        if (HandleControl(type, body)) continue;
        if (type == MT_CMD_AMF0)
        {
            size_t at = 0;
            Amf name, txn, obj, info;
            if (AmfRead(body.data(), body.size(), at, &name, 0) && name.str == "onStatus" &&
                AmfRead(body.data(), body.size(), at, &txn, 0) &&
                AmfRead(body.data(), body.size(), at, &obj, 0) &&
                AmfRead(body.data(), body.size(), at, &info, 0))
            {
                std::string code = info.Prop("code");
                if (code.find("Unpublish") != std::string::npos ||
                    code.find("Failed") != std::string::npos ||
                    code.find("BadName") != std::string::npos)
                {
                    std::string d = info.Prop("description");
                    if (err) *err = L"The server stopped the stream: " +
                                    Widen(d.empty() ? code : d);
                    return false;
                }
            }
        }
    }
    return true;
}

}   // namespace rtmp
