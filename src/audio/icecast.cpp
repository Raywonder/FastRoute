#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "icecast.h"
#include "netdial.h"

#include <stdio.h>
#include <string.h>

namespace icecast {

namespace {

const uintptr_t kNoSocket = ~(uintptr_t)0;

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

std::string Trim(std::string s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n' ||
                          s.back() == '\t'))
        s.pop_back();
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
    return s.substr(a);
}

// Header values may not contain line breaks; one that did would let a station
// name add headers of its own.
std::string HeaderSafe(const std::string& s)
{
    std::string o;
    for (char c : s) o += (c == '\r' || c == '\n') ? ' ' : c;
    return o;
}

std::string UrlEncode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s)
    {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
            o += (char)c;
        else
        {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

bool WinsockUp()
{
    static bool ok = [] {
        WSADATA wd;
        return WSAStartup(MAKEWORD(2, 2), &wd) == 0;
    }();
    return ok;
}

uintptr_t Dial(const std::string& host, int port, unsigned long long deadline,
               std::wstring* err)
{
    if (!WinsockUp())
    {
        if (err) *err = L"Windows networking could not be started.";
        return kNoSocket;
    }
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    char ps[16];
    sprintf_s(ps, "%d", port);
    if (getaddrinfo(host.c_str(), ps, &hints, &res) != 0 || !res)
    {
        if (err) *err = L"Could not find the server " + Widen(host) +
                        L". Check the address and the internet connection.";
        return kNoSocket;
    }
    int addrsLeft = 0;
    for (addrinfo* a = res; a; a = a->ai_next) ++addrsLeft;

    uintptr_t out = kNoSocket;
    for (addrinfo* a = res; a && out == kNoSocket; a = a->ai_next, --addrsLeft)
    {
        SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        bool ok = connect(s, a->ai_addr, (int)a->ai_addrlen) == 0;
        if (!ok && WSAGetLastError() == WSAEWOULDBLOCK)
        {
            unsigned long long now = GetTickCount64();
            unsigned long long left = now < deadline ? deadline - now : 0;
            // A slice each while there are others to try, so one address that
            // swallows the connection cannot use up the whole budget and leave
            // the working address with no time at all. See netdial.h.
            if (addrsLeft > 1 && left > kAddressTryMs) left = kAddressTryMs;
            fd_set ws, es;
            FD_ZERO(&ws); FD_SET(s, &ws);
            FD_ZERO(&es); FD_SET(s, &es);
            timeval tv;
            tv.tv_sec = (long)(left / 1000);
            tv.tv_usec = (long)((left % 1000) * 1000);
            if (select(0, nullptr, &ws, &es, &tv) > 0 && FD_ISSET(s, &ws)) ok = true;
        }
        if (!ok) { AbandonConnect(s); continue; }
        nb = 0;
        ioctlsocket(s, FIONBIO, &nb);
        DWORD ms = 10000;
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&ms, sizeof(ms));
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
        BOOL one = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
        out = (uintptr_t)s;
    }
    freeaddrinfo(res);
    if (out == kNoSocket && err)
        *err = L"Could not connect to " + Widen(host) + L" on port " + Widen(ps) + L".";
    return out;
}

bool SendText(uintptr_t sock, const std::string& s)
{
    size_t at = 0;
    while (at < s.size())
    {
        int r = send((SOCKET)sock, s.data() + at, (int)(s.size() - at), 0);
        if (r <= 0) return false;
        at += (size_t)r;
    }
    return true;
}

bool WaitReadable(uintptr_t sock, unsigned long long deadline)
{
    unsigned long long now = GetTickCount64();
    if (now >= deadline) return false;
    unsigned long long left = deadline - now;
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET((SOCKET)sock, &rs);
    timeval tv;
    tv.tv_sec = (long)(left / 1000);
    tv.tv_usec = (long)((left % 1000) * 1000);
    return select(0, &rs, nullptr, nullptr, &tv) > 0;
}

bool ReadLineFrom(uintptr_t sock, std::string* line, unsigned long long deadline)
{
    line->clear();
    for (;;)
    {
        if (!WaitReadable(sock, deadline)) return !line->empty();
        char c;
        int r = recv((SOCKET)sock, &c, 1, 0);
        if (r <= 0) return !line->empty();
        if (c == '\n') return true;
        if (c != '\r') *line += c;
        if (line->size() > 8192) return true;
    }
}

int StatusCode(const std::string& line)
{
    // "HTTP/1.0 200 OK"
    size_t sp = line.find(' ');
    if (line.compare(0, 5, "HTTP/") != 0 || sp == std::string::npos) return 0;
    return atoi(line.c_str() + sp + 1);
}

std::wstring StatusWords(int code, const std::string& line)
{
    if (code == 401)
        return L"The server refused the user name or password.";
    if (code == 403)
        return L"The server refused the stream. Either something is already "
               L"streaming to that mount point, or the mount is not allowed.";
    if (code == 400 && line.find("Content-type") != std::string::npos)
        return L"The server does not accept this format.";
    return L"The server answered: " + Widen(line);
}

}   // namespace

std::string Base64(const std::string& s)
{
    static const char* t =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    size_t i = 0;
    for (; i + 2 < s.size(); i += 3)
    {
        unsigned v = ((unsigned char)s[i] << 16) | ((unsigned char)s[i + 1] << 8) |
                     (unsigned char)s[i + 2];
        o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += t[v & 63];
    }
    if (i + 1 == s.size())
    {
        unsigned v = (unsigned char)s[i] << 16;
        o += t[v >> 18]; o += t[(v >> 12) & 63]; o += "==";
    }
    else if (i + 2 == s.size())
    {
        unsigned v = ((unsigned char)s[i] << 16) | ((unsigned char)s[i + 1] << 8);
        o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += '=';
    }
    return o;
}

bool ParseAddress(const std::wstring& addressIn, const std::wstring& mountIn, int kind,
                  Target* t, std::wstring* err)
{
    std::string a = Trim(Narrow(addressIn));
    std::string mount = Trim(Narrow(mountIn));
    if (_strnicmp(a.c_str(), "https://", 8) == 0)
    {
        if (err) *err = L"Secure (https) source connections are not supported. Use the "
                        L"server's plain http address and port.";
        return false;
    }
    if (_strnicmp(a.c_str(), "http://", 7) == 0) a = a.substr(7);
    else if (_strnicmp(a.c_str(), "icecast://", 10) == 0) a = a.substr(10);

    std::string path;
    size_t slash = a.find('/');
    if (slash != std::string::npos)
    {
        path = a.substr(slash);
        a = a.substr(0, slash);
    }
    std::string host = a;
    int port = kind == K_ICECAST ? 8000 : 8000;
    size_t colon = a.rfind(':');
    if (colon != std::string::npos)
    {
        host = a.substr(0, colon);
        port = atoi(a.c_str() + colon + 1);
    }
    if (host.empty() || port <= 0 || port > 65535)
    {
        if (err) *err = L"The server address needs a host name, and a port if it is "
                        L"not 8000, such as stream.example.com:8000.";
        return false;
    }
    if (kind == K_ICECAST)
    {
        if (mount.empty()) mount = path;
        while (mount.size() > 1 && mount.back() == '/') mount.pop_back();
        if (mount.empty() || mount == "/")
        {
            if (err) *err = L"Icecast needs a mount point, such as /live.";
            return false;
        }
        if (mount[0] != '/') mount = "/" + mount;
    }
    if (kind == K_SHOUTCAST1 && port >= 65535)
    {
        if (err) *err = L"Shoutcast takes sources on the port after the one given, "
                        L"so the port must be below 65535.";
        return false;
    }
    t->kind = kind;
    t->host = host;
    t->port = port;
    t->mount = mount;
    return true;
}

Source::Source() : m_sock(kNoSocket) {}
Source::~Source() { Close(); }

void Source::Close()
{
    if (m_sock != kNoSocket)
    {
        shutdown((SOCKET)m_sock, SD_BOTH);
        closesocket((SOCKET)m_sock);
        m_sock = kNoSocket;
    }
}

bool Source::Open(const Target& t, int port, unsigned long long deadline, std::wstring* err)
{
    Close();
    m_sock = Dial(t.host, port, deadline, err);
    return m_sock != kNoSocket;
}

bool Source::ReadLine(std::string* line, unsigned long long deadline)
{
    return ReadLineFrom(m_sock, line, deadline);
}

bool Source::Connect(const Target& t, int timeoutMs, std::wstring* err)
{
    m_sent = 0;
    const unsigned long long deadline = GetTickCount64() + (unsigned)timeoutMs;
    const std::string user = t.user.empty() ? std::string("source") : t.user;

    if (t.kind == K_ICECAST)
    {
        std::string info = "bitrate=" + std::to_string(t.kbps) + ";channels=2;samplerate=" +
                           std::to_string(t.sampleRate);
        std::string common =
            "Host: " + t.host + ":" + std::to_string(t.port) + "\r\n"
            "Authorization: Basic " + Base64(user + ":" + t.password) + "\r\n"
            "User-Agent: FastRoute\r\n"
            "Content-Type: " + t.contentType + "\r\n"
            "Ice-Public: " + (t.isPublic ? "1" : "0") + "\r\n"
            "Ice-Name: " + HeaderSafe(t.name) + "\r\n"
            "Ice-Description: " + HeaderSafe(t.description) + "\r\n"
            "Ice-Genre: " + HeaderSafe(t.genre) + "\r\n"
            "Ice-Bitrate: " + std::to_string(t.kbps) + "\r\n"
            "Ice-Audio-Info: " + info + "\r\n";
        if (!t.url.empty()) common += "Ice-URL: " + HeaderSafe(t.url) + "\r\n";

        // PUT first, which is the method Icecast 2.4 and later document;
        // SOURCE for the servers that predate it.
        const char* methods[2] = { "PUT", "SOURCE" };
        std::wstring why;
        for (int m = 0; m < 2; ++m)
        {
            if (!Open(t, t.port, deadline, err)) return false;
            std::string req = std::string(methods[m]) + " " + t.mount +
                              (m == 0 ? " HTTP/1.1\r\n" : " HTTP/1.0\r\n") + common +
                              (m == 0 ? "Expect: 100-continue\r\n" : "") + "\r\n";
            if (!SendText(m_sock, req))
            {
                if (err) *err = L"The server closed the connection.";
                Close();
                return false;
            }
            std::string line;
            if (!ReadLine(&line, deadline))
            {
                why = L"The server did not answer the login.";
                Close();
                continue;
            }
            int code = StatusCode(line);
            // The rest of the headers, up to the blank line.
            std::string h;
            while (ReadLine(&h, deadline) && !h.empty()) {}
            if (code == 100 || code == 200) return true;
            why = StatusWords(code, line);
            Close();
            // Only a server that does not know PUT is worth asking again.
            if (code != 405 && code != 501 && code != 400 && code != 0) break;
        }
        if (err) *err = why;
        return false;
    }

    // Shoutcast: legacy source login on the next port up.
    if (!Open(t, t.port + 1, deadline, err)) return false;
    std::string pass = t.password;
    if (t.kind == K_SHOUTCAST2)
    {
        if (!t.user.empty()) pass = t.user + ":" + pass;
        pass += ":#" + std::to_string(t.sid > 0 ? t.sid : 1);
    }
    if (!SendText(m_sock, pass + "\r\n"))
    {
        if (err) *err = L"The server closed the connection.";
        Close();
        return false;
    }
    std::string line;
    if (!ReadLine(&line, deadline))
    {
        if (err) *err = L"The server did not answer the login.";
        Close();
        return false;
    }
    if (line.compare(0, 2, "OK") != 0)
    {
        if (err) *err = line.find("nvalid") != std::string::npos
            ? std::wstring(L"The server refused the password.")
            : L"The server answered: " + Widen(line);
        Close();
        return false;
    }
    // DNAS sends "OK2" and an icy-caps line, then a blank line; older ones
    // send nothing further. Whatever is there is read off without waiting.
    {
        std::string h;
        while (WaitReadable(m_sock, GetTickCount64() + 300) && ReadLine(&h, deadline) &&
               !h.empty()) {}
    }
    std::string hdr =
        "content-type:" + t.contentType + "\r\n"
        "icy-name:" + HeaderSafe(t.name) + "\r\n"
        "icy-genre:" + HeaderSafe(t.genre) + "\r\n"
        "icy-pub:" + (t.isPublic ? "1" : "0") + "\r\n"
        "icy-br:" + std::to_string(t.kbps) + "\r\n"
        "icy-url:" + HeaderSafe(t.url) + "\r\n"
        "icy-description:" + HeaderSafe(t.description) + "\r\n"
        "\r\n";
    if (!SendText(m_sock, hdr))
    {
        if (err) *err = L"The server closed the connection.";
        Close();
        return false;
    }
    return true;
}

bool Source::Send(const uint8_t* p, size_t n, std::wstring* err)
{
    while (n > 0)
    {
        int r = send((SOCKET)m_sock, (const char*)p, n > 65536 ? 65536 : (int)n, 0);
        if (r <= 0)
        {
            int e = WSAGetLastError();
            if (err) *err = (e == WSAETIMEDOUT) ? L"The server stopped taking audio."
                                                : L"The server closed the connection.";
            return false;
        }
        p += r;
        n -= (size_t)r;
        m_sent += (unsigned long long)r;
    }
    return true;
}

bool Source::Poll(std::wstring* err)
{
    if (m_sock == kNoSocket) { if (err) *err = L"Not connected."; return false; }
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET((SOCKET)m_sock, &rs);
    timeval tv = { 0, 0 };
    if (select(0, &rs, nullptr, nullptr, &tv) > 0)
    {
        char buf[512];
        int r = recv((SOCKET)m_sock, buf, sizeof(buf), 0);
        if (r <= 0)
        {
            if (err) *err = L"The server closed the connection.";
            return false;
        }
    }
    return true;
}

bool UpdateTitle(const Target& t, const std::string& title, std::wstring* err)
{
    const unsigned long long deadline = GetTickCount64() + 5000;
    std::string path, auth;
    const std::string user = t.user.empty() ? std::string("source") : t.user;
    if (t.kind == K_ICECAST)
    {
        path = "/admin/metadata?mount=" + UrlEncode(t.mount) + "&mode=updinfo&song=" +
               UrlEncode(title);
        auth = "Authorization: Basic " + Base64(user + ":" + t.password) + "\r\n";
    }
    else if (t.kind == K_SHOUTCAST1)
        path = "/admin.cgi?pass=" + UrlEncode(t.password) + "&mode=updinfo&song=" +
               UrlEncode(title);
    else
    {
        std::string pass = t.user.empty() ? t.password : t.user + ":" + t.password;
        path = "/admin.cgi?sid=" + std::to_string(t.sid > 0 ? t.sid : 1) + "&pass=" +
               UrlEncode(pass) + "&mode=updinfo&song=" + UrlEncode(title);
    }

    uintptr_t s = Dial(t.host, t.port, deadline, err);
    if (s == kNoSocket) return false;
    // Shoutcast 1 only answers what it takes for a browser.
    std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + t.host + ":" +
                      std::to_string(t.port) + "\r\n" + auth +
                      "User-Agent: Mozilla/5.0 (compatible; FastRoute)\r\n\r\n";
    bool ok = SendText(s, req);
    std::string line;
    if (ok) ok = ReadLineFrom(s, &line, deadline);
    closesocket((SOCKET)s);
    if (!ok)
    {
        if (err) *err = L"The server did not answer the title update.";
        return false;
    }
    int code = StatusCode(line);
    if (code != 200)
    {
        if (err) *err = L"The server would not take the title: " + Widen(line);
        return false;
    }
    return true;
}

}   // namespace icecast
