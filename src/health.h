#pragma once
#include <string>

// ---------------------------------------------------------------------------
// Telling our server what happened
//
// Every one of our applications with an updater reports what its updates did -
// installed, failed, or did not reopen afterwards - so that a broken update is
// noticed by us rather than by the person using it. Without this a failed
// update is a line in a log file on somebody else's machine, which is to say
// invisible, and we also have no idea how many copies are running which build.
// Alongside that: a crash, the handful of errors that mean FastRoute itself is
// broken, and - only ever when the person presses the button - the error log.
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

// Reads the state file, works out what the last update did and reports it,
// reports a crash the last run left behind, and sends one "started" a day so
// that builds in use can be counted. Call once, early, after the settings have
// been loaded - the privacy setting is read from them.
void OnStartup();

// Arms the last-chance exception filter, so that a crash is reported rather
// than being something only the person it happened to ever knows about. Call
// once, after OnStartup, which is what clears away the previous run's report
// before this run can write one over it. With reporting off nothing is armed.
//
// The filter itself does as little as it is possible to do: the process is
// already broken, and a handler that allocates, takes a lock, starts a thread
// or waits on the network turns a crash - which Windows deals with in a second
// - into a hang, which it does not. So it writes one short line to a file of
// its own with no help from the heap, the CRT or any lock, and then hands the
// exception on untouched. The next run reads that line, turns it into a proper
// report and deletes it. Windows itself sees exactly what it sees today.
void InstallCrashHandler();

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

// --- the errors that are ours ---------------------------------------------

// One error, said to be FastRoute's own fault. Reached through LogFault and
// ErrorBoxFault in util.h rather than called directly, so that the judgement
// is made where the failure is, by whoever knows what it means.
//
// Which errors those are is the whole question, and the answer is: very few of
// them. FastRoute's error log fills up mostly with the machine around it - a
// session set to run at startup whose capture target is not open yet, an
// Icecast server that is down, a hotkey another program already holds, a USB
// interface that was unplugged, a folder on a drive that is not in the slot.
// Every one of those is worth telling the person about and none of them is a
// bug, so reporting the log wholesale would bury the few that are ours under
// hundreds that are not, and would do it loudest for whoever has the most
// trouble. `where` is a short word naming the place, for grouping.
void ReportError(const wchar_t* where, const std::wstring& error);

// --- the error log, when the person sends it ------------------------------

// True when there is a collector to talk to: a build made without a key has
// none, so Help > Send Diagnostic Logs is greyed rather than offering
// something that cannot work.
bool CanReport();

// The report, as text: the error log with the user folder and any sign-in
// details taken out of it, under a few lines saying which build and which
// Windows this is. The dialog shows this and sends this same string back to
// SubmitDiagnostics, so that what the person read is what left the machine.
std::wstring DiagnosticsText();

// Posts that report and whatever the person typed about it, and gives back the
// reference the server answers with so they can quote it to us. Blocks for as
// long as the request takes, so call it off the message thread.
//
// This one ignores the Settings checkbox, which governs what FastRoute sends
// by itself. Nothing here happens unless the person presses the button, and
// somebody who has turned the automatic reports off and then chooses to send
// us a log means to send us a log.
bool SubmitDiagnostics(const std::wstring& report, const std::wstring& note,
                       std::wstring* reference, std::wstring* error);

}  // namespace Health
