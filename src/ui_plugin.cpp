#include "app.h"
#include "plugins/plugins.h"

#include <algorithm>
#include <memory>

// ---------------------------------------------------------------------------
// A plugin block's parameters, and the plugin's own editor
//
// The parameter list is the point of this dialog. A plugin's own window is a
// picture of knobs: usable if you can see it, and nothing at all if you cannot.
// So every parameter the plugin declares is listed as text, with its value
// spelled the way the plugin spells it, and edited with a slider or a combo
// box like any other setting in FastRoute. The editor is a button next to it,
// for the parts of a plugin its parameter list does not describe.
//
// The dialog loads its own copy of the plugin rather than reaching into the
// one the audio graph is running. That means changes are heard when OK is
// pressed rather than while the slider moves - the same as every other block
// dialog here - and it keeps the audio thread out of a dialog's lifetime.
// ---------------------------------------------------------------------------

namespace {

const int kSliderRange = 1000;

struct PlugCtx
{
    Block* block = nullptr;
    std::unique_ptr<PluginInstance> plugin;
    std::vector<PlugParam> params;
    HWND editor = nullptr;      // the window the plugin's view sits in
};

// --- the editor window -----------------------------------------------------

const wchar_t* kEditorClass = L"FastRoutePluginEditor";

LRESULT CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PlugCtx* ctx = reinterpret_cast<PlugCtx*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg)
    {
    case WM_CLOSE:
        // The plugin is told to let go before the window it is sitting in
        // disappears, not after.
        if (ctx && ctx->plugin) ctx->plugin->CloseEditor();
        if (ctx) ctx->editor = nullptr;
        DestroyWindow(hwnd);
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterEditorClass()
{
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = EditorProc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kEditorClass;
    wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_APPMAIN));
    RegisterClassExW(&wc);
    done = true;
}

void OpenPluginEditor(HWND owner, PlugCtx* ctx)
{
    if (!ctx->plugin) return;
    if (ctx->editor) { SetForegroundWindow(ctx->editor); return; }
    if (!ctx->plugin->HasEditor())
    {
        InfoBox(owner, L"This plugin has no editor of its own. Its parameters are "
                       L"all in the list.");
        return;
    }

    RegisterEditorClass();

    std::wstring title = ctx->block->plugName + L" - FastRoute";
    HWND win = CreateWindowExW(0, kEditorClass, title.c_str(),
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                               CW_USEDEFAULT, CW_USEDEFAULT, 640, 400,
                               owner, nullptr, g_inst, nullptr);
    if (!win)
    {
        ErrorBox(owner, L"The window for the plugin's editor could not be created.");
        return;
    }
    SetWindowLongPtrW(win, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

    int w = 640, h = 400;
    std::wstring err;
    if (!ctx->plugin->OpenEditor(win, &w, &h, &err))
    {
        DestroyWindow(win);
        ErrorBox(owner, err.empty() ? L"The plugin's editor would not open." : err);
        return;
    }

    // The plugin says how big it wants to be; the frame is sized around that
    // rather than the other way round.
    RECT want{ 0, 0, w, h };
    AdjustWindowRectEx(&want, (DWORD)GetWindowLongPtrW(win, GWL_STYLE), FALSE,
                       (DWORD)GetWindowLongPtrW(win, GWL_EXSTYLE));
    SetWindowPos(win, nullptr, 0, 0, want.right - want.left, want.bottom - want.top,
                 SWP_NOMOVE | SWP_NOZORDER);
    ShowWindow(win, SW_SHOW);
    ctx->editor = win;
}

// --- the parameter list ----------------------------------------------------

std::wstring RowText(PlugCtx* ctx, size_t i)
{
    const PlugParam& p = ctx->params[i];
    const double v = ctx->plugin ? ctx->plugin->GetParam(p.id) : p.defaultValue;
    const std::wstring text = ctx->plugin ? ctx->plugin->ParamText(p.id, v)
                                          : FormatW(L"%.3f", v);
    return FormatW(L"%s: %s", p.name.c_str(), text.c_str());
}

void RefreshRow(HWND dlg, PlugCtx* ctx, int i)
{
    if (i < 0 || (size_t)i >= ctx->params.size()) return;
    HWND list = GetDlgItem(dlg, IDC_PP_LIST);
    // A list box row cannot be edited in place, so it is replaced. The
    // selection is put back, because deleting the selected row loses it.
    const int top = (int)SendMessageW(list, LB_GETTOPINDEX, 0, 0);
    SendMessageW(list, LB_DELETESTRING, (WPARAM)i, 0);
    SendMessageW(list, LB_INSERTSTRING, (WPARAM)i, (LPARAM)RowText(ctx, (size_t)i).c_str());
    SendMessageW(list, LB_SETCURSEL, (WPARAM)i, 0);
    SendMessageW(list, LB_SETTOPINDEX, (WPARAM)top, 0);
}

void FillList(HWND dlg, PlugCtx* ctx)
{
    HWND list = GetDlgItem(dlg, IDC_PP_LIST);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < ctx->params.size(); ++i)
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)RowText(ctx, i).c_str());
    if (!ctx->params.empty()) SendMessageW(list, LB_SETCURSEL, 0, 0);
}

// Shows the right editor for whichever parameter is selected: a combo box for
// a choice, a slider for a quantity. The other is hidden rather than left
// there, so tabbing never lands on a control that does nothing.
void SyncValueControls(HWND dlg, PlugCtx* ctx)
{
    const int sel = (int)SendMessageW(GetDlgItem(dlg, IDC_PP_LIST), LB_GETCURSEL, 0, 0);
    HWND slider = GetDlgItem(dlg, IDC_PP_SLIDER);
    HWND choice = GetDlgItem(dlg, IDC_PP_CHOICE);

    if (sel == LB_ERR || (size_t)sel >= ctx->params.size() || !ctx->plugin)
    {
        ShowWindow(slider, SW_HIDE);
        ShowWindow(choice, SW_HIDE);
        SetSliderReadout(dlg, IDC_PP_SLIDER, IDC_PP_TEXT, L"");
        return;
    }

    const PlugParam& p = ctx->params[(size_t)sel];
    const double v = ctx->plugin->GetParam(p.id);

    SetDlgItemTextW(dlg, IDC_PP_VALUE_LABEL, FormatW(L"&Value of %s:", p.name.c_str()).c_str());
    // The plugin's own spelling of the value - "-6.0 dB", "Low pass" - is what
    // the slider reports, not the position of the thumb along its 1000 steps.
    SetSliderReadout(dlg, IDC_PP_SLIDER, IDC_PP_TEXT,
                     ctx->plugin->ParamText(p.id, v));

    if (p.stepped && p.steps > 0 && p.steps <= 64)
    {
        ShowWindow(slider, SW_HIDE);
        ShowWindow(choice, SW_SHOW);
        SendMessageW(choice, CB_RESETCONTENT, 0, 0);
        for (int k = 0; k <= p.steps; ++k)
        {
            const double step = p.minValue + (p.maxValue - p.minValue) * (double)k / (double)p.steps;
            SendMessageW(choice, CB_ADDSTRING, 0,
                         (LPARAM)ctx->plugin->ParamText(p.id, step).c_str());
        }
        const double span = (p.maxValue - p.minValue);
        int at = span > 0.0 ? (int)((v - p.minValue) / span * (double)p.steps + 0.5) : 0;
        if (at < 0) at = 0;
        if (at > p.steps) at = p.steps;
        SendMessageW(choice, CB_SETCURSEL, (WPARAM)at, 0);
        return;
    }

    ShowWindow(choice, SW_HIDE);
    ShowWindow(slider, SW_SHOW);
    SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, kSliderRange));
    SendMessageW(slider, TBM_SETPAGESIZE, 0, kSliderRange / 20);
    const double span = p.maxValue - p.minValue;
    int pos = span > 0.0 ? (int)((v - p.minValue) / span * kSliderRange + 0.5) : 0;
    if (pos < 0) pos = 0;
    if (pos > kSliderRange) pos = kSliderRange;
    SendMessageW(slider, TBM_SETPOS, TRUE, pos);
}

void ApplyFromControls(HWND dlg, PlugCtx* ctx)
{
    const int sel = (int)SendMessageW(GetDlgItem(dlg, IDC_PP_LIST), LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR || (size_t)sel >= ctx->params.size() || !ctx->plugin) return;

    const PlugParam& p = ctx->params[(size_t)sel];
    double v = p.defaultValue;

    if (IsWindowVisible(GetDlgItem(dlg, IDC_PP_CHOICE)))
    {
        const int at = (int)SendMessageW(GetDlgItem(dlg, IDC_PP_CHOICE), CB_GETCURSEL, 0, 0);
        if (at == CB_ERR || p.steps <= 0) return;
        v = p.minValue + (p.maxValue - p.minValue) * (double)at / (double)p.steps;
    }
    else
    {
        const int pos = (int)SendMessageW(GetDlgItem(dlg, IDC_PP_SLIDER), TBM_GETPOS, 0, 0);
        v = p.minValue + (p.maxValue - p.minValue) * (double)pos / (double)kSliderRange;
    }

    ctx->plugin->SetParam(p.id, v);
    SetSliderReadout(dlg, IDC_PP_SLIDER, IDC_PP_TEXT,
                     ctx->plugin->ParamText(p.id, v));
    RefreshRow(dlg, ctx, sel);
}

// Reads every parameter out of the plugin and into the block. This is what
// makes an edit stick, and it is also how the plugin's own editor is read
// back: whatever it changed is simply what the plugin now says it is set to.
void HarvestParams(PlugCtx* ctx)
{
    if (!ctx->plugin || !ctx->block) return;
    ctx->block->plugParams.clear();
    for (const PlugParam& p : ctx->params)
        ctx->block->plugParams.push_back({ p.id, ctx->plugin->GetParam(p.id) });
}

INT_PTR CALLBACK PluginDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    PlugCtx* ctx = reinterpret_cast<PlugCtx*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        ReverseSliderKeys(dlg);
        ctx = reinterpret_cast<PlugCtx*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);

        CheckDlgButton(dlg, IDC_BLOCK_ENABLED,
                       ctx->block->enabled ? BST_CHECKED : BST_UNCHECKED);

        if (!ctx->plugin)
        {
            SetDlgItemTextW(dlg, IDC_PP_NAME, FormatW(
                L"%s could not be loaded from %s. Its settings are kept, so if the "
                L"plugin comes back this block will work again.",
                ctx->block->plugName.empty() ? L"This plugin" : ctx->block->plugName.c_str(),
                ctx->block->plugPath.c_str()).c_str());
            EnableWindow(GetDlgItem(dlg, IDC_PP_LIST), FALSE);
            EnableWindow(GetDlgItem(dlg, IDC_PP_EDITOR), FALSE);
            EnableWindow(GetDlgItem(dlg, IDC_PP_RESET), FALSE);
            ShowWindow(GetDlgItem(dlg, IDC_PP_SLIDER), SW_HIDE);
            ShowWindow(GetDlgItem(dlg, IDC_PP_CHOICE), SW_HIDE);
            return TRUE;
        }

        const PluginDesc& d = ctx->plugin->Desc();
        SetDlgItemTextW(dlg, IDC_PP_NAME, FormatW(
            L"%s%s%s, %s, %zu parameters.", d.name.c_str(),
            d.vendor.empty() ? L"" : L" by ", d.vendor.c_str(),
            PlugFormatName(d.format), ctx->params.size()).c_str());

        // Whatever the block last had is put back before anything is shown, so
        // the list reads as the settings rather than as the defaults.
        for (const auto& kv : ctx->block->plugParams) ctx->plugin->SetParam(kv.first, kv.second);

        FillList(dlg, ctx);
        SyncValueControls(dlg, ctx);
        EnableWindow(GetDlgItem(dlg, IDC_PP_EDITOR), ctx->plugin->HasEditor());
        if (ctx->params.empty())
            SetSliderReadout(dlg, IDC_PP_SLIDER, IDC_PP_TEXT, L"none");
        return TRUE;
    }

    case WM_HSCROLL:
        ApplyFromControls(dlg, ctx);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_PP_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE) SyncValueControls(dlg, ctx);
            return TRUE;

        case IDC_PP_CHOICE:
            if (HIWORD(wp) == CBN_SELCHANGE) ApplyFromControls(dlg, ctx);
            return TRUE;

        case IDC_PP_EDITOR:
            OpenPluginEditor(dlg, ctx);
            return TRUE;

        case IDC_PP_RESET:
            if (ctx->plugin)
            {
                for (const PlugParam& p : ctx->params) ctx->plugin->SetParam(p.id, p.defaultValue);
                FillList(dlg, ctx);
                SyncValueControls(dlg, ctx);
                SetFocus(GetDlgItem(dlg, IDC_PP_LIST));
            }
            return TRUE;

        case IDC_APPLY:
        case IDOK:
            // The plugin's own editor may have changed things this dialog
            // never saw, so everything is read back rather than only what was
            // touched here. Apply leaves that editor open: the whole point of
            // applying is to keep working, and taking the plugin's own window
            // away mid-edit would be the opposite of that.
            if (LOWORD(wp) == IDOK)
            {
                if (ctx->editor)
                {
                    PostMessageW(ctx->editor, WM_CLOSE, 0, 0);
                    ctx->editor = nullptr;
                }
                if (ctx->plugin) ctx->plugin->CloseEditor();
            }
            HarvestParams(ctx);
            ctx->block->enabled = IsDlgButtonChecked(dlg, IDC_BLOCK_ENABLED) == BST_CHECKED;
            FinishBlockEdit(dlg, LOWORD(wp));
            return TRUE;

        case IDCANCEL:
            if (ctx->editor) { PostMessageW(ctx->editor, WM_CLOSE, 0, 0); ctx->editor = nullptr; }
            if (ctx->plugin) ctx->plugin->CloseEditor();
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

}   // namespace

bool EditPluginBlock(HWND parent, Block& b)
{
    PlugCtx ctx;
    ctx.block = &b;

    PluginDesc d;
    d.format = b.plugFormat == 1 ? PlugFormat::Vst3 : PlugFormat::Clap;
    d.path = b.plugPath;
    d.uid = b.plugUid;
    d.name = b.plugName;

    std::wstring err;
    ctx.plugin = LoadPlugin(d, &err);
    if (ctx.plugin)
    {
        // A plugin has to be running before it will answer about its
        // parameters in any useful way, so it is started here at the rate the
        // session would use. Nothing is fed through it.
        std::wstring startErr;
        if (!ctx.plugin->Start(kDefaultRate, 4096, &startErr))
            LogError(FormatW(L"%s would not start for its settings dialog. %s",
                             d.name.c_str(), startErr.c_str()));
        ctx.params = ctx.plugin->Params();
    }
    else if (!err.empty())
    {
        LogError(err);
    }

    const INT_PTR r = DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_BLOCK_PLUGIN), parent,
                                      PluginDlgProc, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.plugin) ctx.plugin->Stop();
    return r == IDOK;
}
