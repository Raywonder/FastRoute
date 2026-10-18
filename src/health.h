#pragma once
#include <string>

// ---------------------------------------------------------------------------
// Telling our server what happened to an update
//
// Every one of our applications with an updater reports what its updates did -
// installed, failed, or did not reopen afterwards - so that a broken update is
// noticed by us rather than by the person using it. Without this a failed
// update is a line in a log file on somebody else's machine, which is to say
// invisible, and we also have no idea how many copies are running which build.
//
// What goes out is a random install number, the version, the Windows version,
// which release feed this copy follows, and a short description of what went
// wrong. Never a file name, a session, a stream key, a password or anything
// else belonging to the person. The Settings dialog has a checkbox to turn it
// off, and turning it off throws away anything waiting to be sent.
//
// Nothing here blocks the caller or throws: a collector that is down, a
// machine that is offline and a disk that cannot be written to all have to be
// no worse than silence. Reports made while offline are kept in a small queue
// beside the settings and go out on a later run.
// ---------------------------------------------------------------------------
namespace Health
{

// Reads the state file, works out what the last update did and reports it, and
// sends one "started" a day so that builds in use can be counted. Call once,
// early, after the settings have been loaded - the privacy setting is read
// from them.
void OnStartup();

// The person's choice, from Settings. Set whenever the settings change, and
// before OnStartup: with reporting off nothing is sent, nothing is queued and
// nothing is written down. Turning it off throws away whatever was waiting.
void SetEnabled(bool on);

// Gives anything in flight a moment to finish, so the process does not end
// with a sender part way through a request. Call once, at the end, beside
// StopUpdateChecks.
void Shutdown();

// --- the updater's side ---------------------------------------------------

// A check found a newer build. Said once per build found, so that an update
// that is offered and then never installed is distinguishable from one that
// was never offered.
void UpdateAvailable(const std::wstring& toVersion, const std::wstring& toCommit);

// About to hand over to the script that replaces the exe, after which this
// process ends. Writes the marker OnStartup reads, and sends this one report
// synchronously with a short deadline, because there is no later for it.
void UpdateStarted(const std::wstring& toVersion, const std::wstring& toCommit);

// The update was begun - the marker written and UpdateStarted reported - and
// then could not be handed over after all. Takes the marker back, so the next
// run does not report an install that never started, and reports the failure.
void UpdateAbandoned(const std::wstring& error);

// A stage of the update did not work. stage is one of "check", "download",
// "verify" or "install"; errorClass is app-health's classification, for which
// see ClassifyNetwork below.
void UpdateFailed(const wchar_t* stage, const wchar_t* errorClass,
                  const std::wstring& toVersion, const std::wstring& error);

// Which feed this build follows, as it is reported: "api.github.com/Raywonder/
// FastRoute". Exposed so the About dialog and the error log can say the same
// thing the report does.
std::wstring FeedName();

// Where the update script leaves its own failure text, for OnStartup to find
// and report on the next run. The script runs after this process has gone, so
// a message box from it would have nobody to belong to.
std::wstring InstallerErrorPath();

}  // namespace Health
