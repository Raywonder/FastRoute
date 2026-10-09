#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// External effect plugins: CLAP and VST3
//
// FastRoute hosts plugins; it does not write them. Both formats are reduced to
// the same three questions here - what is in this folder, what parameters does
// this one have, and process this buffer - because everything above this file
// should not have to care which format a block happens to be.
//
// Plugins are other people's code running inside FastRoute, so every call into
// one is treated as able to fail or hang. A plugin that will not load is left
// out with a line in the error log; a session does not refuse to start over it.
// ---------------------------------------------------------------------------

enum class PlugFormat { Clap = 0, Vst3 = 1 };

const wchar_t* PlugFormatName(PlugFormat f);      // "CLAP" / "VST3"

// One plugin, as found by a scan. `path` and `uid` together are what a block
// stores: a module can hold several plugins, and the index is not stable
// across versions of the module the way the identifier is.
struct PluginDesc
{
    PlugFormat   format = PlugFormat::Clap;
    std::wstring path;        // the .clap or .vst3 module on disk
    std::wstring uid;         // CLAP plugin id, or the VST3 class id as hex
    std::wstring name;
    std::wstring vendor;

    bool Same(const PluginDesc& o) const { return path == o.path && uid == o.uid; }
    // What the blocks library shows: "TAL Reverb 4 (VST3)".
    std::wstring Label() const;
};

// One parameter a plugin exposes.
struct PlugParam
{
    unsigned long long id = 0;      // clap_id, or VST3 ParamID
    std::wstring name;
    double minValue = 0.0;
    double maxValue = 1.0;
    double defaultValue = 0.0;
    // A stepped parameter is a choice, not a quantity: a filter type, an on
    // and off switch. It gets a combo box rather than a slider, because a
    // slider that reads "2" tells a listener nothing.
    bool   stepped = false;
    int    steps = 0;               // number of steps - 1, when stepped
};

// A loaded plugin. One of these per running block; never shared.
class PluginInstance
{
public:
    virtual ~PluginInstance() {}

    // `maxBlock` is the largest buffer Process will ever be handed. Both
    // formats want to know in advance so they can size their own scratch.
    virtual bool Start(int rate, int maxBlock, std::wstring* err) = 0;
    virtual void Stop() = 0;
    // In place on the interleaved stereo bus, like every other effect here.
    virtual void Process(float* buf, size_t frames) = 0;
    virtual int  LatencyFrames() const { return 0; }

    virtual const std::vector<PlugParam>& Params() const = 0;
    virtual double GetParam(unsigned long long id) const = 0;
    virtual void   SetParam(unsigned long long id, double value) = 0;
    // What the plugin calls that value: "-6.0 dB", "Low pass". Falls back to
    // the number when the plugin has nothing to say.
    virtual std::wstring ParamText(unsigned long long id, double value) const = 0;

    // The plugin's own editor window, for the parts of a plugin its parameter
    // list does not describe. Optional: plenty have none.
    virtual bool HasEditor() const { return false; }
    virtual bool OpenEditor(HWND parent, int* width, int* height, std::wstring* err)
    { (void)parent; (void)width; (void)height; if (err) *err = L"This plugin has no editor."; return false; }
    virtual void CloseEditor() {}
    virtual bool EditorSize(int* width, int* height) { (void)width; (void)height; return false; }

    const PluginDesc& Desc() const { return m_desc; }

protected:
    PluginDesc m_desc;
};

// Loads one plugin. Returns nullptr with `err` set rather than throwing: the
// caller is usually the audio graph being built, and it has somewhere to put
// the message.
std::unique_ptr<PluginInstance> LoadPlugin(const PluginDesc& desc, std::wstring* err);

// Everything in one folder, and the folders below it. A module that will not
// load, or that holds no audio effect, is skipped rather than reported: a
// plugin folder normally has a good deal in it that is not for us.
std::vector<PluginDesc> ScanPluginFolder(const std::wstring& folder);

// The per format scanners, which the folder walk hands each module to. Public
// so a single module can be re-examined without walking anything.
std::vector<PluginDesc> ScanClapModule(const std::wstring& path);
std::vector<PluginDesc> ScanVst3Module(const std::wstring& path);

// Where plugins are normally installed, offered when the paths list is empty
// so nobody has to know these by heart.
std::vector<std::wstring> DefaultPluginFolders();
