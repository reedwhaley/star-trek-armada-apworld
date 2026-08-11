#include <windows.h>
#include <tlhelp32.h>

#include <cwchar>
#include <iostream>
#include <string>
#include <vector>

namespace {

void PrintLastError(const wchar_t* operation) {
    std::wcerr << operation << L" failed (Win32 error " << GetLastError() << L")\n";
}

DWORD FindProcessId(const wchar_t* executable_name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        PrintLastError(L"CreateToolhelp32Snapshot");
        return 0;
    }

    PROCESSENTRY32W entry = {sizeof(entry)};
    DWORD result = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, executable_name) == 0) {
                result = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

bool Inject(DWORD pid, const wchar_t* dll_path) {
    HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
            PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!process) {
        PrintLastError(L"OpenProcess");
        return false;
    }

    const SIZE_T bytes = (wcslen(dll_path) + 1) * sizeof(wchar_t);
    void* remote_path = VirtualAllocEx(process, nullptr, bytes,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_path) {
        PrintLastError(L"VirtualAllocEx");
        CloseHandle(process);
        return false;
    }

    bool success = false;
    if (!WriteProcessMemory(process, remote_path, dll_path, bytes, nullptr)) {
        PrintLastError(L"WriteProcessMemory");
    } else {
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        FARPROC load_library = kernel32 ? GetProcAddress(kernel32, "LoadLibraryW") : nullptr;
        if (!load_library) {
            PrintLastError(L"GetProcAddress(LoadLibraryW)");
        } else {
            HANDLE thread = CreateRemoteThread(
                process, nullptr, 0,
                reinterpret_cast<LPTHREAD_START_ROUTINE>(load_library),
                remote_path, 0, nullptr);
            if (!thread) {
                PrintLastError(L"CreateRemoteThread");
            } else {
                const DWORD wait_result = WaitForSingleObject(thread, 10000);
                if (wait_result != WAIT_OBJECT_0) {
                    if (wait_result == WAIT_TIMEOUT) {
                        std::wcerr << L"Remote LoadLibraryW timed out\n";
                    } else {
                        PrintLastError(L"WaitForSingleObject");
                    }
                } else {
                    DWORD module_result = 0;
                    if (!GetExitCodeThread(thread, &module_result)) {
                        PrintLastError(L"GetExitCodeThread");
                    } else if (module_result == 0) {
                        std::wcerr << L"LoadLibraryW returned NULL; the DLL was not loaded\n";
                    } else {
                        std::wcout << L"Injected observer into PID " << pid
                                   << L" at 0x" << std::hex << module_result << L"\n";
                        success = true;
                    }
                }
                CloseHandle(thread);
            }
        }
    }

    VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
    CloseHandle(process);
    return success;
}

std::wstring AbsolutePath(const wchar_t* path);

bool LaunchSuspendedAndInject(const wchar_t* executable_path, const wchar_t* dll_path, bool no_intro, const wchar_t* launch_map) {
    std::wstring command_line = L"\"";
    command_line += executable_path;
    command_line += L"\"";
    if (no_intro) command_line += L" -nointro";
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');
    std::wstring working_directory = AbsolutePath(executable_path);
    const size_t separator = working_directory.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        std::wcerr << L"Unable to determine Armada working directory\n";
        return false;
    }
    working_directory.resize(separator);
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {};
    // Do not pass a .bzn filename on Armada's command line: stock Armada
    // treats that as a bare skirmish-style map and omits campaign scripts.
    // The preloaded observer reads this inherited value while the process is
    // still suspended, then follows Armada's native campaign controller path.
    wchar_t previous_map[MAX_PATH] = {};
    const DWORD previous_length = GetEnvironmentVariableW(L"ARMADA_LAUNCH_MAP", previous_map, ARRAYSIZE(previous_map));
    const bool had_previous_map = previous_length > 0 && previous_length < ARRAYSIZE(previous_map);
    if (launch_map && !SetEnvironmentVariableW(L"ARMADA_LAUNCH_MAP", launch_map)) {
        PrintLastError(L"SetEnvironmentVariableW(ARMADA_LAUNCH_MAP)");
        return false;
    }
    if (!CreateProcessW(executable_path, mutable_command.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, working_directory.c_str(), &startup, &process)) {
        if (launch_map) SetEnvironmentVariableW(L"ARMADA_LAUNCH_MAP", had_previous_map ? previous_map : nullptr);
        PrintLastError(L"CreateProcessW");
        return false;
    }
    if (launch_map) SetEnvironmentVariableW(L"ARMADA_LAUNCH_MAP", had_previous_map ? previous_map : nullptr);
    const bool injected = Inject(process.dwProcessId, dll_path);
    // The observer initializes its hooks on a short-lived loader thread. Keep
    // Armada suspended until that thread has had an opportunity to install them.
    if (injected) Sleep(500);
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        PrintLastError(L"ResumeThread");
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return false;
    }
    std::wcout << L"Launched Armada with observer preloaded in PID " << process.dwProcessId << L"\n";
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return injected;
}

std::wstring AbsolutePath(const wchar_t* path) {
    DWORD capacity = MAX_PATH;
    std::vector<wchar_t> buffer(capacity);
    while (true) {
        const DWORD length = GetFullPathNameW(path, capacity, buffer.data(), nullptr);
        if (!length) {
            PrintLastError(L"GetFullPathNameW");
            return {};
        }
        if (length < capacity) {
            return std::wstring(buffer.data(), length);
        }
        capacity = length + 1;
        buffer.resize(capacity);
    }
}

bool IsModuleLoaded(DWORD pid, const wchar_t* dll_path) {
    const std::wstring wanted = AbsolutePath(dll_path);
    if (wanted.empty()) return false;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) {
        PrintLastError(L"CreateToolhelp32Snapshot(module)");
        return false;
    }

    MODULEENTRY32W entry = {sizeof(entry)};
    bool loaded = false;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            const std::wstring candidate = AbsolutePath(entry.szExePath);
            if (!candidate.empty() && _wcsicmp(candidate.c_str(), wanted.c_str()) == 0) {
                loaded = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    } else {
        PrintLastError(L"Module32FirstW");
    }
    CloseHandle(snapshot);
    return loaded;
}

void Usage() {
    std::wcerr << L"Usage: armada_injector.exe <observer.dll> [--pid PID] [--if-needed]\n"
                  L"       armada_injector.exe --pid PID <observer.dll> [--if-needed]\n"
                  L"       armada_injector.exe <observer.dll> --launch <Armada.exe> [--map map.bzn] [--nointro]\n"
                  L"If --pid is omitted, the first Armada.exe process is selected.\n";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 6) {
        Usage();
        return 2;
    }

    const wchar_t* dll_path = nullptr;
    const wchar_t* launch_path = nullptr;
    const wchar_t* launch_map = nullptr;
    DWORD pid = 0;
    bool if_needed = false;
    bool no_intro = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--pid") == 0) {
            if (++i >= argc) {
                Usage();
                return 2;
            }
            pid = wcstoul(argv[i], nullptr, 10);
        } else if (_wcsicmp(argv[i], L"--launch") == 0) {
            if (++i >= argc) {
                Usage();
                return 2;
            }
            launch_path = argv[i];
        } else if (_wcsicmp(argv[i], L"--if-needed") == 0) {
            if_needed = true;
        } else if (_wcsicmp(argv[i], L"--nointro") == 0) {
            no_intro = true;
        } else if (_wcsicmp(argv[i], L"--map") == 0) {
            if (++i >= argc) { Usage(); return 2; }
            launch_map = argv[i];
        } else if (!dll_path) {
            dll_path = argv[i];
        } else {
            Usage();
            return 2;
        }
    }

    if (!dll_path) {
        Usage();
        return 2;
    }
    if (GetFileAttributesW(dll_path) == INVALID_FILE_ATTRIBUTES) {
        PrintLastError(L"GetFileAttributesW");
        return 1;
    }
    if (launch_path) {
        if (pid || if_needed || GetFileAttributesW(launch_path) == INVALID_FILE_ATTRIBUTES) {
            Usage();
            return 2;
        }
        return LaunchSuspendedAndInject(launch_path, dll_path, no_intro, launch_map) ? 0 : 1;
    }
    if (no_intro || launch_map) { Usage(); return 2; }
    if (!pid) {
        pid = FindProcessId(L"Armada.exe");
    }
    if (!pid) {
        std::wcerr << L"Armada.exe is not running; pass --pid explicitly if needed\n";
        return 1;
    }
    if (if_needed && IsModuleLoaded(pid, dll_path)) {
        std::wcout << L"Observer already loaded in PID " << pid << L"\n";
        return 0;
    }
    return Inject(pid, dll_path) ? 0 : 1;
}
