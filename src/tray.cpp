#include "app.h"
#include <shellapi.h>
#include <thread>
#include <vector>
#include <map>

// ---------------------------------------------------------------------------
// Notification area and the global hotkey
//
// Hiding is deliberately not something that happens by accident. Closing the
// window still exits and minimising still minimises; FastRoute only goes to
// the tray when it is asked to, by Ctrl+H, by the menu item, or by the global
// hotkey. A router that vanished when you pressed the wrong button would be a
// router you could not find again.
//
// The tray icon exists only while hidden, so the notification area is not
// carrying an icon for a window that is right there on screen.
// ---------------------------------------------------------------------------

static const UINT kTrayIconId = 1;
static const wchar_t* kCrLf = L"\r\n";

static bool g_hidden = false;
static bool g_iconLive = false;
static bool g_hotkeyLive = false;
static HWND g_lastFocus = nullptr;

static NOTIFYICONDATAW MakeIconData(HWND owner)
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = owner;
    nid.uID = kTrayIconId;
    return nid;
}

// What the icon says when you hover or read it: whether anything is running is
// the one thing worth knowing about a router you cannot see.
static std::wstring TrayTip()
{
    int running = 0;
    for (const Session& s : g_store.sessions)
        if (SessionRunning(s.id)) ++running;

    if (running == 0) return L"FastRoute - nothing running";
    if (running == 1) return L"FastRoute - 1 session running";
    return FormatW(L"FastRoute - %d sessions running", running);
}

static void AddTrayIcon(HWND owner)
{
    if (g_iconLive) return;

    NOTIFYICONDATAW nid = MakeIconData(owner);
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_APPMAIN));
    std::wstring tip = TrayTip();
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);

    if (!Shell_NotifyIconW(NIM_ADD, &nid))
    {
        LogError(L"FastRoute could not put an icon in the notification area, so "
                 L"while it is hidden the global hotkey is the only way back.");
        return;
    }
    g_iconLive = true;

    // Version 4 is what turns a keyboard selection on the icon into a message
    // we can act on, rather than only mouse clicks.
    NOTIFYICONDATAW ver = MakeIconData(owner);
    ver.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &ver);
}

static void RemoveTrayIcon(HWND owner)
{
    if (!g_iconLive) return;
    NOTIFYICONDATAW nid = MakeIconData(owner);
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_iconLive = false;
}

void TrayUpdateTip(HWND owner)
{
    if (!g_iconLive) return;
    NOTIFYICONDATAW nid = MakeIconData(owner);
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    std::wstring tip = TrayTip();
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ---------------------------------------------------------------------------

bool IsHiddenToTray() { return g_hidden; }

void HideToTray(HWND owner)
{
    if (g_hidden || !owner) return;

    // Remember where the user was, so showing again puts them back there
    // rather than always at the sessions list.
    HWND focus = GetForegroundWindow();
    g_lastFocus = (focus == owner || SessionWindowIdFor(focus) != 0) ? focus : nullptr;

    AddTrayIcon(owner);
    SetSessionWindowsVisible(false);
    ShowWindow(owner, SW_HIDE);
    g_hidden = true;
}

void ShowFromTray(HWND owner)
{
    if (!owner) return;
    g_hidden = false;

    ShowWindow(owner, SW_SHOW);
    if (IsIconic(owner)) ShowWindow(owner, SW_RESTORE);
    SetSessionWindowsVisible(true);
    RemoveTrayIcon(owner);

    HWND to = (g_lastFocus && IsWindow(g_lastFocus)) ? g_lastFocus : owner;
    SetForegroundWindow(to);
    SetActiveWindow(to);
    g_lastFocus = nullptr;
}

void ToggleTray(HWND owner)
{
    if (g_hidden) ShowFromTray(owner);
    else          HideToTray(owner);
}

// ---------------------------------------------------------------------------
// Global hotkey
// ---------------------------------------------------------------------------

// Every hotkey but the show/hide one is rebuilt whenever the document changes,
// so a binding is never left pointing at a block that has moved or gone. Ids
// are handed out from kHotkeyId + 1 upwards and mean nothing beyond this table.
struct Binding
{
    int id = 0;
    int sessionId = 0;
    int blockIndex = -1;   // -1: the binding is the session's own run toggle
    int action = -1;       // -1: enable and disable the block, rather than act
};
static std::vector<Binding> g_bindings;

// The window scoped keys, which are never handed to RegisterHotKey. They are
// matched against WM_KEYDOWN in the message loop instead, so they only mean
// anything while a FastRoute window is in front and are invisible to every
// other program. Kept apart from g_bindings because the two are looked up by
// different things: a hotkey id there, a key and modifier state here.
struct WindowBinding
{
    int mods = 0;
    int vk = 0;
    int sessionId = 0;
    int blockIndex = -1;
    int action = -1;
};
static std::vector<WindowBinding> g_windowBindings;

static UINT Win32Mods(int mods)
{
    UINT out = 0;
    if (mods & HK_CTRL)  out |= MOD_CONTROL;
    if (mods & HK_ALT)   out |= MOD_ALT;
    if (mods & HK_SHIFT) out |= MOD_SHIFT;
    if (mods & HK_WIN)   out |= MOD_WIN;
    return out;
}

void UnregisterAppHotkey(HWND owner)
{
    if (!g_hotkeyLive || !owner) return;
    UnregisterHotKey(owner, kHotkeyId);
    g_hotkeyLive = false;
}

// True when `h` can be registered at all; `err` says why not.
static bool HotkeyUsable(const Hotkey& h, std::wstring* err)
{
    if (!h.enabled || h.vk == 0) return false;
    if (Win32Mods(h.mods) == 0)
    {
        // A hotkey with no modifier would swallow that key everywhere, in
        // every application. Refuse rather than do that to the machine.
        if (err) *err = L"A global hotkey needs at least one of Ctrl, Alt, Shift "
                        L"or Windows. Without one it would take that key away "
                        L"from every other program.";
        return false;
    }
    return true;
}

// MOD_NOREPEAT: holding the key down should act once, not flicker.
static bool Register(HWND owner, int id, const Hotkey& h)
{
    return RegisterHotKey(owner, id, Win32Mods(h.mods) | MOD_NOREPEAT, (UINT)h.vk) != FALSE;
}

bool ApplyAppHotkey(HWND owner, std::wstring* err)
{
    UnregisterAppHotkey(owner);
    if (!owner) return true;

    const Hotkey& h = g_store.settings.showHide;
    if (!h.enabled || h.vk == 0) return true;
    if (!HotkeyUsable(h, err)) return false;

    if (!Register(owner, kHotkeyId, h))
    {
        std::wstring msg = FormatW(
            L"%s could not be registered as a global hotkey. Another program is "
            L"probably already using it. Pick a different key in Settings, or "
            L"FastRoute can only be hidden and shown from its own window.",
            HotkeyText(h).c_str());
        LogError(msg);
        if (err) *err = msg;
        return false;
    }
    g_hotkeyLive = true;
    return true;
}

// ---------------------------------------------------------------------------
// Session and block hotkeys
//
// These are what make a router usable while it is out of sight: mute a block
// or start a session from inside whatever you are actually doing. There is
// nothing on screen to confirm it worked, so each one answers with a tone.
// ---------------------------------------------------------------------------

void ClearItemHotkeys(HWND owner)
{
    if (owner)
        for (const Binding& b : g_bindings) UnregisterHotKey(owner, b.id);
    g_bindings.clear();
    g_windowBindings.clear();
}

void RefreshItemHotkeys(HWND owner)
{
    ClearItemHotkeys(owner);
    if (!owner) return;

    int next = kHotkeyId + 1;
    std::wstring clashes;

    auto take = [&](const Hotkey& h, int sessionId, int blockIndex, int action,
                    const std::wstring& what) {
        if (!HotkeyUsable(h, nullptr)) return;

        // A window key is never handed to the system, so nothing outside
        // FastRoute can refuse it. It can still collide with another window
        // key here though, and the first match is the one that would fire, so
        // the second is reported rather than left to quietly do nothing.
        if (!h.global)
        {
            for (const WindowBinding& e : g_windowBindings)
                if (e.mods == h.mods && e.vk == h.vk)
                {
                    clashes += FormatW(L"%s    %s, for %s", kCrLf,
                                       HotkeyText(h).c_str(), what.c_str());
                    return;
                }
            WindowBinding w;
            w.mods = h.mods;
            w.vk = h.vk;
            w.sessionId = sessionId;
            w.blockIndex = blockIndex;
            w.action = action;
            g_windowBindings.push_back(w);
            return;
        }

        Binding b;
        b.id = next;
        b.sessionId = sessionId;
        b.blockIndex = blockIndex;
        b.action = action;
        if (!Register(owner, b.id, h))
        {
            // Another program, or another block here, already has it. Said
            // once at the end rather than a message per key.
            clashes += FormatW(L"%s    %s, for %s", kCrLf,
                               HotkeyText(h).c_str(), what.c_str());
            return;
        }
        g_bindings.push_back(b);
        ++next;
    };

    for (const Session& s : g_store.sessions)
    {
        take(s.hotkey, s.id, -1, -1, FormatW(L"running %s", s.name.c_str()));

        // A block action is done to a running graph and there has never been
        // anything for it to do while the session is stopped - it answered
        // with the low tone and changed nothing. So it is only registered
        // while that session runs, which leaves the key to another session's
        // action, and to every other program, the rest of the time. One key
        // can therefore mean "save the buffer" in every session there is.
        //
        // A block's own on/off key is deliberately not treated this way.
        // Turning a block off is an edit to the document rather than something
        // done to the graph, and it means what it means whether or not the
        // session is running.
        const bool running = SessionRunning(s.id);

        for (size_t i = 0; i < s.blocks.size(); ++i)
        {
            const Block& blk = s.blocks[i];
            take(blk.hotkey, s.id, (int)i, -1,
                 FormatW(L"the %s block in %s", blk.TypeName().c_str(),
                         s.name.c_str()));

            if (!running) continue;
            for (int a : BlockActionsFor(blk.typeId))
                take(blk.actionKey[a], s.id, (int)i, a,
                     FormatW(L"%s on the %s block in %s", BlockActionName(a),
                             blk.TypeName().c_str(), s.name.c_str()));
        }
    }

    // Said when it changes, not every time the table is rebuilt. The table is
    // rebuilt on every session start and stop now, and a key some other
    // program holds for good would otherwise write the same entry into the
    // log every time a session was run.
    static std::wstring g_saidClashes;
    if (clashes != g_saidClashes)
    {
        g_saidClashes = clashes;
        if (!clashes.empty())
            LogError(L"These hotkeys are not in use. A global one is most likely "
                     L"already taken by another program; a window one is already "
                     L"used by something else in FastRoute:" + clashes);
    }
}

// 800 Hz for on and 400 Hz for off. It is the only feedback there is when the
// key was pressed in another application with FastRoute out of sight. On its
// own thread: Beep blocks for its full duration and the message loop has
// better things to do with those 50 ms.
static void BeepFor(bool on)
{
    std::thread([on] { Beep(on ? 800 : 400, 50); }).detach();
}

// Acts on a binding. False if it no longer points at anything, which can only
// happen if the document changed without the table being rebuilt.
static bool FireBinding(const Binding& b)
{
    Session* s = g_store.ById(b.sessionId);
    if (!s) return false;

    if (b.blockIndex < 0)
    {
        if (SessionRunning(s->id))
        {
            StopSession(s->id);
            BeepFor(false);
            return true;
        }
        // A failure here must not put a modal box in front of whatever the
        // user is actually doing; it goes to the error log, and the low tone
        // says the session is not running.
        std::wstring err;
        bool ok = StartSessionSilently(s->id, &err);
        if (!ok)
            LogError(FormatW(L"%s did not start: %s", s->name.c_str(),
                             err.empty() ? L"no reason given." : err.c_str()));
        BeepFor(ok);
        return true;
    }

    if ((size_t)b.blockIndex >= s->blocks.size()) return false;

    // An action is done to the running graph and changes nothing that is kept,
    // so it never touches the document and never saves. Pressed while the
    // session is stopped there is nothing to act on, and the low tone says so.
    if (b.action >= 0)
    {
        const bool ok = PerformBlockAction(s->id, b.blockIndex, b.action);
        BeepFor(ok);
        if (ok) RefreshSessionWindow(s->id);
        return true;
    }

    Block& block = s->blocks[(size_t)b.blockIndex];
    block.enabled = !block.enabled;

    std::wstring err;
    if (!g_store.Save(&err)) LogError(err);
    ApplyLiveEditQuiet(s->id);
    RefreshSessionWindow(s->id);
    RefreshMainList();
    BeepFor(block.enabled);
    return true;
}

// ---------------------------------------------------------------------------
// Window scoped hotkeys
//
// Checked from the message loop rather than registered with the system. That
// is the whole point of them: the key means this only while FastRoute is in
// front, and is left alone everywhere else.
// ---------------------------------------------------------------------------

// The modifiers held down right now, in the same bits the document uses.
static int CurrentMods()
{
    int m = 0;
    if (GetKeyState(VK_CONTROL) < 0) m |= HK_CTRL;
    if (GetKeyState(VK_MENU) < 0)    m |= HK_ALT;
    if (GetKeyState(VK_SHIFT) < 0)   m |= HK_SHIFT;
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) m |= HK_WIN;
    return m;
}

// The editing keys every Windows text field is expected to have. A window
// hotkey is allowed to be anything, but while the caret is in an edit box
// these have to keep meaning what they mean everywhere else - a router that
// eats Ctrl+V in its own file name field would be its own bug report.
static bool ShadowsTextEditing(int mods, int vk)
{
    if (mods == HK_CTRL)
        return vk == 'A' || vk == 'C' || vk == 'V' || vk == 'X' ||
               vk == 'Z' || vk == 'Y';
    if (mods == HK_SHIFT) return vk == VK_INSERT || vk == VK_DELETE;
    return false;
}

static bool FocusIsTextField()
{
    HWND f = GetFocus();
    if (!f) return false;
    wchar_t cls[64] = { 0 };
    GetClassNameW(f, cls, 64);
    if (IEquals(cls, L"Edit")) return true;
    // A combo box you can type into carries an edit of its own.
    if (IEquals(cls, L"ComboBox"))
        return (GetWindowLongPtrW(f, GWL_STYLE) & 0x3) != CBS_DROPDOWNLIST;
    return false;
}

bool HandleWindowHotkey(const MSG& msg)
{
    if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN) return false;
    if (g_windowBindings.empty()) return false;

    // Bit 30 of lParam is set when the key was already down, so holding a key
    // acts once rather than repeating - the same as MOD_NOREPEAT gives the
    // global ones.
    if (msg.lParam & (1 << 30)) return false;

    const int mods = CurrentMods();
    if (mods == 0) return false;

    const int vk = (int)msg.wParam;
    for (const WindowBinding& w : g_windowBindings)
    {
        if (w.mods != mods || w.vk != vk) continue;
        if (ShadowsTextEditing(mods, vk) && FocusIsTextField()) return false;

        Binding b;
        b.sessionId = w.sessionId;
        b.blockIndex = w.blockIndex;
        b.action = w.action;
        return FireBinding(b);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Messages, forwarded from the main window
// ---------------------------------------------------------------------------

bool TrayHandleMessage(HWND owner, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_HOTKEY)
    {
        if ((int)wp == kHotkeyId) { ToggleTray(owner); return true; }
        for (const Binding& b : g_bindings)
            if (b.id == (int)wp)
            {
                // By value. Firing this can start or stop a session, and that
                // now rebuilds the very table the reference points into.
                const Binding fire = b;
                return FireBinding(fire);
            }
        return false;
    }

    if (msg != WM_APP_TRAY) return false;

    // With version 4 the event is in the low word of lParam and the cursor
    // position is in wParam.
    switch (LOWORD(lp))
    {
    case NIN_SELECT:
    case NIN_KEYSELECT:
    case WM_LBUTTONUP:
        ShowFromTray(owner);
        return true;

    case WM_CONTEXTMENU:
    case WM_RBUTTONUP:
    {
        POINT pt{ (int)(short)LOWORD(wp), (int)(short)HIWORD(wp) };
        // A tray menu has to be dismissable by clicking away from it, which is
        // what this foreground dance buys.
        SetForegroundWindow(owner);
        std::map<UINT, bool> enabled;
        enabled[IDM_TRAY_SHOW] = true;
        enabled[IDM_APP_SETTINGS] = true;
        enabled[IDM_APP_EXIT] = true;
        ShowContextMenu(owner, IDR_TRAY_CTX, pt, IDM_TRAY_SHOW, enabled);
        PostMessageW(owner, WM_NULL, 0, 0);
        return true;
    }
    }
    return false;
}

void TrayShutdown(HWND owner)
{
    ClearItemHotkeys(owner);
    UnregisterAppHotkey(owner);
    RemoveTrayIcon(owner);
    g_hidden = false;
}
