#include "updater.h"
#include "version.h"
#include "json.h"
#include "util.h"
#include "health.h"

#include <winhttp.h>
#include <shellapi.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Where the builds are
// ---------------------------------------------------------------------------
namespace {

const wchar_t* kHost = L"api.github.com";
const wchar_t* kRepo = L"Raywonder/FastRoute";
const wchar_t* kPath = L"/repos/Raywonder/FastRoute/releases/tags/latest";
// GitHub refuses a request without one, so this is not decoration.
const wchar_t* kAgent = L"FastRoute-Updater";

std::wstring TempFile(const wchar_t* name)
{
    wchar_t dir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + name;
}

std::wstring UpdateZipPath() { return TempFile(L"FastRoute-update.zip"); }

// ExeDir is util.h's; only the file name is wanted here as well.
std::wstring ExeName()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    const size_t slash = s.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? s : s.substr(slash + 1);
}

// Which of app-health's classes a refused request belongs to. The distinction
// matters: a problem at the user's end - offline, a captive portal, a proxy
// that wants a password - is counted and never alerted on, while one at ours
// is something we have to go and fix.
const wchar_t* ClassifyStatus(DWORD status)
{
    switch (status)
    {
    // A portal or a proxy standing in the way. None of these can come from
    // GitHub's release API, which is asked for without any credentials.
    case 401: case 407: case 511: return L"network";
    default: return L"server";
    }
}

// --- a blocking HTTPS GET --------------------------------------------------
//
// Small and on its own rather than borrowing the stream block's HttpSource:
// that one is built to keep a connection open and hand back audio forever,
// and this wants a whole file and then to stop.
struct Http
{
    HINTERNET session = nullptr, conn = nullptr, req = nullptr;

    ~Http()
    {
        if (req) WinHttpCloseHandle(req);
        if (conn) WinHttpCloseHandle(conn);
        if (session) WinHttpCloseHandle(session);
    }

    bool Open(const std::wstring& host, const std::wstring& path,
              bool json, UpdateFault* fault)
    {
        UpdateFault scratch;
        UpdateFault& f = fault ? *fault : scratch;

        session = WinHttpOpen(kAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)
        {
            f.message = L"Could not start a network session.";
            f.cls = L"app";
            return false;
        }
        // Long enough for a slow link, short enough that a dead one does not
        // leave a thread sitting there for the rest of the session.
        WinHttpSetTimeouts(session, 15000, 15000, 15000, 30000);

        conn = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!conn)
        {
            f.message = FormatW(L"Could not reach %s.", host.c_str());
            // Nothing resolved or nothing answered: the machine's end of it.
            f.cls = L"network";
            return false;
        }

        req = WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr,
                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE);
        if (!req)
        {
            f.message = L"Could not form the request.";
            f.cls = L"app";
            return false;
        }
        if (json)
            WinHttpAddRequestHeaders(req, L"Accept: application/vnd.github+json",
                                     (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(req, nullptr))
        {
            f.message = FormatW(L"%s did not answer.", host.c_str());
            // A timeout, a dropped connection, a certificate a proxy signed
            // itself: all of them the user's side of the wire.
            f.cls = L"network";
            return false;
        }

        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX);
        if (status != 200)
        {
            f.message = (status == 404)
                ? std::wstring(L"There is no build published yet.")
                : FormatW(L"The server answered %u.", status);
            f.cls = ClassifyStatus(status);
            f.notPublished = (status == 404);
            return false;
        }
        return true;
    }

    unsigned long long Length() const
    {
        DWORD len = 0, size = sizeof(len);
        if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH |
                                 WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &len, &size,
                                 WINHTTP_NO_HEADER_INDEX))
            return 0;
        return len;
    }

    // Returns false only on a read error; running out of data is the end.
    bool Read(std::vector<char>& chunk, DWORD* got)
    {
        *got = 0;
        return WinHttpReadData(req, chunk.data(), (DWORD)chunk.size(), got) != FALSE;
    }
};

// --- reading the release ---------------------------------------------------

// Pulls "**Version:** 0.6" and "**Commit:** abc123" out of the release body.
// The continuous build writes both, in those shapes, for exactly this.
}  // namespace

std::wstring ReleaseField(const std::wstring& body, const wchar_t* label)
{
    const std::wstring tag = std::wstring(L"**") + label + L":**";
    size_t at = body.find(tag);
    if (at == std::wstring::npos) return std::wstring();
    at += tag.size();
    while (at < body.size() && (body[at] == L' ' || body[at] == L'\t')) ++at;
    size_t end = at;
    while (end < body.size() && body[end] != L'\r' && body[end] != L'\n' &&
           body[end] != L' ')
        ++end;
    return body.substr(at, end - at);
}

// "0.6" against "0.10" the way a person means it, rather than the way strcmp
// does - which would have 0.10 as older than 0.6.
int CompareVersions(const std::wstring& a, const std::wstring& b)
{
    size_t i = 0, j = 0;
    for (;;)
    {
        long x = 0, y = 0;
        bool hasX = false, hasY = false;
        while (i < a.size() && a[i] >= L'0' && a[i] <= L'9')
        { x = x * 10 + (a[i++] - L'0'); hasX = true; }
        while (j < b.size() && b[j] >= L'0' && b[j] <= L'9')
        { y = y * 10 + (b[j++] - L'0'); hasY = true; }

        if (!hasX && !hasY) return 0;
        if (x != y) return x < y ? -1 : 1;

        while (i < a.size() && (a[i] < L'0' || a[i] > L'9')) ++i;
        while (j < b.size() && (b[j] < L'0' || b[j] > L'9')) ++j;
        if (i >= a.size() && j >= b.size()) return 0;
    }
}

const wchar_t* UpdateFeedHost() { return kHost; }
const wchar_t* UpdateFeedRepo() { return kRepo; }

std::wstring RunningVersion() { return FASTROUTE_VERSION_W; }

std::wstring RunningCommit()
{
    const std::string c = FASTROUTE_BUILD_COMMIT;
    return std::wstring(c.begin(), c.end());
}

// ---------------------------------------------------------------------------
// Asking what is out there
// ---------------------------------------------------------------------------
UpdateInfo CheckForUpdates()
{
    UpdateInfo info;

    Http http;
    if (!http.Open(kHost, kPath, true, &info.fault)) return info;

    std::string body;
    std::vector<char> chunk(16 * 1024);
    for (;;)
    {
        DWORD got = 0;
        if (!http.Read(chunk, &got)) break;
        if (got == 0) break;
        body.append(chunk.data(), got);
        // A release document is a few kilobytes. Far past that means
        // something other than what was asked for.
        if (body.size() > 4u * 1024u * 1024u) break;
    }
    if (body.empty())
    {
        info.fault.message = L"The server sent nothing back.";
        info.fault.cls = L"server";
        return info;
    }

    const int need = MultiByteToWideChar(CP_UTF8, 0, body.data(), (int)body.size(),
                                         nullptr, 0);
    std::wstring text((size_t)(need > 0 ? need : 0), L'\0');
    if (need > 0)
        MultiByteToWideChar(CP_UTF8, 0, body.data(), (int)body.size(), &text[0], need);

    Json doc;
    if (!Json::Parse(text, doc))
    {
        info.fault.message = L"The server's answer could not be read.";
        // GitHub's release API does not answer 200 with something that is not
        // JSON. A sign-in page from a hotel network does, so this counts as
        // the user's side rather than ours.
        info.fault.cls = L"network";
        return info;
    }

    const std::wstring notes = doc.AsString(L"body");
    info.notes = notes;
    info.version = ReleaseField(notes, L"Version");
    info.commit = ReleaseField(notes, L"Commit");

    // The portable zip, which is the only asset there is. Chosen by shape
    // rather than by position so that adding another one later cannot make
    // this pick up the wrong thing.
    const Json* assets = doc.Find(L"assets");
    if (assets && assets->IsArray())
    {
        for (const Json& a : assets->Items())
        {
            const std::wstring name = a.AsString(L"name");
            if (name.size() < 4) continue;
            if (name.substr(name.size() - 4) != L".zip") continue;
            if (name.find(L"macOS") != std::wstring::npos) continue;
            info.zipUrl = a.AsString(L"browser_download_url");
            break;
        }
    }
    if (info.zipUrl.empty())
    {
        info.fault.message = L"That build has nothing in it to download.";
        info.fault.cls = L"server";
        return info;
    }

    // Which is newer. A commit is exact, and between releases it is the only
    // thing that distinguishes two builds of the same version - so it is used
    // when both ends have one, and the version is the fallback.
    const std::wstring mine = RunningCommit();
    if (!mine.empty() && !info.commit.empty())
    {
        // The release carries the full hash and the build a short one, so the
        // comparison is by prefix, in whichever direction is shorter.
        const size_t n = mine.size() < info.commit.size() ? mine.size()
                                                          : info.commit.size();
        info.available = (_wcsnicmp(mine.c_str(), info.commit.c_str(), n) != 0);
    }
    else if (!info.version.empty())
    {
        info.available = CompareVersions(RunningVersion(), info.version) < 0;
    }
    return info;
}

// ---------------------------------------------------------------------------
// Fetching it
// ---------------------------------------------------------------------------
bool DownloadUpdate(const std::wstring& url, UpdateProgressFn progress,
                    UpdateFault* fault)
{
    UpdateFault scratch;
    UpdateFault& f = fault ? *fault : scratch;

    // The asset lives on a different host from the API, and the URL redirects
    // again from there, so the host and path are taken from the URL rather
    // than assumed. WinHttpCrackUrl does the splitting.
    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {}, path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 255;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &parts))
    {
        f.message = L"That download address could not be read.";
        f.cls = L"app";
        return false;
    }

    Http http;
    if (!http.Open(host, path, false, &f)) return false;

    const std::wstring to = UpdateZipPath();
    HANDLE file = CreateFileW(to.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        f.message = FormatW(L"%s could not be written.", to.c_str());
        f.cls = L"disk";
        return false;
    }

    const unsigned long long total = http.Length();
    unsigned long long done = 0;
    std::vector<char> chunk(64 * 1024);
    bool ok = true;

    for (;;)
    {
        DWORD got = 0;
        if (!http.Read(chunk, &got))
        {
            f.message = L"The download did not finish.";
            f.cls = L"network";
            ok = false;
            break;
        }
        if (got == 0) break;

        DWORD written = 0;
        if (!WriteFile(file, chunk.data(), got, &written, nullptr) ||
            written != got)
        {
            f.message = L"The download could not be written to disk.";
            f.cls = L"disk";
            ok = false;
            break;
        }
        done += got;
        if (progress && !progress(done, total))
        {
            // Asked to stop. Not a failure, but there is nothing usable.
            f.message = L"The download was stopped.";
            f.cls = L"user_cancelled";
            ok = false;
            break;
        }
    }

    CloseHandle(file);
    if (!ok)
    {
        DeleteFileW(to.c_str());
        if (f.message.empty()) f.message = L"The download did not finish.";
        return false;
    }
    if (total > 0 && done != total)
    {
        DeleteFileW(to.c_str());
        f.message = L"The download was cut short.";
        f.cls = L"network";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Putting it in place
//
// The exe cannot overwrite itself while it is running, so a batch file does it
// once FastRoute has gone: wait, unpack, copy over, tidy up, start the new one
// and delete itself. Nothing in the zip is a settings file, so sessions.json
// and the rest are left where they are.
//
// The script runs after this process has gone, so there is nobody for it to
// put a message box in front of. What it writes instead, when it cannot do its
// job, is a line in app_health_installer_error.txt, which the next run reads,
// reports and deletes. It starts FastRoute again either way: a copy that comes
// back and says what went wrong beats one that never comes back at all.
// ---------------------------------------------------------------------------
void ApplyUpdate(const UpdateInfo& info)
{
    const std::wstring zip = UpdateZipPath();
    if (GetFileAttributesW(zip.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        const std::wstring why = L"The downloaded update has gone missing, so "
                                 L"nothing has been changed.";
        Health::UpdateFailed(L"install", L"app", info.version, why);
        ErrorBox(g_mainDlg, why);
        return;
    }

    const std::wstring dir = ExeDir();
    const std::wstring bat = dir + L"\\update.bat";
    const std::wstring tmp = dir + L"\\update_temp";
    const std::wstring note = Health::InstallerErrorPath();

    HANDLE f = CreateFileW(bat.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        const std::wstring why =
            FormatW(L"%s could not be written, so the update has not been "
                    L"applied.", bat.c_str());
        Health::UpdateFailed(L"install", L"permission", info.version, why);
        ErrorBox(g_mainDlg, why);
        return;
    }

    // Written as UTF-8 without a marker: cmd reads a batch file as bytes, and
    // a byte order mark at the top of one is echoed as rubbish.
    //
    // Labels rather than nested parentheses: an if block with a redirection
    // inside it is one of the places batch parsing gives way, and this script
    // only gets one attempt, with nobody there to watch it.
    //
    // -ErrorAction Stop so that a failure to unpack is a terminating one and
    // PowerShell leaves with a code to test. Without it, Expand-Archive can
    // complain and still exit zero, and the copy that followed would be of an
    // empty folder - which xcopy does catch, but one step later and as the
    // wrong problem.
    const std::wstring script = FormatW(
        L"@echo off\r\n"
        L"timeout /t 2 /nobreak > nul\r\n"
        L"powershell -NoProfile -Command \"Expand-Archive -LiteralPath '%s' "
        L"-DestinationPath '%s' -Force -ErrorAction Stop\"\r\n"
        L"if errorlevel 1 goto unpackfailed\r\n"
        L"xcopy /s /y /q \"%s\\*\" \"%s\\\"\r\n"
        L"if errorlevel 1 goto copyfailed\r\n"
        L"goto tidy\r\n"
        L":unpackfailed\r\n"
        L">\"%s\" echo The downloaded update could not be unpacked.\r\n"
        L"goto tidy\r\n"
        L":copyfailed\r\n"
        L">\"%s\" echo The new files could not be copied into place.\r\n"
        L":tidy\r\n"
        L"rmdir /s /q \"%s\"\r\n"
        L"del \"%s\"\r\n"
        L"start \"\" \"%s\\%s\"\r\n"
        L"del \"%%~f0\"\r\n",
        zip.c_str(), tmp.c_str(),
        tmp.c_str(), dir.c_str(),
        note.c_str(),
        note.c_str(),
        tmp.c_str(),
        zip.c_str(),
        dir.c_str(), ExeName().c_str());

    const int bytes = WideCharToMultiByte(CP_UTF8, 0, script.c_str(), -1,
                                          nullptr, 0, nullptr, nullptr);
    std::vector<char> utf8((size_t)(bytes > 0 ? bytes : 1), 0);
    if (bytes > 0)
        WideCharToMultiByte(CP_UTF8, 0, script.c_str(), -1, utf8.data(), bytes,
                            nullptr, nullptr);
    DWORD written = 0;
    // Less one, so the terminating zero does not go into the file.
    WriteFile(f, utf8.data(), (DWORD)(utf8.size() - 1), &written, nullptr);
    CloseHandle(f);

    // Before the script rather than after, and the one report that is sent
    // where it is made rather than queued. Two reasons, both about timing: the
    // script gives this process two seconds to close before it replaces the
    // exe, so nothing slow may happen between starting it and asking to close;
    // and an update that is begun and then never heard of again is the failure
    // most worth knowing about, so it cannot wait for a run that may not come.
    Health::UpdateStarted(info.version, info.commit);

    const HINSTANCE r = ShellExecuteW(nullptr, L"open", bat.c_str(), nullptr,
                                      dir.c_str(), SW_HIDE);
    if (reinterpret_cast<intptr_t>(r) <= 32)
    {
        const std::wstring why = L"The update script could not be started, so "
                                 L"nothing has been changed.";
        // Takes the marker back as well: nothing was handed over, so the next
        // run must not read one and report an install that never began.
        Health::UpdateAbandoned(why);
        ErrorBox(g_mainDlg, why);
        return;
    }

    // And now go, so that the exe can be replaced.
    PostMessageW(g_mainDlg, WM_CLOSE, 0, 0);
}

// ---------------------------------------------------------------------------
// Asking, telling, and doing it on a timer
//
// All of this happens on threads of its own. The check and the download both
// block, and a message box put up from a worker thread is modal only to that
// thread - so an hourly check that finds something does not freeze the window
// somebody is working in while it waits to be answered.
// ---------------------------------------------------------------------------
namespace {

// One check at a time. Two overlapping ones would download the same build
// twice and then race to unpack it.
std::atomic<bool> g_busy{ false };

struct Scheduler
{
    std::mutex lock;
    std::condition_variable wake;
    std::thread thread;
    bool stop = false;
    HWND owner = nullptr;
    // Bumped whenever the plan changes - a failed check that wants trying
    // again sooner than the interval. A thread already asleep tells the nudge
    // apart from the wait running out by this having moved, so being woken to
    // re-think does not run a check that was not due.
    unsigned replan = 0;
};
Scheduler g_sched;

// --- a check that failed is tried again before it is believed --------------
//
// How long to wait before each further attempt. A check can fail for reasons
// that pass on their own: a laptop that has just woken and has no network yet,
// a hotel page that has not been agreed to, and - every single time we publish
// - the few seconds in which the build job has deleted the "latest" release
// and not yet put the new one up. None of that is worth telling anybody about,
// and none of it is worth reporting as a fault.
//
// So a silent check that fails says nothing and comes back: five minutes, then
// half an hour, then two hours. Only when every one of those has failed too -
// something over two and a half hours of a feed that is not there - is it
// reported to app-health as a real failure.
const int kRetryMinutes[] = { 5, 30, 120 };
const int kRetryCount = (int)(sizeof(kRetryMinutes) / sizeof(kRetryMinutes[0]));

// How many quiet retries have been made already. -1 is "the last check was
// fine", which is also where it goes back to once a failure has been reported.
std::atomic<int> g_retryStep{ -1 };

// Wakes the scheduler so that it works its next wait out again rather than
// sleeping out an interval that no longer applies.
void Replan()
{
    {
        std::lock_guard<std::mutex> g(g_sched.lock);
        ++g_sched.replan;
    }
    g_sched.wake.notify_all();
}

// There is no progress window. The zip is about a megabyte, so one would be on
// screen for less time than it takes to read it; what matters more is that
// nothing is put in front of somebody who did not ask for it.
bool DownloadAndApply(HWND parent, const UpdateInfo& info, bool silent)
{
    UpdateFault fault;
    if (!DownloadUpdate(info.zipUrl, nullptr, &fault))
    {
        if (fault.message.empty())
            fault.message = L"The update could not be downloaded.";

        if (!silent)
            ErrorBox(parent, fault.message);
        else
            LogError(FormatW(L"An update to %s could not be downloaded: %s",
                             info.version.c_str(), fault.message.c_str()));

        // The same publishing window as a failed check: between reading the
        // release and fetching its zip, the build job can have taken both
        // away. Reported only once the retries have run out, from the check
        // that fails with them.
        if (fault.notPublished)
        {
            if (silent) { g_retryStep.store(0); Replan(); }
        }
        else
        {
            Health::UpdateFailed(L"download", fault.cls, info.version,
                                 fault.message);
        }
        return false;
    }
    ApplyUpdate(info);
    return true;
}

void RunCheck(HWND parent, bool silent)
{
    const UpdateInfo info = CheckForUpdates();

    if (!info.fault.message.empty())
    {
        // A failed check is not worth a box when nobody asked: a laptop that
        // was asleep, or a network that is not there yet, is the usual cause.
        if (silent)
        {
            LogError(L"Checking for an update failed: " + info.fault.message);

            const int tried = g_retryStep.load() + 1;
            if (tried < kRetryCount)
            {
                g_retryStep.store(tried);
                Replan();
            }
            else
            {
                g_retryStep.store(-1);
                Health::UpdateFailed(L"check", info.fault.cls, std::wstring(),
                                     info.fault.message);
            }
        }
        else
        {
            ErrorBox(parent, info.fault.message);
            // Somebody asked and has been told. Reported too, so that a feed
            // that has gone is visible to us rather than only to them -
            // except in the seconds around a release, when there is genuinely
            // nothing published and the answer given was the right one.
            if (!info.fault.notPublished)
                Health::UpdateFailed(L"check", info.fault.cls, std::wstring(),
                                     info.fault.message);
        }
        g_busy.store(false);
        return;
    }

    // The feed answered, so whatever was wrong before has passed.
    if (g_retryStep.exchange(-1) != -1) Replan();

    if (!info.available)
    {
        if (!silent)
            MessageBoxW(parent,
                        FormatW(L"FastRoute %s is the latest build.",
                                RunningVersion().c_str()).c_str(),
                        L"No update", MB_OK | MB_ICONINFORMATION);
        g_busy.store(false);
        return;
    }

    // Said once per build found, so that a build which was offered and never
    // installed is distinguishable from one that was never offered - the
    // difference between an updater that is not working and a person who said
    // no.
    Health::UpdateAvailable(info.version, info.commit);

    // Silent means get on with it. The program restarts itself afterwards.
    if (silent)
    {
        DownloadAndApply(parent, info, true);
        g_busy.store(false);
        return;
    }

    std::wstring what = info.version.empty()
        ? std::wstring(L"A newer build")
        : FormatW(L"FastRoute %s", info.version.c_str());
    if (!info.commit.empty() && !info.version.empty())
        what += FormatW(L" (%s)", info.commit.substr(0, 7).c_str());

    const std::wstring ask = FormatW(
        L"%s is available. This copy is %s.\n\n"
        L"Downloading it takes a moment, after which FastRoute closes, "
        L"replaces itself and starts again. Your sessions and settings are "
        L"left alone.\n\nDownload and install it now?",
        what.c_str(), RunningVersion().c_str());

    if (MessageBoxW(parent, ask.c_str(), L"Update available",
                    MB_YESNO | MB_ICONQUESTION) == IDYES)
        DownloadAndApply(parent, info, false);

    g_busy.store(false);
}

void Start(HWND parent, bool silent)
{
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) return;
    std::thread([parent, silent] { RunCheck(parent, silent); }).detach();
}

int IntervalMinutes(int setting)
{
    switch (setting)
    {
    case UC_HOURLY: return 60;
    case UC_DAILY:  return 60 * 24;
    default:        return 0;      // never, or startup only
    }
}

void SchedulerThread()
{
    for (;;)
    {
        int minutes = 0;
        HWND owner = nullptr;
        bool retrying = false;
        {
            std::unique_lock<std::mutex> g(g_sched.lock);
            if (g_sched.stop) return;
            owner = g_sched.owner;

            // A check that failed and has retries left comes first: it is
            // waiting on five minutes, not on the next hour or the next day.
            const int step = g_retryStep.load();
            retrying = (step >= 0 && step < kRetryCount);
            minutes = retrying ? kRetryMinutes[step]
                               : IntervalMinutes(g_store.settings.updateCheck);

            const unsigned planned = g_sched.replan;
            const auto changed = [&] {
                return g_sched.stop || g_sched.replan != planned;
            };
            if (minutes <= 0)
            {
                // Nothing to do, but the thread stays so that turning the
                // setting on does not need it restarted.
                g_sched.wake.wait_for(g, std::chrono::minutes(5), changed);
                continue;
            }
            // True when it was woken rather than when the wait ran out, which
            // is a change of plan to work out again and not a check that is
            // due.
            if (g_sched.wake.wait_for(g, std::chrono::minutes(minutes), changed))
                continue;
            if (g_sched.stop) return;
            // The setting may have changed while this was asleep. A retry is
            // left alone unless checking has been turned off altogether: the
            // setting that asked for the check was on when it was made.
            if (retrying)
            {
                if (g_store.settings.updateCheck == UC_NEVER)
                {
                    g_retryStep.store(-1);
                    continue;
                }
            }
            else if (IntervalMinutes(g_store.settings.updateCheck) <= 0)
            {
                continue;
            }
        }
        Start(owner, g_store.settings.updateSilent);
    }
}

}  // namespace

void ShowCheckForUpdates(HWND parent, bool silent)
{
    Start(parent ? parent : g_mainDlg, silent);
}

void CheckForUpdatesOnStartup(HWND owner)
{
    // Startup covers the periodic settings too: somebody who asked for hourly
    // wants one when the program opens as well, not fifty-nine minutes later.
    if (g_store.settings.updateCheck == UC_NEVER) return;
    Start(owner, g_store.settings.updateSilent);
}

void StartUpdateChecks(HWND owner)
{
    std::lock_guard<std::mutex> g(g_sched.lock);
    g_sched.owner = owner;
    if (g_sched.thread.joinable()) return;
    g_sched.stop = false;
    g_sched.thread = std::thread(SchedulerThread);
}

void StopUpdateChecks()
{
    {
        std::lock_guard<std::mutex> g(g_sched.lock);
        g_sched.stop = true;
    }
    g_sched.wake.notify_all();
    if (g_sched.thread.joinable()) g_sched.thread.join();
}
