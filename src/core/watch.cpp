// Debug: hardware data watchpoint on the active camera node's data (node+0x120) to find which renderer code reads it
// and when. A helper thread sets DR0 on the render thread (suspend / SetThreadContext / resume); a vectored handler
// logs each hit (module + offset of the instruction and of the first few return addresses) and keeps counting.
// Enabled with DebugWatchCameraFrames = N (frames to watch). Off by default; costs one exception per access.
#include <windows.h>
#include <cstdio>
#include <atomic>
#include <tlhelp32.h>
#include <vector>
#include "core.h"
#include "common/log.h"

namespace svr {

static PVOID g_veh = nullptr;
static std::atomic<int> g_hits{0};
static uintptr_t g_watchAddr = 0;
static DWORD g_watchThread = 0;
static char g_seen[64][96];
static volatile LONG g_cnt[64];
static DWORD g_tid[64];
static int g_nSeen = 0;
static bool g_rw = false;   // read/write watch (else write only)
static int g_len = 8;

static void Describe(uintptr_t a, char* out, size_t n) {
    HMODULE m = nullptr;
    wchar_t name[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m))
        GetModuleFileNameW(m, name, MAX_PATH);
    const wchar_t* b = wcsrchr(name, L'\\');
    snprintf(out, n, "%ls+0x%llx", b ? b + 1 : name, (unsigned long long)(a - (uintptr_t)m));
}

static LONG CALLBACK WatchVeh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    if (!(ep->ContextRecord->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    ep->ContextRecord->Dr6 = 0;
    int n = g_hits++;
    char where[96];
    Describe((uintptr_t)ep->ExceptionRecord->ExceptionAddress, where, sizeof(where));
    bool known = false;
    for (int i = 0; i < g_nSeen; i++) if (!strcmp(g_seen[i], where)) { known = true; InterlockedIncrement(&g_cnt[i]); break; }
    if (!known && g_nSeen < 64) {
        g_tid[g_nSeen] = GetCurrentThreadId();
        g_cnt[g_nSeen] = 1;
        strcpy(g_seen[g_nSeen++], where);
        // first few return addresses from the stack (heuristic: values pointing into RemoteRenderer / swtor.exe)
        char chain[512] = "";
        uintptr_t* sp = (uintptr_t*)ep->ContextRecord->Rsp;
        int found = 0;
        for (int i = 0; i < 96 && found < 8; i++) {
            uintptr_t v = sp[i];
            HMODULE m = nullptr;
            if (v > 0x10000 && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)v, &m) &&
                (m == GetModuleHandleW(L"RemoteRenderer.dll") || m == GetModuleHandleW(nullptr))) {
                char d[96];
                Describe(v, d, sizeof(d));
                strncat(chain, " <- ", sizeof(chain) - strlen(chain) - 1);
                strncat(chain, d, sizeof(chain) - strlen(chain) - 1);
                found++;
            }
        }
        DWORD64 imgBase = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry((DWORD64)ep->ExceptionRecord->ExceptionAddress, &imgBase, nullptr);
        char fn[96] = "";
        if (rf) Describe((uintptr_t)(imgBase + rf->BeginAddress), fn, sizeof(fn));
        LOGI("watch: hit #%d at %s (function %s) thread %lu (pass %d, trace %d)%s", n, where, fn, GetCurrentThreadId(), Stereo_Pass(), Trace_Active(), chain);
        if (Trace_Active()) Trace_Event("WATCH camera read at %s", where);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

struct DrArgs { DWORD tid; uintptr_t addr; bool set; bool writeOnly = false; int len = 8; };
static DWORD WINAPI DrThread(void* p) {
    DrArgs* a = (DrArgs*)p;
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, a->tid);
    if (!th) return 1;
    SuspendThread(th);
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(th, &c);
    if (a->set) {
        c.Dr0 = a->addr;
        c.Dr7 = (c.Dr7 & ~0xF0003ull) | 1ull | ((a->writeOnly ? 1ull : 3ull) << 16) | ((a->len == 4 ? 3ull : 2ull) << 18);   // L0, W or R/W, LEN 11b = 4 / 10b = 8 bytes
    } else {
        c.Dr0 = 0;
        c.Dr7 &= ~0xF0003ull;
    }
    SetThreadContext(th, &c);
    ResumeThread(th);
    CloseHandle(th);
    return 0;
}

static void SetDr(DWORD tid, uintptr_t addr, bool set, bool writeOnly = false) {
    DrArgs a = { tid, addr, set, writeOnly, g_len };
    HANDLE t = CreateThread(nullptr, 0, DrThread, &a, 0, nullptr);
    if (t) { WaitForSingleObject(t, 2000); CloseHandle(t); }
}

void Watch_Start(void* node) {
    if (g_veh || !node) return;
    g_watchAddr = (uintptr_t)node + 0x120;
    g_watchThread = GetCurrentThreadId();
    g_veh = AddVectoredExceptionHandler(1, WatchVeh);
    SetDr(g_watchThread, g_watchAddr, true);
    LOGI("watch: data breakpoint on %p (camera node %p + 0x120), thread %lu", (void*)g_watchAddr, node, g_watchThread);
}

// Write watchpoint on any address, on EVERY thread of the process (the game may update it on another thread).
static std::vector<DWORD> g_allThreads;
static void ForAllThreads(uintptr_t addr, bool set) {
    if (set) {
        g_allThreads.clear();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te = { sizeof(te) };
        if (snap != INVALID_HANDLE_VALUE) {
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
                if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != GetCurrentThreadId()) g_allThreads.push_back(te.th32ThreadID);
            CloseHandle(snap);
        }
    }
    for (DWORD t : g_allThreads) SetDr(t, addr, set, !g_rw);
}
void Watch_StartWrite(void* addr, const char* what) {
    if (g_veh || !addr) return;
    g_watchAddr = (uintptr_t)addr & ~7ull;
    g_watchThread = 0;
    g_veh = AddVectoredExceptionHandler(1, WatchVeh);
    ForAllThreads(g_watchAddr, true);
    // the current thread too (SetDr suspends the target, so do ours through a helper thread as well)
    g_watchThread = GetCurrentThreadId();
    SetDr(g_watchThread, g_watchAddr, true, true);
    LOGI("watch: write breakpoint on %p (%s), %zu threads", (void*)g_watchAddr, what, g_allThreads.size() + 1);
}

// Read + write watch (4 bytes) on every thread: who reads a value (e.g. the camera FOV), how often.
void Watch_StartAccess(void* addr, const char* what) {
    if (g_veh || !addr) return;
    g_rw = true; g_len = 4;
    g_watchAddr = (uintptr_t)addr & ~3ull;
    g_veh = AddVectoredExceptionHandler(1, WatchVeh);
    ForAllThreads(g_watchAddr, true);
    g_watchThread = GetCurrentThreadId();
    SetDr(g_watchThread, g_watchAddr, true, false);
    LOGI("watch: read/write breakpoint on %p (%s), %zu threads", (void*)g_watchAddr, what, g_allThreads.size() + 1);
}
bool Watch_Active() { return g_veh != nullptr; }
// Log and reset the hit count of each location seen so far.
void Watch_ReportCounts(const char* label) {
    char line[2048]; int k = snprintf(line, sizeof(line), "watch: counts [%s]:", label);
    for (int i = 0; i < g_nSeen && k < (int)sizeof(line) - 128; i++) {
        LONG c = InterlockedExchange(&g_cnt[i], 0);
        k += snprintf(line + k, sizeof(line) - k, " %s(t%lu)=%ld", g_seen[i], g_tid[i], c);
    }
    LOGI("%s", line);
}

void Watch_Stop() {
    if (!g_veh) return;
    SetDr(g_watchThread, 0, false, !g_rw);
    if (!g_allThreads.empty()) { ForAllThreads(0, false); g_allThreads.clear(); }
    RemoveVectoredExceptionHandler(g_veh);
    g_veh = nullptr;
    LOGI("watch: stopped after %d hits, %d distinct locations", g_hits.load(), g_nSeen);
}

} // namespace svr
