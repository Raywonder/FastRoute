#include "audio.h"
#include "util.h"

#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <shlwapi.h>

// ---------------------------------------------------------------------------

static std::wstring GetEndpointFriendlyName(IMMDevice* dev)
{
    std::wstring name;
    IPropertyStore* props = nullptr;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props)
    {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
            name = pv.pwszVal;
        PropVariantClear(&pv);
        props->Release();
    }
    return name;
}

std::vector<AudioEndpoint> EnumEndpoints(bool render)
{
    std::vector<AudioEndpoint> out;

    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&en))) || !en)
        return out;

    EDataFlow flow = render ? eRender : eCapture;

    std::wstring defaultId;
    IMMDevice* def = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eMultimedia, &def)) && def)
    {
        LPWSTR id = nullptr;
        if (SUCCEEDED(def->GetId(&id)) && id) { defaultId = id; CoTaskMemFree(id); }
        def->Release();
    }

    IMMDeviceCollection* coll = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll)) && coll)
    {
        UINT count = 0;
        coll->GetCount(&count);
        for (UINT i = 0; i < count; ++i)
        {
            IMMDevice* dev = nullptr;
            if (FAILED(coll->Item(i, &dev)) || !dev) continue;

            AudioEndpoint ep;
            LPWSTR id = nullptr;
            if (SUCCEEDED(dev->GetId(&id)) && id) { ep.id = id; CoTaskMemFree(id); }
            ep.name = GetEndpointFriendlyName(dev);
            ep.isDefault = (!ep.id.empty() && ep.id == defaultId);
            if (!ep.id.empty()) out.push_back(ep);

            dev->Release();
        }
        coll->Release();
    }
    en->Release();
    return out;
}

HRESULT OpenEndpoint(const std::wstring& id, bool render, IMMDevice** out)
{
    *out = nullptr;
    IMMDeviceEnumerator* en = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&en));
    if (FAILED(hr)) return hr;

    if (id.empty())
        hr = en->GetDefaultAudioEndpoint(render ? eRender : eCapture, eMultimedia, out);
    else
        hr = en->GetDevice(id.c_str(), out);

    en->Release();
    return hr;
}

bool QueryEndpointFormat(const std::wstring& id, bool render, int* rate, int* channels)
{
    IMMDevice* dev = nullptr;
    if (FAILED(OpenEndpoint(id, render, &dev)) || !dev) return false;

    bool ok = false;
    IAudioClient* client = nullptr;
    if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                (void**)&client)) && client)
    {
        WAVEFORMATEX* mix = nullptr;
        if (SUCCEEDED(client->GetMixFormat(&mix)) && mix)
        {
            if (rate)     *rate = (int)mix->nSamplesPerSec;
            if (channels) *channels = (int)mix->nChannels;
            ok = true;
            CoTaskMemFree(mix);
        }
        client->Release();
    }
    dev->Release();
    return ok;
}

std::wstring EndpointNameById(const std::wstring& id, bool render)
{
    if (id.empty()) return L"System default";
    for (const AudioEndpoint& ep : EnumEndpoints(render))
        if (ep.id == id) return ep.name;
    return L"";
}

// ---------------------------------------------------------------------------

static std::wstring ExeNameForPid(DWORD pid)
{
    std::wstring name;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h)
    {
        wchar_t path[MAX_PATH] = { 0 };
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &n))
            name = PathFindFileNameW(path);
        CloseHandle(h);
    }
    return name;
}

unsigned long FindProcessIdByName(const std::wstring& exeName)
{
    if (exeName.empty()) return 0;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    DWORD best = 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (IEquals(pe.szExeFile, exeName))
            {
                // Prefer the process that actually owns an audio session; failing
                // that, take the first match.
                if (!best) best = pe.th32ProcessID;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    // If several instances exist, prefer one currently playing audio.
    for (const AudioProcess& ap : EnumAudioProcesses())
        if (IEquals(ap.exeName, exeName)) return ap.pid;

    return best;
}

std::vector<ProcessEntry> EnumProcessTree()
{
    std::vector<ProcessEntry> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
        do {
            ProcessEntry e;
            e.pid = pe.th32ProcessID;
            e.parent = pe.th32ParentProcessID;
            e.exeName = pe.szExeFile;
            out.push_back(e);
        } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return out;
}

std::wstring DefaultEndpointId(bool render)
{
    IMMDevice* dev = nullptr;
    if (FAILED(OpenEndpoint(L"", render, &dev)) || !dev) return L"";

    std::wstring id;
    LPWSTR raw = nullptr;
    if (SUCCEEDED(dev->GetId(&raw)) && raw)
    {
        id = raw;
        CoTaskMemFree(raw);
    }
    dev->Release();
    return id;
}

bool WouldCaptureSelf(unsigned long pid)
{
    DWORD self = GetCurrentProcessId();
    if (pid == self) return true;

    // Walk our own ancestry: if the target is anywhere above us, a tree
    // capture takes in this process too, and the session captures its own
    // output back into itself.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    std::vector<std::pair<DWORD, DWORD>> parents;   // pid -> parent pid
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
        do { parents.push_back({ pe.th32ProcessID, pe.th32ParentProcessID }); }
        while (Process32NextW(snap, &pe));
    CloseHandle(snap);

    DWORD cur = self;
    for (int hop = 0; hop < 64 && cur != 0; ++hop)
    {
        DWORD parent = 0;
        for (const auto& e : parents) if (e.first == cur) { parent = e.second; break; }
        if (parent == 0) break;
        if (parent == pid) return true;
        cur = parent;
    }
    return false;
}

// The sessions on one endpoint, appended to `out`. `skipExpired` drops the
// sessions Windows keeps around after a process has stopped using the device -
// which is how an application that has moved to another endpoint leaves its
// mark on the one it left.
static void AppendSessionsOn(IMMDevice* dev, bool skipExpired,
                             std::vector<AudioProcess>& out)
{
    if (!dev) return;

    IAudioSessionManager2* mgr = nullptr;
    if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                             nullptr, (void**)&mgr)) || !mgr)
        return;

    IAudioSessionEnumerator* se = nullptr;
    if (SUCCEEDED(mgr->GetSessionEnumerator(&se)) && se)
    {
        int count = 0;
        se->GetCount(&count);
        for (int i = 0; i < count; ++i)
        {
            IAudioSessionControl* ctl = nullptr;
            if (FAILED(se->GetSession(i, &ctl)) || !ctl) continue;

            // Inactive is kept and expired is not: a paused player is still on
            // this device and should come back when it resumes, whereas an
            // expired session is one the process has finished with.
            bool skip = false;
            if (skipExpired)
            {
                AudioSessionState st = AudioSessionStateInactive;
                if (SUCCEEDED(ctl->GetState(&st)) && st == AudioSessionStateExpired)
                    skip = true;
            }

            IAudioSessionControl2* ctl2 = nullptr;
            if (!skip && SUCCEEDED(ctl->QueryInterface(IID_PPV_ARGS(&ctl2))) && ctl2)
            {
                DWORD pid = 0;
                if (SUCCEEDED(ctl2->GetProcessId(&pid)) && pid != 0 &&
                    ctl2->IsSystemSoundsSession() != S_OK)
                {
                    AudioProcess ap;
                    ap.pid = pid;
                    ap.exeName = ExeNameForPid(pid);

                    LPWSTR disp = nullptr;
                    if (SUCCEEDED(ctl2->GetDisplayName(&disp)) && disp)
                    {
                        ap.displayName = disp;
                        CoTaskMemFree(disp);
                    }

                    bool dup = false;
                    for (const AudioProcess& e : out)
                        if (e.pid == ap.pid) dup = true;
                    if (!dup && !ap.exeName.empty()) out.push_back(ap);
                }
                ctl2->Release();
            }
            ctl->Release();
        }
        se->Release();
    }
    mgr->Release();
}

std::vector<AudioProcess> EnumAudioProcesses()
{
    std::vector<AudioProcess> out;

    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&en))) || !en)
        return out;

    IMMDeviceCollection* coll = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)) && coll)
    {
        UINT devCount = 0;
        coll->GetCount(&devCount);
        for (UINT d = 0; d < devCount; ++d)
        {
            IMMDevice* dev = nullptr;
            if (FAILED(coll->Item(d, &dev)) || !dev) continue;
            AppendSessionsOn(dev, false, out);
            dev->Release();
        }
        coll->Release();
    }
    en->Release();
    return out;
}

std::vector<AudioProcess> EnumAudioProcessesOn(const std::wstring& endpointId)
{
    std::vector<AudioProcess> out;
    IMMDevice* dev = nullptr;
    if (FAILED(OpenEndpoint(endpointId, true, &dev)) || !dev) return out;
    AppendSessionsOn(dev, true, out);
    dev->Release();
    return out;
}
