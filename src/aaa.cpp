#include "aaa.h"
#include "json.h"
#include "util.h"
#include "version.h"

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>

namespace aaa {

namespace {

const wchar_t* kHost = L"aaastreamer.devinecreations.net";
const wchar_t* kDefaultRtmp = L"rtmp://aaastreamer.devinecreations.net:1935/live";

std::wstring TokenPath() { return AppDataDir() + L"\\aaastreamer-token.dat"; }

std::wstring LoadToken()
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, TokenPath().c_str(), L"rb") != 0 || !f) return std::wstring();
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    std::wstring stored(buf, buf + n);
    return UnprotectText(stored);
}

bool SaveToken(const std::wstring& token)
{
    std::wstring prot = ProtectText(token);
    if (prot.empty()) return false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, TokenPath().c_str(), L"wb") != 0 || !f) return false;
    std::string ascii;                             // base64 is plain ASCII
    for (wchar_t ch : prot) ascii += (char)ch;
    fwrite(ascii.data(), 1, ascii.size(), f);
    fclose(f);
    return true;
}

std::wstring FromUtf8(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string ToUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0,
                                nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n,
                                   nullptr, nullptr);
    return s;
}

// One HTTPS request to the AAA Streamer API. The bearer token, if any, goes in
// a header and nowhere else.
bool Request(const wchar_t* method, const std::wstring& path, const std::string& body,
             bool withToken, int* status, Json* doc, std::wstring* err)
{
    std::wstring token;
    if (withToken)
    {
        token = LoadToken();
        if (token.empty())
        {
            if (err) *err = L"Not signed in to AAA Streamer.";
            return false;
        }
    }

    HINTERNET s = WinHttpOpen(L"FastRoute/" FASTROUTE_VERSION_W,
                              WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { if (err) *err = L"Windows could not start an internet connection."; return false; }
    // Windows 7 does not offer TLS 1.2 unless asked, and the server needs it.
    DWORD protocols = 0x00000800 /* TLS1_2 */ | 0x00000200 /* TLS1_1 */;
    WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 15000);

    bool ok = false;
    HINTERNET c = WinHttpConnect(s, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, method, path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                    : nullptr;
    if (r)
    {
        std::wstring headers = L"Accept: application/json\r\n";
        if (!body.empty()) headers += L"Content-Type: application/json\r\n";
        if (!token.empty()) headers += L"Authorization: Bearer " + token + L"\r\n";
        if (WinHttpSendRequest(r, headers.c_str(), (DWORD)-1L,
                               body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                               (DWORD)body.size(), (DWORD)body.size(), 0) &&
            WinHttpReceiveResponse(r, nullptr))
        {
            DWORD code = 0, len = sizeof(code);
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &code, &len,
                                WINHTTP_NO_HEADER_INDEX);
            if (status) *status = (int)code;
            std::string text;
            for (;;)
            {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(r, &avail) || avail == 0) break;
                std::string chunk(avail, '\0');
                DWORD got = 0;
                if (!WinHttpReadData(r, &chunk[0], avail, &got) || got == 0) break;
                text.append(chunk.data(), got);
                if (text.size() > 4u * 1024u * 1024u) break;
            }
            ok = Json::Parse(FromUtf8(text), *doc);
            if (!ok && err) *err = FormatW(L"AAA Streamer sent an answer that could not be "
                                           L"read (HTTP %lu).", code);
        }
        else if (err)
            *err = FormatW(L"Could not reach AAA Streamer (error %lu). Check the internet "
                           L"connection.", GetLastError());
    }
    else if (err)
        *err = L"Could not reach AAA Streamer.";
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

}   // namespace

std::wstring BaseUrl() { return std::wstring(L"https://") + kHost; }

bool StartSignIn(DeviceStart* out, std::wstring* err)
{
    std::string body = "{\"clientName\":\"FastRoute on " + ToUtf8([] {
        wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = { 0 };
        DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
        GetComputerNameW(name, &n);
        std::wstring s;
        for (wchar_t* p = name; *p; ++p)
            if (*p != L'"' && *p != L'\\') s += *p;   // stays valid JSON
        return s;
    }()) + "\",\"platform\":\"windows\",\"version\":\"" FASTROUTE_VERSION
           "\",\"clientId\":\"fastroute\"}";
    int status = 0;
    Json doc;
    if (!Request(L"POST", L"/api/client/v1/auth/start", body, false, &status, &doc, err))
        return false;
    out->userCode = doc.AsString(L"userCode");
    out->pollToken = doc.AsString(L"pollToken");
    out->authorizeUrl = doc.AsString(L"authorizeUrl");
    if (!doc.AsBool(L"success") || out->pollToken.empty())
    {
        if (err) *err = L"AAA Streamer did not start the sign-in: " +
                        doc.AsString(L"error", FormatW(L"HTTP %d", status));
        return false;
    }
    if (out->authorizeUrl.empty())
        out->authorizeUrl = BaseUrl() + L"/client/authorize?code=" + out->userCode;
    return true;
}

PollResult PollSignIn(const std::wstring& pollToken, std::wstring* err)
{
    int status = 0;
    Json doc;
    if (!Request(L"GET", L"/api/client/v1/auth/poll/" + pollToken, std::string(), false,
                 &status, &doc, err))
        return PR_ERROR;
    const std::wstring st = doc.AsString(L"status");
    if (st == L"pending") return PR_PENDING;
    if (st == L"denied") return PR_DENIED;
    if (st == L"expired" || st == L"not_found") return PR_EXPIRED;
    if (st == L"approved")
    {
        // Handed over once only: the server forgets it straight after.
        const std::wstring token = doc.AsString(L"accessToken");
        if (token.empty())
        {
            if (err) *err = L"The sign-in was approved, but the token had already been "
                            L"collected. Sign in again.";
            return PR_ERROR;
        }
        if (!SaveToken(token))
        {
            if (err) *err = L"The sign-in worked, but FastRoute could not save it in " +
                            AppDataDir() + L".";
            return PR_ERROR;
        }
        return PR_APPROVED;
    }
    if (err) *err = L"AAA Streamer gave an unexpected answer to the sign-in.";
    return PR_ERROR;
}

bool SignedIn() { return !LoadToken().empty(); }

void SignOut() { DeleteFileW(TokenPath().c_str()); }

std::wstring WhoAmI(std::wstring* err)
{
    int status = 0;
    Json doc;
    if (!Request(L"GET", L"/api/client/v1/device/me", std::string(), true, &status, &doc, err))
        return std::wstring();
    if (status == 401)
    {
        if (err) *err = L"AAA Streamer no longer accepts this sign-in. Sign in again.";
        return std::wstring();
    }
    const Json* user = doc.Find(L"user");
    return user ? user->AsString(L"username") : std::wstring();
}

bool ListStreams(std::vector<Stream>* out, std::wstring* err)
{
    out->clear();

    // Where to publish comes from the service's own description rather than
    // from each stream, whose stored ingest address can be stale.
    std::wstring rtmpBase = kDefaultRtmp;
    {
        int st = 0;
        Json health;
        if (Request(L"GET", L"/api/client/v1/health", std::string(), false, &st, &health,
                    nullptr))
            if (const Json* svc = health.Find(L"service"))
            {
                std::wstring u = svc->AsString(L"rtmpUrl");
                if (u.compare(0, 7, L"rtmp://") == 0) rtmpBase = u;
            }
    }

    int status = 0;
    Json doc;
    if (!Request(L"GET", L"/api/client/v1/streams", std::string(), true, &status, &doc, err))
        return false;
    if (status == 401)
    {
        if (err) *err = L"AAA Streamer no longer accepts this sign-in. Sign in again.";
        return false;
    }
    const Json* arr = doc.Find(L"streams");
    if (!arr || !arr->IsArray())
    {
        if (err) *err = L"AAA Streamer did not send a list of streams.";
        return false;
    }
    for (const Json& j : arr->Items())
    {
        Stream s;
        s.key = j.AsString(L"streamKey");
        if (s.key.empty()) continue;     // somebody else's public stream
        s.id = j.AsString(L"id");
        s.title = j.AsString(L"title");
        if (s.title.empty()) s.title = j.AsString(L"slug");
        s.status = j.AsString(L"status");
        s.watchUrl = j.AsString(L"watchUrl");
        s.rtmpUrl = rtmpBase;
        out->push_back(s);
    }
    return true;
}

}   // namespace aaa
