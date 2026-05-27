module;

#include <windows.h>
#include <dbghelp.h>
#include <wchar.h>
#include <signal.h>

export module Platform:Windows;

import Core;

namespace Platform {

// Dump directory — captured during InstallCrashHandler (normal context).
wchar_t g_DumpDir[MAX_PATH + 1] = {};

/// Write a minidump to Engine/Binaries/SoulEngine_Timestamp_PID.dmp.
/// @param ExcPtr  Exception info (can be nullptr for SIGABRT).
auto WriteMiniDump(EXCEPTION_POINTERS* ExcPtr) -> void {
    SYSTEMTIME SysTm = {};
    GetLocalTime(&SysTm);
    wchar_t Timestamp[32] = {};
    swprintf_s(Timestamp, L"_%04u%02u%02u_%02u%02u%02u_%lu.dmp",
               SysTm.wYear, SysTm.wMonth, SysTm.wDay,
               SysTm.wHour, SysTm.wMinute, SysTm.wSecond, GetCurrentProcessId());

    wchar_t DumpPath[MAX_PATH + 64] = {};
    wcscpy_s(DumpPath, g_DumpDir);
    wcscat_s(DumpPath, L"\\SoulEngine");
    wcscat_s(DumpPath, Timestamp);

    HANDLE hFile = CreateFileW(DumpPath, GENERIC_WRITE, 0,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return;

    MINIDUMP_EXCEPTION_INFORMATION Mdei = {
        .ThreadId          = GetCurrentThreadId(),
        .ExceptionPointers = ExcPtr,
        .ClientPointers    = FALSE,
    };

    auto DumpType = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithIndirectlyReferencedMemory |
        MiniDumpWithDataSegs |
        MiniDumpWithHandleData |
        MiniDumpWithFullMemoryInfo |
        MiniDumpWithThreadInfo);

    auto Ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                hFile, DumpType,
                                ExcPtr ? &Mdei : nullptr, nullptr, nullptr);
    if (!Ok) {
        // Dump failed (e.g. out of disk space). Cannot safely log here.
    }
    CloseHandle(hFile);
}

/// SEH unhandled exception filter — writes MiniDump, terminates process.
auto CALLBACK CrashHandler(EXCEPTION_POINTERS* ExcPtr) -> LONG {
    WriteMiniDump(ExcPtr);
    return EXCEPTION_EXECUTE_HANDLER;
}

/// Handler for assert() / abort() — SIGABRT doesn't provide exception pointers.
auto AbortHandler(int /*Signal*/) -> void {
    // If a debugger is attached, break immediately so the developer sees the
    // assertion call site without needing a dump.
    if (IsDebuggerPresent())
        __debugbreak();

    // assert() / abort() do not provide EXCEPTION_POINTERS. Raise a synthetic
    // non-continuable SEH so the dump captures the real call stack.
    __try {
        RaiseException(0xE0000001, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    } __except (CrashHandler(GetExceptionInformation()), EXCEPTION_EXECUTE_HANDLER) {
    }

    // Prevent infinite loop if abort is called again during our handler.
    _exit(3);
}

export auto InstallCrashHandler() -> void {
    auto BinDir = SoulEngine::Core::ConfigManager::Get().BinariesDirPath();
    wcscpy_s(g_DumpDir, BinDir.wstring().c_str());

    // Catch SEH crashes (access violation, divide-by-zero, etc.)
    SetUnhandledExceptionFilter(&CrashHandler);

    // Catch assert() / abort() crashes — SEH filter doesn't cover SIGABRT.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    signal(SIGABRT, &AbortHandler);
}

} // namespace Platform
