#include "plugins.h"
#include "../util.h"

#include <clap/clap.h>
#include <algorithm>
#include <cstring>
#include <mutex>

// ---------------------------------------------------------------------------
// CLAP hosting
//
// CLAP is a plain C ABI: a module exports one symbol, `clap_entry`, and
// everything else hangs off structures of function pointers. There is no COM,
// no reference counting and no registry, which is why this file is a good deal
// shorter than its VST3 counterpart for the same job.
//
// The host side of the contract is a `clap_host` with a handful of callbacks.
// Only what a plugin can reasonably expect is implemented; the rest are
// answered honestly with null, which the specification allows and plugins are
// required to cope with.
// ---------------------------------------------------------------------------

namespace {

std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring Widen(const char* s)
{
    if (!s || !*s) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring out((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], n);
    return out;
}

// A loaded .clap module. Modules are reference counted because two blocks may
// use two plugins out of the same file, and `clap_entry::deinit` must be
// called exactly once, after the last of them has gone.
struct Module
{
    HMODULE dll = nullptr;
    const clap_plugin_entry_t* entry = nullptr;
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

    HMODULE dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll)
    {
        if (err) *err = FormatW(L"%s could not be loaded. %s", path.c_str(),
                                HrText(HRESULT_FROM_WIN32(GetLastError())).c_str());
        return nullptr;
    }

    auto entry = (const clap_plugin_entry_t*)GetProcAddress(dll, "clap_entry");
    if (!entry || !entry->init)
    {
        FreeLibrary(dll);
        if (err) *err = FormatW(L"%s is not a CLAP plugin: it has no clap_entry.",
                                path.c_str());
        return nullptr;
    }
    if (!clap_version_is_compatible(entry->clap_version))
    {
        FreeLibrary(dll);
        if (err) *err = FormatW(L"%s was built for a later version of CLAP than "
                                L"FastRoute understands.", path.c_str());
        return nullptr;
    }
    if (!entry->init(Narrow(path).c_str()))
    {
        FreeLibrary(dll);
        if (err) *err = FormatW(L"%s refused to initialise.", path.c_str());
        return nullptr;
    }

    Module* m = new Module();
    m->dll = dll;
    m->entry = entry;
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
    if (m->entry && m->entry->deinit) m->entry->deinit();
    // The library is deliberately not freed. A plugin that has spun up a
    // worker thread, registered a window class or left a static alive does not
    // necessarily survive having its code unmapped, and the cost of keeping it
    // is a few hundred kilobytes of address space until FastRoute exits.
    delete m;
}

const clap_plugin_factory_t* FactoryOf(Module* m)
{
    if (!m || !m->entry || !m->entry->get_factory) return nullptr;
    return (const clap_plugin_factory_t*)m->entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
}

// Whether a descriptor is something FastRoute can use as an effect block: it
// has to take audio in and give audio back. Instruments and note effects are
// left out of the library rather than added and then found to be silent.
bool IsAudioEffect(const clap_plugin_descriptor_t* d)
{
    if (!d || !d->features) return false;
    bool effect = false, instrument = false;
    for (const char* const* f = d->features; *f; ++f)
    {
        if (std::strcmp(*f, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT) == 0) effect = true;
        if (std::strcmp(*f, CLAP_PLUGIN_FEATURE_INSTRUMENT) == 0) instrument = true;
        if (std::strcmp(*f, CLAP_PLUGIN_FEATURE_NOTE_EFFECT) == 0) instrument = true;
    }
    // A plugin that declares nothing useful is given the benefit of the doubt:
    // the feature list is advisory and some plugins fill it in poorly.
    return effect || !instrument;
}

// ---------------------------------------------------------------------------

class ClapInstance : public PluginInstance
{
public:
    ClapInstance() { m_host = MakeHost(this); }
    ~ClapInstance() override { Stop(); ReleaseModule(m_module); }

    bool Open(const PluginDesc& desc, std::wstring* err)
    {
        m_desc = desc;
        m_module = AcquireModule(desc.path, err);
        if (!m_module) return false;

        const clap_plugin_factory_t* f = FactoryOf(m_module);
        if (!f)
        {
            if (err) *err = FormatW(L"%s has no CLAP plugin factory.", desc.path.c_str());
            return false;
        }

        m_plugin = f->create_plugin(f, &m_host, Narrow(desc.uid).c_str());
        if (!m_plugin)
        {
            if (err) *err = FormatW(L"%s is not in %s any more. Scan the plugin "
                                    L"folders again.", desc.name.c_str(), desc.path.c_str());
            return false;
        }
        if (!m_plugin->init(m_plugin))
        {
            m_plugin->destroy(m_plugin);
            m_plugin = nullptr;
            if (err) *err = FormatW(L"%s refused to initialise.", desc.name.c_str());
            return false;
        }

        m_params = (const clap_plugin_params_t*)m_plugin->get_extension(m_plugin, CLAP_EXT_PARAMS);
        m_gui    = (const clap_plugin_gui_t*)m_plugin->get_extension(m_plugin, CLAP_EXT_GUI);
        m_latency= (const clap_plugin_latency_t*)m_plugin->get_extension(m_plugin, CLAP_EXT_LATENCY);
        ReadParams();
        return true;
    }

    bool Start(int rate, int maxBlock, std::wstring* err) override
    {
        if (!m_plugin) { if (err) *err = L"This plugin is not loaded."; return false; }
        if (m_active) return true;

        m_rate = rate;
        m_maxBlock = maxBlock;
        if (!m_plugin->activate(m_plugin, (double)rate, 1, (uint32_t)maxBlock))
        {
            if (err) *err = FormatW(L"%s would not run at %d Hz.",
                                    m_desc.name.c_str(), rate);
            return false;
        }
        m_active = true;

        m_left.assign((size_t)maxBlock, 0.0f);
        m_right.assign((size_t)maxBlock, 0.0f);

        if (m_latency && m_latency->get) m_latencyFrames = (int)m_latency->get(m_plugin);

        if (!m_plugin->start_processing(m_plugin))
        {
            m_plugin->deactivate(m_plugin);
            m_active = false;
            if (err) *err = FormatW(L"%s would not start processing.", m_desc.name.c_str());
            return false;
        }
        m_processing = true;
        return true;
    }

    void Stop() override
    {
        if (!m_plugin) return;
        if (m_processing) { m_plugin->stop_processing(m_plugin); m_processing = false; }
        if (m_active) { m_plugin->deactivate(m_plugin); m_active = false; }
        CloseEditor();
        m_plugin->destroy(m_plugin);
        m_plugin = nullptr;
    }

    void Process(float* buf, size_t frames) override
    {
        if (!m_processing || m_left.empty()) return;

        size_t done = 0;
        while (done < frames)
        {
            const size_t n = (std::min)(frames - done, (size_t)m_maxBlock);
            for (size_t i = 0; i < n; ++i)
            {
                m_left[i]  = buf[(done + i) * 2 + 0];
                m_right[i] = buf[(done + i) * 2 + 1];
            }

            float* chans[2] = { m_left.data(), m_right.data() };
            clap_audio_buffer_t audio{};
            audio.data32 = chans;
            audio.channel_count = 2;
            audio.latency = 0;
            audio.constant_mask = 0;

            // The event lists are required even when empty: a plugin is
            // entitled to call size() on them without checking for null.
            clap_input_events_t in{};
            in.ctx = nullptr;
            in.size = [](const clap_input_events_t*) -> uint32_t { return 0; };
            in.get  = [](const clap_input_events_t*, uint32_t) -> const clap_event_header_t* { return nullptr; };

            clap_output_events_t out{};
            out.ctx = nullptr;
            out.try_push = [](const clap_output_events_t*, const clap_event_header_t*) -> bool { return true; };

            clap_process_t p{};
            p.steady_time = m_steady;
            p.frames_count = (uint32_t)n;
            p.transport = nullptr;
            p.audio_inputs = &audio;
            p.audio_inputs_count = 1;
            p.audio_outputs = &audio;      // in place; CLAP allows the same buffer
            p.audio_outputs_count = 1;
            p.in_events = &in;
            p.out_events = &out;

            const clap_process_status st = m_plugin->process(m_plugin, &p);
            if (st == CLAP_PROCESS_ERROR)
            {
                // Leave the audio as it was rather than passing on whatever the
                // plugin left in the buffer.
                done += n;
                m_steady += (int64_t)n;
                continue;
            }

            for (size_t i = 0; i < n; ++i)
            {
                buf[(done + i) * 2 + 0] = m_left[i];
                buf[(done + i) * 2 + 1] = m_right[i];
            }
            done += n;
            m_steady += (int64_t)n;
        }
    }

    int LatencyFrames() const override { return m_latencyFrames; }

    const std::vector<PlugParam>& Params() const override { return m_paramList; }

    double GetParam(unsigned long long id) const override
    {
        if (!m_params || !m_plugin) return 0.0;
        double v = 0.0;
        if (m_params->get_value(m_plugin, (clap_id)id, &v)) return v;
        return 0.0;
    }

    void SetParam(unsigned long long id, double value) override
    {
        // Setting a parameter means sending the plugin an event, and events
        // only arrive through process(). Rather than run a queue across the
        // audio thread, the plugin is taken out of processing, told, and put
        // back: parameter edits here come from a dialog being closed, not from
        // anything happening while audio is flowing.
        if (!m_plugin || !m_params) return;

        clap_event_param_value_t ev{};
        ev.header.size = sizeof(ev);
        ev.header.time = 0;
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.type = CLAP_EVENT_PARAM_VALUE;
        ev.header.flags = 0;
        ev.param_id = (clap_id)id;
        ev.cookie = nullptr;
        ev.note_id = -1;
        ev.port_index = -1;
        ev.channel = -1;
        ev.key = -1;
        ev.value = value;

        FlushEvent(ev);
    }

    std::wstring ParamText(unsigned long long id, double value) const override
    {
        if (m_params && m_plugin && m_params->value_to_text)
        {
            char buf[CLAP_NAME_SIZE] = { 0 };
            if (m_params->value_to_text(m_plugin, (clap_id)id, value, buf, sizeof(buf)))
                return Widen(buf);
        }
        return FormatW(L"%.3f", value);
    }

    bool HasEditor() const override { return m_gui != nullptr; }

    bool OpenEditor(HWND parent, int* width, int* height, std::wstring* err) override
    {
        if (!m_gui || !m_plugin)
        {
            if (err) *err = L"This plugin has no editor.";
            return false;
        }
        if (m_guiOpen) return true;

        if (!m_gui->is_api_supported(m_plugin, CLAP_WINDOW_API_WIN32, false))
        {
            if (err) *err = L"This plugin's editor cannot be shown on Windows.";
            return false;
        }
        if (!m_gui->create(m_plugin, CLAP_WINDOW_API_WIN32, false))
        {
            if (err) *err = L"The plugin's editor would not open.";
            return false;
        }

        m_gui->set_scale(m_plugin, 1.0);

        uint32_t w = 0, h = 0;
        if (!m_gui->get_size(m_plugin, &w, &h)) { w = 640; h = 400; }

        clap_window_t win{};
        win.api = CLAP_WINDOW_API_WIN32;
        win.win32 = parent;
        if (!m_gui->set_parent(m_plugin, &win))
        {
            m_gui->destroy(m_plugin);
            if (err) *err = L"The plugin's editor would not attach to its window.";
            return false;
        }
        m_gui->show(m_plugin);
        m_guiOpen = true;
        if (width) *width = (int)w;
        if (height) *height = (int)h;
        return true;
    }

    void CloseEditor() override
    {
        if (!m_guiOpen || !m_gui || !m_plugin) return;
        m_gui->hide(m_plugin);
        m_gui->destroy(m_plugin);
        m_guiOpen = false;
    }

    bool EditorSize(int* width, int* height) override
    {
        if (!m_guiOpen || !m_gui || !m_plugin) return false;
        uint32_t w = 0, h = 0;
        if (!m_gui->get_size(m_plugin, &w, &h)) return false;
        if (width) *width = (int)w;
        if (height) *height = (int)h;
        return true;
    }

private:
    // Delivers one event by running a single silent, zero length process call.
    // CLAP has no other way in: parameters are events, and events arrive with
    // audio.
    void FlushEvent(const clap_event_param_value_t& ev)
    {
        if (m_active)
        {
            // Active and running: the plugin's own flush is the supported way
            // to deliver events outside the audio thread.
            if (m_params->flush)
            {
                EventList list{ &ev };
                clap_input_events_t in{};
                in.ctx = &list;
                in.size = [](const clap_input_events_t* e) -> uint32_t
                    { return ((EventList*)e->ctx)->one ? 1u : 0u; };
                in.get = [](const clap_input_events_t* e, uint32_t) -> const clap_event_header_t*
                    { return &((EventList*)e->ctx)->one->header; };
                clap_output_events_t out{};
                out.ctx = nullptr;
                out.try_push = [](const clap_output_events_t*, const clap_event_header_t*) -> bool { return true; };
                m_params->flush(m_plugin, &in, &out);
            }
            return;
        }
        // Not active yet. flush is still allowed on an inactive plugin, and is
        // how a stored value is put in place before the session starts.
        if (m_params->flush)
        {
            EventList list{ &ev };
            clap_input_events_t in{};
            in.ctx = &list;
            in.size = [](const clap_input_events_t* e) -> uint32_t
                { return ((EventList*)e->ctx)->one ? 1u : 0u; };
            in.get = [](const clap_input_events_t* e, uint32_t) -> const clap_event_header_t*
                { return &((EventList*)e->ctx)->one->header; };
            clap_output_events_t out{};
            out.ctx = nullptr;
            out.try_push = [](const clap_output_events_t*, const clap_event_header_t*) -> bool { return true; };
            m_params->flush(m_plugin, &in, &out);
        }
    }

    struct EventList { const clap_event_param_value_t* one; };

    void ReadParams()
    {
        m_paramList.clear();
        if (!m_params || !m_plugin) return;
        const uint32_t n = m_params->count(m_plugin);
        for (uint32_t i = 0; i < n; ++i)
        {
            clap_param_info_t info{};
            if (!m_params->get_info(m_plugin, i, &info)) continue;
            if (info.flags & CLAP_PARAM_IS_HIDDEN) continue;

            PlugParam p;
            p.id = info.id;
            p.name = Widen(info.name);
            if (p.name.empty()) p.name = FormatW(L"Parameter %u", i + 1);
            p.minValue = info.min_value;
            p.maxValue = info.max_value;
            p.defaultValue = info.default_value;
            p.stepped = (info.flags & CLAP_PARAM_IS_STEPPED) != 0;
            if (p.stepped) p.steps = (int)(info.max_value - info.min_value);
            m_paramList.push_back(p);
        }
    }

    // --- the host side of the contract -------------------------------------
    static const void* HostExt(const clap_host_t*, const char*) { return nullptr; }
    static void HostRequestRestart(const clap_host_t*) {}
    static void HostRequestProcess(const clap_host_t*) {}
    static void HostRequestCallback(const clap_host_t*) {}

    static clap_host_t MakeHost(ClapInstance* self)
    {
        clap_host_t h{};
        h.clap_version = CLAP_VERSION;
        h.host_data = self;
        h.name = "FastRoute";
        h.vendor = "FastRoute";
        h.url = "";
        h.version = "1.0";
        h.get_extension = HostExt;
        h.request_restart = HostRequestRestart;
        h.request_process = HostRequestProcess;
        h.request_callback = HostRequestCallback;
        return h;
    }

    Module* m_module = nullptr;
    clap_host_t m_host{};
    const clap_plugin_t* m_plugin = nullptr;
    const clap_plugin_params_t* m_params = nullptr;
    const clap_plugin_gui_t* m_gui = nullptr;
    const clap_plugin_latency_t* m_latency = nullptr;

    std::vector<PlugParam> m_paramList;
    std::vector<float> m_left, m_right;
    int  m_rate = 48000;
    int  m_maxBlock = 0;
    int  m_latencyFrames = 0;
    int64_t m_steady = 0;
    bool m_active = false, m_processing = false, m_guiOpen = false;
};

}   // namespace

// ---------------------------------------------------------------------------

std::vector<PluginDesc> ScanClapModule(const std::wstring& path)
{
    std::vector<PluginDesc> out;
    std::wstring err;
    Module* m = AcquireModule(path, &err);
    if (!m) return out;

    const clap_plugin_factory_t* f = FactoryOf(m);
    if (f)
    {
        const uint32_t n = f->get_plugin_count(f);
        for (uint32_t i = 0; i < n; ++i)
        {
            const clap_plugin_descriptor_t* d = f->get_plugin_descriptor(f, i);
            if (!d || !d->id) continue;
            if (!IsAudioEffect(d)) continue;

            PluginDesc pd;
            pd.format = PlugFormat::Clap;
            pd.path = path;
            pd.uid = Widen(d->id);
            pd.name = Widen(d->name);
            pd.vendor = Widen(d->vendor);
            if (pd.name.empty()) pd.name = pd.uid;
            out.push_back(pd);
        }
    }
    ReleaseModule(m);
    return out;
}

std::unique_ptr<PluginInstance> LoadClapPlugin(const PluginDesc& desc, std::wstring* err)
{
    std::unique_ptr<ClapInstance> p(new ClapInstance());
    if (!p->Open(desc, err)) return nullptr;
    return std::unique_ptr<PluginInstance>(p.release());
}
