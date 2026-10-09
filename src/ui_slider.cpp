#include "app.h"
#include <commctrl.h>
// The oleacc GUIDs - the property service and PROPID_ACC_VALUE - are declared
// by oleacc.h but live in no import library, so one translation unit has to
// hold them. This is that one.
#include <initguid.h>
#include <oleacc.h>

// ---------------------------------------------------------------------------
// Slider keys
//
// Its own file so that the offline harness can link this and nothing else:
// what it checks is a promise about how the keyboard behaves, which is worth
// checking against the code that is shipped rather than against a copy of it.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// What a slider says its value is
//
// A trackbar reports its position as its accessible value, and a position is
// not a parameter. The parametric frequency slider reports 137 for 250 Hz,
// because 137 is where the thumb is on a logarithmic scale of 200 steps. A pan
// hard left reports -100. A gain reports -6 with no dB after it. The label
// beside the slider says the right thing in every case, but that label is not
// what gets read when the arrow key moves the thumb: the reader reads the
// control that has focus, and the control says where the thumb is.
//
// MSAA dynamic annotation replaces the value a control reports without
// touching the control. SetHwndPropStr puts a string of our own in front of
// the trackbar's own accessible value, so the slider answers "250 Hz", "hard
// left", "-6 dB" - the same words as the label beside it, which is where the
// string comes from.
//
// The trackbar already raises EVENT_OBJECT_VALUECHANGE when its position
// moves, and nothing here raises another: the annotation is rewritten inside
// the WM_HSCROLL that the trackbar sends while it is still handling the key,
// so by the time a reader has taken the event and come back to ask what the
// value is, the answer waiting for it is the new one. A second event would
// only make the value be read twice.
// ---------------------------------------------------------------------------

static IAccPropServices* Annotations()
{
    static IAccPropServices* svc = nullptr;
    static bool tried = false;
    if (!tried)
    {
        tried = true;
        // The UI thread is already an apartment; this is the same one the
        // shell dialogs and the device enumerator use.
        if (FAILED(CoCreateInstance(CLSID_AccPropServices, nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&svc))))
            svc = nullptr;
    }
    return svc;
}

// Annotating is best effort. If oleacc will not start, the sliders go back to
// reading out their positions, which is what they did before and is worse
// rather than broken - so nothing here fails a dialog.
void SetSliderValue(HWND slider, const std::wstring& text)
{
    IAccPropServices* svc = Annotations();
    if (!svc || !slider) return;
    svc->SetHwndPropStr(slider, OBJID_CLIENT, CHILDID_SELF, PROPID_ACC_VALUE,
                        text.c_str());
}

// An annotation is remembered against the window handle, and handles are
// reused, so it has to be given back or the next dialog to be handed this
// handle inherits the last one's words.
static void ClearSliderValue(HWND slider)
{
    IAccPropServices* svc = Annotations();
    if (!svc || !slider) return;
    MSAAPROPID prop = PROPID_ACC_VALUE;
    svc->ClearHwndProps(slider, OBJID_CLIENT, CHILDID_SELF, &prop, 1);
}

// The one way a slider's readout is written. The label beside the slider and
// the value the slider reports are set from the same string in the same call,
// so the two cannot come to disagree - which they would, eventually, if it
// were left to every dialog to remember to do both.
void SetSliderReadout(HWND dlg, int sliderId, int textId,
                      const std::wstring& text)
{
    SetDlgItemTextW(dlg, textId, text.c_str());
    SetSliderValue(GetDlgItem(dlg, sliderId), text);
}

// Sliders: up raises the value, down lowers it.
//
// A trackbar takes its key directions from its geometry rather than from what
// the number means. On a horizontal one, Up and Left both step towards the
// minimum and Down and Right towards the maximum, which is right for left and
// right and backwards for up and down: nobody turns a volume up by pressing
// Down. It is worse than merely surprising when the slider is being driven
// blind, one arrow press at a time, with the reader saying the new number
// after each - the control disagrees with the word for it.
//
// Page Up and Page Down go the same way for the same reason, and are swapped
// with them, so that every key that moves the value moves it the way its name
// says. Home, End, the tick keys and the mouse are untouched.
static LRESULT CALLBACK SliderKeySubclass(HWND hwnd, UINT msg, WPARAM wp,
                                          LPARAM lp, UINT_PTR id, DWORD_PTR)
{
    switch (msg)
    {
    case WM_KEYDOWN:
    case WM_KEYUP:
        // Both halves of the press, or the trackbar sees a key go down that
        // never comes up.
        if      (wp == VK_UP)    wp = VK_DOWN;
        else if (wp == VK_DOWN)  wp = VK_UP;
        else if (wp == VK_PRIOR) wp = VK_NEXT;
        else if (wp == VK_NEXT)  wp = VK_PRIOR;
        break;

    case WM_NCDESTROY:
        ClearSliderValue(hwnd);
        RemoveWindowSubclass(hwnd, SliderKeySubclass, id);
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static BOOL CALLBACK ReverseIfSlider(HWND child, LPARAM)
{
    wchar_t cls[32] = { 0 };
    GetClassNameW(child, cls, 32);
    if (lstrcmpiW(cls, TRACKBAR_CLASSW) == 0)
        SetWindowSubclass(child, SliderKeySubclass, 1, 0);
    return TRUE;
}

// Called once from a dialog's WM_INITDIALOG. It finds the sliders itself
// rather than being told where they are, so a slider added later to a dialog
// that already calls this gets the same keys without anyone remembering to
// say so.
void ReverseSliderKeys(HWND dlg)
{
    EnumChildWindows(dlg, ReverseIfSlider, 0);
}
