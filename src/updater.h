#pragma once
#include "app.h"

// ---------------------------------------------------------------------------
// Updating itself
//
// FastRoute is portable: a folder with the exe in it and the settings beside
// it, which is also what makes updating simple. The continuous build replaces
// one release, tagged "latest", on every push, and this looks at that release
// and unpacks it over the folder it is running from. Nothing is installed and
// nothing in the registry is touched.
//
// Settings are left alone by that, because they are not in the zip: the only
// things it contains are the exe and the documentation, so sessions.json and
// the rest survive being copied over.
//
// Every call that talks to the network blocks, and none of them may be made
// from the UI thread. ShowCheckForUpdates is the exception - it is the UI
// thread's entry point and starts its own.
// ---------------------------------------------------------------------------

// Why something did not work, in the two ways it has to be said: once in
// words, for whoever is looking at the screen, and once as one of app-health's
// classifications, so that a problem at the user's end is counted rather than
// alerted on. See ClassifyStatus in updater.cpp for which is which.
struct UpdateFault
{
    std::wstring message;              // shown, or logged
    const wchar_t* cls = L"app";       // app-health's error_class
    // A 404 from our own feed. The build job replaces the "latest" release by
    // deleting it first, so every release has a few seconds in which there is
    // genuinely nothing there - which is not a fault worth reporting until it
    // has lasted longer than a release takes.
    bool notPublished = false;
};

struct UpdateInfo
{
    bool available = false;
    std::wstring version;      // what the release says it is
    std::wstring commit;       // and which commit it was built from
    std::wstring zipUrl;
    std::wstring notes;
    UpdateFault fault;         // message empty unless the check itself failed
};

// Which releases this build updates from: "api.github.com" and
// "Raywonder/FastRoute". Reported to app-health with every report, so that a
// feed which has to move can be retired on numbers rather than on hope - and
// so a fork that repoints its updater is counted as its own thing.
const wchar_t* UpdateFeedHost();
const wchar_t* UpdateFeedRepo();

// What the server has. Blocking; call from a background thread.
UpdateInfo CheckForUpdates();

// Fetches it to a temporary file. The callback is given bytes so far and the
// total, which is zero when the server did not say, and returns false to give
// up. Blocking.
using UpdateProgressFn = std::function<bool(unsigned long long, unsigned long long)>;
bool DownloadUpdate(const std::wstring& url, UpdateProgressFn progress,
                    UpdateFault* fault);

// Unpacks what was downloaded over this folder and restarts. This does not
// return anything useful: it asks the application to close so that the exe can
// be replaced. The build being installed is passed in so that the next run can
// tell whether it arrived, and so app-health knows what was attempted.
void ApplyUpdate(const UpdateInfo& info);

// The UI thread's way in. Checks on a thread of its own and then, on the UI
// thread, either offers the update or says there is none - except when silent,
// which says nothing unless there is something to say.
void ShowCheckForUpdates(HWND parent, bool silent);

// Starts and stops the timer that honours the update setting. Called when the
// program starts and whenever the settings change.
void StartUpdateChecks(HWND owner);
void StopUpdateChecks();

// Runs the check the settings ask for on startup, if they ask for one.
void CheckForUpdatesOnStartup(HWND owner);

// The two decisions, exposed so they can be checked without a network.
//
// ReleaseField pulls "**Version:** 0.6" and "**Commit:** abc" out of a release
// body, in the shapes the build workflow writes them. CompareVersions orders
// two version strings the way a person reads them, so 0.10 is newer than 0.6
// rather than older as a plain string comparison would have it. Negative when
// the first is older, zero when they match, positive when it is newer.
std::wstring ReleaseField(const std::wstring& body, const wchar_t* label);
int CompareVersions(const std::wstring& a, const std::wstring& b);

// What this build is, for the About dialog and for comparing against a
// release. The commit is empty when the build did not know its own.
std::wstring RunningVersion();
std::wstring RunningCommit();
