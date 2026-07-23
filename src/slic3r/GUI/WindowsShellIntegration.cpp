#include "WindowsShellIntegration.hpp"

#include "libslic3r/BuildPathClassifier.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#ifdef _WIN32

#include <windows.h>
#include <appmodel.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <array>
#include <iomanip>
#include <vector>

namespace Slic3r::GUI::WindowsShellIntegration {
namespace {

namespace fs = boost::filesystem;

constexpr wchar_t INSTALLED_APP_ID[] = L"QuackSlicer.QuackSlicer";
constexpr wchar_t BUILD_APP_ID[]     = L"QuackSlicer.QuackSlicer.Build";
constexpr wchar_t BUILD_REGISTRY[]   = L"Software\\QuackSlicer\\BuildIntegration";
constexpr wchar_t BUILD_PATH_VALUE[] = L"ExecutablePath";

fs::path    g_executable;
std::wstring g_app_id;
bool         g_is_build = false;

struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComScope()
    {
        if (SUCCEEDED(result))
            CoUninitialize();
    }
};

std::wstring packaged_app_id()
{
    UINT32 length = 0;
    if (GetCurrentApplicationUserModelId(&length, nullptr) != ERROR_INSUFFICIENT_BUFFER)
        return {};

    std::wstring result(length, L'\0');
    if (GetCurrentApplicationUserModelId(&length, result.data()) != ERROR_SUCCESS)
        return {};
    result.resize(length > 0 ? length - 1 : 0);
    return result;
}

bool same_path(const fs::path &left, const fs::path &right)
{
    return boost::algorithm::iequals(
        fs::absolute(left).lexically_normal().wstring(),
        fs::absolute(right).lexically_normal().wstring());
}

std::wstring read_previous_build_path()
{
    std::array<wchar_t, 32768> buffer {};
    DWORD size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, BUILD_REGISTRY, BUILD_PATH_VALUE, RRF_RT_REG_SZ, nullptr, buffer.data(), &size) != ERROR_SUCCESS)
        return {};
    return buffer.data();
}

void store_current_build_path()
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, BUILD_REGISTRY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;

    const std::wstring path = g_executable.wstring();
    RegSetValueExW(key, BUILD_PATH_VALUE, 0, REG_SZ, reinterpret_cast<const BYTE *>(path.c_str()),
        static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

std::optional<fs::path> known_folder(REFKNOWNFOLDERID id)
{
    PWSTR raw_path = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw_path)))
        return std::nullopt;
    fs::path path(raw_path);
    CoTaskMemFree(raw_path);
    return path;
}

bool load_shortcut_target(const fs::path &shortcut, fs::path &target)
{
    IShellLinkW *link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
        return false;

    IPersistFile *persist = nullptr;
    HRESULT result = link->QueryInterface(IID_PPV_ARGS(&persist));
    if (SUCCEEDED(result))
        result = persist->Load(shortcut.c_str(), STGM_READ);

    std::array<wchar_t, 32768> path {};
    if (SUCCEEDED(result))
        result = link->GetPath(path.data(), static_cast<int>(path.size()), nullptr, SLGP_RAWPATH);
    if (SUCCEEDED(result) && path[0] != L'\0')
        target = fs::path(path.data());

    if (persist != nullptr)
        persist->Release();
    link->Release();
    return SUCCEEDED(result) && !target.empty();
}

bool save_shortcut(const fs::path &shortcut, const fs::path &target, const std::wstring &app_id)
{
    IShellLinkW *link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
        return false;

    HRESULT result = link->SetPath(target.c_str());
    if (SUCCEEDED(result))
        result = link->SetWorkingDirectory(target.parent_path().c_str());
    if (SUCCEEDED(result))
        result = link->SetIconLocation(target.c_str(), 0);

    IPropertyStore *properties = nullptr;
    if (SUCCEEDED(result))
        result = link->QueryInterface(IID_PPV_ARGS(&properties));
    if (SUCCEEDED(result)) {
        PROPVARIANT value;
        result = InitPropVariantFromString(app_id.c_str(), &value);
        if (SUCCEEDED(result)) {
            result = properties->SetValue(PKEY_AppUserModel_ID, value);
            PropVariantClear(&value);
        }
        if (SUCCEEDED(result))
            result = properties->Commit();
    }

    IPersistFile *persist = nullptr;
    if (SUCCEEDED(result))
        result = link->QueryInterface(IID_PPV_ARGS(&persist));
    if (SUCCEEDED(result)) {
        boost::system::error_code ec;
        fs::create_directories(shortcut.parent_path(), ec);
        result = persist->Save(shortcut.c_str(), TRUE);
    }

    if (persist != nullptr)
        persist->Release();
    if (properties != nullptr)
        properties->Release();
    link->Release();
    return SUCCEEDED(result);
}

bool is_owned_build_target(const fs::path &target, const fs::path &previous_build)
{
    return (!previous_build.empty() && same_path(target, previous_build)) ||
           is_cmake_build_executable(target);
}

void repair_shortcuts_in(const fs::path &directory, const fs::path &previous_build, bool recursive)
{
    boost::system::error_code ec;
    if (!fs::is_directory(directory, ec))
        return;

    auto repair = [&](const fs::path &shortcut) {
        if (!boost::algorithm::iequals(shortcut.extension().string(), ".lnk"))
            return;

        fs::path target;
        if (!load_shortcut_target(shortcut, target))
            return;

        const bool owned = g_is_build
            ? is_owned_build_target(target, previous_build)
            : same_path(target, g_executable);
        if (!owned)
            return;

        if (save_shortcut(shortcut, g_executable, g_app_id))
            BOOST_LOG_TRIVIAL(info) << "Updated QuackSlicer Windows shortcut: " << shortcut.string();
    };

    if (recursive) {
        for (fs::recursive_directory_iterator it(directory, ec), end; it != end && !ec; it.increment(ec))
            if (fs::is_regular_file(it->path(), ec))
                repair(it->path());
    } else {
        for (fs::directory_iterator it(directory, ec), end; it != end && !ec; it.increment(ec))
            if (fs::is_regular_file(it->path(), ec))
                repair(it->path());
    }
}

} // namespace

void initialize_process_app_id()
{
    g_executable = boost::dll::program_location();
    g_is_build   = is_cmake_build_executable(g_executable);
    g_app_id     = packaged_app_id();
    if (g_app_id.empty()) {
        g_app_id = g_is_build ? BUILD_APP_ID : INSTALLED_APP_ID;
        const HRESULT result = SetCurrentProcessExplicitAppUserModelID(g_app_id.c_str());
        if (FAILED(result))
            BOOST_LOG_TRIVIAL(warning) << "Failed to set Windows AppUserModelID: " << std::hex << result;
    }
}

void repair_shortcuts()
{
    if (g_executable.empty())
        initialize_process_app_id();

    ComScope com;
    if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE)
        return;

    const fs::path previous_build = g_is_build ? fs::path(read_previous_build_path()) : fs::path();

    if (const auto programs = known_folder(FOLDERID_Programs))
        repair_shortcuts_in(*programs, previous_build, true);

    if (const auto roaming = known_folder(FOLDERID_RoamingAppData)) {
        const fs::path pinned = *roaming / "Microsoft" / "Internet Explorer" / "Quick Launch" / "User Pinned" / "TaskBar";
        repair_shortcuts_in(pinned, previous_build, false);
    }

    if (g_is_build) {
        if (const auto programs = known_folder(FOLDERID_Programs))
            save_shortcut(*programs / "QuackSlicer (Build).lnk", g_executable, g_app_id);
        store_current_build_path();
    }
}

void add_recent_document(const std::wstring &filename)
{
    if (filename.empty())
        return;
    if (g_executable.empty())
        initialize_process_app_id();

    ComScope com;
    if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE)
        return;

    IShellItem *item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(filename.c_str(), nullptr, IID_PPV_ARGS(&item))))
        return;

    SHARDAPPIDINFO recent { item, g_app_id.c_str() };
    SHAddToRecentDocs(SHARD_APPIDINFO, &recent);
    item->Release();
}

} // namespace Slic3r::GUI::WindowsShellIntegration

#else

namespace Slic3r::GUI::WindowsShellIntegration {
void initialize_process_app_id() {}
void repair_shortcuts() {}
void add_recent_document(const std::wstring &) {}
} // namespace Slic3r::GUI::WindowsShellIntegration

#endif
