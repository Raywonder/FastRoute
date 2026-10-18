#include "health.h"
#include "updater.h"
#include "version.h"
#include "json.h"
#include "util.h"
#include "audio/audio.h"      // WindowsVersionText

#include <windows.h>
#include <winhttp.h>
#include <objbase.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// The collector
// ---------------------------------------------------------------------------
namespace {

const wchar_t* kApp       = L"fastroute";
const wchar_t* kHost      = L"files.tappedin.fm";
const wchar_t* kEventPath = L"/app-health/v1/event";
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
    return FormatW(L"%04u-%02u-%02u", t.wYear, t.wMonth, t.wDay);
}

std::wstring Clip(const std::wstring& s, size_t n)
{
    return s.size() <= n ? s : s.substr(0, n);
}

// Takes the person out of an error message. The collector redacts as well, but
// a path through somebody's home folder has no business leaving the machine in
// the first place, and the folder name is the one part of these messages that
// is about a person rather than about FastRoute.
std::wstring Depersonalise(const std::wstring& text)
{
    std::wstring out = text;
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
    return Clip(out, 1000);
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

    // Beside the program first, then the per-user folder a read-only copy
    // would have fallen back to.
    const std::wstring places[2] = { ConfigDir(), AppDataDir() };
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

Posted PostEvent(const std::wstring& body)
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
    WinHttpSetTimeouts(session, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);

    HINTERNET conn = WinHttpConnect(session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"POST", kEventPath, nullptr,
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
            DWORD status = 0, size = sizeof(status);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE |
                                WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                WINHTTP_NO_HEADER_INDEX);
            if (status >= 200 && status < 300)      result = Posted::Sent;
            else if (status == 400 || status == 403 || status == 413)
                                                    result = Posted::Refused;
            // Anything else - 429 from the rate limit, 502 while the collector
            // restarts - is worth trying again later.
        }
        WinHttpCloseHandle(req);
    }
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return result;
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
    // The script writes it with whatever cmd's code page is, so plain text
    // first and UTF-8 only if that is what it turns out to be.
    if (_wfopen_s(&f, path.c_str(), L"rt") != 0 || !f) return std::wstring();
    std::wstring text;
    wchar_t buf[1024];
    while (fgetws(buf, 1024, f)) text += buf;
    fclose(f);
    DeleteFileW(path.c_str());
    return Trim(text);
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
        // Nothing is reported and nothing is remembered. The script's note is
        // still cleared away rather than left lying in the folder.
        TakeInstallerError();
        return;
    }

    Shared& s = S();
    State was;
    bool  newDay = false;
    {
        std::lock_guard<std::mutex> g(s.lock);
        Load(s);
        was = s.st;

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
