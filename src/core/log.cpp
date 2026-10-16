#include "core/log.hpp"

#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <string>

namespace core
{

    namespace
    {
        CRITICAL_SECTION g_lock;
        bool g_lock_ready = false;
        FILE* g_file = nullptr;
        wchar_t g_dir[MAX_PATH] = L"";
    } // namespace

    void log_init(HMODULE self, const wchar_t* file_name)
    {
        if (!g_lock_ready)
        {
            InitializeCriticalSection(&g_lock);
            g_lock_ready = true;
        }
        wchar_t path[MAX_PATH];
        const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            return;
        std::wstring dir(path, n);
        dir.resize(dir.find_last_of(L'\\') + 1);
        wcsncpy_s(g_dir, dir.c_str(), _TRUNCATE);

        const std::wstring file = dir + file_name;
        EnterCriticalSection(&g_lock);
        if (g_file == nullptr)
            g_file = _wfsopen(file.c_str(), L"w", _SH_DENYNO); // readable while the game runs
        LeaveCriticalSection(&g_lock);
    }

    void log_shutdown()
    {
        if (!g_lock_ready)
            return;
        EnterCriticalSection(&g_lock);
        if (g_file != nullptr)
        {
            fclose(g_file);
            g_file = nullptr;
        }
        LeaveCriticalSection(&g_lock);
    }

    const wchar_t* module_dir()
    {
        return g_dir;
    }

    void logf(const char* fmt, ...)
    {
        char line[1024];
        SYSTEMTIME t;
        GetLocalTime(&t);
        int n = snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        va_list args;
        va_start(args, fmt);
        vsnprintf(line + n, sizeof(line) - static_cast<size_t>(n) - 2, fmt, args);
        va_end(args);
        strcat_s(line, "\n");

        OutputDebugStringA(line);
        if (!g_lock_ready)
            return;
        EnterCriticalSection(&g_lock);
        if (g_file != nullptr)
        {
            fputs(line, g_file);
            fflush(g_file);
        }
        LeaveCriticalSection(&g_lock);
    }

} // namespace core
