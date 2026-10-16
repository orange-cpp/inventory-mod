#pragma once
// A line-per-call log file next to the DLL (inventory_mod.log), also sent to the debugger.

#include <windows.h>

namespace core
{

    // `file_name`: the log's name next to the DLL.
    void log_init(HMODULE self, const wchar_t* file_name = L"inventory_mod.log");
    void log_shutdown();
    void logf(const char* fmt, ...);
    // The folder the DLL was loaded from, with a trailing backslash (set by log_init).
    const wchar_t* module_dir();

} // namespace core

#define INV_LOG(...) ::core::logf(__VA_ARGS__)
