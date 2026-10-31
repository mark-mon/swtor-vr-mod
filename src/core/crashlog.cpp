// Crash report: a vectored handler that logs access violations and other fatal exceptions with the faulting module
// (full path), registers, and the chain of return addresses on the stack that point into our core, the game,
// RemoteRenderer or d3d9. First-chance only (the game may handle some itself), so at most a few per core load.
#include <windows.h>
#include <cstdio>
#include "core.h"
#include "common/log.h"

namespace svr {

static PVOID g_veh = nullptr;
static volatile LONG g_reports = 0;

static void Where(uintptr_t a, char* out, size_t n, bool full) {
    HMODULE m = nullptr;
    wchar_t name[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m))
        GetModuleFileNameW(m, name, MAX_PATH);
    const wchar_t* b = full ? name : (wcsrchr(name, L'\\') ? wcsrchr(name, L'\\') + 1 : name);
    snprintf(out, n, "%ls+0x%llx", b, (unsigned long long)(a - (uintptr_t)m));
}

static LONG CALLBACK CrashVeh(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != 0xC0000409 /*stack buffer overrun*/)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_reports) > 3) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    char where[400];
    Where((uintptr_t)ep->ExceptionRecord->ExceptionAddress, where, sizeof(where), true);
    ULONG_PTR rw = ep->ExceptionRecord->NumberParameters > 1 ? ep->ExceptionRecord->ExceptionInformation[0] : 0;
    ULONG_PTR addr = ep->ExceptionRecord->NumberParameters > 1 ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
    LOGE("crash: exception 0x%08lx at %s (%s address 0x%llx), thread %lu", code, where, rw == 1 ? "write" : rw == 8 ? "execute" : "read",
         (unsigned long long)addr, GetCurrentThreadId());
    LOGE("crash: rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx r8 %llx r9 %llx rsp %llx", c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi,
         c->Rdi, c->R8, c->R9, c->Rsp);
    char chain[1500] = "";
    size_t k = 0;
    uintptr_t* sp = (uintptr_t*)c->Rsp;
    int found = 0;
    HMODULE mods[4] = { GetModuleHandleW(nullptr), GetModuleHandleW(L"RemoteRenderer.dll"), nullptr, nullptr };
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&CrashVeh, &mods[2]);
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)ep->ExceptionRecord->ExceptionAddress, &mods[3]);
    for (int i = 0; i < 512 && found < 16; i++) {
        uintptr_t v;
        if (!ReadProcessMemory(GetCurrentProcess(), sp + i, &v, sizeof(v), nullptr)) break;
        HMODULE m = nullptr;
        if (v < 0x10000 || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)v, &m)) continue;
        bool interesting = false;
        for (HMODULE x : mods) if (x && m == x) interesting = true;
        wchar_t nm[MAX_PATH];
        if (!interesting && GetModuleFileNameW(m, nm, MAX_PATH) && (wcsstr(nm, L"d3d9") || wcsstr(nm, L"swtorvr"))) interesting = true;
        if (!interesting) continue;
        char d[128];
        Where(v, d, sizeof(d), false);
        k += snprintf(chain + k, sizeof(chain) - k, " <- %s", d);
        found++;
        if (k > sizeof(chain) - 140) break;
    }
    LOGE("crash: stack%s", chain);
    xrb::LogFlush();
    return EXCEPTION_CONTINUE_SEARCH;
}

void CrashLog_Install() { if (!g_veh) g_veh = AddVectoredExceptionHandler(0, CrashVeh); }
void CrashLog_Remove() { if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = nullptr; } }

} // namespace svr
