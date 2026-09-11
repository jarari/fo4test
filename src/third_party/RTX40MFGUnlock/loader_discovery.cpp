// Loader-return discovery adapted from RTX40MFG-Unlock v1.2 patcher.cpp.
// MIT License; see LICENSE. No OTA redirection or process-wide loader detours.
#include "integration.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

namespace RTX40MFGUnlock
{
namespace
{
std::mutex g_loaderMutex;
std::atomic<decltype(&::LoadLibraryA)> g_LoadLibraryA{nullptr};
HMODULE WINAPI HookLoadLibraryA(LPCSTR name)
{
    const auto original = g_LoadLibraryA.load(std::memory_order_acquire);
    if (!original) {
        return nullptr;
    }
    const auto module = original(name);
    const auto error = GetLastError();
    InspectLoadedModule(module, true);
    SetLastError(error);
    return module;
}
std::atomic<decltype(&::LoadLibraryW)> g_LoadLibraryW{nullptr};
HMODULE WINAPI HookLoadLibraryW(LPCWSTR name)
{
    const auto original = g_LoadLibraryW.load(std::memory_order_acquire);
    if (!original) {
        return nullptr;
    }
    const auto module = original(name);
    const auto error = GetLastError();
    InspectLoadedModule(module, true);
    SetLastError(error);
    return module;
}
std::atomic<decltype(&::LoadLibraryExA)> g_LoadLibraryExA{nullptr};
HMODULE WINAPI HookLoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags)
{
    const auto original = g_LoadLibraryExA.load(std::memory_order_acquire);
    if (!original) {
        return nullptr;
    }
    const auto module = original(name, file, flags);
    const auto error = GetLastError();
    InspectLoadedModule(module, true);
    SetLastError(error);
    return module;
}
std::atomic<decltype(&::LoadLibraryExW)> g_LoadLibraryExW{nullptr};
HMODULE WINAPI HookLoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags)
{
    const auto original = g_LoadLibraryExW.load(std::memory_order_acquire);
    if (!original) {
        return nullptr;
    }
    const auto module = original(name, file, flags);
    const auto error = GetLastError();
    InspectLoadedModule(module, true);
    SetLastError(error);
    return module;
}

// Upstream's import-table approach, with original publication before the IAT
// exchange and conflicting chains rejected before modification.
template <typename Function>
bool HookImport(HMODULE module, const char* name, Function replacement,
    std::atomic<Function>& original)
{
    auto* base = reinterpret_cast<unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return false;
    }
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || !directory.Size) {
        return false;
    }
    bool changed = false;
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        // Do not interpret resolved addresses as import names when OFT is absent.
        if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk) {
            continue;
        }
        const auto* library = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(library, "KERNEL32.dll") != 0 &&
            _stricmp(library, "KERNELBASE.dll") != 0 &&
            _strnicmp(library, "api-ms-win-core-libraryloader-", 30) != 0) {
            continue;
        }
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->OriginalFirstThunk);
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) {
                continue;
            }
            const auto* import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(import->Name), name) != 0) {
                continue;
            }
            auto** slot = reinterpret_cast<void**>(&thunk->u1.Function);
            const auto current = reinterpret_cast<Function>(*slot);
            if (current == replacement) {
                continue;
            }
            auto expected = original.load(std::memory_order_acquire);
            if (expected && expected != current) {
                logger::warn("[RTX40MFGUnlock] Skipped conflicting loader import: {}", name);
                continue;
            }
            DWORD protection = 0;
            if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection)) {
                continue;
            }
            original.store(current, std::memory_order_release);
            InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slot),
                reinterpret_cast<void*>(replacement));
            DWORD ignored = 0;
            if (!VirtualProtect(slot, sizeof(*slot), protection, &ignored)) {
                logger::warn("[RTX40MFGUnlock] Loader IAT protection restore failed: {}", name);
            }
            changed = true;
        }
    }
    return changed;
}
}

void InstallLoaderDiscovery(HMODULE module) noexcept
{
    if (!module || (reinterpret_cast<std::uintptr_t>(module) & 3)) {
        return;
    }
    try {
        wchar_t path[32768]{};
        if (!GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)))) {
            return;
        }
        const std::wstring fullPath(path);
        const auto name = fullPath.substr(fullPath.find_last_of(L"\\/") + 1);
        // Only the NVIDIA loading chain, never the game's/global loader.
        if (_wcsnicmp(name.c_str(), L"sl.", 3) != 0 &&
            _wcsnicmp(name.c_str(), L"_nvngx", 6) != 0 &&
            _wcsnicmp(name.c_str(), L"nvngx", 5) != 0 &&
            !GetProcAddress(module, "slGetPluginFunction")) {
            return;
        }
        std::lock_guard lock(g_loaderMutex);
        bool changed = false;
        changed = HookImport(module, "LoadLibraryA", &HookLoadLibraryA, g_LoadLibraryA) || changed;
        changed = HookImport(module, "LoadLibraryW", &HookLoadLibraryW, g_LoadLibraryW) || changed;
        changed = HookImport(module, "LoadLibraryExA", &HookLoadLibraryExA, g_LoadLibraryExA) || changed;
        changed = HookImport(module, "LoadLibraryExW", &HookLoadLibraryExW, g_LoadLibraryExW) || changed;
        if (changed) {
            logger::info("[RTX40MFGUnlock] Installed loader-return discovery at {:x}",
                reinterpret_cast<std::uintptr_t>(module));
        }
    } catch (...) {
        logger::warn("[RTX40MFGUnlock] Loader discovery installation failed");
    }
}
}
