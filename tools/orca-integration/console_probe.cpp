// Console entry point for unchanged Windows Orca/Quack DLLs during CLI trials.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <filesystem>
#include <iostream>

int wmain(int argc, wchar_t **argv)
{
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return 126;
    const auto directory = std::filesystem::path(exe).parent_path();
    SetDllDirectoryW((directory / L"python").c_str());
    HMODULE library = LoadLibraryW((directory / L"OrcaSlicer.dll").c_str());
    if (!library) { std::cerr << "Cannot load OrcaSlicer.dll: " << GetLastError() << '\n'; return 126; }
    using Main = int (*)(int, wchar_t **);
    const auto entry = reinterpret_cast<Main>(GetProcAddress(library, "orcaslicer_main"));
    if (!entry) return 127;
    return entry(argc, argv);
}
