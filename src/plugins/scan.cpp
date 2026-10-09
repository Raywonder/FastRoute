#include "plugins.h"
#include "../util.h"

#include <shlobj.h>
#include <algorithm>

// ---------------------------------------------------------------------------
// Finding plugins
//
// A plugin folder is walked for .clap files and .vst3 modules, and each one is
// loaded far enough to ask what is inside it. That means running other
// people's initialisation code, so a scan is done deliberately - from the Scan
// button in Settings - rather than every time FastRoute starts. What it finds
// is written into the document, and that is what the blocks library reads.
//
// A module that fails is skipped in silence. A plugin folder normally contains
// a good deal that is not a plugin, and a scan that reported each one would be
// a scan nobody ever got to the end of.
// ---------------------------------------------------------------------------

const wchar_t* PlugFormatName(PlugFormat f)
{
    return f == PlugFormat::Vst3 ? L"VST3" : L"CLAP";
}

std::wstring PluginDesc::Label() const
{
    return FormatW(L"%s (%s)", name.c_str(), PlugFormatName(format));
}

std::unique_ptr<PluginInstance> LoadClapPlugin(const PluginDesc& desc, std::wstring* err);
std::unique_ptr<PluginInstance> LoadVst3Plugin(const PluginDesc& desc, std::wstring* err);

std::unique_ptr<PluginInstance> LoadPlugin(const PluginDesc& desc, std::wstring* err)
{
    if (desc.path.empty())
    {
        if (err) *err = L"This block has no plugin set. Press Enter on it to choose one.";
        return nullptr;
    }
    if (desc.format == PlugFormat::Vst3) return LoadVst3Plugin(desc, err);
    return LoadClapPlugin(desc, err);
}

namespace {

bool EndsWith(const std::wstring& s, const wchar_t* suffix)
{
    const size_t n = wcslen(suffix);
    if (s.size() < n) return false;
    return IEquals(s.substr(s.size() - n), suffix);
}

std::wstring KnownFolder(REFKNOWNFOLDERID id, const wchar_t* tail)
{
    wchar_t* base = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &base)) && base)
    {
        out = base;
        out += tail;
    }
    if (base) CoTaskMemFree(base);
    return out;
}

// Recursion is bounded: plugin folders nest a few levels at most, and a depth
// limit is what stops a symbolic link loop turning a scan into a hang.
void Walk(const std::wstring& folder, int depth, std::vector<PluginDesc>* out)
{
    if (depth > 6) return;

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do
    {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring full = folder + L"\\" + name;
        const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

        if (EndsWith(name, L".clap") && !isDir)
        {
            for (PluginDesc& d : ScanClapModule(full)) out->push_back(d);
            continue;
        }
        if (EndsWith(name, L".vst3"))
        {
            // Either a plain DLL or a bundle folder; the VST3 side works out
            // which. A bundle is not descended into afterwards.
            for (PluginDesc& d : ScanVst3Module(full)) out->push_back(d);
            continue;
        }
        // A reparse point is followed only if it is not a mount point: those
        // are how a folder ends up containing itself.
        if (isDir && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            Walk(full, depth + 1, out);
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

}   // namespace

std::vector<PluginDesc> ScanPluginFolder(const std::wstring& folder)
{
    std::vector<PluginDesc> out;
    if (folder.empty()) return out;
    DWORD attr = GetFileAttributesW(folder.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return out;
    Walk(folder, 0, &out);
    return out;
}

std::vector<std::wstring> DefaultPluginFolders()
{
    std::vector<std::wstring> out;
    // Where each format's own specification says installers put things.
    std::wstring vst3 = KnownFolder(FOLDERID_ProgramFilesCommon, L"\\VST3");
    std::wstring clapAll = KnownFolder(FOLDERID_ProgramFilesCommon, L"\\CLAP");
    std::wstring clapUser = KnownFolder(FOLDERID_LocalAppData, L"\\Programs\\Common\\CLAP");

    for (const std::wstring& p : { vst3, clapAll, clapUser })
    {
        if (p.empty()) continue;
        DWORD attr = GetFileAttributesW(p.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) continue;
        out.push_back(p);
    }
    return out;
}
