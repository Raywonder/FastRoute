#include "app.h"
#include <vector>

// ---------------------------------------------------------------------------
// The scheduler
//
// Everything here works in whole minutes since 1601, which is what a
// SYSTEMTIME converts to and what makes "is this due yet" a comparison rather
// than a case analysis of dates rolling over. It is checked a few times a
// minute rather than on a precise alarm: a router starting a second late is
// nobody's problem, and a timer that has to survive sleep, clock changes and
// the machine being off is a great deal of machinery for that second.
//
// An occurrence that has been and gone is not started late. If FastRoute was
// not running at nine in the morning, starting the nine o'clock session when
// it opens at three in the afternoon is not what anybody meant; that goes to
// the error log instead.
// ---------------------------------------------------------------------------

static const long long kGraceMinutes = 5;

// A stop that is owed. Kept apart from the event so a single-use event can be
// deleted the moment it fires and still be stopped afterwards.
struct PendingStop
{
    int eventId = 0;
    int sessionId = 0;
    long long at = 0;
};
static std::vector<PendingStop> g_pending;

static long long MinutesOf(const SYSTEMTIME& st)
{
    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (long long)(u.QuadPart / 600000000ull);   // 100 ns ticks to minutes
}

long long ScheduleNowMinutes()
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return MinutesOf(now);
}

// The minute a time of day falls on, on the same day as `like`.
static long long MinuteOnDayOf(const SYSTEMTIME& like, int hour, int minute)
{
    SYSTEMTIME t = like;
    t.wHour = (WORD)hour;
    t.wMinute = (WORD)minute;
    t.wSecond = 0;
    t.wMilliseconds = 0;
    return MinutesOf(t);
}

long long ScheduleDueAt(const ScheduleEvent& e, const SYSTEMTIME& now)
{
    if (e.repeat == SR_ONCE)
    {
        if (e.year < 1601 || e.month < 1 || e.month > 12 || e.day < 1) return 0;
        SYSTEMTIME t{};
        t.wYear = (WORD)e.year;
        t.wMonth = (WORD)e.month;
        t.wDay = (WORD)e.day;
        t.wHour = (WORD)e.hour;
        t.wMinute = (WORD)e.minute;
        // The day of week is filled in by the conversion; it must be right or
        // SystemTimeToFileTime refuses, so it is left at zero deliberately and
        // the date is validated by the conversion failing.
        return MinutesOf(t);
    }

    if (e.repeat == SR_WEEKDAYS && !(e.weekdays & (1 << now.wDayOfWeek))) return 0;
    return MinuteOnDayOf(now, e.hour, e.minute);
}

// When the session this event started should be stopped, or 0 for never.
static long long StopAt(const ScheduleEvent& e, const SYSTEMTIME& now, long long started)
{
    if (e.stopMode == SS_AFTER)
        return started + (e.afterMinutes > 0 ? e.afterMinutes : 1);

    if (e.stopMode == SS_AT_TIME)
    {
        long long at = MinuteOnDayOf(now, e.stopHour, e.stopMinute);
        // A stop time at or before the start means the following day, which is
        // how an overnight run is expressed.
        if (at <= started) at += 24 * 60;
        return at;
    }
    return 0;
}

// ---------------------------------------------------------------------------

void SchedulerTick(HWND owner)
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const long long minute = MinutesOf(now);
    if (minute == 0) return;

    bool changed = false;

    // Stops first: an event that starts a session this same minute should not
    // have last night's stop applied to it.
    for (size_t i = g_pending.size(); i-- > 0; )
    {
        if (minute < g_pending[i].at) continue;
        int sessionId = g_pending[i].sessionId;
        g_pending.erase(g_pending.begin() + (ptrdiff_t)i);
        if (SessionRunning(sessionId)) StopSession(sessionId);
    }

    for (size_t i = g_store.events.size(); i-- > 0; )
    {
        ScheduleEvent& e = g_store.events[i];
        if (!e.enabled) continue;

        const long long due = ScheduleDueAt(e, now);
        if (due == 0 || minute < due || due <= e.lastFired) continue;

        e.lastFired = due;
        changed = true;

        Session* s = g_store.ById(e.sessionId);
        if (!s)
        {
            LogError(FormatW(L"A scheduled event was due at %02d:%02d but the "
                             L"session it names has been deleted.", e.hour, e.minute));
            continue;
        }

        if (minute - due > kGraceMinutes)
        {
            LogError(FormatW(
                L"%s was due to start at %02d:%02d but FastRoute was not there to "
                L"start it, and %lld minutes is too late to be what you meant. It "
                L"has been left alone.", s->name.c_str(), e.hour, e.minute,
                minute - due));
            continue;
        }

        std::wstring err;
        if (SessionRunning(s->id) || StartSessionSilently(s->id, &err))
        {
            long long stop = StopAt(e, now, minute);
            if (stop)
            {
                PendingStop p;
                p.eventId = e.id;
                p.sessionId = s->id;
                p.at = stop;
                g_pending.push_back(p);
            }
        }
        else
        {
            LogError(FormatW(L"%s was due to start now but did not: %s",
                             s->name.c_str(),
                             err.empty() ? L"no reason given." : err.c_str()));
        }

        if (e.singleUse)
            g_store.events.erase(g_store.events.begin() + (ptrdiff_t)i);
    }

    if (changed)
    {
        std::wstring err;
        if (!g_store.Save(&err)) LogError(err);
        RefreshScheduleList();
        RefreshMainList();
    }
    (void)owner;
}

void SchedulerForget(int eventId)
{
    for (size_t i = g_pending.size(); i-- > 0; )
        if (g_pending[i].eventId == eventId)
            g_pending.erase(g_pending.begin() + (ptrdiff_t)i);
}

// A newly written event should not look like it has already missed today's
// occurrence, so it starts out having "fired" at the present minute.
void ScheduleMarkCurrent(ScheduleEvent& e)
{
    e.lastFired = ScheduleNowMinutes();
}

std::wstring ScheduleNextText(const ScheduleEvent& e)
{
    if (!e.enabled) return L"Disabled";

    SYSTEMTIME now{};
    GetLocalTime(&now);
    const long long minute = MinutesOf(now);
    const long long due = ScheduleDueAt(e, now);

    if (e.repeat == SR_ONCE && due != 0 && due <= e.lastFired) return L"Done";
    if (due != 0 && minute < due)
    {
        long long left = due - minute;
        if (left < 60) return FormatW(L"In %lld minutes", left);
        if (left < 24 * 60) return FormatW(L"In %lld hours", left / 60);
    }
    return L"Waiting";
}
