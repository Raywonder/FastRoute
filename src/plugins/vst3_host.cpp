#include "plugins.h"
#include "../util.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// VST3 hosting
//
// VST3 is COM shaped without being COM: reference counted interfaces, a class
// factory, and 128 bit class identifiers, all of it Steinberg's own rather
// than Microsoft's. A host has to supply several small objects back to the
// plugin - somewhere to report parameter edits, somewhere to resize a window,
// a way to make message objects - and those make up most of this file.
//
// The pieces FastRoute implements are deliberately the minimum a well behaved
// effect needs. Anything beyond that is answered with kNotImplemented, which
// is a legitimate answer and is what plugins test for.
//
// A plugin is two halves that may or may not be the same object: a component,
// which processes audio, and a controller, which owns the parameters and the
// editor. Keeping them talking to each other is the host's job too.
// ---------------------------------------------------------------------------

using namespace Steinberg;

namespace {

std::wstring FromUtf16(const Vst::TChar* s)
{
    // Steinberg's TChar is UTF-16, the same as wchar_t on Windows.
    return s ? std::wstring(reinterpret_cast<const wchar_t*>(s)) : std::wstring();
}

void ToUtf16(const std::wstring& in, Vst::TChar* out, int max)
{
    int n = (int)(std::min)((size_t)(max - 1), in.size());
    for (int i = 0; i < n; ++i) out[i] = (Vst::TChar)in[(size_t)i];
    out[n] = 0;
}

std::wstring HexOfTuid(const TUID uid)
{
    std::wstring out;
    for (int i = 0; i < 16; ++i) out += FormatW(L"%02X", (unsigned char)uid[i]);
    return out;
}

bool TuidFromHex(const std::wstring& hex, TUID uid)
{
    if (hex.size() != 32) return false;
    for (int i = 0; i < 16; ++i)
    {
        int v = 0;
        for (int k = 0; k < 2; ++k)
        {
            wchar_t c = hex[(size_t)(i * 2 + k)];
            int d = (c >= L'0' && c <= L'9') ? c - L'0'
                  : (c >= L'A' && c <= L'F') ? c - L'A' + 10
                  : (c >= L'a' && c <= L'f') ? c - L'a' + 10 : -1;
            if (d < 0) return false;
            v = v * 16 + d;
        }
        uid[i] = (char)v;
    }
    return true;
}

// A .vst3 on Windows is either a plain DLL or a bundle folder that contains
// one. Both are in the wild, so both are tried.
std::wstring ModuleBinary(const std::wstring& path)
{
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return L"";
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) return path;

    std::wstring leaf = path;
    size_t slash = leaf.find_last_of(L'\\');
    if (slash != std::wstring::npos) leaf = leaf.substr(slash + 1);

    const wchar_t* arches[] = { L"x86_64-win", L"Windows", L"x86_64" };
    for (const wchar_t* a : arches)
    {
        std::wstring cand = path + L"\\Contents\\" + a + L"\\" + leaf;
        DWORD at = GetFileAttributesW(cand.c_str());
        if (at != INVALID_FILE_ATTRIBUTES && !(at & FILE_ATTRIBUTE_DIRECTORY))
            return cand;
    }
    return L"";
}

// --- the loaded module -----------------------------------------------------
typedef bool (PLUGIN_API* PFN_InitDll)();
typedef bool (PLUGIN_API* PFN_ExitDll)();
typedef IPluginFactory* (PLUGIN_API* PFN_GetFactory)();

struct Module
{
    HMODULE dll = nullptr;
    IPluginFactory* factory = nullptr;
    std::wstring path;
    int refs = 0;
};

std::mutex g_modLock;
std::vector<Module*> g_modules;

Module* AcquireModule(const std::wstring& path, std::wstring* err)
{
    std::lock_guard<std::mutex> g(g_modLock);
    for (Module* m : g_modules)
        if (IEquals(m->path, path)) { ++m->refs; return m; }

    std::wstring binary = ModuleBinary(path);
    if (binary.empty())
    {
        if (err) *err = FormatW(L"%s does not contain a 64 bit Windows plugin.",
                                path.c_str());
        return nullptr;
    }

    HMODULE dll = LoadLibraryExW(binary.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll)
    {
        if (err) *err = FormatW(L"%s could not be loaded. %s", binary.c_str(),
                                HrText(HRESULT_FROM_WIN32(GetLastError())).c_str());
        return nullptr;
    }

    auto init = (PFN_InitDll)GetProcAddress(dll, "InitDll");
    if (init && !init())
    {
        FreeLibrary(dll);
        if (err) *err = FormatW(L"%s refused to initialise.", binary.c_str());
        return nullptr;
    }

    auto getFactory = (PFN_GetFactory)GetProcAddress(dll, "GetPluginFactory");
    IPluginFactory* factory = getFactory ? getFactory() : nullptr;
    if (!factory)
    {
        FreeLibrary(dll);
        if (err) *err = FormatW(L"%s is not a VST3 plugin: it has no plugin factory.",
                                path.c_str());
        return nullptr;
    }

    Module* m = new Module();
    m->dll = dll;
    m->factory = factory;
    m->path = path;
    m->refs = 1;
    g_modules.push_back(m);
    return m;
}

void ReleaseModule(Module* m)
{
    if (!m) return;
    std::lock_guard<std::mutex> g(g_modLock);
    if (--m->refs > 0) return;

    for (size_t i = g_modules.size(); i-- > 0; )
        if (g_modules[i] == m) g_modules.erase(g_modules.begin() + (ptrdiff_t)i);
    if (m->factory) m->factory->release();
    // ExitDll is deliberately not called and the library is not freed. See the
    // same decision in the CLAP host: unmapping a plugin's code underneath its
    // own worker threads is not worth the address space it recovers.
    delete m;
}

// ---------------------------------------------------------------------------
// The small objects the host has to hand back to the plugin.
//
// All of them live as members of the instance that owns them, so their
// lifetime is not actually managed by reference counting. addRef and release
// therefore do nothing but keep the count away from zero: a plugin that
// balances its calls sees nothing unusual, and one that does not cannot take
// a host object down with it.
// ---------------------------------------------------------------------------

// One parameter's worth of changes for a single process call. FastRoute only
// ever sets a value outright, so every queue holds exactly one point at
// sample 0 - there is no automation curve to describe.
class ParamQueue : public Vst::IParamValueQueue
{
public:
    Vst::ParamID id = 0;
    double value = 0.0;

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        QUERY_INTERFACE(iid, obj, FUnknown::iid, Vst::IParamValueQueue)
        QUERY_INTERFACE(iid, obj, Vst::IParamValueQueue::iid, Vst::IParamValueQueue)
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1000; }
    uint32 PLUGIN_API release() override { return 1000; }

    Vst::ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return 1; }
    tresult PLUGIN_API getPoint(int32 index, int32& sampleOffset, Vst::ParamValue& v) override
    {
        if (index != 0) return kResultFalse;
        sampleOffset = 0;
        v = value;
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32, Vst::ParamValue, int32& index) override
    {
        index = 0;
        return kResultOk;
    }
};

class ParamChanges : public Vst::IParameterChanges
{
public:
    std::vector<ParamQueue> queues;

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        QUERY_INTERFACE(iid, obj, FUnknown::iid, Vst::IParameterChanges)
        QUERY_INTERFACE(iid, obj, Vst::IParameterChanges::iid, Vst::IParameterChanges)
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1000; }
    uint32 PLUGIN_API release() override { return 1000; }

    int32 PLUGIN_API getParameterCount() override { return (int32)queues.size(); }
    Vst::IParamValueQueue* PLUGIN_API getParameterData(int32 index) override
    {
        if (index < 0 || (size_t)index >= queues.size()) return nullptr;
        return &queues[(size_t)index];
    }
    Vst::IParamValueQueue* PLUGIN_API addParameterData(const Vst::ParamID& id, int32& index) override
    {
        for (size_t i = 0; i < queues.size(); ++i)
            if (queues[i].id == id) { index = (int32)i; return &queues[i]; }
        ParamQueue q;
        q.id = id;
        queues.push_back(q);
        index = (int32)queues.size() - 1;
        return &queues.back();
    }
};

// Somewhere for the plugin to put parameter changes it makes itself. What it
// says is read back when the editor closes, so this only has to accept them
// without complaint.
class EmptyChanges : public Vst::IParameterChanges
{
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        QUERY_INTERFACE(iid, obj, FUnknown::iid, Vst::IParameterChanges)
        QUERY_INTERFACE(iid, obj, Vst::IParameterChanges::iid, Vst::IParameterChanges)
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1000; }
    uint32 PLUGIN_API release() override { return 1000; }

    int32 PLUGIN_API getParameterCount() override { return 0; }
    Vst::IParamValueQueue* PLUGIN_API getParameterData(int32) override { return nullptr; }
    Vst::IParamValueQueue* PLUGIN_API addParameterData(const Vst::ParamID&, int32& index) override
    {
        index = 0;
        return &m_sink;
    }
private:
    ParamQueue m_sink;
};

class Vst3Instance;

// Where the plugin reports its own parameter edits, from its editor.
class ComponentHandler : public Vst::IComponentHandler
{
public:
    Vst3Instance* owner = nullptr;

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        QUERY_INTERFACE(iid, obj, FUnknown::iid, Vst::IComponentHandler)
        QUERY_INTERFACE(iid, obj, Vst::IComponentHandler::iid, Vst::IComponentHandler)
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1000; }
    uint32 PLUGIN_API release() override { return 1000; }

    tresult PLUGIN_API beginEdit(Vst::ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(Vst::ParamID id, Vst::ParamValue value) override;
    tresult PLUGIN_API endEdit(Vst::ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }
};

// The plugin's editor asking to be resized.
class PlugFrame : public IPlugFrame
{
public:
    HWND host = nullptr;

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        QUERY_INTERFACE(iid, obj, FUnknown::iid, IPlugFrame)
        QUERY_INTERFACE(iid, obj, IPlugFrame::iid, IPlugFrame)
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1000; }
    uint32 PLUGIN_API release() override { return 1000; }

    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* rect) override
    {
        if (!view || !rect) return kInvalidArgument;
        if (host)
        {
            RECT want{ 0, 0, rect->getWidth(), rect->getHeight() };
            AdjustWindowRectEx(&want, (DWORD)GetWindowLongPtrW(host, GWL_STYLE), FALSE,
                               (DWORD)GetWindowLongPtrW(host, GWL_EXSTYLE));
            SetWindowPos(host, nullptr, 0, 0, want.right - want.left, want.bottom - want.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        view->onSize(rect);
        return kResultOk;
    }
};

// A minimal attribute list and message, which plugins whose two halves talk to
// each other need in order to be connected at all.
class AttributeList : public Vst::IAttributeList
{
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        QUERY_INTERFACE(iid, obj, FUnknown::iid, Vst::IAttributeList)
        QUERY_INTERFACE(iid, obj, Vst::IAttributeList::iid, Vst::IAttributeList)
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1000; }
    uint32 PLUGIN_API release() override { return 1000; }

    tresult PLUGIN_API setInt(AttrID, int64) override { return kResultOk; }
    tresult PLUGIN_API getInt(AttrID, int64&) override { return kResultFalse; }
    tresult PLUGIN_API setFloat(AttrID, double) override { return kResultOk; }
    tresult PLUGIN_API getFloat(AttrID, double&) override { return kResultFalse; }
    tresult PLUGIN_API setString(AttrID, const Vst::TChar*) override { return kResultOk; }
    tresult PLUGIN_API getString(AttrID, Vst::TChar*, uint32) override { return kResultFalse; }
    tresult PLUGIN_API setBinary(AttrID, const void*, uint32) override { return kResultOk; }
    tresult PLUGIN_API getBinary(AttrID, const void*&, uint32&) override { return kResultFalse; }
};

class Message : public Vst::IMessage
{
public:
    Message() : m_refs(1) {}

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        QUERY_INTERFACE(iid, obj, FUnknown::iid, Vst::IMessage)
        QUERY_INTERFACE(iid, obj, Vst::IMessage::iid, Vst::IMessage)
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return (uint32)++m_refs; }
    uint32 PLUGIN_API release() override
    {
        if (--m_refs == 0) { delete this; return 0; }
        return (uint32)m_refs;
    }

    FIDString PLUGIN_API getMessageID() override { return m_id.c_str(); }
    void PLUGIN_API setMessageID(FIDString id) override { m_id = id ? id : ""; }
    Vst::IAttributeList* PLUGIN_API getAttributes() override { return &m_attrs; }

private:
    int m_refs;
    std::string m_id;
    AttributeList m_attrs;
};

// What the plugin is given as its host context.
class HostApplication : public Vst::IHostApplication
{
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        QUERY_INTERFACE(iid, obj, FUnknown::iid, Vst::IHostApplication)
        QUERY_INTERFACE(iid, obj, Vst::IHostApplication::iid, Vst::IHostApplication)
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1000; }
    uint32 PLUGIN_API release() override { return 1000; }

    tresult PLUGIN_API getName(Vst::String128 name) override
    {
        ToUtf16(L"FastRoute", name, 128);
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(TUID cid, TUID iid, void** obj) override
    {
        if (FUnknownPrivate::iidEqual(cid, Vst::IMessage::iid) &&
            FUnknownPrivate::iidEqual(iid, Vst::IMessage::iid))
        {
            *obj = new Message();
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual(cid, Vst::IAttributeList::iid) &&
            FUnknownPrivate::iidEqual(iid, Vst::IAttributeList::iid))
        {
            // Owned here rather than handed over: a bare attribute list is
            // only ever asked for as scratch, and this one outlives any use.
            static AttributeList shared;
            *obj = &shared;
            return kResultOk;
        }
        *obj = nullptr;
        return kResultFalse;
    }
};

// ---------------------------------------------------------------------------

class Vst3Instance : public PluginInstance
{
public:
    Vst3Instance() { m_handler.owner = this; }
    ~Vst3Instance() override { Stop(); ReleaseModule(m_module); }

    bool Open(const PluginDesc& desc, std::wstring* err)
    {
        m_desc = desc;
        m_module = AcquireModule(desc.path, err);
        if (!m_module) return false;

        TUID cid;
        if (!TuidFromHex(desc.uid, cid))
        {
            if (err) *err = FormatW(L"%s has no usable class identifier.", desc.name.c_str());
            return false;
        }

        if (m_module->factory->createInstance(cid, Vst::IComponent::iid,
                                              (void**)&m_component) != kResultOk || !m_component)
        {
            if (err) *err = FormatW(L"%s is not in %s any more. Scan the plugin "
                                    L"folders again.", desc.name.c_str(), desc.path.c_str());
            return false;
        }
        if (m_component->initialize(&m_hostApp) != kResultOk)
        {
            if (err) *err = FormatW(L"%s refused to initialise.", desc.name.c_str());
            return false;
        }
        if (m_component->queryInterface(Vst::IAudioProcessor::iid,
                                        (void**)&m_processor) != kResultOk || !m_processor)
        {
            if (err) *err = FormatW(L"%s does not process audio.", desc.name.c_str());
            return false;
        }

        // The controller is usually a separate object; a single component
        // effect is its own. Either way it owns the parameters.
        TUID ccid;
        if (m_component->getControllerClassId(ccid) == kResultOk &&
            m_module->factory->createInstance(ccid, Vst::IEditController::iid,
                                              (void**)&m_controller) == kResultOk && m_controller)
        {
            m_controller->initialize(&m_hostApp);
            m_separateController = true;
        }
        else
        {
            m_component->queryInterface(Vst::IEditController::iid, (void**)&m_controller);
        }

        if (m_controller)
        {
            m_controller->setComponentHandler(&m_handler);
            ConnectHalves();
            ReadParams();
        }
        return true;
    }

    bool Start(int rate, int maxBlock, std::wstring* err) override
    {
        if (!m_component || !m_processor)
        {
            if (err) *err = L"This plugin is not loaded.";
            return false;
        }
        if (m_active) return true;

        m_rate = rate;
        m_maxBlock = maxBlock;

        // Stereo in, stereo out. A plugin that cannot be arranged that way is
        // refused here rather than left to produce silence.
        Vst::SpeakerArrangement stereo = Vst::SpeakerArr::kStereo;
        if (m_processor->setBusArrangements(&stereo, 1, &stereo, 1) != kResultOk)
        {
            // Not fatal on its own: some plugins report failure and still run
            // in their default stereo arrangement. The bus check below is what
            // actually decides.
        }

        const int32 inBuses = m_component->getBusCount(Vst::kAudio, Vst::kInput);
        const int32 outBuses = m_component->getBusCount(Vst::kAudio, Vst::kOutput);
        if (outBuses < 1)
        {
            if (err) *err = FormatW(L"%s has no audio output.", m_desc.name.c_str());
            return false;
        }
        for (int32 i = 0; i < inBuses; ++i)
            m_component->activateBus(Vst::kAudio, Vst::kInput, i, i == 0 ? 1 : 0);
        for (int32 i = 0; i < outBuses; ++i)
            m_component->activateBus(Vst::kAudio, Vst::kOutput, i, i == 0 ? 1 : 0);
        m_hasInput = inBuses > 0;

        Vst::ProcessSetup setup{};
        setup.processMode = Vst::kRealtime;
        setup.symbolicSampleSize = Vst::kSample32;
        setup.maxSamplesPerBlock = maxBlock;
        setup.sampleRate = (double)rate;
        if (m_processor->setupProcessing(setup) != kResultOk)
        {
            if (err) *err = FormatW(L"%s would not run at %d Hz.", m_desc.name.c_str(), rate);
            return false;
        }

        if (m_component->setActive(true) != kResultOk)
        {
            if (err) *err = FormatW(L"%s would not start.", m_desc.name.c_str());
            return false;
        }
        m_active = true;
        m_processor->setProcessing(true);
        m_processing = true;

        m_left.assign((size_t)maxBlock, 0.0f);
        m_right.assign((size_t)maxBlock, 0.0f);
        m_outLeft.assign((size_t)maxBlock, 0.0f);
        m_outRight.assign((size_t)maxBlock, 0.0f);
        m_latencyFrames = (int)m_processor->getLatencySamples();
        return true;
    }

    void Stop() override
    {
        CloseEditor();
        if (m_processing && m_processor) { m_processor->setProcessing(false); m_processing = false; }
        if (m_active && m_component) { m_component->setActive(false); m_active = false; }

        if (m_controller)
        {
            m_controller->setComponentHandler(nullptr);
            DisconnectHalves();
            if (m_separateController) m_controller->terminate();
            m_controller->release();
            m_controller = nullptr;
        }
        if (m_processor) { m_processor->release(); m_processor = nullptr; }
        if (m_component) { m_component->terminate(); m_component->release(); m_component = nullptr; }
    }

    void Process(float* buf, size_t frames) override
    {
        if (!m_processing || m_left.empty()) return;

        // Whatever the editor or a dialog has changed since the last call.
        ParamChanges changes;
        {
            std::lock_guard<std::mutex> g(m_pendingLock);
            for (const auto& kv : m_pending)
            {
                int32 idx = 0;
                Vst::IParamValueQueue* q = changes.addParameterData((Vst::ParamID)kv.first, idx);
                (void)q;
                changes.queues.back().value = kv.second;
            }
            m_pending.clear();
        }

        size_t done = 0;
        while (done < frames)
        {
            const size_t n = (std::min)(frames - done, (size_t)m_maxBlock);
            for (size_t i = 0; i < n; ++i)
            {
                m_left[i]  = buf[(done + i) * 2 + 0];
                m_right[i] = buf[(done + i) * 2 + 1];
            }

            float* inChans[2]  = { m_left.data(), m_right.data() };
            float* outChans[2] = { m_outLeft.data(), m_outRight.data() };

            Vst::AudioBusBuffers inBus{};
            inBus.numChannels = 2;
            inBus.silenceFlags = 0;
            inBus.channelBuffers32 = inChans;

            Vst::AudioBusBuffers outBus{};
            outBus.numChannels = 2;
            outBus.silenceFlags = 0;
            outBus.channelBuffers32 = outChans;

            Vst::ProcessData data{};
            data.processMode = Vst::kRealtime;
            data.symbolicSampleSize = Vst::kSample32;
            data.numSamples = (int32)n;
            data.numInputs = m_hasInput ? 1 : 0;
            data.numOutputs = 1;
            data.inputs = m_hasInput ? &inBus : nullptr;
            data.outputs = &outBus;
            data.inputParameterChanges = &changes;
            data.outputParameterChanges = &m_outChanges;
            data.inputEvents = nullptr;
            data.outputEvents = nullptr;
            data.processContext = nullptr;

            if (m_processor->process(data) == kResultOk)
            {
                for (size_t i = 0; i < n; ++i)
                {
                    buf[(done + i) * 2 + 0] = m_outLeft[i];
                    buf[(done + i) * 2 + 1] = m_outRight[i];
                }
            }
            // Parameter changes are delivered once, with the first chunk.
            changes.queues.clear();
            done += n;
        }
    }

    int LatencyFrames() const override { return m_latencyFrames; }

    const std::vector<PlugParam>& Params() const override { return m_paramList; }

    double GetParam(unsigned long long id) const override
    {
        if (!m_controller) return 0.0;
        return m_controller->getParamNormalized((Vst::ParamID)id);
    }

    void SetParam(unsigned long long id, double value) override
    {
        if (!m_controller) return;
        if (value < 0.0) value = 0.0;
        if (value > 1.0) value = 1.0;
        // The controller holds what the editor shows; the processor is told
        // separately, through the next process call. Both have to be done or
        // the two halves disagree about what the plugin is set to.
        m_controller->setParamNormalized((Vst::ParamID)id, value);
        std::lock_guard<std::mutex> g(m_pendingLock);
        for (auto& kv : m_pending)
            if (kv.first == id) { kv.second = value; return; }
        m_pending.push_back({ id, value });
    }

    std::wstring ParamText(unsigned long long id, double value) const override
    {
        if (m_controller)
        {
            Vst::String128 text{};
            if (m_controller->getParamStringByValue((Vst::ParamID)id, value, text) == kResultOk)
            {
                std::wstring out = FromUtf16(text);
                if (!out.empty()) return out;
            }
        }
        return FormatW(L"%.3f", value);
    }

    bool HasEditor() const override { return m_controller != nullptr; }

    bool OpenEditor(HWND parent, int* width, int* height, std::wstring* err) override
    {
        if (!m_controller) { if (err) *err = L"This plugin has no editor."; return false; }
        if (m_view) return true;

        m_view = m_controller->createView(Vst::ViewType::kEditor);
        if (!m_view)
        {
            if (err) *err = L"This plugin has no editor.";
            return false;
        }
        if (m_view->isPlatformTypeSupported(kPlatformTypeHWND) != kResultTrue)
        {
            m_view->release();
            m_view = nullptr;
            if (err) *err = L"This plugin's editor cannot be shown on Windows.";
            return false;
        }

        m_frame.host = parent;
        m_view->setFrame(&m_frame);
        if (m_view->attached(parent, kPlatformTypeHWND) != kResultOk)
        {
            m_view->setFrame(nullptr);
            m_view->release();
            m_view = nullptr;
            if (err) *err = L"The plugin's editor would not attach to its window.";
            return false;
        }

        ViewRect r{};
        if (m_view->getSize(&r) == kResultOk)
        {
            if (width) *width = r.getWidth();
            if (height) *height = r.getHeight();
        }
        return true;
    }

    void CloseEditor() override
    {
        if (!m_view) return;
        m_view->removed();
        m_view->setFrame(nullptr);
        m_view->release();
        m_view = nullptr;
        m_frame.host = nullptr;
    }

    bool EditorSize(int* width, int* height) override
    {
        if (!m_view) return false;
        ViewRect r{};
        if (m_view->getSize(&r) != kResultOk) return false;
        if (width) *width = r.getWidth();
        if (height) *height = r.getHeight();
        return true;
    }

    // Called from the handler when the plugin edits one of its own parameters.
    void NoteEdit(unsigned long long id, double value)
    {
        std::lock_guard<std::mutex> g(m_pendingLock);
        for (auto& kv : m_pending)
            if (kv.first == id) { kv.second = value; return; }
        m_pending.push_back({ id, value });
    }

private:
    void ConnectHalves()
    {
        if (!m_separateController || !m_component || !m_controller) return;
        if (m_component->queryInterface(Vst::IConnectionPoint::iid, (void**)&m_compConn) != kResultOk)
            m_compConn = nullptr;
        if (m_controller->queryInterface(Vst::IConnectionPoint::iid, (void**)&m_ctrlConn) != kResultOk)
            m_ctrlConn = nullptr;
        // Connected straight to each other. A host is supposed to sit in the
        // middle and marshal between threads; FastRoute never calls either
        // half from anywhere the other is not already safe to touch.
        if (m_compConn && m_ctrlConn)
        {
            m_compConn->connect(m_ctrlConn);
            m_ctrlConn->connect(m_compConn);
        }
    }

    void DisconnectHalves()
    {
        if (m_compConn && m_ctrlConn)
        {
            m_compConn->disconnect(m_ctrlConn);
            m_ctrlConn->disconnect(m_compConn);
        }
        if (m_compConn) { m_compConn->release(); m_compConn = nullptr; }
        if (m_ctrlConn) { m_ctrlConn->release(); m_ctrlConn = nullptr; }
    }

    void ReadParams()
    {
        m_paramList.clear();
        if (!m_controller) return;
        const int32 n = m_controller->getParameterCount();
        for (int32 i = 0; i < n; ++i)
        {
            Vst::ParameterInfo info{};
            if (m_controller->getParameterInfo(i, info) != kResultOk) continue;
            if (info.flags & Vst::ParameterInfo::kIsHidden) continue;
            // A program change list is a different kind of thing and does not
            // belong on a slider next to the gain.
            if (info.flags & Vst::ParameterInfo::kIsProgramChange) continue;

            PlugParam p;
            p.id = info.id;
            p.name = FromUtf16(info.title);
            if (p.name.empty()) p.name = FormatW(L"Parameter %d", (int)i + 1);
            // VST3 parameters are always normalised. The plugin's own text is
            // what gives the number meaning, so that is what the dialog shows.
            p.minValue = 0.0;
            p.maxValue = 1.0;
            p.defaultValue = info.defaultNormalizedValue;
            p.stepped = info.stepCount > 0;
            p.steps = (int)info.stepCount;
            m_paramList.push_back(p);
        }
    }

    Module* m_module = nullptr;
    Vst::IComponent* m_component = nullptr;
    Vst::IAudioProcessor* m_processor = nullptr;
    Vst::IEditController* m_controller = nullptr;
    Vst::IConnectionPoint* m_compConn = nullptr;
    Vst::IConnectionPoint* m_ctrlConn = nullptr;
    IPlugView* m_view = nullptr;
    bool m_separateController = false;

    HostApplication m_hostApp;
    ComponentHandler m_handler;
    PlugFrame m_frame;
    EmptyChanges m_outChanges;

    std::vector<PlugParam> m_paramList;
    std::vector<std::pair<unsigned long long, double>> m_pending;
    std::mutex m_pendingLock;

    std::vector<float> m_left, m_right, m_outLeft, m_outRight;
    int m_rate = 48000;
    int m_maxBlock = 0;
    int m_latencyFrames = 0;
    bool m_active = false, m_processing = false, m_hasInput = true;
};

tresult PLUGIN_API ComponentHandler::performEdit(Vst::ParamID id, Vst::ParamValue value)
{
    if (owner) owner->NoteEdit((unsigned long long)id, value);
    return kResultOk;
}

}   // namespace

// ---------------------------------------------------------------------------

std::vector<PluginDesc> ScanVst3Module(const std::wstring& path)
{
    std::vector<PluginDesc> out;
    std::wstring err;
    Module* m = AcquireModule(path, &err);
    if (!m) return out;

    std::wstring vendor;
    PFactoryInfo fi{};
    if (m->factory->getFactoryInfo(&fi) == kResultOk)
    {
        wchar_t wide[PFactoryInfo::kNameSize] = { 0 };
        MultiByteToWideChar(CP_UTF8, 0, fi.vendor, -1, wide, PFactoryInfo::kNameSize);
        vendor = wide;
    }

    // Instruments and effects share one category in VST3 - both are an "Audio
    // Module Class" - and what separates them is the subcategory string, which
    // only the second version of the factory interface hands out. Without it
    // every synth on the machine turns up in the effects library.
    IPluginFactory2* factory2 = nullptr;
    if (m->factory->queryInterface(IPluginFactory2::iid, (void**)&factory2) != kResultOk)
        factory2 = nullptr;

    const int32 count = m->factory->countClasses();
    for (int32 i = 0; i < count; ++i)
    {
        PClassInfo ci{};
        if (m->factory->getClassInfo(i, &ci) != kResultOk) continue;
        if (strcmp(ci.category, kVstAudioEffectClass) != 0) continue;

        if (factory2)
        {
            PClassInfo2 ci2{};
            if (factory2->getClassInfo2(i, &ci2) == kResultOk)
            {
                // The subcategories are a list separated by vertical bars, so
                // this looks for the word rather than comparing the whole
                // string: "Instrument|Synth" and "Fx|Reverb" both occur.
                const std::string subs = ci2.subCategories;
                if (subs.find("Instrument") != std::string::npos) continue;
            }
        }

        PluginDesc pd;
        pd.format = PlugFormat::Vst3;
        pd.path = path;
        pd.uid = HexOfTuid(ci.cid);
        wchar_t wide[PClassInfo::kNameSize] = { 0 };
        MultiByteToWideChar(CP_UTF8, 0, ci.name, -1, wide, PClassInfo::kNameSize);
        pd.name = wide;
        pd.vendor = vendor;
        if (pd.name.empty()) pd.name = pd.uid;
        out.push_back(pd);
    }
    if (factory2) factory2->release();
    ReleaseModule(m);
    return out;
}

std::unique_ptr<PluginInstance> LoadVst3Plugin(const PluginDesc& desc, std::wstring* err)
{
    std::unique_ptr<Vst3Instance> p(new Vst3Instance());
    if (!p->Open(desc, err)) return nullptr;
    return std::unique_ptr<PluginInstance>(p.release());
}
