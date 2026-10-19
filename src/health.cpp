#include "health.h"
#include "updater.h"
#include "version.h"
#include "json.h"
#include "util.h"
#include "audio/audio.h"      // WindowsVersionText

#include <windows.h>
#include <winhttp.h>
#include <objbase.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// The collector
// ---------------------------------------------------------------------------
namespace {

const wchar_t* kApp       = L"fastroute";
const wchar_t* kHost      = L"files.tappedin.fm";
const wchar_t* kEventPath = L"/app-health/v1/event";
const wchar_t* kDiagPath  = L"/app-health/v1/diagnostics";
const wchar_t* kAgent     = L"FastRoute app-health";
const wchar_t* kChannel   = L"stable";

// The collector's client id for this application. Public by design: it says
// which application a report came from, not who sent it, and it grants nothing
// beyond posting these reports. Every one of our applications carries its own
// the same way. A fork that does not want its copies reporting to us builds
// with -DFASTROUTE_APP_HEALTH_KEY="" - an empty key turns reporting off
// altogether rather than sending reports nothing will accept.
#ifndef FASTROUTE_APP_HEALTH_KEY
#define FASTROUTE_APP_HEALTH_KEY "ah1-fastroute-a72001adc976e260"
#endif

// How many reports are kept while there is no way to send them. Thirty is more
// than a month of ordinary use; past that the oldest go, because what happened
// an hour ago matters more than what happened in March.
const size_t kQueueMax = 30;

// An update applied by the script reopens FastRoute within seconds. Longer
// than this and it did not come back by itself, which is its own failure even
// though the files were replaced: somebody had to start it.
const long long kRelaunchGraceSeconds = 10 * 60;

// Long enough for a slow link, short enough that a report nobody is waiting
// for cannot hold a thread - or the way out of the program - for long.
const DWORD kTimeoutMs = 5000;

// The error log is larger than a report and somebody is sitting watching the
// button, so this one is allowed longer before it gives up.
const DWORD kDiagTimeoutMs = 20000;

// How many of our own errors one run will report. An error that repeats - a
// scheduled event firing every minute against a session that is not there -
// would otherwise send the same thing until the hourly limit at the collector
// cut it off, and the one worth having is the first. The rest are still in the
// error log, and the person can send that if it matters.
const int kMaxFaultsPerRun = 5;

// ---------------------------------------------------------------------------
// What is remembered between runs
//
// Beside the settings, in app_health.json. It is not in the release zip, so it
// survives an update being unpacked over the folder - which it has to, because
// the marker written before the update is the only way the next run can tell
// an update that worked from one that did not.
// ---------------------------------------------------------------------------
struct State
{
    std::wstring installId;
    std::wstring lastVersion;
    std::wstring lastCommit;
    std::wstring startedDay;         // "2026-10-18", so one report a day
    std::wstring offeredCommit;      // the last build reported as available

    bool         pending = false;    // an update was handed to the script
    std::wstring fromVersion, fromCommit, toVersion, toCommit;
    long long    startedAt = 0;      // unix seconds, for the relaunch grace

    std::vector<std::wstring> queue; // reports with nowhere to go yet
};

// All of this is made once and never destroyed, on purpose, and that includes
// the sender's own thread object. Two things would otherwise go wrong on the
// way out of the program: a report still in flight would be writing to globals
// that static destruction had already taken apart, and a std::thread destroyed
// while still joinable ends the process through terminate rather than through
// the error path that was being taken.
struct Shared
{
    std::mutex   lock;
    State        st;
    bool         loaded = false;
    std::wstring dir;                // where app_health.json settled

    std::mutex   senderLock;         // held while starting or waiting for it
    std::thread  sender;
};
Shared& S()
{
    static Shared* s = new Shared();
    return *s;
}

std::atomic<bool> g_enabled{ false };   // nothing happens until the setting is read
std::atomic<bool> g_stopping{ false };
std::atomic<bool> g_senderIdle{ true };

bool HaveKey()
{
    return FASTROUTE_APP_HEALTH_KEY[0] != '\0';
}

// --- small helpers ---------------------------------------------------------

long long NowSeconds()
{
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    // 1601 to 1970, in hundreds of nanoseconds.
    return (long long)((u.QuadPart - 116444736000000000ull) / 10000000ull);
}

std::wstring Today()
{
    SYSTEMTIME t{};
    GetSystemTime(&t);
    return FormatW(L"%04u-%02u-%02u", (unsigned)t.wYear, (unsigned)t.wMonth,
                   (unsigned)t.wDay);
}

std::wstring Clip(const std::wstring& s, size_t n)
{
    return s.size() <= n ? s : s.substr(0, n);
}

// Takes a sign-in out of any URL in the text. A Stream Out block's address can
// be written as http://source:password@host:8000/mount, and that password is
// the person's - it is their broadcast server, not ours. The collector's own
// redaction looks for the word "password" beside a value and would not see
// this one, because there is no word here, only a colon. So it goes before
// anything leaves the machine.
std::wstring ScrubUrlSignIns(const std::wstring& text)
{
    std::wstring out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size())
    {
        const size_t mark = text.find(L"://", i);
        if (mark == std::wstring::npos) { out.append(text, i, std::wstring::npos); break; }
        const size_t hostAt = mark + 3;
        // The authority runs to the next slash, and an @ before that one is a
        // sign-in. A space or a quote ends it too: these are sentences, and
        // "see http://a/b and then" must not swallow the rest of the line.
        size_t end = text.find_first_of(L"/?# \t\r\n\"'", hostAt);
        if (end == std::wstring::npos) end = text.size();
        const size_t at = text.rfind(L'@', end == 0 ? 0 : end - 1);
        out.append(text, i, hostAt - i);
        if (at != std::wstring::npos && at >= hostAt && at < end)
        {
            out += L"<sign-in>@";
            i = at + 1;
        }
        else i = hostAt;
    }
    return out;
}

// Takes the person out of text on its way off the machine. The collector
// redacts as well, but a path through somebody's home folder has no business
// leaving in the first place, and the folder name is the one part of these
// messages that is about a person rather than about FastRoute.
std::wstring Scrub(const std::wstring& text)
{
    std::wstring out = ScrubUrlSignIns(text);
    wchar_t home[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    if (n > 0 && n < MAX_PATH && !out.empty())
    {
        const std::wstring find = home;
        const std::wstring with = L"<user folder>";
        // Case insensitively: Windows hands paths back in whichever case the
        // caller wrote them. Both copies are rewritten together, so the
        // positions found in one are the positions in the other.
        std::wstring lowerOut = out, lowerFind = find;
        CharLowerBuffW(&lowerOut[0], (DWORD)lowerOut.size());
        CharLowerBuffW(&lowerFind[0], (DWORD)lowerFind.size());
        size_t at = lowerOut.find(lowerFind);
        while (at != std::wstring::npos)
        {
            out.replace(at, find.size(), with);
            lowerOut.replace(at, find.size(), with);
            at = lowerOut.find(lowerFind, at + with.size());
        }
    }
    return out;
}

// The same, cut to what one report's error field holds. The error log that
// goes with Send Diagnostic Logs is far longer than this and uses Scrub
// directly: a log cut off at its first thousand characters would be missing
// the end, which is the part somebody pressing that button is complaining
// about.
std::wstring Depersonalise(const std::wstring& text)
{
    return Clip(Scrub(text), 1000);
}

std::wstring ArchName()
{
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture)
    {
    case PROCESSOR_ARCHITECTURE_AMD64: return L"x64";
    case PROCESSOR_ARCHITECTURE_INTEL: return L"x86";
    case PROCESSOR_ARCHITECTURE_ARM64: return L"arm64";
    default: return std::wstring();
    }
}

std::wstring NewInstallId()
{
    GUID g{};
    if (FAILED(CoCreateGuid(&g))) return std::wstring();
    wchar_t buf[40] = {};
    if (StringFromGUID2(g, buf, 40) == 0) return std::wstring();
    // Without the braces: the collector accepts letters, digits, dots, colons,
    // dashes and underscores, and nothing else.
    std::wstring s = buf;
    if (s.size() >= 2 && s.front() == L'{') s = s.substr(1, s.size() - 2);
    return s;
}

// --- the state file --------------------------------------------------------

std::wstring StatePath(const std::wstring& dir)
{
    return dir + L"\\app_health.json";
}

// The same folder AppDataDir names, worked out rather than asked for, because
// AppDataDir creates it. Looking to see whether a file is there must not be
// what leaves one behind: FastRoute is portable, and a copy whose own folder
// is writable never uses this place at all.
std::wstring RoamingDirIfThere()
{
    wchar_t base[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    const std::wstring dir = std::wstring(base) + L"\\FastRoute";
    return DirectoryExists(dir) ? dir : std::wstring();
}

bool WriteText(const std::wstring& path, const std::wstring& text)
{
    const std::wstring tmp = path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wt, ccs=UTF-8") != 0 || !f) return false;
    fputws(text.c_str(), f);
    fputwc(L'\n', f);
    fclose(f);
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

// Caller holds the lock.
void Save(Shared& s)
{
    Json doc = Json::Obj();
    doc.Set(L"installId", Json::Str(s.st.installId));
    doc.Set(L"lastVersion", Json::Str(s.st.lastVersion));
    doc.Set(L"lastCommit", Json::Str(s.st.lastCommit));
    doc.Set(L"startedDay", Json::Str(s.st.startedDay));
    doc.Set(L"offeredCommit", Json::Str(s.st.offeredCommit));
    if (s.st.pending)
    {
        Json p = Json::Obj();
        p.Set(L"fromVersion", Json::Str(s.st.fromVersion));
        p.Set(L"fromCommit", Json::Str(s.st.fromCommit));
        p.Set(L"toVersion", Json::Str(s.st.toVersion));
        p.Set(L"toCommit", Json::Str(s.st.toCommit));
        p.Set(L"startedAt", Json::Num((double)s.st.startedAt));
        doc.Set(L"pending", std::move(p));
    }
    Json q = Json::Arr();
    for (const std::wstring& e : s.st.queue) q.Push(Json::Str(e));
    doc.Set(L"queue", std::move(q));

    const std::wstring text = doc.Serialise();

    // Beside the program, like the settings, so a copy on a memory stick keeps
    // its own install number. A folder nobody may write to - Program Files,
    // most often - falls back to the per-user one, the only place left.
    if (!s.dir.empty())
    {
        if (WriteText(StatePath(s.dir), text)) return;
        if (s.dir != ConfigDir()) return;        // already the fallback
    }
    const std::wstring fallback = AppDataDir();
    if (fallback.empty() || fallback == s.dir) return;
    if (WriteText(StatePath(fallback), text)) s.dir = fallback;
}

// Caller holds the lock.
void Load(Shared& s)
{
    if (s.loaded) return;
    s.loaded = true;

    // Beside the program first, then the per-user folder a copy in a place it
    // may not write to would have fallen back to - and that one only if it is
    // already there, so that looking is not what creates it.
    const std::wstring places[2] = { ConfigDir(), RoamingDirIfThere() };
    for (const std::wstring& dir : places)
    {
        if (dir.empty()) continue;
        FILE* f = nullptr;
        if (_wfopen_s(&f, StatePath(dir).c_str(), L"rt, ccs=UTF-8") != 0 || !f)
            continue;
        std::wstring text;
        wchar_t buf[4096];
        while (fgetws(buf, 4096, f)) text += buf;
        fclose(f);

        Json doc;
        if (!Json::Parse(text, doc) || !doc.IsObject()) continue;
        s.dir = dir;
        s.st.installId   = doc.AsString(L"installId");
        s.st.lastVersion = doc.AsString(L"lastVersion");
        s.st.lastCommit  = doc.AsString(L"lastCommit");
        s.st.startedDay  = doc.AsString(L"startedDay");
        s.st.offeredCommit = doc.AsString(L"offeredCommit");
        const Json* p = doc.Find(L"pending");
        if (p && p->IsObject())
        {
            s.st.pending     = true;
            s.st.fromVersion = p->AsString(L"fromVersion");
            s.st.fromCommit  = p->AsString(L"fromCommit");
            s.st.toVersion   = p->AsString(L"toVersion");
            s.st.toCommit    = p->AsString(L"toCommit");
            s.st.startedAt   = (long long)p->AsNumberAt(L"startedAt");
        }
        const Json* q = doc.Find(L"queue");
        if (q && q->IsArray())
            for (const Json& e : q->Items())
            {
                const std::wstring one = e.AsText();
                if (!one.empty()) s.st.queue.push_back(one);
            }
        break;
    }

    if (s.dir.empty()) s.dir = ConfigDir();
    if (s.st.installId.empty())
    {
        s.st.installId = NewInstallId();
        // Only written out once there is reporting to do: a program told not
        // to report should not be leaving a new file in the folder either.
        if (g_enabled.load()) Save(s);
    }
}

std::wstring InstallId()
{
    Shared& s = S();
    std::lock_guard<std::mutex> g(s.lock);
    Load(s);
    return s.st.installId;
}

// --- sending ---------------------------------------------------------------

// What became of one report.
enum class Posted
{
    Sent,       // gone; drop it
    Refused,    // the collector read it and said no, so retrying is pointless
    Deferred,   // offline, throttled or down; keep it for later
};

// One request. `reply` takes the collector's answer when a caller wants it -
// the diagnostics upload does, because the reference it gives back is the
// whole point of that one. `status` takes the HTTP code, for a caller that has
// to tell somebody what went wrong in words.
Posted Post(const wchar_t* path, const std::wstring& body, DWORD timeoutMs,
            std::wstring* reply = nullptr, DWORD* status = nullptr)
{
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, body.c_str(), (int)body.size(),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return Posted::Refused;
    std::vector<char> utf8((size_t)bytes);
    WideCharToMultiByte(CP_UTF8, 0, body.c_str(), (int)body.size(), utf8.data(),
                        bytes, nullptr, nullptr);

    HINTERNET session = WinHttpOpen(kAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return Posted::Deferred;
    WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HINTERNET conn = WinHttpConnect(session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"POST", path, nullptr,
                                              WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              WINHTTP_FLAG_SECURE)
                         : nullptr;
    Posted result = Posted::Deferred;
    if (req)
    {
        const std::wstring headers =
            FormatW(L"Content-Type: application/json\r\nX-App-Key: %s\r\n",
                    FASTROUTE_WIDEN(FASTROUTE_APP_HEALTH_KEY));
        if (WinHttpSendRequest(req, headers.c_str(), (DWORD)-1, utf8.data(),
                               (DWORD)utf8.size(), (DWORD)utf8.size(), 0) &&
            WinHttpReceiveResponse(req, nullptr))
        {
            DWORD code = 0, size = sizeof(code);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE |
                                WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &code, &size,
                                WINHTTP_NO_HEADER_INDEX);
            if (status) *status = code;
            if (code >= 200 && code < 300)      result = Posted::Sent;
            else if (code == 400 || code == 403 || code == 413)
                                                result = Posted::Refused;
            // Anything else - 429 from the rate limit, 502 while the collector
            // restarts - is worth trying again later.

            if (reply)
            {
                // Bounded: nothing we ask for answers with more than a few
                // hundred bytes, and a collector that answered with a
                // megabyte would be a reason to stop reading, not to grow.
                std::string raw;
                char buf[1024];
                DWORD got = 0;
                while (raw.size() < 8192 &&
                       WinHttpReadData(req, buf, sizeof(buf), &got) && got > 0)
                    raw.append(buf, got);
                if (!raw.empty())
                {
                    const int wide = MultiByteToWideChar(CP_UTF8, 0, raw.data(),
                                                         (int)raw.size(), nullptr, 0);
                    if (wide > 0)
                    {
                        reply->resize((size_t)wide);
                        MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(),
                                            &(*reply)[0], wide);
                    }
                }
            }
        }
        WinHttpCloseHandle(req);
    }
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return result;
}

Posted PostEvent(const std::wstring& body)
{
    return Post(kEventPath, body, kTimeoutMs);
}

// Sends what is queued, oldest first, and stops at the first one that could
// not go: the order is worth keeping, and a collector that is down will not
// have changed its mind two reports later.
void Flush()
{
    Shared& s = S();
    for (;;)
    {
        std::wstring next;
        {
            std::lock_guard<std::mutex> g(s.lock);
            if (s.st.queue.empty()) return;
            next = s.st.queue.front();
        }

        if (PostEvent(next) == Posted::Deferred) return;

        {
            std::lock_guard<std::mutex> g(s.lock);
            // It may have been thrown away while this was in flight, by the
            // setting being turned off.
            if (!s.st.queue.empty() && s.st.queue.front() == next)
            {
                s.st.queue.erase(s.st.queue.begin());
                Save(s);
            }
        }
        // On the way out, one report is enough; the rest keep.
        if (g_stopping.load()) return;
    }
}

// Asks the sender to get on with it. The caller must not be holding the state
// lock: the sender takes it, and Shutdown holds this one while it waits.
void Nudge()
{
    if (g_stopping.load() || !g_enabled.load()) return;
    Shared& s = S();
    std::lock_guard<std::mutex> g(s.senderLock);
    if (s.sender.joinable())
    {
        if (!g_senderIdle.load()) return;       // one is already at it
        s.sender.join();
    }
    g_senderIdle.store(false);
    s.sender = std::thread([] { Flush(); g_senderIdle.store(true); });
}

// --- building a report -----------------------------------------------------

Json Base(const wchar_t* event, const std::wstring& installId)
{
    Json o = Json::Obj();
    o.Set(L"app", Json::Str(kApp));
    o.Set(L"install_id", Json::Str(installId));
    o.Set(L"platform", Json::Str(L"windows"));
    o.Set(L"os", Json::Str(Clip(WindowsVersionText(), 80)));
    o.Set(L"arch", Json::Str(ArchName()));
    o.Set(L"channel", Json::Str(kChannel));
    o.Set(L"version", Json::Str(RunningVersion()));
    o.Set(L"event", Json::Str(event));
    return o;
}

// The same two facts on every report: which build this is exactly, and whose
// releases it follows. Together they answer "how many copies are on which
// build, updating from where", which is what a feed that has to move cannot be
// retired without.
void AddDetail(Json& o, const wchar_t* extraKey = nullptr,
               const std::wstring& extraValue = std::wstring())
{
    Json d = Json::Obj();
    d.Set(L"feed", Json::Str(Health::FeedName()));
    const std::wstring commit = RunningCommit();
    if (!commit.empty()) d.Set(L"commit", Json::Str(Clip(commit, 40)));
    if (extraKey) d.Set(extraKey, Json::Str(Clip(extraValue, 300)));
    o.Set(L"detail", std::move(d));
}

// Queues one report and asks the sender to get on with it. The caller must not
// hold the state lock.
void Queue(Json&& o)
{
    if (!g_enabled.load()) return;
    Shared& s = S();
    const std::wstring body = o.Serialise();
    {
        std::lock_guard<std::mutex> g(s.lock);
        Load(s);
        s.st.queue.push_back(body);
        while (s.st.queue.size() > kQueueMax) s.st.queue.erase(s.st.queue.begin());
        Save(s);
    }
    Nudge();
}

// True when the running build is the one the marker said was being installed -
// or at least no longer the one it replaced. The commit is what decides it:
// FastRoute's version string stays at one number across many builds, so
// comparing versions alone would call every successful update a failure.
bool CommitsDiffer(const std::wstring& a, const std::wstring& b)
{
    // The release carries the full hash and the build a short one, so the
    // comparison is by prefix, in whichever direction is shorter.
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    return _wcsnicmp(a.c_str(), b.c_str(), n) != 0;
}

bool LooksInstalled(const State& st)
{
    const std::wstring commit = RunningCommit();
    if (!commit.empty())
    {
        if (!st.toCommit.empty() && !CommitsDiffer(commit, st.toCommit)) return true;
        // A second release may have landed between the download and this run,
        // so a build that is simply no longer the one that was replaced counts.
        if (!st.fromCommit.empty()) return CommitsDiffer(commit, st.fromCommit);
    }
    if (!st.toVersion.empty()) return RunningVersion() == st.toVersion;
    return false;
}

// What the update script left behind, if it could not do its job. Read once
// and removed, so the same failure is not reported on every later run.
std::wstring TakeInstallerError()
{
    const std::wstring path = Health::InstallerErrorPath();
    FILE* f = nullptr;
    // Plain text, not UTF-8: cmd's echo writes in whatever code page the
    // console has, and the two sentences the script can write are ASCII, where
    // every code page agrees. Reading it as UTF-8 would be the guess that goes
    // wrong on a machine whose console is not.
    if (_wfopen_s(&f, path.c_str(), L"rt") != 0 || !f) return std::wstring();
    std::wstring text;
    wchar_t buf[1024];
    while (fgetws(buf, 1024, f)) text += buf;
    fclose(f);
    DeleteFileW(path.c_str());
    return Trim(text);
}

// ---------------------------------------------------------------------------
// A crash
//
// Everything the filter will need is worked out here, before anything has gone
// wrong, and left in plain variables: a path, two numbers and a short string.
// The filter itself then has nothing to ask anybody for.
//
// That is the whole design, and the reason for it is that by the time the
// filter runs the process is already broken. The heap may be the thing that
// broke it, so it must not allocate. The lock that guards the state file may
// be held by the thread that just died, so it must not lock. A thread started
// here may never be scheduled, and a network request may sit for its whole
// timeout, so it must do neither - a crash that Windows deals with in a second
// should not become half a minute of a window that does not repaint.
//
// So it writes one line of ASCII to a file of its own with CreateFile and
// WriteFile, which are as close to nothing as this gets, and hands the
// exception on. The next run turns that line into a sentence, where sentences
// are safe to make, and sends it like any other report.
// ---------------------------------------------------------------------------

wchar_t   g_crashPath[MAX_PATH] = {};   // empty until the filter is armed
char      g_crashPrefix[160]    = {};   // "version=0.6 commit=abc1234"
uintptr_t g_imageBase = 0;
uintptr_t g_imageEnd  = 0;
LONG      g_crashWritten = 0;
LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;

// Hex, by hand. The CRT's own formatting takes a lock the first time it is
// asked for a locale, which is one of the things that cannot be risked here.
char* AppendHex(char* p, char* end, unsigned long long v, int digits)
{
    static const char* kDigits = "0123456789abcdef";
    if (end - p < digits + 2) return p;
    *p++ = '0';
    *p++ = 'x';
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4)
        *p++ = kDigits[(v >> shift) & 0xF];
    return p;
}

char* AppendText(char* p, char* end, const char* text)
{
    while (*text && p < end) *p++ = *text++;
    return p;
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    // Once. A second crash - including one caused by this very handler - must
    // not come back round through here, and must not overwrite the first
    // report, which is the one that says what actually went wrong.
    if (InterlockedCompareExchange(&g_crashWritten, 1, 0) != 0 ||
        !g_crashPath[0] || !ep || !ep->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    const EXCEPTION_RECORD& er = *ep->ExceptionRecord;

    char line[512];
    char* p = line;
    char* const end = line + sizeof(line) - 2;

    p = AppendText(p, end, "code=");
    p = AppendHex(p, end, er.ExceptionCode, 8);

    p = AppendText(p, end, " at=");
    const uintptr_t at = (uintptr_t)er.ExceptionAddress;
    if (g_imageBase && at >= g_imageBase && at < g_imageEnd)
    {
        // An offset into our own exe, which is what a build's map file can be
        // read against. An absolute address says nothing: Windows moves the
        // image somewhere different on every run.
        p = AppendText(p, end, "FastRoute.exe+");
        p = AppendHex(p, end, at - g_imageBase, 8);
    }
    else
    {
        p = AppendText(p, end, "elsewhere+");
        p = AppendHex(p, end, at, 16);
    }

    // For the common one, which address was out of bounds and which way it was
    // being used. A null in a member read looks nothing like a wild pointer in
    // a write, and that difference is most of the diagnosis.
    if (er.ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        er.NumberParameters >= 2)
    {
        const ULONG_PTR how = er.ExceptionInformation[0];
        p = AppendText(p, end, how == 0 ? " access=read" :
                               how == 1 ? " access=write" :
                               how == 8 ? " access=execute" : " access=unknown");
        p = AppendText(p, end, " address=");
        p = AppendHex(p, end, (unsigned long long)er.ExceptionInformation[1], 16);
    }

    p = AppendText(p, end, " ");
    p = AppendText(p, end, g_crashPrefix);
    *p++ = '\n';

    // CREATE_ALWAYS: if the last run's report somehow survived being read, the
    // one from the crash that just happened is the better of the two.
    HANDLE h = CreateFileW(g_crashPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        DWORD wrote = 0;
        WriteFile(h, line, (DWORD)(p - line), &wrote, nullptr);
        CloseHandle(h);
    }

    // Whatever Windows was going to do, it still does: the error-reporting
    // dialog, a debugger, the process ending. Nothing about this is a reason
    // to change that, and a filter that swallowed the exception would be.
    if (g_previousFilter) return g_previousFilter(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

std::wstring StateDir()
{
    Shared& s = S();
    std::lock_guard<std::mutex> g(s.lock);
    Load(s);
    return s.dir;
}

std::wstring CrashNotePath(const std::wstring& dir)
{
    return dir + L"\\app_health_crash.txt";
}

// The line the last run's filter left, read once and removed, the same way the
// update script's note is. ASCII, written by code that could not have made
// anything else.
std::wstring TakeCrashNote(const std::wstring& dir)
{
    const std::wstring path = CrashNotePath(dir);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rt") != 0 || !f) return std::wstring();
    std::wstring text;
    wchar_t buf[512];
    while (fgetws(buf, 512, f)) text += buf;
    fclose(f);
    DeleteFileW(path.c_str());
    return Trim(text);
}

// The name for an exception code, for the sentence the report carries. Made
// here, on a run that is working, rather than in the filter.
const wchar_t* CrashWord(unsigned long code)
{
    switch (code)
    {
    case 0xC0000005: return L"an access violation";
    case 0xC0000006: return L"a file on a drive that went away mid-read";
    case 0xC000001D: return L"an illegal instruction";
    case 0xC0000025: return L"an exception that could not be continued";
    case 0xC000008C: return L"an array index out of bounds";
    case 0xC000008E: return L"a division by zero in floating point";
    case 0xC0000090: return L"an invalid floating-point operation";
    case 0xC0000094: return L"a division by zero";
    case 0xC00000FD: return L"a stack overflow";
    case 0xC0000374: return L"a corrupted heap";
    case 0xC0000409: return L"a stack buffer that was written past its end";
    case 0xC0000417: return L"an invalid parameter to the C library";
    case 0xE06D7363: return L"a C++ exception nothing caught";
    default:         return L"an error";
    }
}

// The code out of "code=0x...", so the sentence can name it. Zero when the
// line is not one of ours, which is the signal to report it as it stands
// rather than dressing up something unrecognised.
unsigned long CrashCodeFrom(const std::wstring& note)
{
    const size_t at = note.find(L"code=0x");
    if (at == std::wstring::npos) return 0;
    return wcstoul(note.c_str() + at + 7, nullptr, 16);
}

}  // namespace

// ---------------------------------------------------------------------------
namespace Health {

std::wstring FeedName()
{
    return std::wstring(UpdateFeedHost()) + L"/" + UpdateFeedRepo();
}

std::wstring InstallerErrorPath()
{
    Shared& s = S();
    std::lock_guard<std::mutex> g(s.lock);
    Load(s);
    return s.dir + L"\\app_health_installer_error.txt";
}

void SetEnabled(bool on)
{
    g_enabled.store(on && HaveKey());
    if (g_enabled.load()) { Nudge(); return; }

    // Off means off: what was waiting to be sent is thrown away rather than
    // kept in case the setting comes back on.
    Shared& s = S();
    std::lock_guard<std::mutex> g(s.lock);
    Load(s);
    if (s.st.queue.empty()) return;
    s.st.queue.clear();
    Save(s);
}

void OnStartup()
{
    if (!g_enabled.load())
    {
        // Nothing is reported and nothing is remembered. The notes the last run
        // left are still cleared away rather than left lying in the folder.
        TakeInstallerError();
        TakeCrashNote(StateDir());
        return;
    }

    Shared& s = S();
    State was;
    bool  newDay = false;
    std::wstring stateDir;
    {
        std::lock_guard<std::mutex> g(s.lock);
        Load(s);
        was = s.st;
        stateDir = s.dir;

        // The marker is spent whether or not anything comes of it, so a run
        // that goes wrong here cannot leave one behind to be misread later.
        s.st.pending = false;
        s.st.fromVersion.clear(); s.st.fromCommit.clear();
        s.st.toVersion.clear();   s.st.toCommit.clear();
        s.st.startedAt = 0;

        s.st.lastVersion = RunningVersion();
        s.st.lastCommit  = RunningCommit();

        const std::wstring day = Today();
        newDay = (s.st.startedDay != day);
        if (newDay) s.st.startedDay = day;
        Save(s);
    }

    const std::wstring installerError = Depersonalise(TakeInstallerError());

    // What the last run died of, if it died. First, because a crash is the
    // most important thing a run can have to say, and because a crash in the
    // middle of an update is a pair of reports that only make sense together.
    const std::wstring crash = TakeCrashNote(stateDir);
    if (!crash.empty())
    {
        const unsigned long code = CrashCodeFrom(crash);
        Json o = Base(L"crash", was.installId);
        o.Set(L"stage", Json::Str(L"runtime"));
        o.Set(L"error_class", Json::Str(L"app"));
        o.Set(L"error", Json::Str(code
            ? FormatW(L"FastRoute stopped with %s. %s", CrashWord(code),
                      crash.c_str())
            : L"FastRoute stopped unexpectedly: " + crash));
        AddDetail(o, L"crash", Clip(crash, 300));
        Queue(std::move(o));
    }

    if (was.pending)
    {
        const long long waited = NowSeconds() - was.startedAt;
        if (LooksInstalled(was))
        {
            const bool byItself = (waited >= 0 && waited <= kRelaunchGraceSeconds);
            Json o = Base(L"update_succeeded", was.installId);
            o.Set(L"from_version", Json::Str(was.fromVersion));
            o.Set(L"to_version", Json::Str(RunningVersion()));
            o.Set(L"stage", Json::Str(L"relaunch"));
            AddDetail(o, L"relaunched_automatically", byItself ? L"true" : L"false");
            Queue(std::move(o));

            if (!byItself)
            {
                Json n = Base(L"update_not_relaunched", was.installId);
                n.Set(L"from_version", Json::Str(was.fromVersion));
                n.Set(L"to_version", Json::Str(RunningVersion()));
                n.Set(L"stage", Json::Str(L"relaunch"));
                n.Set(L"error_class", Json::Str(L"app"));
                n.Set(L"error", Json::Str(FormatW(
                    L"The files were replaced, but FastRoute only started "
                    L"again %lld minutes later rather than by itself.",
                    waited / 60)));
                AddDetail(n);
                Queue(std::move(n));
            }
        }
        else
        {
            Json o = Base(L"update_install_failed", was.installId);
            o.Set(L"from_version", Json::Str(was.fromVersion));
            o.Set(L"to_version", Json::Str(was.toVersion));
            o.Set(L"stage", Json::Str(L"install"));
            o.Set(L"error_class", Json::Str(L"app"));
            o.Set(L"error", Json::Str(installerError.empty()
                ? FormatW(L"Still running %s after the update script ran.",
                          RunningVersion().c_str())
                : installerError));
            AddDetail(o);
            Queue(std::move(o));
        }
    }
    else if (!installerError.empty())
    {
        // No marker but the script complained: the marker went with the folder
        // it was in, which is itself worth knowing.
        Json o = Base(L"update_install_failed", was.installId);
        o.Set(L"stage", Json::Str(L"install"));
        o.Set(L"error_class", Json::Str(L"app"));
        o.Set(L"error", Json::Str(installerError));
        AddDetail(o);
        Queue(std::move(o));
    }
    else if (!was.lastCommit.empty() && !RunningCommit().empty() &&
             CommitsDiffer(was.lastCommit, RunningCommit()))
    {
        // A different build with no marker: somebody unpacked it themselves,
        // which is how the first build of this fork has to be installed.
        Json o = Base(L"update_succeeded", was.installId);
        o.Set(L"from_version", Json::Str(was.lastVersion));
        o.Set(L"to_version", Json::Str(RunningVersion()));
        AddDetail(o, L"method", L"by hand");
        Queue(std::move(o));
    }

    if (newDay)
    {
        Json o = Base(L"app_started", was.installId);
        AddDetail(o);
        Queue(std::move(o));
    }
    else
    {
        Nudge();        // anything left over from a run that had no network
    }
}

void UpdateAvailable(const std::wstring& toVersion, const std::wstring& toCommit)
{
    if (!g_enabled.load()) return;

    // Once per build, not once per check. An hourly check against a build
    // somebody keeps saying no to would otherwise report the same thing all
    // day, and the one fact worth having - that it was offered at all - would
    // be buried in it.
    Shared& s = S();
    std::wstring installId;
    {
        std::lock_guard<std::mutex> g(s.lock);
        Load(s);
        const std::wstring mark = toCommit.empty() ? toVersion : toCommit;
        if (mark.empty() || s.st.offeredCommit == mark) return;
        s.st.offeredCommit = mark;
        installId = s.st.installId;
        Save(s);
    }

    Json o = Base(L"update_available", installId);
    o.Set(L"from_version", Json::Str(RunningVersion()));
    o.Set(L"to_version", Json::Str(Clip(toVersion, 40)));
    o.Set(L"stage", Json::Str(L"check"));
    AddDetail(o, L"to_commit", Clip(toCommit, 40));
    Queue(std::move(o));
}

void UpdateStarted(const std::wstring& toVersion, const std::wstring& toCommit)
{
    if (!g_enabled.load()) return;

    Shared& s = S();
    std::wstring body;
    {
        std::lock_guard<std::mutex> g(s.lock);
        Load(s);
        s.st.pending     = true;
        s.st.fromVersion = RunningVersion();
        s.st.fromCommit  = RunningCommit();
        s.st.toVersion   = Clip(toVersion, 40);
        s.st.toCommit    = Clip(toCommit, 40);
        s.st.startedAt   = NowSeconds();
        Save(s);

        Json o = Base(L"update_started", s.st.installId);
        o.Set(L"from_version", Json::Str(s.st.fromVersion));
        o.Set(L"to_version", Json::Str(s.st.toVersion));
        o.Set(L"stage", Json::Str(L"install"));
        AddDetail(o, L"to_commit", s.st.toCommit);
        body = o.Serialise();
    }

    // Sent here and now rather than queued: the process is about to end, and
    // the point of this one is that an update that was begun is known about
    // even if nothing ever reports how it went. If it cannot go, it waits in
    // the queue like any other and the next run sends it.
    if (PostEvent(body) == Posted::Sent) return;
    std::lock_guard<std::mutex> g(s.lock);
    s.st.queue.push_back(body);
    while (s.st.queue.size() > kQueueMax) s.st.queue.erase(s.st.queue.begin());
    Save(s);
}

void UpdateAbandoned(const std::wstring& error)
{
    if (!g_enabled.load()) return;

    Shared& s = S();
    std::wstring toVersion;
    {
        std::lock_guard<std::mutex> g(s.lock);
        Load(s);
        toVersion = s.st.toVersion;
        s.st.pending = false;
        s.st.fromVersion.clear(); s.st.fromCommit.clear();
        s.st.toVersion.clear();   s.st.toCommit.clear();
        s.st.startedAt = 0;
        Save(s);
    }
    UpdateFailed(L"install", L"app", toVersion, error);
}

void UpdateFailed(const wchar_t* stage, const wchar_t* errorClass,
                  const std::wstring& toVersion, const std::wstring& error)
{
    if (!g_enabled.load()) return;

    const wchar_t* event = L"update_install_failed";
    if (wcscmp(stage, L"check") == 0)         event = L"update_check_failed";
    else if (wcscmp(stage, L"download") == 0 ||
             wcscmp(stage, L"verify") == 0)   event = L"update_download_failed";

    Json o = Base(event, InstallId());
    o.Set(L"from_version", Json::Str(RunningVersion()));
    if (!toVersion.empty()) o.Set(L"to_version", Json::Str(Clip(toVersion, 40)));
    o.Set(L"stage", Json::Str(stage));
    o.Set(L"error_class", Json::Str(errorClass));
    o.Set(L"error", Json::Str(Depersonalise(error)));
    AddDetail(o);
    Queue(std::move(o));
}

void InstallCrashHandler()
{
    if (!g_enabled.load() || g_crashPath[0]) return;

    // Where the note goes. Worked out now, copied into a plain array now: the
    // filter cannot be handed a std::wstring, whose buffer lives on the heap
    // that may be what broke.
    const std::wstring path = CrashNotePath(StateDir());
    if (path.empty() || path.size() + 1 > MAX_PATH) return;
    wcscpy_s(g_crashPath, MAX_PATH, path.c_str());

    // Where our own code is, so that the filter can turn an address into an
    // offset with two comparisons and a subtraction.
    const HMODULE self = GetModuleHandleW(nullptr);
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)self;
    if (dos && dos->e_magic == IMAGE_DOS_SIGNATURE)
    {
        const IMAGE_NT_HEADERS* nt =
            (const IMAGE_NT_HEADERS*)((const char*)dos + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE)
        {
            g_imageBase = (uintptr_t)self;
            g_imageEnd  = g_imageBase + nt->OptionalHeader.SizeOfImage;
        }
    }

    // Which build it was, as ASCII, because that is all the filter can write.
    // The version and the commit are both made of characters that mean the
    // same thing in every code page, so this is not a guess.
    const std::wstring stamp = L"version=" + RunningVersion() +
                               L" commit=" + Clip(RunningCommit(), 12);
    WideCharToMultiByte(CP_ACP, 0, stamp.c_str(), -1, g_crashPrefix,
                        (int)sizeof(g_crashPrefix) - 1, nullptr, nullptr);

    g_previousFilter = SetUnhandledExceptionFilter(CrashFilter);
}

void ReportError(const wchar_t* where, const std::wstring& error)
{
    if (!g_enabled.load()) return;

    // Said once each, and not many times in one run. Both guards are about the
    // same thing: a fault that repeats says no more the fourth time than the
    // first, and a run that spent its whole report budget on one repeating
    // error would have nothing left for the next one, which might be the
    // interesting one.
    static std::mutex seenLock;
    static std::vector<std::wstring> seen;
    {
        std::lock_guard<std::mutex> g(seenLock);
        if ((int)seen.size() >= kMaxFaultsPerRun) return;
        for (const std::wstring& had : seen)
            if (had == error) return;
        seen.push_back(error);
    }

    Json o = Base(L"error", InstallId());
    o.Set(L"stage", Json::Str(L"runtime"));
    o.Set(L"error_class", Json::Str(L"app"));
    o.Set(L"error", Json::Str(Depersonalise(error)));
    AddDetail(o, L"where", where ? where : L"");
    Queue(std::move(o));
}

bool CanReport()
{
    return HaveKey();
}

std::wstring DiagnosticsText()
{
    std::wstring out = L"FastRoute " + RunningVersion();
    const std::wstring commit = RunningCommit();
    if (!commit.empty()) out += L" (" + Clip(commit, 12) + L")";
    out += L"\r\n" + WindowsVersionText() + L", " + ArchName();
    out += L"\r\nUpdates from " + FeedName();
    out += g_enabled.load() ? L"\r\nAutomatic reports are on."
                            : L"\r\nAutomatic reports are off.";
    out += L"\r\n\r\n";

    const std::vector<LogEntry> entries = ErrorLogEntries();
    if (entries.empty())
    {
        out += L"No errors have been reported since FastRoute started.";
    }
    else
    {
        for (const LogEntry& e : entries)
            out += e.when + L"  " + e.text + L"\r\n";
    }
    return Scrub(out);
}

bool SubmitDiagnostics(const std::wstring& report, const std::wstring& note,
                       std::wstring* reference, std::wstring* error)
{
    const wchar_t* kNoWay = L"FastRoute could not reach our server. Check the "
                            L"connection and try again in a moment.";
    if (reference) reference->clear();
    if (!HaveKey())
    {
        if (error) *error = L"This build of FastRoute has no server to send to.";
        return false;
    }

    // The install number, if this copy already has one. A copy whose automatic
    // reports are off has not been given one, and must not be given one by
    // pressing this button either - so it gets a number for this one message
    // that is written down nowhere and never used again. The report still goes
    // in; it simply cannot be tied to anything else.
    std::wstring installId = InstallId();
    if (installId.empty()) installId = NewInstallId();
    if (installId.empty())
    {
        if (error) *error = kNoWay;
        return false;
    }

    Json o = Base(L"diagnostics", installId);
    o.Set(L"log", Json::Str(report));
    if (!note.empty()) o.Set(L"note", Json::Str(Clip(note, 2000)));
    AddDetail(o);

    std::wstring reply;
    DWORD status = 0;
    const Posted sent = Post(kDiagPath, o.Serialise(), kDiagTimeoutMs, &reply,
                             &status);
    if (sent != Posted::Sent)
    {
        if (error)
        {
            if (status == 429)
                *error = L"Our server has had as many logs from this copy as it "
                         L"takes in a day. Try again tomorrow, or tell us what "
                         L"happened in your own words.";
            else if (status == 413)
                *error = L"The log was too large for our server to take.";
            else if (status == 400 || status == 403)
                *error = L"Our server would not take the log. This is ours to "
                         L"fix, not yours.";
            else
                *error = kNoWay;
        }
        return false;
    }

    Json doc;
    if (Json::Parse(reply, doc) && doc.IsObject())
    {
        const std::wstring ref = doc.AsString(L"reference");
        if (!ref.empty() && reference) *reference = Clip(ref, 60);
    }
    // A reply we could not read is not a failure: the log is on our server
    // either way, and saying so without a reference beats saying it did not
    // work when it did.
    return true;
}

void Shutdown()
{
    g_stopping.store(true);
    Shared& s = S();
    std::lock_guard<std::mutex> g(s.senderLock);
    if (!s.sender.joinable()) return;
    // Two seconds for whatever is in flight, and then it is left to finish on
    // its own: a report is not worth holding the program open for. Nothing it
    // touches is ever destroyed, so letting go of it here is safe.
    for (int i = 0; i < 20 && !g_senderIdle.load(); ++i) Sleep(100);
    if (g_senderIdle.load()) s.sender.join();
    else                     s.sender.detach();
}

}  // namespace Health
