#include "armada_observer.h"

#include <intrin.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <string.h>

namespace {
typedef void (__cdecl *MissionResultFn)(float, const char*);
typedef void (__cdecl *ObjectiveTextFn)(const char*);
typedef void (__cdecl *MovieFn)(char*, int);
typedef bool (__cdecl *MoviePlayingFn)();
typedef void (__cdecl *StopMovieFn)();
typedef void (__cdecl *FrontEndModalDispatchFn)(int);
typedef INT_PTR (CALLBACK *CampaignDialogProcFn)(HWND, UINT, WPARAM, LPARAM);
typedef bool (__thiscall *CampaignControllerDialogRouteFn)(void*);
typedef void* (__stdcall *BinkOpenFn)(const char*, uint32_t);
typedef DWORD (WINAPI *GetTickCountFn)();
typedef DWORD (WINAPI *TimeGetTimeFn)();
typedef BOOL (WINAPI *QueryPerformanceCounterFn)(LARGE_INTEGER*);
typedef int (__cdecl *BuildObjectAtReferenceFn)(char*, int, int, float, float, float);
typedef void (__cdecl *RemoveObjectFn)(int);
typedef int (__cdecl *GetHandleFn)(char*);
typedef bool (__cdecl *IsValidFn)(int);
typedef void (__thiscall *CampaignSelectionDispatchFn)(void*);
typedef void (__thiscall *EngineSelectionSetupFn)(void*, const void*, const char*);
typedef void (__thiscall *CampaignPickerRouteFn)(void*);
typedef bool (__thiscall *CampaignPickerPrepareFn)(void*);
static MissionResultFn g_original_succeed = nullptr;
static MissionResultFn g_original_fail = nullptr;
static ObjectiveTextFn g_original_objective_text = nullptr;
static MovieFn g_original_play_bridge_movie = nullptr;
static MovieFn g_original_play_cinematic_movie = nullptr;
static BinkOpenFn g_original_bink_open = nullptr;
static GetTickCountFn g_original_get_tick_count = nullptr;
static TimeGetTimeFn g_original_time_get_time = nullptr;
static QueryPerformanceCounterFn g_original_query_performance_counter = nullptr;
#if defined(ARMADA_FULL_PICKER_TEST)
static FrontEndModalDispatchFn g_original_front_end_modal_dispatch = nullptr;
static CampaignDialogProcFn g_original_campaign_dialog_proc = nullptr;
static CampaignDialogProcFn g_original_mission_dialog_proc = nullptr;
static EngineSelectionSetupFn g_original_engine_selection_setup = nullptr;
static CampaignSelectionDispatchFn g_original_campaign_selection_dispatch = nullptr;
static CampaignControllerDialogRouteFn g_original_campaign_controller_dialog_route = nullptr;
static volatile LONG g_redirect_initial_front_end_mode = 0;
static volatile LONG g_auto_select_campaign_map = 0;
static volatile LONG g_auto_select_mission = 0;
#endif
static HANDLE g_wake = nullptr;
static HANDLE g_worker = nullptr;
static HANDLE g_control_worker = nullptr;
static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static HHOOK g_campaign_dispatch_hook = nullptr;
static HHOOK g_metaphasic_dispatch_hook = nullptr;
static HMODULE g_observer_module = nullptr;
static CRITICAL_SECTION g_queue_lock;
static volatile LONG g_sequence = 0;
static volatile LONG g_running = 1;
static volatile LONG g_failure_exit_queued = 0;
static volatile LONG g_skip_next_launch_movie = 0;
static volatile LONG g_campaign_dispatch_retries = 0;
static ULONGLONG g_skip_movie_deadline = 0;
static volatile DWORD g_startup_game_thread = 0;
// This is an explicitly requested, Federation 1-only proof harness.  It is
// intentionally separate from the eventual AP trap implementation: nothing
// in a received item can set this flag.
static volatile LONG g_metaphasic_test_requested = 0;
static volatile LONG g_metaphasic_object = 0;
static volatile LONG g_nebula_trap_active = 0;
static volatile DWORD g_metaphasic_game_thread = 0;
static char g_pending_nebula_kind[24] = {};
static char g_active_mission_module[MAX_PATH] = {};
static volatile DWORD g_active_nebula_duration_ms = 0;
static ULONGLONG g_active_nebula_deadline = 0;
static volatile LONG g_missing_player_target_logged = 0;
static const wchar_t kPipeName[] = L"\\\\.\\pipe\\armada_result_observer";
static const wchar_t kControlPipeName[] = L"\\\\.\\pipe\\armada_launcher_control";
// A private thread message enters Armada's own UI message queue. Its hook
// executes on the game thread; it is not mouse or keyboard input.
static const UINT kCampaignDispatchMessage = WM_APP + 0x2A;
static const UINT kMetaphasicDispatchMessage = WM_APP + 0x2B;
static const UINT kMetaphasicRemoveMessage = WM_APP + 0x2C;
static const DWORD kNebulaPulseMilliseconds = 250;
// This window timer retries the native setup after early-startup message
// delivery. It is a UI-queue wakeup only, never simulated player input.
static const UINT_PTR kCampaignDispatchTimer = 0x41524D44;
static const UINT_PTR kCampaignAutoSelectTimer = 0x41524D45;
static const UINT_PTR kMissionAutoSelectTimer = 0x41524D46;
static char g_pending_campaign_map[MAX_PATH] = {};

struct ResultEvent {
    char type[24], result[8], text[MAX_PATH];
    DWORD sequence, pid, tid, arg1_bits; uintptr_t arg2, caller; ULONGLONG tick; wchar_t module[MAX_PATH];
};
static ResultEvent g_queue[64]; static unsigned g_queue_head = 0, g_queue_tail = 0;

static void Status(const char* message) {
    OutputDebugStringA(message);
    wchar_t path[MAX_PATH] = {};
    if (!GetTempPathW(ARRAYSIZE(path), path)) return;
    wcscat_s(path, L"armada-result-observer-status.log");
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, message, static_cast<DWORD>(strlen(message)), &written, nullptr);
    CloseHandle(file);
}

static void QueueEvent(const char* type, const char* result, float arg1, const char* arg2, void* caller) {
    ResultEvent ev = {};
    strncpy_s(ev.type, type, _TRUNCATE);
    strncpy_s(ev.result, result, _TRUNCATE);
    if (arg2) strncpy_s(ev.text, arg2, _TRUNCATE);
    ev.sequence = static_cast<DWORD>(InterlockedIncrement(&g_sequence));
    ev.pid = GetCurrentProcessId(); ev.tid = GetCurrentThreadId(); ev.tick = GetTickCount64();
    memcpy(&ev.arg1_bits, &arg1, sizeof(ev.arg1_bits)); ev.arg2 = reinterpret_cast<uintptr_t>(arg2);
    ev.caller = reinterpret_cast<uintptr_t>(caller);
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(caller), &module)) GetModuleFileNameW(module, ev.module, MAX_PATH);
    EnterCriticalSection(&g_queue_lock);
    unsigned next = (g_queue_tail + 1) % ARRAYSIZE(g_queue);
    if (next != g_queue_head) { g_queue[g_queue_tail] = ev; g_queue_tail = next; SetEvent(g_wake); }
    LeaveCriticalSection(&g_queue_lock);
}

static bool PopEvent(ResultEvent* out) {
    bool have = false; EnterCriticalSection(&g_queue_lock);
    if (g_queue_head != g_queue_tail) { *out = g_queue[g_queue_head]; g_queue_head = (g_queue_head + 1) % ARRAYSIZE(g_queue); have = true; }
    LeaveCriticalSection(&g_queue_lock); return have;
}

static std::string EscapeJson(const char* source) {
    std::string escaped;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(source); *p; ++p) {
        switch (*p) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (*p < 0x20) { char code[7] = {}; _snprintf_s(code, ARRAYSIZE(code), _TRUNCATE, "\\u%04x", *p); escaped += code; }
            else escaped += static_cast<char>(*p);
        }
    }
    return escaped;
}

static void FormatEvent(const ResultEvent& ev, char* out, size_t out_size) {
    char module[MAX_PATH] = {}; WideCharToMultiByte(CP_UTF8, 0, ev.module, -1, module, ARRAYSIZE(module), nullptr, nullptr);
    const std::string module_json = EscapeJson(module);
    float value = 0.0f; memcpy(&value, &ev.arg1_bits, sizeof(value));
    if (strcmp(ev.type, "mission_result") == 0) {
        _snprintf_s(out, out_size, _TRUNCATE,
            "{\"type\":\"mission_result\",\"result\":\"%s\",\"sequence\":%lu,\"pid\":%lu,\"tid\":%lu,\"tick_ms\":%llu,\"caller\":\"0x%p\",\"caller_module\":\"%s\",\"arg1_float\":%.6f,\"arg1_bits\":\"0x%08lx\",\"arg2\":\"0x%p\"}\n",
            ev.result, ev.sequence, ev.pid, ev.tid, ev.tick, reinterpret_cast<void*>(ev.caller), module_json.c_str(), value, ev.arg1_bits, reinterpret_cast<void*>(ev.arg2));
        return;
    }
    const std::string text_json = EscapeJson(ev.text);
    _snprintf_s(out, out_size, _TRUNCATE,
        "{\"type\":\"%s\",\"sequence\":%lu,\"pid\":%lu,\"tid\":%lu,\"tick_ms\":%llu,\"caller\":\"0x%p\",\"caller_module\":\"%s\",\"objective_file\":\"%s\"}\n",
        ev.type, ev.sequence, ev.pid, ev.tid, ev.tick, reinterpret_cast<void*>(ev.caller), module_json.c_str(), text_json.c_str());
}

static HANDLE OpenLog() {
    wchar_t path[MAX_PATH] = {}; GetTempPathW(ARRAYSIZE(path), path); wcscat_s(path, L"armada-result-observer.jsonl");
    return CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

static HANDLE CreatePipe() {
    return CreateNamedPipeW(kPipeName, PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT, 1, 4096, 4096, 0, nullptr);
}

static HANDLE CreateControlPipe() {
    return CreateNamedPipeW(kControlPipeName, PIPE_ACCESS_DUPLEX,
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                            1, 512, 512, 0, nullptr);
}

static bool IsApprovedCampaignMap(const char* map) {
    static const char* const approved[] = {
        "fed1.bzn", "fed2.bzn", "fed5.bzn", "fed3.bzn",
        "kling3.bzn", "kling1.bzn", "kling4.bzn", "kling5.bzn",
        "rom2.bzn", "rom4.bzn", "rom3.bzn", "rom5.bzn",
        "borg1.bzn", "borg3.bzn", "borg4.bzn", "borg5.bzn",
        "finale4.bzn", "finale1.bzn", "finale5.bzn", "finale6.bzn",
    };
    for (const char* candidate : approved) if (strcmp(map, candidate) == 0) return true;
    return false;
}

struct FindWindowData { DWORD pid; HWND hwnd; };
static BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM param) {
    FindWindowData* data = reinterpret_cast<FindWindowData*>(param);
    DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
    if (pid == data->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
        wchar_t class_name[64] = {};
        GetClassNameW(hwnd, class_name, ARRAYSIZE(class_name));
        // Startup also exposes an untitled #32770 dialog. Its thread happens
        // to match the game, but it is not the window that owns Armada's
        // campaign state. Target the stable DirectDraw host explicitly.
        if (_wcsicmp(class_name, L"Armada") == 0) {
            data->hwnd = hwnd;
            return FALSE;
        }
    }
    return TRUE;
}

static HWND FindGameWindow() {
    FindWindowData data = { GetCurrentProcessId(), nullptr };
    EnumWindows(FindGameWindowProc, reinterpret_cast<LPARAM>(&data));
    return data.hwnd;
}

static uintptr_t FindExport(const char* export_name);
static void RunNebulaTrapFromGameThread();
static void PulseNebulaTrapFromGameThread();
static void RemoveNebulaTrapFromGameThread();

static bool HasPendingCampaignMap() {
    bool pending = false;
    EnterCriticalSection(&g_queue_lock);
    pending = g_pending_campaign_map[0] != 0;
    LeaveCriticalSection(&g_queue_lock);
    return pending;
}

struct CampaignSelectionSource {
    uintptr_t prefix;
    const char* mode;
    uint32_t mode_length;
    uint32_t mode_capacity;
};
struct CampaignSelectionConfig { uint32_t fields[4]; };

static bool DispatchStockCampaignSelection() {
    char map[MAX_PATH] = {};
    EnterCriticalSection(&g_queue_lock);
    strncpy_s(map, g_pending_campaign_map, _TRUNCATE);
    LeaveCriticalSection(&g_queue_lock);
    if (!map[0]) return false;

    uint8_t* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
#if defined(ARMADA_FULL_PICKER_TEST)
    if (!base) return false;
    // Armada+0x14F910 dispatches mode 2 to the campaign modal dialog. The
    // stock controller route reads this mode from the front-end global before
    // it creates the dialog, then performs the normal engine setup after the
    // dialog returns. This is the minimum observed campaign state transition.
    *reinterpret_cast<uint32_t*>(base + 0x28B8C0) = 2;
    CampaignPickerRouteFn picker = reinterpret_cast<CampaignPickerRouteFn>(base + 0x41450);
    EnterCriticalSection(&g_queue_lock);
    g_pending_campaign_map[0] = 0;
    LeaveCriticalSection(&g_queue_lock);
    Status("[ARMADA_OBSERVER] selecting native campaign mode and opening full picker route\n");
    picker(reinterpret_cast<void*>(base + 0x2336E8));
    return true;
#else
    const uintptr_t engine = base ? *reinterpret_cast<uintptr_t*>(base + 0x278810) : 0;
    if (!engine) { Status("[ARMADA_OBSERVER] native campaign setup: engine unavailable\n"); return false; }

    // Captured from the stock Romulan Mission 1 confirmation path:
    // bool/prefix = 0, std::string-like mode = "Single Player Game", and
    // a zeroed 16-byte configuration object. Armada owns the resulting
    // selection; this temporary input is read-only during the call.
    static const char mode[] = "Single Player Game";
    CampaignSelectionSource source = {};
    source.mode = mode; source.mode_length = 18; source.mode_capacity = 31;
    CampaignSelectionConfig config = {};
    EngineSelectionSetupFn setup = reinterpret_cast<EngineSelectionSetupFn>(base + 0xC9ED0);
    Status("[ARMADA_OBSERVER] invoking stock campaign-selection setup on UI thread\n");
    setup(reinterpret_cast<void*>(engine), &source, reinterpret_cast<const char*>(&config));

    const uintptr_t selection = *reinterpret_cast<uintptr_t*>(engine + 0x20);
    const uintptr_t descriptor = selection ? *reinterpret_cast<uintptr_t*>(selection + 0x0C) : 0;
    if (!descriptor) { Status("[ARMADA_OBSERVER] native campaign setup produced no selection\n"); return false; }
    strcpy_s(reinterpret_cast<char*>(descriptor + 0x1C), MAX_PATH - 0x1C, map);
    uint8_t* controller = base + 0x2336E8;
    strcpy_s(reinterpret_cast<char*>(controller + 0x74), 64, map);
    EnterCriticalSection(&g_queue_lock); g_pending_campaign_map[0] = 0; LeaveCriticalSection(&g_queue_lock);
    Status("[ARMADA_OBSERVER] dispatching native campaign selection after stock UI setup\n");
    reinterpret_cast<CampaignSelectionDispatchFn>(base + 0x407B0)(controller);
    return true;
#endif
}

static LRESULT CALLBACK CampaignDispatchHook(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0) {
        MSG* message = reinterpret_cast<MSG*>(lparam);
        if (message && (message->message == kCampaignDispatchMessage ||
                        (message->message == WM_TIMER && message->wParam == kCampaignDispatchTimer))) {
            message->message = WM_NULL;
            // The initial private message can arrive while Bink/startup is
            // still constructing Armada's campaign engine. Keep the map
            // queued and let the UI timer retry once that stock state exists.
            if (DispatchStockCampaignSelection()) {
                HWND window = FindGameWindow();
                if (window) KillTimer(window, kCampaignDispatchTimer);
            } else if (InterlockedIncrement(&g_campaign_dispatch_retries) >= 3) {
                // The campaign engine does not exist at the main menu.  Do
                // not leave a repeating timer behind when a caller requests
                // direct dispatch before Armada has completed its own modal
                // campaign transition.
                EnterCriticalSection(&g_queue_lock);
                g_pending_campaign_map[0] = 0;
                LeaveCriticalSection(&g_queue_lock);
                HWND window = FindGameWindow();
                if (window) KillTimer(window, kCampaignDispatchTimer);
                Status("[ARMADA_OBSERVER] native campaign request rejected: campaign engine requires stock modal completion\n");
            }
        }
    }
    return CallNextHookEx(g_campaign_dispatch_hook, code, wparam, lparam);
}

static bool EnsureCampaignDispatchHook(HWND* window_out) {
    HWND window = FindGameWindow();
    if (!window) return false;
    if (!g_campaign_dispatch_hook) {
        DWORD thread_id = GetWindowThreadProcessId(window, nullptr);
        g_campaign_dispatch_hook = SetWindowsHookExW(WH_GETMESSAGE, CampaignDispatchHook,
                                                       g_observer_module, thread_id);
        if (!g_campaign_dispatch_hook) return false;
        Status("[ARMADA_OBSERVER] native campaign UI-thread message hook installed\n");
    }
    if (window_out) *window_out = window;
    return true;
}

// This hook is attached specifically to the script thread identified from a
// live objective callback.  It is a private thread-message dispatch, not
// synthetic keyboard or mouse input.
static LRESULT CALLBACK MetaphasicDispatchHook(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0) {
        MSG* message = reinterpret_cast<MSG*>(lparam);
        if (message && message->message == kMetaphasicDispatchMessage) {
            message->message = WM_NULL;
            RunNebulaTrapFromGameThread();
        } else if (message && message->message == kMetaphasicRemoveMessage) {
            message->message = WM_NULL;
            PulseNebulaTrapFromGameThread();
        }
    }
    return CallNextHookEx(g_metaphasic_dispatch_hook, code, wparam, lparam);
}

static DWORD WINAPI NebulaPulseWorker(void*) {
    Sleep(kNebulaPulseMilliseconds);
    if (!PostThreadMessageW(g_metaphasic_game_thread, kMetaphasicRemoveMessage, 0, 0)) {
        Status("[ARMADA_OBSERVER] active nebula trap pulse post failed\n");
    }
    return 0;
}

static bool EnsureMetaphasicDispatchHook() {
    const DWORD thread_id = g_metaphasic_game_thread;
    if (!thread_id) return false;
    if (!g_metaphasic_dispatch_hook) {
        g_metaphasic_dispatch_hook = SetWindowsHookExW(WH_GETMESSAGE, MetaphasicDispatchHook,
                                                        g_observer_module, thread_id);
        if (!g_metaphasic_dispatch_hook) return false;
        Status("[ARMADA_OBSERVER] nebula trap script-thread message hook installed\n");
    }
    return true;
}

struct NebulaTrapConfig { const char* command; const char* odf; DWORD duration_ms; };
static const NebulaTrapConfig* FindNebulaTrap(const char* command) {
    static const NebulaTrapConfig traps[] = {
        {"mutara", "mnebula3", 20000}, {"cerulean", "mnebula5", 20000},
        {"metrion", "mnebula2", 10000}, {"radioactive", "mnebula1", 10000},
        {"metaphasic", "mnebula4", 5000},
    };
    if (strcmp(command, "random") == 0) {
        // The generic Archipelago trap preserves the intended weighted list:
        // Mutara 50%, Cerulean 30%, Metrion 12%, Radioactive 8%.
        const DWORD roll = static_cast<DWORD>(GetTickCount64() % 100);
        if (roll < 50) return &traps[0];
        if (roll < 80) return &traps[1];
        if (roll < 92) return &traps[2];
        return &traps[3];
    }
    for (const NebulaTrapConfig& trap : traps) if (strcmp(command, trap.command) == 0) return &trap;
    return nullptr;
}

static bool QueueNebulaTrap(const char* command, char* reply, size_t reply_size) {
    const NebulaTrapConfig* trap = FindNebulaTrap(command);
    if (!trap) { strncpy_s(reply, reply_size, "rejected unknown nebula trap\n", _TRUNCATE); return false; }
    if (InterlockedCompareExchange(&g_metaphasic_test_requested, 1, 0) != 0 ||
        InterlockedCompareExchange(&g_nebula_trap_active, 0, 0) != 0) {
        strncpy_s(reply, reply_size, "busy nebula trap pending or active\n", _TRUNCATE); return false;
    }
    EnterCriticalSection(&g_queue_lock);
    strncpy_s(g_pending_nebula_kind, trap->command, _TRUNCATE);
    LeaveCriticalSection(&g_queue_lock);
    if (!EnsureMetaphasicDispatchHook()) {
        InterlockedExchange(&g_metaphasic_test_requested, 0);
        strncpy_s(reply, reply_size, "rejected nebula script thread unavailable\n", _TRUNCATE); return false;
    }
    if (!PostThreadMessageW(g_metaphasic_game_thread, kMetaphasicDispatchMessage, 0, 0)) {
        InterlockedExchange(&g_metaphasic_test_requested, 0);
        strncpy_s(reply, reply_size, "rejected nebula script-thread post failed\n", _TRUNCATE); return false;
    }
    _snprintf_s(reply, reply_size, _TRUNCATE, "dispatched nebula trap %s\n", trap->command);
    return true;
}

static DWORD WINAPI ControlWorker(void*) {
    for (;;) {
        HANDLE pipe = CreateControlPipe();
        if (pipe == INVALID_HANDLE_VALUE) { Status("[ARMADA_OBSERVER] CreateNamedPipeW control failed\\n"); return 0; }
        BOOL connected = ConnectNamedPipe(pipe, nullptr);
        if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) { CloseHandle(pipe); if (!g_running) break; continue; }
        char command[256] = {}; DWORD read = 0;
        const BOOL ok = ReadFile(pipe, command, sizeof(command) - 1, &read, nullptr);
        command[ok ? read : 0] = 0;
        char map[MAX_PATH] = {};
        char trap[24] = {};
        if (ok && strcmp(command, "test_metaphasic_fed1") == 0) {
            char reply[128] = {};
            QueueNebulaTrap("metaphasic", reply, ARRAYSIZE(reply));
            Status("[ARMADA_OBSERVER] Metaphasic Fed1 diagnostic test request received\n");
            DWORD written = 0; WriteFile(pipe, reply, static_cast<DWORD>(strlen(reply)), &written, nullptr);
        } else if (ok && sscanf_s(command, "apply_trap %23s", trap, static_cast<unsigned>(_countof(trap))) == 1) {
            char reply[128] = {};
            QueueNebulaTrap(trap, reply, ARRAYSIZE(reply));
            char status[192] = {};
            _snprintf_s(status, ARRAYSIZE(status), _TRUNCATE, "[ARMADA_OBSERVER] nebula trap control request %s: %s", trap, reply);
            Status(status);
            DWORD written = 0; WriteFile(pipe, reply, static_cast<DWORD>(strlen(reply)), &written, nullptr);
        } else if (ok && sscanf_s(command, "launch_map %259s", map, static_cast<unsigned>(_countof(map))) == 1 && IsApprovedCampaignMap(map)) {
            HWND game_window = nullptr;
            if (!EnsureCampaignDispatchHook(&game_window)) {
                Status("[ARMADA_OBSERVER] native campaign dispatch rejected: game window unavailable\n");
                const char reply[] = "rejected game window unavailable\n";
                DWORD written = 0; WriteFile(pipe, reply, sizeof(reply) - 1, &written, nullptr);
            } else {
                EnterCriticalSection(&g_queue_lock);
                strncpy_s(g_pending_campaign_map, map, _TRUNCATE);
                LeaveCriticalSection(&g_queue_lock);
                InterlockedExchange(&g_campaign_dispatch_retries, 0);
                const DWORD thread_id = GetWindowThreadProcessId(game_window, nullptr);
                if (!SetTimer(game_window, kCampaignDispatchTimer, 100, nullptr)) {
                    EnterCriticalSection(&g_queue_lock); g_pending_campaign_map[0] = 0; LeaveCriticalSection(&g_queue_lock);
                    Status("[ARMADA_OBSERVER] native campaign dispatch rejected: retry timer failed\n");
                    const char reply[] = "rejected native dispatch timer failed\n";
                    DWORD written = 0; WriteFile(pipe, reply, sizeof(reply) - 1, &written, nullptr);
                    DisconnectNamedPipe(pipe); CloseHandle(pipe); continue;
                }
                if (!PostThreadMessageW(thread_id, kCampaignDispatchMessage, 0, 0)) {
                    EnterCriticalSection(&g_queue_lock); g_pending_campaign_map[0] = 0; LeaveCriticalSection(&g_queue_lock);
                    KillTimer(game_window, kCampaignDispatchTimer);
                    Status("[ARMADA_OBSERVER] native campaign dispatch rejected: post failed\n");
                    const char reply[] = "rejected native dispatch post failed\n";
                    DWORD written = 0; WriteFile(pipe, reply, sizeof(reply) - 1, &written, nullptr);
                } else {
                    Status("[ARMADA_OBSERVER] native campaign request queued\n");
                    const char reply[] = "queued native campaign controller route\n";
                    DWORD written = 0; WriteFile(pipe, reply, sizeof(reply) - 1, &written, nullptr);
                }
            }
        } else {
            Status("[ARMADA_OBSERVER] campaign-launch request rejected: unrecognized map\\n");
            const char reply[] = "rejected unrecognized map\n";
            DWORD written = 0; WriteFile(pipe, reply, sizeof(reply) - 1, &written, nullptr);
        }
        DisconnectNamedPipe(pipe); CloseHandle(pipe);
        if (!g_running) break;
    }
    return 0;
}

static DWORD WINAPI Worker(void*) {
    HANDLE log = OpenLog(), pipe = g_pipe; bool connected = false;
    for (;;) {
#if defined(ARMADA_FULL_PICKER_TEST)
        // The injector loads this test DLL before the suspended Armada process
        // resumes.  Startup must enter its own front-end route; recursively
        // calling the controller from a later UI message only delays the
        // picker until the main menu exits.  The startup map remains queued
        // solely as proof that this isolated build was launched by the test.
#endif
        ResultEvent ev;
        while (PopEvent(&ev)) {
            char line[1024] = {}; FormatEvent(ev, line, ARRAYSIZE(line));
            if (log != INVALID_HANDLE_VALUE) { DWORD n = 0; WriteFile(log, line, static_cast<DWORD>(strlen(line)), &n, nullptr); }
            if (connected && pipe != INVALID_HANDLE_VALUE) { DWORD n = 0; if (!WriteFile(pipe, line, static_cast<DWORD>(strlen(line)), &n, nullptr)) { DisconnectNamedPipe(pipe); CloseHandle(pipe); pipe = CreatePipe(); connected = false; } }
            OutputDebugStringA(line);
        }
        if (!connected && pipe != INVALID_HANDLE_VALUE) { BOOL ok = ConnectNamedPipe(pipe, nullptr); if (ok || GetLastError() == ERROR_PIPE_CONNECTED) connected = true; }
        if (WaitForSingleObject(g_wake, 100) == WAIT_OBJECT_0) ResetEvent(g_wake);
        if (!g_running) break;
    }
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe); if (log != INVALID_HANDLE_VALUE) CloseHandle(log); return 0;
}

static uintptr_t FindResultFunction(const char* export_name, uint8_t terminal_state) {
    HMODULE image = GetModuleHandleW(nullptr); if (!image) return 0;
    FARPROC named = GetProcAddress(image, export_name);
    if (named) { uint8_t* p = reinterpret_cast<uint8_t*>(named); if (memcmp(p, "\x55\x8b\xec\x8b\x15", 5) == 0 && p[80] == terminal_state) return reinterpret_cast<uintptr_t>(p); }
    uint8_t* base = reinterpret_cast<uint8_t*>(image); IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base); IMAGE_NT_HEADERS32* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew); IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s) { if (memcmp(sections[s].Name, ".text", 5) != 0) continue; uint8_t* start = base + sections[s].VirtualAddress; SIZE_T size = sections[s].Misc.VirtualSize; for (SIZE_T i = 0; i + 81 < size; ++i) if (memcmp(start+i, "\x55\x8b\xec\x8b\x15", 5) == 0 && start[i+80] == terminal_state) return reinterpret_cast<uintptr_t>(start+i); }
    return 0;
}

static bool InstallHook(uintptr_t target, void* hook, void** original, SIZE_T overwrite) {
    if (!target || !hook || overwrite < 5 || overwrite > 16) return false;
    uint8_t* trampoline = reinterpret_cast<uint8_t*>(VirtualAlloc(nullptr, overwrite+5, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE)); if (!trampoline) return false;
    memcpy(trampoline, reinterpret_cast<void*>(target), overwrite); trampoline[overwrite] = 0xE9; *reinterpret_cast<int32_t*>(trampoline+overwrite+1) = static_cast<int32_t>((target+overwrite) - (reinterpret_cast<uintptr_t>(trampoline+overwrite)+5));
    DWORD old = 0; if (!VirtualProtect(reinterpret_cast<void*>(target), overwrite, PAGE_EXECUTE_READWRITE, &old)) { VirtualFree(trampoline, 0, MEM_RELEASE); return false; }
    uint8_t patch[16] = {}; patch[0] = 0xE9; memset(patch + 5, 0x90, overwrite - 5); *reinterpret_cast<int32_t*>(patch+1) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(hook)-target-5); memcpy(reinterpret_cast<void*>(target), patch, overwrite); FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), overwrite); VirtualProtect(reinterpret_cast<void*>(target), overwrite, old, &old);
    *original = trampoline; return true;
}

static uintptr_t FindExport(const char* export_name) {
    HMODULE image = GetModuleHandleW(nullptr);
    return image ? reinterpret_cast<uintptr_t>(GetProcAddress(image, export_name)) : 0;
}

#if defined(ARMADA_FULL_PICKER_TEST)
static bool GetCampaignMapSelection(const char* map, uint32_t* faction, uint8_t* mission) {
    struct Entry { const char* map; uint32_t faction; uint8_t mission; };
    static const Entry entries[] = {
        {"fed1.bzn", 0, 0}, {"fed2.bzn", 0, 1}, {"fed5.bzn", 0, 2}, {"fed3.bzn", 0, 3},
        {"kling3.bzn", 1, 0}, {"kling1.bzn", 1, 1}, {"kling4.bzn", 1, 2}, {"kling5.bzn", 1, 3},
        {"rom2.bzn", 2, 0}, {"rom4.bzn", 2, 1}, {"rom3.bzn", 2, 2}, {"rom5.bzn", 2, 3},
        {"borg1.bzn", 3, 0}, {"borg3.bzn", 3, 1}, {"borg4.bzn", 3, 2}, {"borg5.bzn", 3, 3},
        {"finale4.bzn", 4, 0}, {"finale1.bzn", 4, 1}, {"finale5.bzn", 4, 2}, {"finale6.bzn", 4, 3},
    };
    for (const Entry& entry : entries) {
        if (strcmp(map, entry.map) == 0) {
            *faction = entry.faction;
            *mission = entry.mission;
            return true;
        }
    }
    return false;
}

static bool CompleteStockCampaignPickerSelection(HWND hwnd) {
    char map[MAX_PATH] = {};
    EnterCriticalSection(&g_queue_lock);
    strncpy_s(map, g_pending_campaign_map, _TRUNCATE);
    LeaveCriticalSection(&g_queue_lock);
    uint32_t faction = 0; uint8_t mission = 0;
    if (!map[0] || !GetCampaignMapSelection(map, &faction, &mission)) return false;
    uint8_t* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return false;

    // Mirrors the stock mission-click prelude at Armada+0x14A915: the picker
    // keeps a UI timer while it is open, and that timer otherwise restores
    // campaign mode after EndDialog succeeds.
    const UINT_PTR picker_timer = static_cast<UINT_PTR>(*reinterpret_cast<uintptr_t*>(base + 0x28B3A8));
    if (picker_timer) KillTimer(hwnd, picker_timer);
    *reinterpret_cast<uintptr_t*>(base + 0x28B3A8) = 0;

    // Mirrors the completed stock picker branch at Armada+0x14AAC4. The
    // dialog has already initialized its campaign tables; this provides its
    // selected entry and normal completion result to the owning route.
    *reinterpret_cast<uint32_t*>(base + 0x28B8FC) = faction;
    *reinterpret_cast<uint8_t*>(base + 0x28B904) = mission;
    strcpy_s(reinterpret_cast<char*>(base + 0x2336E8 + 0x74), 64, map);
    *reinterpret_cast<uint8_t*>(base + 0x28B9D5) = mission;
    // Stock picker branch at Armada+0x14AAD5 passes front-end state 3 here.
    // This is cdecl(int), not a parameterless notification.
    reinterpret_cast<void (__cdecl*)(int)>(base + 0x3F0D0)(3);
    *reinterpret_cast<uint32_t*>(base + 0x2337AC) = 0;
    EnterCriticalSection(&g_queue_lock);
    g_pending_campaign_map[0] = 0;
    LeaveCriticalSection(&g_queue_lock);
    SetLastError(ERROR_SUCCESS);
    const BOOL ended = EndDialog(hwnd, 1);
    // The stock branch writes mode zero immediately before EndDialog.  The
    // timer-driven test callback can still see mode two restored while USER32
    // unwinds; reassert the already-selected stock exit mode before returning
    // to the modal loop.
    if (ended) *reinterpret_cast<uint32_t*>(base + 0x28B8C0) = 0;
    char status[192] = {};
    _snprintf_s(status, ARRAYSIZE(status), _TRUNCATE,
                "[ARMADA_OBSERVER] completed native campaign picker selection EndDialog=%d error=%lu\n",
                ended ? 1 : 0, GetLastError());
    Status(status);
    return true;
}

static bool OpenStockMissionPicker(HWND hwnd) {
    char map[MAX_PATH] = {};
    EnterCriticalSection(&g_queue_lock);
    strncpy_s(map, g_pending_campaign_map, _TRUNCATE);
    LeaveCriticalSection(&g_queue_lock);
    uint32_t faction = 0; uint8_t mission = 0;
    if (!map[0] || !GetCampaignMapSelection(map, &faction, &mission)) return false;
    uint8_t* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return false;

    // The outer picker assigns this faction index before it opens the stock
    // resource-0x124 mission dialog at Armada+0x146FF0.
    const UINT_PTR picker_timer = static_cast<UINT_PTR>(*reinterpret_cast<uintptr_t*>(base + 0x28B3A8));
    if (picker_timer) KillTimer(hwnd, picker_timer);
    *reinterpret_cast<uintptr_t*>(base + 0x28B3A8) = 0;
    static const size_t outer_entry_offsets[] = {
        0x28B560, 0x28B558, 0x28B3D0, 0x28B3C8, 0x28B564,
    };
    // The real Romulan outer click enters the stock branch with active flags
    // [0, 0, 1, 0, 0].  This is the missing state that makes the modal loop
    // accept the later mission confirmation instead of reopening the picker.
    for (size_t index = 0; index < ARRAYSIZE(outer_entry_offsets); ++index) {
        uintptr_t entry = *reinterpret_cast<uintptr_t*>(base + outer_entry_offsets[index]);
        if (entry) *reinterpret_cast<uint32_t*>(entry + 0x154) = index == faction ? 1u : 0u;
    }
    *reinterpret_cast<uint32_t*>(base + 0x28B8FC) = faction;
    InterlockedExchange(&g_auto_select_mission, 1);
    const int result = reinterpret_cast<int (__cdecl*)(HWND)>(base + 0x146FF0)(hwnd);
    if (result == 1) return CompleteStockCampaignPickerSelection(hwnd);
    InterlockedExchange(&g_auto_select_mission, 0);
    Status("[ARMADA_OBSERVER] native mission dialog did not confirm selection\n");
    return false;
}

static INT_PTR CALLBACK HookCampaignDialogProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_TIMER && wparam == kCampaignAutoSelectTimer &&
        InterlockedCompareExchange(&g_auto_select_campaign_map, 0, 1) == 1) {
        KillTimer(hwnd, kCampaignAutoSelectTimer);
        // This invokes the stock mission dialog and selection-model action;
        // it does not synthesize pointer or keyboard input.
        if (OpenStockMissionPicker(hwnd)) return FALSE;
    }
    const INT_PTR result = g_original_campaign_dialog_proc
        ? g_original_campaign_dialog_proc(hwnd, message, wparam, lparam) : FALSE;
    if (message == WM_INITDIALOG &&
        InterlockedCompareExchange(&g_auto_select_campaign_map, 1, 0) == 0 &&
        HasPendingCampaignMap()) {
        if (SetTimer(hwnd, kCampaignAutoSelectTimer, 100, nullptr)) {
            Status("[ARMADA_OBSERVER] native campaign picker initialized; selection queued\n");
        } else {
            InterlockedExchange(&g_auto_select_campaign_map, 0);
            Status("[ARMADA_OBSERVER] native campaign picker selection timer failed\n");
        }
    }
    return result;
}

static INT_PTR CALLBACK HookMissionDialogProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_TIMER && wparam == kMissionAutoSelectTimer &&
        InterlockedCompareExchange(&g_auto_select_mission, 0, 1) == 1) {
        KillTimer(hwnd, kMissionAutoSelectTimer);
        // Exact local coordinate observed for the first Romulan mission in
        // the stock mission dialog.  The real dialog handler marks the row;
        // ending the dialog then lets the real outer picker finish its normal
        // map-copy and campaign-dispatch route.
        Status("[ARMADA_OBSERVER] dispatching local Romulan mission-list message\n");
        SendMessageW(hwnd, WM_LBUTTONUP, 0x80, MAKELPARAM(0x5A, 0x69));
        SetLastError(ERROR_SUCCESS);
        const BOOL ended = EndDialog(hwnd, 1);
        char status[192] = {};
        _snprintf_s(status, ARRAYSIZE(status), _TRUNCATE,
                    "[ARMADA_OBSERVER] completed local mission dialog EndDialog=%d error=%lu\n",
                    ended ? 1 : 0, GetLastError());
        Status(status);
        return FALSE;
    }
    const INT_PTR result = g_original_mission_dialog_proc
        ? g_original_mission_dialog_proc(hwnd, message, wparam, lparam) : FALSE;
    if (message == WM_INITDIALOG && InterlockedCompareExchange(&g_auto_select_mission, 1, 1) == 1) {
        if (SetTimer(hwnd, kMissionAutoSelectTimer, 100, nullptr)) {
            Status("[ARMADA_OBSERVER] native mission dialog initialized; selection queued\n");
        } else {
            InterlockedExchange(&g_auto_select_mission, 0);
            Status("[ARMADA_OBSERVER] native mission dialog selection timer failed\n");
        }
    }
    return result;
}

static void __fastcall HookEngineSelectionSetup(void* engine, void*, const void* source, const char* config) {
    uint8_t* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    char before_map[64] = {};
    if (base) strncpy_s(before_map, reinterpret_cast<const char*>(base + 0x2336E8 + 0x74), _TRUNCATE);
    char status[256] = {};
    _snprintf_s(status, ARRAYSIZE(status), _TRUNCATE,
                "[ARMADA_OBSERVER] stock engine selection setup entered engine=%p controller_map=%s\n",
                engine, before_map);
    Status(status);
    if (g_original_engine_selection_setup) g_original_engine_selection_setup(engine, source, config);
    const uintptr_t selection = engine ? *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(engine) + 0x20) : 0;
    _snprintf_s(status, ARRAYSIZE(status), _TRUNCATE,
                "[ARMADA_OBSERVER] stock engine selection setup returned selection=%p\n",
                reinterpret_cast<void*>(selection));
    Status(status);
}

static void __fastcall HookCampaignSelectionDispatch(void* controller, void*) {
    const char* map = reinterpret_cast<const char*>(reinterpret_cast<uint8_t*>(controller) + 0x74);
    char status[256] = {};
    _snprintf_s(status, ARRAYSIZE(status), _TRUNCATE,
                "[ARMADA_OBSERVER] stock campaign selection dispatch entered controller=%p map=%s\n",
                controller, map ? map : "");
    Status(status);
    if (g_original_campaign_selection_dispatch) g_original_campaign_selection_dispatch(controller);
}

static bool __fastcall HookCampaignControllerDialogRoute(void* controller, void*) {
    char status[192] = {};
    _snprintf_s(status, ARRAYSIZE(status), _TRUNCATE,
                "[ARMADA_OBSERVER] stock campaign controller dialog route entered controller=%p\n", controller);
    Status(status);
    const bool result = g_original_campaign_controller_dialog_route
        ? g_original_campaign_controller_dialog_route(controller) : false;
    _snprintf_s(status, ARRAYSIZE(status), _TRUNCATE,
                "[ARMADA_OBSERVER] stock campaign controller dialog route returned result=%d mode=%lu\n",
                result ? 1 : 0,
                *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)) + 0x1FA810));
    Status(status);
    return result;
}

// Armada+0x14F910 is the stock cdecl modal dispatcher.  Redirect only its
// first startup main-menu request; the original function then creates and
// owns the campaign dialog exactly as it does after a player selects Campaign.
static void __cdecl HookFrontEndModalDispatch(int mode) {
    int dispatch_mode = mode;
    if (mode == 1 && InterlockedCompareExchange(&g_redirect_initial_front_end_mode, 0, 1) == 1) {
        dispatch_mode = 2;
        Status("[ARMADA_OBSERVER] redirected initial stock front-end mode 1 to campaign mode 2\n");
    }
    if (g_original_front_end_modal_dispatch) g_original_front_end_modal_dispatch(dispatch_mode);
    uint8_t* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (base && dispatch_mode == 2) {
        char status[256] = {};
        _snprintf_s(status, ARRAYSIZE(status), _TRUNCATE,
                    "[ARMADA_OBSERVER] stock campaign modal returned mode=%lu completion=%lu controller_map=%s\n",
                    *reinterpret_cast<uint32_t*>(base + 0x28B8C0),
                    *reinterpret_cast<uint32_t*>(base + 0x2337AC),
                    reinterpret_cast<const char*>(base + 0x2336E8 + 0x74));
        Status(status);
    }
}
#endif

static DWORD WINAPI FailureExitWorker(void*) {
    // Let the result callback return cleanly and allow the pipe worker one
    // short scheduling interval to persist its diagnostic event.  Failure
    // carries no Archipelago check, so it must not depend on a client pipe
    // delivery in order to return the player to the mission launcher.
    Sleep(750);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

static void __cdecl HookSucceed(float value, const char* text) { void* caller = _ReturnAddress(); QueueEvent("mission_result", "success", value, text, caller); if (g_original_succeed) g_original_succeed(value, text); }
static void __cdecl HookFail(float value, const char* text) {
    void* caller = _ReturnAddress();
    QueueEvent("mission_result", "failure", value, text, caller);
    if (g_original_fail) g_original_fail(value, text);
    if (InterlockedExchange(&g_failure_exit_queued, 1) == 0) {
        Status("[ARMADA_OBSERVER] failure observed; scheduling Armada exit\n");
        HANDLE worker = CreateThread(nullptr, 0, FailureExitWorker, nullptr, 0, nullptr);
        if (worker) CloseHandle(worker);
        else Status("[ARMADA_OBSERVER] failure exit worker creation failed\n");
    }
}
static void RunNebulaTrapFromGameThread() {
    if (InterlockedCompareExchange(&g_metaphasic_test_requested, 0, 1) != 1) return;
    char trap_name[24] = {};
    EnterCriticalSection(&g_queue_lock);
    strncpy_s(trap_name, g_pending_nebula_kind, _TRUNCATE);
    LeaveCriticalSection(&g_queue_lock);
    const NebulaTrapConfig* trap = FindNebulaTrap(trap_name);
    if (!trap) {
        InterlockedExchange(&g_metaphasic_test_requested, 0);
        Status("[ARMADA_OBSERVER] nebula trap rejected: missing queued trap configuration\n");
        return;
    }
    g_active_nebula_duration_ms = trap->duration_ms;
    g_active_nebula_deadline = GetTickCount64() + trap->duration_ms;
    InterlockedExchange(&g_nebula_trap_active, 1);
    InterlockedExchange(&g_missing_player_target_logged, 0);
    char status[160] = {};
    _snprintf_s(status, ARRAYSIZE(status), _TRUNCATE,
                "[ARMADA_OBSERVER] nebula trap %s started: %s pulses every %lu ms for %lu ms\n",
                trap->command, trap->odf, kNebulaPulseMilliseconds, trap->duration_ms);
    Status(status);
    PulseNebulaTrapFromGameThread();
}

static int FindLivePlayerTarget() {
    struct HeroShip { const char* module; const char* object; };
    static const HeroShip heroes[] = {
        {"federation1s.dsl", "Enterprise"}, {"federation2s.dsl", "Avenger"},
        {"federation5s.dsl", "Avenger"}, {"federation3s.dsl", "Enterprise"},
        {"klingon3s.dsl", "Martok"}, {"klingon1s.dsl", "Martok"},
        {"klingon4s.dsl", "StarbaseM"}, {"klingon5s.dsl", "Martok"},
        {"romulan2s.dsl", "Sela"}, {"romulan4s.dsl", "Sela"},
        {"romulan3s.dsl", "Sela"}, {"romulan5s.dsl", "Sela"},
        {"borg1s.dsl", "Locutus"}, {"borg3s.dsl", "Locutus"},
        {"borg4s.dsl", "Locutus"}, {"borg5s.dsl", "Locutus"},
        {"finale4s.dsl", "Enterprise"}, {"finale1s.dsl", "Enterprise"},
        {"finale5s.dsl", "Enterprise"}, {"finale6s.dsl", "Enterprise"},
    };
    char module[MAX_PATH] = {};
    EnterCriticalSection(&g_queue_lock);
    strncpy_s(module, g_active_mission_module, _TRUNCATE);
    LeaveCriticalSection(&g_queue_lock);
    const char* hero = nullptr;
    for (const HeroShip& entry : heroes) {
        if (_stricmp(entry.module, module) == 0) { hero = entry.object; break; }
    }
    if (!hero) return 0;
    GetHandleFn get_handle = reinterpret_cast<GetHandleFn>(FindExport("?GetHandle@@YAHPAD@Z"));
    IsValidFn valid = reinterpret_cast<IsValidFn>(FindExport("?IsValid@@YA_NH@Z"));
    if (!get_handle || !valid) return 0;
    char object[32] = {};
    strncpy_s(object, hero, _TRUNCATE);
    const int target = get_handle(object);
    return target && valid(target) ? target : 0;
}

static void PulseNebulaTrapFromGameThread() {
    if (InterlockedCompareExchange(&g_nebula_trap_active, 0, 0) != 1) return;
    if (GetTickCount64() >= g_active_nebula_deadline) {
        RemoveNebulaTrapFromGameThread();
        return;
    }
    RemoveObjectFn remove = reinterpret_cast<RemoveObjectFn>(FindExport("?RemoveObject@@YAXH@Z"));
    const int previous = InterlockedExchange(&g_metaphasic_object, 0);
    if (previous && remove) remove(previous);
    const int target = FindLivePlayerTarget();
    if (!target) {
        if (InterlockedCompareExchange(&g_missing_player_target_logged, 1, 0) == 0)
            Status("[ARMADA_OBSERVER] nebula trap waiting for the named mission hero target\n");
    } else {
        char trap_name[24] = {};
        EnterCriticalSection(&g_queue_lock);
        strncpy_s(trap_name, g_pending_nebula_kind, _TRUNCATE);
        LeaveCriticalSection(&g_queue_lock);
        const NebulaTrapConfig* trap = FindNebulaTrap(trap_name);
        BuildObjectAtReferenceFn build = reinterpret_cast<BuildObjectAtReferenceFn>(FindExport("?BuildObject@@YAHPADHHMMM@Z"));
        if (trap && build) {
            char nebula_name[16] = {};
            strncpy_s(nebula_name, trap->odf, _TRUNCATE);
            const int nebula = build(nebula_name, 1, target, 0.0f, 0.0f, 0.0f);
            if (nebula) InterlockedExchange(&g_metaphasic_object, nebula);
        }
    }
    HANDLE pulse = CreateThread(nullptr, 0, NebulaPulseWorker, nullptr, 0, nullptr);
    if (pulse) CloseHandle(pulse);
    else Status("[ARMADA_OBSERVER] active nebula trap pulse worker creation failed\n");
}

static void RemoveNebulaTrapFromGameThread() {
    if (InterlockedExchange(&g_nebula_trap_active, 0) != 1) return;
    const int nebula = InterlockedExchange(&g_metaphasic_object, 0);
    RemoveObjectFn remove = reinterpret_cast<RemoveObjectFn>(FindExport("?RemoveObject@@YAXH@Z"));
    if (nebula && remove) remove(nebula);
    Status("[ARMADA_OBSERVER] active nebula trap removed after its configured duration\n");
}

static void __cdecl HookObjectiveText(const char* text) {
    void* caller = _ReturnAddress();
    QueueEvent("objective_display", "", 0.0f, text, caller);
    if (g_original_objective_text) g_original_objective_text(text);
    // This records the real script/game thread and its current mission module.
    // Trap delivery is still control-message driven, never objective driven.
    g_metaphasic_game_thread = GetCurrentThreadId();
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(caller), &module) &&
        GetModuleFileNameW(module, path, ARRAYSIZE(path))) {
        const wchar_t* filename = wcsrchr(path, L'\\');
        filename = filename ? filename + 1 : path;
        char module_name[MAX_PATH] = {};
        WideCharToMultiByte(CP_ACP, 0, filename, -1, module_name, ARRAYSIZE(module_name), nullptr, nullptr);
        if (strstr(module_name, ".dsl")) {
            EnterCriticalSection(&g_queue_lock);
            strncpy_s(g_active_mission_module, module_name, _TRUNCATE);
            LeaveCriticalSection(&g_queue_lock);
        }
    }
}
static bool ConsumeLaunchMovieSkip() {
    if (GetTickCount64() > g_skip_movie_deadline) { InterlockedExchange(&g_skip_next_launch_movie, 0); return false; }
    return InterlockedCompareExchange(&g_skip_next_launch_movie, 0, 1) == 1;
}
static void __cdecl HookPlayBridgeMovie(char* filename, int value) {
    if (ConsumeLaunchMovieSkip()) { Status("[ARMADA_OBSERVER] suppressed initial bridge movie\n"); return; }
    if (g_original_play_bridge_movie) g_original_play_bridge_movie(filename, value);
}
static void __cdecl HookPlayCinematicMovie(char* filename, int value) {
    if (ConsumeLaunchMovieSkip()) { Status("[ARMADA_OBSERVER] suppressed initial cinematic movie\n"); return; }
    if (g_original_play_cinematic_movie) g_original_play_cinematic_movie(filename, value);
}

static bool IsStartupIntroBink(const char* filename) {
    if (!filename) return false;
    static const char needle[] = "stintro.bik";
    for (const char* p = filename; *p; ++p) {
        if (_strnicmp(p, needle, ARRAYSIZE(needle) - 1) == 0) return true;
    }
    return false;
}

static bool DispatchPendingCampaignFromGameThread() {
    char map[MAX_PATH] = {};
    EnterCriticalSection(&g_queue_lock);
    strncpy_s(map, g_pending_campaign_map, _TRUNCATE);
    LeaveCriticalSection(&g_queue_lock);
    if (!map[0]) return false;
    uint8_t* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    const uintptr_t engine = *reinterpret_cast<uintptr_t*>(base + 0x278810);
    if (!engine) {
        Status("[ARMADA_OBSERVER] native campaign setup: engine unavailable\n");
        return false;
    }
    const uintptr_t selection = *reinterpret_cast<uintptr_t*>(engine + 0x20);
    const uintptr_t descriptor = selection ? *reinterpret_cast<uintptr_t*>(selection + 0x0C) : 0;
    if (!descriptor) {
        Status("[ARMADA_OBSERVER] native campaign setup: selection unavailable\n");
        return false;
    }
    strcpy_s(reinterpret_cast<char*>(descriptor + 0x1C), MAX_PATH - 0x1C, map);
    // The stock click handler maintains this parallel controller field before
    // it invokes the dispatcher. Without it, Armada can produce an empty map
    // shell instead of loading the campaign's .dsl/.drl scripts.
    strcpy_s(reinterpret_cast<char*>(base + 0x2336E8 + 0x74), 64, map);
    CampaignSelectionDispatchFn dispatch = reinterpret_cast<CampaignSelectionDispatchFn>(base + 0x407B0);
    Status("[ARMADA_OBSERVER] dispatching native campaign selection after stock setup\n");
    EnterCriticalSection(&g_queue_lock);
    g_pending_campaign_map[0] = 0;
    LeaveCriticalSection(&g_queue_lock);
    dispatch(reinterpret_cast<void*>(base + 0x2336E8));
    return true;
}

// Armada+0x4C9ED0 is the observed stock initializer that constructs and
// assigns engine->selection (+0x20). Run the queued request only after that
// initializer returns, on Armada's own game thread.
static DWORD WINAPI HookGetTickCount() {
    const DWORD tick = g_original_get_tick_count ? g_original_get_tick_count() : 0;
    if (g_startup_game_thread && GetCurrentThreadId() == g_startup_game_thread && HasPendingCampaignMap()) {
        DispatchPendingCampaignFromGameThread();
    }
    return tick;
}

static DWORD WINAPI HookTimeGetTime() {
    const DWORD tick = g_original_time_get_time ? g_original_time_get_time() : 0;
    if (g_startup_game_thread && GetCurrentThreadId() == g_startup_game_thread && HasPendingCampaignMap()) {
        DispatchPendingCampaignFromGameThread();
    }
    return tick;
}

static BOOL WINAPI HookQueryPerformanceCounter(LARGE_INTEGER* value) {
    const BOOL result = g_original_query_performance_counter ? g_original_query_performance_counter(value) : FALSE;
    if (HasPendingCampaignMap()) DispatchPendingCampaignFromGameThread();
    return result;
}

static bool InstallArmadaGetTickCountHook() {
    uint8_t* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return false;
    IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    IMAGE_NT_HEADERS32* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress) return false;
    IMAGE_IMPORT_DESCRIPTOR* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        const char* module_name = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(module_name, "KERNEL32.dll") != 0) continue;
        IMAGE_THUNK_DATA32* names = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + descriptor->OriginalFirstThunk);
        IMAGE_THUNK_DATA32* addresses = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addresses) {
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(import->Name), "GetTickCount") != 0) continue;
            DWORD old_protection = 0;
            if (!VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function), PAGE_READWRITE, &old_protection)) return false;
            g_original_get_tick_count = reinterpret_cast<GetTickCountFn>(addresses->u1.Function);
            addresses->u1.Function = reinterpret_cast<ULONG_PTR>(&HookGetTickCount);
            VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function), old_protection, &old_protection);
            FlushInstructionCache(GetCurrentProcess(), &addresses->u1.Function, sizeof(addresses->u1.Function));
            Status("[ARMADA_OBSERVER] Armada GetTickCount scheduler hook installed\n");
            return true;
        }
    }
    return false;
}

static bool InstallArmadaTimeGetTimeHook() {
    uint8_t* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return false;
    IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    IMAGE_NT_HEADERS32* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress) return false;
    IMAGE_IMPORT_DESCRIPTOR* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        const char* module_name = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(module_name, "WINMM.dll") != 0) continue;
        IMAGE_THUNK_DATA32* names = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + descriptor->OriginalFirstThunk);
        IMAGE_THUNK_DATA32* addresses = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addresses) {
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(import->Name), "timeGetTime") != 0) continue;
            DWORD old_protection = 0;
            if (!VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function), PAGE_READWRITE, &old_protection)) return false;
            g_original_time_get_time = reinterpret_cast<TimeGetTimeFn>(addresses->u1.Function);
            addresses->u1.Function = reinterpret_cast<ULONG_PTR>(&HookTimeGetTime);
            VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function), old_protection, &old_protection);
            FlushInstructionCache(GetCurrentProcess(), &addresses->u1.Function, sizeof(addresses->u1.Function));
            Status("[ARMADA_OBSERVER] Armada timeGetTime scheduler hook installed\n");
            return true;
        }
    }
    return false;
}

static bool InstallArmadaQueryPerformanceCounterHook() {
    uint8_t* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return false;
    IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    IMAGE_NT_HEADERS32* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress) return false;
    IMAGE_IMPORT_DESCRIPTOR* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        const char* module_name = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(module_name, "KERNEL32.dll") != 0) continue;
        IMAGE_THUNK_DATA32* names = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + descriptor->OriginalFirstThunk);
        IMAGE_THUNK_DATA32* addresses = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addresses) {
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(import->Name), "QueryPerformanceCounter") != 0) continue;
            DWORD old_protection = 0;
            if (!VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function), PAGE_READWRITE, &old_protection)) return false;
            g_original_query_performance_counter = reinterpret_cast<QueryPerformanceCounterFn>(addresses->u1.Function);
            addresses->u1.Function = reinterpret_cast<ULONG_PTR>(&HookQueryPerformanceCounter);
            VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function), old_protection, &old_protection);
            FlushInstructionCache(GetCurrentProcess(), &addresses->u1.Function, sizeof(addresses->u1.Function));
            Status("[ARMADA_OBSERVER] Armada QueryPerformanceCounter scheduler hook installed\n");
            return true;
        }
    }
    return false;
}

static void* __stdcall HookBinkOpen(const char* filename, uint32_t flags) {
    if (IsStartupIntroBink(filename)) {
        Status("[ARMADA_OBSERVER] suppressed STIntro.bik through Bink open failure\n");
        return nullptr;
    }
    return g_original_bink_open ? g_original_bink_open(filename, flags) : nullptr;
}

static void InstallStartupBinkHook() {
    const ULONGLONG deadline = GetTickCount64() + 60000;
    while (GetTickCount64() < deadline && !g_original_bink_open) {
        HMODULE bink = GetModuleHandleW(L"binkw32.dll");
        if (bink) {
            const uintptr_t bink_open = reinterpret_cast<uintptr_t>(GetProcAddress(bink, "_BinkOpen@8"));
            if (InstallHook(bink_open, reinterpret_cast<void*>(&HookBinkOpen), reinterpret_cast<void**>(&g_original_bink_open), 9)) {
                Status("[ARMADA_OBSERVER] Bink startup-video hook installed\n");
            } else {
                Status("[ARMADA_OBSERVER] Bink startup-video hook installation failed\n");
            }
            return;
        }
        Sleep(100);
    }
}

static DWORD WINAPI Initialize(void*) {
    InitializeCriticalSection(&g_queue_lock);
    g_wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_wake) { Status("[ARMADA_OBSERVER] CreateEventW failed\n"); return 0; }
    g_pipe = CreatePipe();
    if (g_pipe == INVALID_HANDLE_VALUE) { Status("[ARMADA_OBSERVER] CreateNamedPipeW failed\n"); return 0; }
    g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (!g_worker) { Status("[ARMADA_OBSERVER] CreateThread failed\n"); CloseHandle(g_pipe); g_pipe = INVALID_HANDLE_VALUE; return 0; }
    g_control_worker = CreateThread(nullptr, 0, ControlWorker, nullptr, 0, nullptr);
    if (!g_control_worker) { Status("[ARMADA_OBSERVER] control worker creation failed\n"); }
    char startup_map[MAX_PATH] = {};
    const DWORD startup_map_length = GetEnvironmentVariableA("ARMADA_LAUNCH_MAP", startup_map, ARRAYSIZE(startup_map));
    if (startup_map_length > 0 && startup_map_length < ARRAYSIZE(startup_map) && IsApprovedCampaignMap(startup_map) && !g_pending_campaign_map[0]) {
        EnterCriticalSection(&g_queue_lock);
        strncpy_s(g_pending_campaign_map, startup_map, _TRUNCATE);
        LeaveCriticalSection(&g_queue_lock);
        Status("[ARMADA_OBSERVER] startup campaign map queued before Armada resume\n");
    } else if (g_pending_campaign_map[0]) {
        Status("[ARMADA_OBSERVER] startup campaign map was queued during DLL attach\n");
    }
#if defined(ARMADA_FULL_PICKER_TEST)
    if (HasPendingCampaignMap()) {
        uint8_t* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
        if (base) {
            // The process is still suspended.  The dispatcher prologue is
            // 9 bytes of whole instructions, verified against this GOG build.
            if (InstallHook(reinterpret_cast<uintptr_t>(base + 0x14F910),
                            reinterpret_cast<void*>(&HookFrontEndModalDispatch),
                            reinterpret_cast<void**>(&g_original_front_end_modal_dispatch), 9)) {
                InterlockedExchange(&g_redirect_initial_front_end_mode, 1);
                Status("[ARMADA_OBSERVER] initial stock front-end redirect armed before Armada resume\n");
            } else {
                Status("[ARMADA_OBSERVER] initial stock front-end redirect installation failed\n");
            }
            // Campaign dialog procedure prologue has a verified 16-byte
            // instruction boundary in the supported GOG executable.
            if (InstallHook(reinterpret_cast<uintptr_t>(base + 0x1499C0),
                            reinterpret_cast<void*>(&HookCampaignDialogProc),
                            reinterpret_cast<void**>(&g_original_campaign_dialog_proc), 16)) {
                Status("[ARMADA_OBSERVER] native campaign picker selection hook armed before Armada resume\n");
            } else {
                Status("[ARMADA_OBSERVER] native campaign picker selection hook installation failed\n");
            }
            if (InstallHook(reinterpret_cast<uintptr_t>(base + 0x147020),
                            reinterpret_cast<void*>(&HookMissionDialogProc),
                            reinterpret_cast<void**>(&g_original_mission_dialog_proc), 16)) {
                Status("[ARMADA_OBSERVER] native mission-list selection hook armed before Armada resume\n");
            } else {
                Status("[ARMADA_OBSERVER] native mission-list selection hook installation failed\n");
            }
            if (InstallHook(reinterpret_cast<uintptr_t>(base + 0xC9ED0),
                            reinterpret_cast<void*>(&HookEngineSelectionSetup),
                            reinterpret_cast<void**>(&g_original_engine_selection_setup), 16) &&
                InstallHook(reinterpret_cast<uintptr_t>(base + 0x407B0),
                            reinterpret_cast<void*>(&HookCampaignSelectionDispatch),
                            reinterpret_cast<void**>(&g_original_campaign_selection_dispatch), 15)) {
                Status("[ARMADA_OBSERVER] post-picker stock route diagnostics armed\n");
            } else {
                Status("[ARMADA_OBSERVER] post-picker stock route diagnostic installation failed\n");
            }
            if (InstallHook(reinterpret_cast<uintptr_t>(base + 0x419F0),
                            reinterpret_cast<void*>(&HookCampaignControllerDialogRoute),
                            reinterpret_cast<void**>(&g_original_campaign_controller_dialog_route), 16)) {
                Status("[ARMADA_OBSERVER] stock campaign controller route diagnostic armed\n");
            } else {
                Status("[ARMADA_OBSERVER] stock campaign controller route diagnostic installation failed\n");
            }
        }
    }
#endif
    uintptr_t succeed = FindResultFunction("?SucceedMission@@YAXMPAD@Z", 0x00), fail = FindResultFunction("?FailMission@@YAXMPAD@Z", 0x01);
    uintptr_t objective_text = FindExport("?ObjectivesDisplay_Set_Text_From_File@@YAXPAD@Z");
    uintptr_t play_bridge_movie = FindExport("?PlayBridgeMovie@@YAXPADH@Z");
    uintptr_t play_cinematic_movie = FindExport("?PlayCinematicMovie@@YAXPADH@Z");
    bool ok_succeed = InstallHook(succeed, reinterpret_cast<void*>(&HookSucceed), reinterpret_cast<void**>(&g_original_succeed), 9);
    bool ok_fail = InstallHook(fail, reinterpret_cast<void*>(&HookFail), reinterpret_cast<void**>(&g_original_fail), 9);
    // Objective display has a 10-byte instruction boundary at its entry; do
    // not reuse the mission-result 9-byte boundary here.
    bool ok_objective = InstallHook(objective_text, reinterpret_cast<void*>(&HookObjectiveText), reinterpret_cast<void**>(&g_original_objective_text), 10);
    // Both movie exports have a verified 9-byte instruction boundary.
    bool ok_bridge_movie = InstallHook(play_bridge_movie, reinterpret_cast<void*>(&HookPlayBridgeMovie), reinterpret_cast<void**>(&g_original_play_bridge_movie), 9);
    bool ok_cinematic_movie = InstallHook(play_cinematic_movie, reinterpret_cast<void*>(&HookPlayCinematicMovie), reinterpret_cast<void**>(&g_original_play_cinematic_movie), 9);
    const bool ok_tick_scheduler = InstallArmadaGetTickCountHook();
    if (!FindGameWindow()) {
        InterlockedExchange(&g_skip_next_launch_movie, 1);
        g_skip_movie_deadline = GetTickCount64() + 60000;
        Status("[ARMADA_OBSERVER] startup movie suppression armed\n");
    }
    InstallStartupBinkHook();
    if (!ok_succeed || !ok_fail || !ok_objective || !ok_bridge_movie || !ok_cinematic_movie || !ok_tick_scheduler) Status("[ARMADA_OBSERVER] hook installation failed\n"); else Status("[ARMADA_OBSERVER] result, objective-display, movie, and Metaphasic test scheduler hooks installed\n"); return 0;
}
}

extern "C" __declspec(dllexport) DWORD WINAPI ArmadaObserverVersion() { return 2; }
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_observer_module = instance;
        DisableThreadLibraryCalls(instance);
        CreateThread(nullptr, 0, Initialize, nullptr, 0, nullptr);
    }
    return TRUE;
}
