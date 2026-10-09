#include "audio.h"
#include "util.h"

#include <tlhelp32.h>
#include <mutex>
#include <algorithm>

// ---------------------------------------------------------------------------
// The foreground application
//
// An application block can be pointed at whatever is in front rather than at a
// process by name. That sounds like one call to GetForegroundWindow, and it is
// not, for two reasons.
//
// The first is that by the time a session starts, FastRoute is very often what
// is in front: the user pressed Ctrl+R in the FastRoute window. Sampling then
// gives the wrong answer every time. So the foreground is *tracked* as it
// changes, and what a block asks for is the last application that was in front
// and could actually be captured - which is exactly what someone means when
// they set this up, hide FastRoute, and start the session from a hotkey.
//
// The second is that a good deal of what comes to the front is not an
// application at all. The Start menu, the task switcher, the desktop and the
// taskbar all take the foreground, and treating them as the new target would
// mean losing the audio you were listening to every time you pressed a key.
// They are ignored, and the last real application stands.
// ---------------------------------------------------------------------------

namespace {

std::mutex g_lock;
unsigned long g_pid = 0;
std::wstring  g_name;
HWINEVENTHOOK g_hook = nullptr;

// The shell's own surfaces. These come to the front constantly and are never
// what someone means by "the application I am listening to".
bool ShellClass(const std::wstring& cls)
{
    static const wchar_t* kShell[] = {
        L"Shell_TrayWnd",              // the taskbar
        L"Shell_SecondaryTrayWnd",     // and on the other monitors
        L"Progman", L"WorkerW",        // the desktop
        L"NotifyIconOverflowWindow",   // the tray overflow
        L"MultitaskingViewFrame",      // task view
        L"XamlExplorerHostIslandWindow",   // Alt+Tab and task view on Windows 11
        L"ForegroundStaging",
    };
    for (const wchar_t* s : kShell) if (IEquals(cls, s)) return true;
    return false;
}

// Likewise by process, for the shell surfaces that are hosted out of process
// and so have ordinary looking window classes.
bool ShellProcess(const std::wstring& exe)
{
    static const wchar_t* kShell[] = {
        L"StartMenuExperienceHost.exe",
        L"SearchHost.exe", L"SearchApp.exe", L"SearchUI.exe",
        L"ShellExperienceHost.exe",
        L"TextInputHost.exe",
        L"LockApp.exe",
    };
    for (const wchar_t* s : kShell) if (IEquals(exe, s)) return true;
    return false;
}

std::wstring ExeNameOf(unsigned long pid)
{
    if (!pid) return L"";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return L"";

    wchar_t path[MAX_PATH] = { 0 };
    DWORD n = MAX_PATH;
    std::wstring out;
    if (QueryFullProcessImageNameW(h, 0, path, &n) && n)
    {
        out = path;
        size_t slash = out.find_last_of(L'\\');
        if (slash != std::wstring::npos) out = out.substr(slash + 1);
    }
    CloseHandle(h);
    return out;
}

// A packaged application's window belongs to ApplicationFrameHost, not to the
// application: the frame is the shell's, the content is a child window owned
// by the real process. Capturing the frame host would capture nothing.
struct FrameSearch
{
    unsigned long host = 0;
    unsigned long found = 0;
};

BOOL CALLBACK FrameChild(HWND child, LPARAM lp)
{
    FrameSearch* s = reinterpret_cast<FrameSearch*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(child, &pid);
    if (pid && pid != s->host)
    {
        s->found = pid;
        return FALSE;
    }
    return TRUE;
}

// The process an application block should capture for `hwnd`, with its exe
// name, or false when that window is not an application this can capture.
bool TargetFor(HWND hwnd, unsigned long* pidOut, std::wstring* nameOut)
{
    if (!hwnd || hwnd == GetShellWindow()) return false;

    wchar_t cls[128] = { 0 };
    GetClassNameW(hwnd, cls, 128);

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return false;

    std::wstring name = ExeNameOf(pid);
    if (name.empty()) return false;

    if (IEquals(name, L"ApplicationFrameHost.exe"))
    {
        FrameSearch s;
        s.host = pid;
        EnumChildWindows(hwnd, FrameChild, reinterpret_cast<LPARAM>(&s));
        if (!s.found) return false;
        pid = s.found;
        name = ExeNameOf(pid);
        if (name.empty()) return false;
    }

    if (!ForegroundCandidate(cls, name)) return false;
    // Never FastRoute, and never anything above it: a tree capture would take
    // this process with it and the session would capture its own output.
    if (WouldCaptureSelf(pid)) return false;

    *pidOut = pid;
    *nameOut = name;
    return true;
}

void CALLBACK HookProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                       LONG idObject, LONG idChild, DWORD, DWORD)
{
    if (event != EVENT_SYSTEM_FOREGROUND) return;
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    ForegroundNote(hwnd);
}

}   // namespace

bool ForegroundCandidate(const std::wstring& windowClass, const std::wstring& exeName)
{
    if (exeName.empty()) return false;
    if (ShellClass(windowClass)) return false;
    if (ShellProcess(exeName)) return false;
    return true;
}

void ForegroundNote(HWND hwnd)
{
    unsigned long pid = 0;
    std::wstring name;
    if (!TargetFor(hwnd, &pid, &name)) return;   // not an application; keep the last

    std::lock_guard<std::mutex> g(g_lock);
    g_pid = pid;
    g_name = name;
}

void ForegroundSample()
{
    ForegroundNote(GetForegroundWindow());
}

void ForegroundWatchStart()
{
    if (g_hook) return;
    // Out of context, so the callback arrives on this thread's message queue
    // rather than in every application's address space. Our own process is
    // skipped, which is the same thing TargetFor would do and cheaper.
    g_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                             nullptr, HookProc, 0, 0,
                             WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    // Whatever is in front right now counts too, so a session started before
    // the first switch still has something to work with.
    ForegroundSample();
}

void ForegroundWatchStop()
{
    if (!g_hook) return;
    UnhookWinEvent(g_hook);
    g_hook = nullptr;
}

bool ForegroundApp(unsigned long* pid, std::wstring* name)
{
    bool empty;
    {
        std::lock_guard<std::mutex> g(g_lock);
        empty = (g_pid == 0);
    }
    // Nothing recorded means the hook could not be installed, so take a
    // reading now. It is better than nothing: it fails only when FastRoute
    // itself is in front, which is the case the tracking exists for.
    if (empty) ForegroundSample();

    std::lock_guard<std::mutex> g(g_lock);
    if (!g_pid) return false;

    // A remembered process that has since exited is no target at all.
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)g_pid);
    if (!h)
    {
        g_pid = 0;
        g_name.clear();
        return false;
    }
    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    if (!alive)
    {
        g_pid = 0;
        g_name.clear();
        return false;
    }

    if (pid) *pid = g_pid;
    if (name) *name = g_name;
    return true;
}

// ---------------------------------------------------------------------------
// An application block pointed at the foreground rather than at a name.
//
// It owns an ordinary process loopback capture and swaps it for another one
// when the target changes. Read() forwards straight through, so there is no
// second buffer and no latency beyond what the inner capture already has.
//
// Opening a capture takes milliseconds, so it is always done on the watcher
// thread with no lock held; the lock is taken only to exchange the pointer.
// Read() runs on the audio clock and must never wait for a device to open.
// ---------------------------------------------------------------------------
namespace {

class ForegroundCapture : public CaptureSource
{
public:
    ForegroundCapture(bool follow, int rate)
        : CaptureSource(rate), m_follow(follow) {}
    ~ForegroundCapture() override { Stop(); }

    bool Start(std::wstring* err) override
    {
        unsigned long pid = 0;
        std::wstring name;
        const bool have = ForegroundApp(&pid, &name);

        if (!have && !m_follow)
        {
            if (err) *err =
                L"There is no application to capture: nothing has been in front "
                L"of FastRoute since it started, or what is in front cannot be "
                L"captured. Switch to the application you want, then run the "
                L"session - a global hotkey is the usual way, so FastRoute "
                L"itself does not have to come to the front to do it.";
            return false;
        }

        m_stop = false;
        m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        // The first target is resolved here rather than on the thread, so that
        // a session which cannot start says so instead of starting silent.
        if (have && !Retarget(pid, name, err) && !m_follow)
        {
            if (m_wake) { CloseHandle(m_wake); m_wake = nullptr; }
            return false;
        }

        m_thread = std::thread([this] { WatchLoop(); });
        return true;
    }

    void Stop() override
    {
        m_stop = true;
        if (m_wake) SetEvent(m_wake);
        if (m_thread.joinable()) m_thread.join();
        if (m_wake) { CloseHandle(m_wake); m_wake = nullptr; }

        std::unique_ptr<CaptureSource> dying;
        {
            std::lock_guard<std::mutex> g(m_lock);
            dying = std::move(m_inner);
        }
        dying.reset();
    }

    // Enabling a block is a fresh decision about what is in front, the same as
    // starting the session is. Following blocks are already keeping up.
    void Configure(const Block& b) override
    {
        const bool was = m_enabled;
        m_enabled = b.enabled;
        if (!m_follow && b.enabled && !was)
        {
            m_recheck.store(true);
            if (m_wake) SetEvent(m_wake);
        }
    }

    std::wstring TargetName() const override
    {
        std::lock_guard<std::mutex> g(m_lock);
        return m_inner ? m_name : std::wstring();
    }

    size_t Read(float* dst, size_t frames) override
    {
        std::lock_guard<std::mutex> g(m_lock);
        if (m_inner) return m_inner->Read(dst, frames);
        std::fill(dst, dst + frames * kChannels, 0.0f);
        return frames;
    }

    size_t Backlog() override
    {
        std::lock_guard<std::mutex> g(m_lock);
        return m_inner ? m_inner->Backlog() : 0;
    }

    unsigned PacketFrames() const override
    {
        std::lock_guard<std::mutex> g(m_lock);
        return m_inner ? m_inner->PacketFrames() : 0;
    }

    unsigned Glitches() const override
    {
        std::lock_guard<std::mutex> g(m_lock);
        // Only the current capture's. Retiring one capture and opening
        // another is a discontinuity by definition, and counting it as a
        // dropout would report a fault where there is only a decision.
        return m_inner ? m_inner->Glitches() : 0;
    }

    unsigned long long FramesIn() const override
    {
        std::lock_guard<std::mutex> g(m_lock);
        return m_inner ? m_inner->FramesIn() : 0;
    }

    unsigned long long FramesOut() const override
    {
        std::lock_guard<std::mutex> g(m_lock);
        return m_inner ? m_inner->FramesOut() : 0;
    }

    double Ratio() const override
    {
        std::lock_guard<std::mutex> g(m_lock);
        return m_inner ? m_inner->Ratio() : 1.0;
    }

    void Resync() override
    {
        std::lock_guard<std::mutex> g(m_lock);
        if (m_inner) m_inner->Resync();
    }

private:
    // Opens a capture on `pid` and puts it in place of whatever is there.
    bool Retarget(unsigned long pid, const std::wstring& name, std::wstring* err)
    {
        std::unique_ptr<CaptureSource> next = MakeProcessLoopback(pid, m_rate);
        std::wstring why;
        if (!next || !next->Start(&why))
        {
            if (err) *err = why.empty()
                ? FormatW(L"%s cannot be captured.", name.c_str()) : why;
            return false;
        }

        std::unique_ptr<CaptureSource> dying;
        {
            std::lock_guard<std::mutex> g(m_lock);
            dying = std::move(m_inner);
            m_inner = std::move(next);
            m_pid = pid;
            m_name = name;
        }
        dying.reset();      // joins its thread, outside the lock
        return true;
    }

    void WatchLoop()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (!m_stop.load())
        {
            // A following block looks again on a timer; a one-shot block only
            // wakes when it is told to, which is when it is switched back on.
            DWORD wait = m_follow ? kFollowIntervalMs : INFINITE;
            if (WaitForSingleObject(m_wake, wait) == WAIT_OBJECT_0 && m_stop.load())
                break;
            if (m_stop.load()) break;
            if (!m_follow && !m_recheck.exchange(false)) continue;

            unsigned long pid = 0;
            std::wstring name;
            if (!ForegroundApp(&pid, &name)) continue;   // nothing to move to

            {
                std::lock_guard<std::mutex> g(m_lock);
                if (m_inner && pid == m_pid) continue;   // already on it
            }
            std::wstring err;
            if (!Retarget(pid, name, &err) && !err.empty())
                LogError(FormatW(L"Could not capture %s: %s", name.c_str(), err.c_str()));
        }
        CoUninitialize();
    }

    static const DWORD kFollowIntervalMs = 250;

    const bool m_follow;
    bool m_enabled = true;
    std::atomic<bool> m_recheck{ false };

    std::unique_ptr<CaptureSource> m_inner;
    unsigned long m_pid = 0;
    std::wstring  m_name;
    mutable std::mutex m_lock;
    HANDLE m_wake = nullptr;
};

}   // namespace

std::unique_ptr<CaptureSource> MakeForegroundCapture(bool follow, int rate)
{
    return std::unique_ptr<CaptureSource>(new ForegroundCapture(follow, rate));
}
