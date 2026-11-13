// Diagnostic-only debugger for the child executable passed on the command line.
// Never attach to existing processes, and never use these timings as benchmarks.
// Build (x64 MSVC): cl /EHsc /std:c++20 windows_crash_probe.cpp /link dbghelp.lib
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>

static std::wstring Quote(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += c;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

int wmain(int argc, wchar_t** argv) {
    bool lateAttach = false, showPlayer = false;
    while (argc > 1) {
        if (std::wstring(argv[1]) == L"--late-attach") lateAttach = true;
        else if (std::wstring(argv[1]) == L"--show-player") showPlayer = true;
        else break;
        ++argv; --argc;
    }
    if (argc < 4) {
        std::cerr << "Usage: windows_crash_probe [--late-attach] [--show-player] REPORT TIMEOUT_SECONDS EXE [ARGS...]\n";
        return 2;
    }
    const int seconds = _wtoi(argv[2]);
    if (seconds < 1 || seconds > 600) return 2;
    std::ofstream log{std::filesystem::path(argv[1])};
    if (!log) return 2;
    std::wstring command;
    for (int i = 3; i < argc; ++i) command += (i == 3 ? L"" : L" ") + Quote(argv[i]);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = showPlayer ? 0 : STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    const DWORD oldMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const BOOL created = CreateProcessW(argv[3], command.data(), nullptr, nullptr, FALSE,
        (lateAttach ? 0 : DEBUG_ONLY_THIS_PROCESS) | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    SetErrorMode(oldMode);
    if (!created) { log << "CreateProcess error " << GetLastError() << '\n'; return 2; }
    if (lateAttach) {
        // Let this child initialize/quit without debugger timing effects. A 1-second
        // sample normally exits within 6 seconds; attach only if it is still alive.
        if (WaitForSingleObject(process.hProcess, 6000) == WAIT_OBJECT_0) {
            DWORD code{};
            GetExitCodeProcess(process.hProcess, &code);
            log << "EXIT before attach code=0x" << std::hex << code << '\n';
            CloseHandle(process.hThread); CloseHandle(process.hProcess);
            return static_cast<int>(code);
        }
        if (!DebugActiveProcess(process.dwProcessId)) {
            log << "Attach to own child failed " << GetLastError() << '\n';
            TerminateProcess(process.hProcess, 125);
            CloseHandle(process.hThread); CloseHandle(process.hProcess);
            return 2;
        }
        log << "ATTACHED after 6 seconds to this probe's child\n";
    }
    // Local PE/PDB data only; no symbol-server uploads or downloads.
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
    const bool symbols = SymInitialize(process.hProcess, ".", FALSE) != FALSE;
    struct Module { std::string path; DWORD size; };
    std::map<DWORD64, Module> modules;
    std::map<DWORD, HANDLE> threads;
    auto address = [&](DWORD64 pc) {
        log << "0x" << std::hex << pc;
        auto it = modules.upper_bound(pc);
        if (it != modules.begin()) {
            --it;
            if (pc - it->first < it->second.size)
                log << " " << it->second.path << "+0x" << (pc - it->first);
        }
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
        auto symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 offset{};
        if (symbols && SymFromAddr(process.hProcess, pc, &offset, symbol))
            log << " " << symbol->Name << "+0x" << offset;
        log << std::dec << '\n';
    };
    auto module = [&](HANDLE file, void* base) {
        char path[32768]{};
        if (file) GetFinalPathNameByHandleA(file, path, sizeof(path), FILE_NAME_NORMALIZED);
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        DWORD size{};
        if (ReadProcessMemory(process.hProcess, base, &dos, sizeof(dos), nullptr) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
            ReadProcessMemory(process.hProcess, static_cast<char*>(base) + dos.e_lfanew, &nt, sizeof(nt), nullptr) &&
            nt.Signature == IMAGE_NT_SIGNATURE) size = nt.OptionalHeader.SizeOfImage;
        modules[reinterpret_cast<DWORD64>(base)] = {path, size};
        log << "LOAD " << path << " base=0x" << std::hex << reinterpret_cast<DWORD64>(base)
            << " size=0x" << size << std::dec << '\n';
        if (symbols) SymLoadModuleEx(process.hProcess, file, *path ? path : nullptr, nullptr,
                                   reinterpret_cast<DWORD64>(base), size, nullptr, 0);
        if (file) CloseHandle(file);
    };
    const ULONGLONG started = GetTickCount64();
    bool firstBreakpoint = true, timedOut = false, dumped = false;
    DWORD result = 2;
    while (true) {
        if (!timedOut && GetTickCount64() - started > static_cast<ULONGLONG>(seconds) * 1000) {
            log << "TIMEOUT (terminating this probe's child only)\n";
            TerminateProcess(process.hProcess, 124);
            timedOut = true;
        }
        DEBUG_EVENT event{};
        if (!WaitForDebugEvent(&event, 500)) {
            if (GetLastError() == ERROR_SEM_TIMEOUT) continue;
            log << "WaitForDebugEvent error " << GetLastError() << '\n';
            TerminateProcess(process.hProcess, 125);
            break;
        }
        DWORD disposition = DBG_CONTINUE;
        bool done = false;
        switch (event.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
            threads[event.dwThreadId] = event.u.CreateProcessInfo.hThread;
            module(event.u.CreateProcessInfo.hFile, event.u.CreateProcessInfo.lpBaseOfImage);
            break;
        case CREATE_THREAD_DEBUG_EVENT: threads[event.dwThreadId] = event.u.CreateThread.hThread; break;
        case EXIT_THREAD_DEBUG_EVENT: threads.erase(event.dwThreadId); break;
        case LOAD_DLL_DEBUG_EVENT: module(event.u.LoadDll.hFile, event.u.LoadDll.lpBaseOfDll); break;
        case UNLOAD_DLL_DEBUG_EVENT:
            if (symbols) SymUnloadModule64(process.hProcess, reinterpret_cast<DWORD64>(event.u.UnloadDll.lpBaseOfDll));
            modules.erase(reinterpret_cast<DWORD64>(event.u.UnloadDll.lpBaseOfDll)); break;
        case EXCEPTION_DEBUG_EVENT: {
            const auto& info = event.u.Exception;
            const auto& ex = info.ExceptionRecord;
            if (ex.ExceptionCode == EXCEPTION_BREAKPOINT && firstBreakpoint) {
                firstBreakpoint = false;
                if (lateAttach) {
                    for (const auto& [id, handle] : threads) {
                        CONTEXT context{}; context.ContextFlags = CONTEXT_FULL;
                        if (!GetThreadContext(handle, &context)) continue;
                        log << "ATTACH STACK thread=" << id << '\n';
                        address(context.Rip);
                        STACKFRAME64 frame{};
                        frame.AddrPC = {context.Rip, 0, AddrModeFlat};
                        frame.AddrFrame = {context.Rbp, 0, AddrModeFlat};
                        frame.AddrStack = {context.Rsp, 0, AddrModeFlat};
                        for (int depth = 0; depth < 40 && symbols; ++depth) {
                            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process.hProcess, handle, &frame, &context,
                                nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !frame.AddrPC.Offset) break;
                            address(frame.AddrPC.Offset);
                        }
                    }
                    const std::wstring path = std::wstring(argv[1]) + L".dmp";
                    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (file != INVALID_HANDLE_VALUE) {
                        const BOOL saved = MiniDumpWriteDump(process.hProcess, process.dwProcessId, file,
                            MiniDumpWithThreadInfo, nullptr, nullptr, nullptr);
                        log << "ATTACH MINIDUMP saved=" << saved << '\n';
                        CloseHandle(file); dumped = true;
                    }
                }
                break;
            }
            disposition = DBG_EXCEPTION_NOT_HANDLED; // Let Mono/Unity handle first-chance exceptions normally.
            if (ex.ExceptionCode != EXCEPTION_ACCESS_VIOLATION && info.dwFirstChance) break;
            log << "EXCEPTION code=0x" << std::hex << ex.ExceptionCode << std::dec
                << " first_chance=" << info.dwFirstChance << " thread=" << event.dwThreadId
                << " elapsed_ms=" << (GetTickCount64() - started) << '\n';
            address(reinterpret_cast<DWORD64>(ex.ExceptionAddress));
            for (DWORD i = 0; i < ex.NumberParameters; ++i)
                log << "parameter[" << i << "]=0x" << std::hex << ex.ExceptionInformation[i] << std::dec << '\n';
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            const HANDLE thread = threads[event.dwThreadId];
            if (thread && GetThreadContext(thread, &context)) {
                log << "STACK\n";
                CONTEXT walkContext = context;
                STACKFRAME64 frame{};
                frame.AddrPC = {context.Rip, 0, AddrModeFlat};
                frame.AddrFrame = {context.Rbp, 0, AddrModeFlat};
                frame.AddrStack = {context.Rsp, 0, AddrModeFlat};
                for (int depth = 0; depth < 40 && symbols; ++depth) {
                    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process.hProcess, thread, &frame, &walkContext,
                        nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !frame.AddrPC.Offset) break;
                    address(frame.AddrPC.Offset);
                }
                if (!info.dwFirstChance && !dumped) {
                    const std::wstring path = std::wstring(argv[1]) + L".dmp";
                    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (file != INVALID_HANDLE_VALUE) {
                        EXCEPTION_RECORD record = ex;
                        EXCEPTION_POINTERS pointers{&record, &context};
                        MINIDUMP_EXCEPTION_INFORMATION exception{event.dwThreadId, &pointers, FALSE};
                        const BOOL saved = MiniDumpWriteDump(process.hProcess, process.dwProcessId, file,
                            MiniDumpWithThreadInfo, &exception, nullptr, nullptr);
                        log << "MINIDUMP saved=" << saved << " error=" << (saved ? 0 : GetLastError()) << '\n';
                        CloseHandle(file);
                        dumped = true;
                    }
                }
            }
            break;
        }
        case EXIT_PROCESS_DEBUG_EVENT:
            result = event.u.ExitProcess.dwExitCode;
            log << "EXIT code=0x" << std::hex << result << std::dec << '\n';
            done = true; break;
        }
        log.flush();
        if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, disposition)) {
            log << "ContinueDebugEvent error " << GetLastError() << '\n';
            TerminateProcess(process.hProcess, 125);
            break;
        }
        if (done) break;
    }
    if (symbols) SymCleanup(process.hProcess);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(result);
}
