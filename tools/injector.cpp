// Development injector: LoadLibraryW in a running process.
//   injector.exe [path\to\inventory_mod.dll] [process.exe | pid]     (default process: left4dead2.exe)
// Start the game with -insecure first.  A 32-bit build, like the game.

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <tlhelp32.h>

namespace
{
    DWORD find_process(const wchar_t* name)
    {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return 0;
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        DWORD pid = 0;
        for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry))
        {
            if (_wcsicmp(entry.szExeFile, name) == 0)
            {
                pid = entry.th32ProcessID;
                break;
            }
        }
        CloseHandle(snapshot);
        return pid;
    }
} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::wstring dll;
    if (argc > 1)
        dll = argv[1];
    else
    {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        dll = self;
        dll.resize(dll.find_last_of(L'\\') + 1);
        dll += L"inventory_mod.dll";
    }
    wchar_t full[MAX_PATH];
    if (GetFullPathNameW(dll.c_str(), MAX_PATH, full, nullptr) == 0 || GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES)
    {
        std::fwprintf(stderr, L"DLL not found: %ls\n", dll.c_str());
        return 1;
    }

    const wchar_t* process = argc > 2 ? argv[2] : L"left4dead2.exe";
    // a number is a process id (two srcds.exe can run at once)
    wchar_t* end = nullptr;
    const unsigned long number = std::wcstoul(process, &end, 10);
    const DWORD pid = end != process && *end == 0 ? static_cast<DWORD>(number) : find_process(process);
    if (pid == 0)
    {
        std::fwprintf(stderr, L"%ls is not running\n", process);
        return 2;
    }

    const HANDLE target = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                          PROCESS_VM_WRITE | PROCESS_VM_READ,
                                      FALSE, pid);
    if (target == nullptr)
    {
        std::fwprintf(stderr, L"OpenProcess failed (%lu): run as the same user, or elevated\n", GetLastError());
        return 3;
    }

    BOOL wow64 = FALSE;
    IsWow64Process(target, &wow64);
    BOOL self_wow64 = FALSE;
    IsWow64Process(GetCurrentProcess(), &self_wow64);
    if (wow64 != self_wow64)
    {
        std::fwprintf(stderr, L"architecture mismatch: build the injector as x86 like the game\n");
        CloseHandle(target);
        return 4;
    }

    const SIZE_T bytes = (wcslen(full) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(target, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote == nullptr || !WriteProcessMemory(target, remote, full, bytes, nullptr))
    {
        std::fwprintf(stderr, L"could not write into the process (%lu)\n", GetLastError());
        CloseHandle(target);
        return 5;
    }

    const auto load_library =
        reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    const HANDLE thread = CreateRemoteThread(target, nullptr, 0, load_library, remote, 0, nullptr);
    if (thread == nullptr)
    {
        std::fwprintf(stderr, L"CreateRemoteThread failed (%lu)\n", GetLastError());
        VirtualFreeEx(target, remote, 0, MEM_RELEASE);
        CloseHandle(target);
        return 6;
    }
    WaitForSingleObject(thread, 10000);
    DWORD module = 0;
    GetExitCodeThread(thread, &module);
    CloseHandle(thread);
    VirtualFreeEx(target, remote, 0, MEM_RELEASE);
    CloseHandle(target);

    if (module == 0)
    {
        std::fwprintf(stderr, L"LoadLibrary failed in the game (missing dependency, or wrong architecture)\n");
        return 7;
    }
    std::wprintf(L"loaded %ls into %ls (pid %lu)\n", full, process, pid);
    return 0;
}
