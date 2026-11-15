// Culling boost per reader: the game reads its camera FOV at several places each frame (culling, LOD, the projection
// that mouse picking uses, ...). Widening the FOV in memory widens all of them, which breaks clicking on world objects.
// Here memory keeps the game's own FOV and only chosen reading instructions see the widened value: an execute
// breakpoint (debug registers, all threads) fires just before such an instruction, the handler writes the wide value
// and single-steps the instruction, the single-step trap writes the original back.
//
// Debug: DebugCullSiteScan = 1 finds the readers (a read watch on the FOV for a moment, then each hit is decoded
// backwards to the reading instruction) and measures draws per frame with the wide value given to one reader at a time.
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include "core.h"
#include "common/log.h"
#include "common/config.h"
extern "C" {
#include "hde/hde64.h"
}

namespace svr {

static int g_scan = 0;
static std::vector<uintptr_t> g_cfgSites;      // CullSites = RVA list (reading instructions in swtor.exe)
static int g_siteMode = 0;                     // 0 auto (signatures), 1 explicit list, 2 off (widen the FOV everywhere)
static float* volatile g_fov = nullptr;
static volatile float g_orig = 0, g_wide = 0;
static PVOID g_veh = nullptr;
static uintptr_t g_bp[4];
static int g_nbp = 0;
static __declspec(thread) bool t_armed = false;
static volatile LONG g_bpHits = 0;
// Split mode (CullSplit = 1): instead of a wider FOV, the culling view gets the game's own vertical FOV (x
// CullVertBoost in tangent) and a wider ASPECT, by editing the arguments of the call that builds it
// (swtor.exe+0x77d4c0: [rsp+20] = vertical FOV, [rsp+28] = aspect) at the call instruction. Objects keep their
// on-screen height, so the game's "too small to draw" test is not fooled (no pop-in straight ahead).
static bool g_split = true;
static float g_vertBoost = 1.5f;
static volatile float g_widenTan = 1.0f;   // wanted horizontal widening (tangent ratio)
static int g_logDraws = 0;
// Head-aimed culling (CullHeadAim = 1): the culling view also turns with the head (the game camera, the character and
// the image stay as they are) and covers the headset's own field of view x CullHeadMargin. [rdx] at the call = the
// culling view matrix (world -> camera, row-vector; same camera axes as the eye cameras): M' = M * inverse(head).
static bool g_headAim = false;
static float g_headMargin = 1.35f;
static float g_headR[3][3];
static float g_headTan[4];
static volatile bool g_headOk = false;
static bool g_headFlip = false;
static int g_headMirror = 3;   // DebugCullHeadFlip: which way the culling view turns (test aid)

// learn phase (data watch on the FOV)
static bool g_learning = false;
static uintptr_t g_next[32], g_start[32];
static volatile LONG g_nNext = 0;

static float g_lastM[16], g_lastM2[16];
static bool g_rowMix = true;
// Candidate axis conventions for head-aimed culling (mirror axis, flip) - all four pass the simulated poses.
// {mirror axis, flip, rowmix}. v0 = measured best 2026-11-14 (culling space = camera space with z mirrored).
static const int kVariant[4][3] = { { 3, 0, 1 }, { 0, 1, 1 }, { 1, 0, 1 }, { 2, 1, 1 } };
void CullSites_SetVariant(int v) {
    if (v < 0 || v > 3) v = 0;
    g_headMirror = kVariant[v][0]; g_headFlip = kVariant[v][1] != 0; g_rowMix = kVariant[v][2] != 0;
    LOGI("cullsite: head culling variant %d (mirror %d, flip %d)", v, g_headMirror, (int)g_headFlip);
}
void CullSites_SetHead(bool aim, float margin, float vert) {
    g_headAim = aim;
    g_headMargin = margin < 1.f ? 1.f : margin;
    g_vertBoost = vert < 1.f ? 1.f : vert;
}
void CullSites_LoadSettings(const xrb::Config& c) {
    g_scan = c.GetInt("DebugCullSiteScan", 0);
    g_split = c.GetBool("CullSplit", true);
    g_vertBoost = c.GetFloat("DebugCullVertBoost", 1.0f);   // only without head aim
    g_logDraws = c.GetInt("DebugLogDraws", 0);
    g_headAim = c.GetBool("DebugCullHeadAim", true);
    g_headMargin = c.GetFloat("CullHeadMarginV2", 1.0f);
    g_headFlip = c.GetInt("DebugCullHeadFlip", 0) != 0;
    g_headMirror = c.GetInt("DebugCullHeadMirror", 3);
    g_rowMix = c.GetBool("DebugCullHeadRowMix", true);
    { int v = c.GetInt("DebugCullHeadVariant", 0); if (v >= 0) CullSites_SetVariant(v); }
    if (g_headMargin < 1.f) g_headMargin = 1.f;
    g_cfgSites.clear();
    std::string s = c.GetStr("CullSites", "auto");
    g_siteMode = (s.empty() || s == "auto") ? 0 : (s == "off" || s == "0") ? 2 : 1;
    const char* p = g_siteMode == 1 ? s.c_str() : "";
    while (*p) {
        char* e;
        unsigned long long v = strtoull(p, &e, 16);
        if (e == p) { p++; continue; }
        if (v) g_cfgSites.push_back((uintptr_t)v);
        p = e;
    }
}

// The reading instruction ends at `next`: among the instructions that could end there, the one whose memory operand
// (evaluated with the registers at the trap) points at the FOV. A data breakpoint traps right after the access, so the
// registers still hold the address (unless the load overwrote its own base register).
static uintptr_t DecodeReader(uintptr_t next, const CONTEXT* c, uintptr_t target) {
    const uint8_t* code = (const uint8_t*)next;
    const DWORD64* R = &c->Rax;   // Rax Rcx Rdx Rbx Rsp Rbp Rsi Rdi R8..R15
    for (int k = 15; k >= 2; k--) {
        hde64s hs;
        unsigned len = hde64_disasm(code - k, &hs);
        if (len != (unsigned)k || (hs.flags & F_ERROR) || !(hs.flags & F_MODRM) || hs.modrm_mod == 3) continue;
        int64_t disp = (hs.flags & F_DISP8) ? (int64_t)(int8_t)hs.disp.disp8 : (hs.flags & F_DISP32) ? (int64_t)(int32_t)hs.disp.disp32 : 0;
        uint64_t ea;
        if (hs.modrm_mod == 0 && hs.modrm_rm == 5) ea = next + disp;   // RIP-relative
        else if (hs.flags & F_SIB) {
            int base = hs.sib_base | (hs.rex_b << 3), index = hs.sib_index | (hs.rex_x << 3);
            ea = (hs.modrm_mod == 0 && hs.sib_base == 5) ? 0 : R[base];
            if (index != 4) ea += R[index] << hs.sib_scale;
            ea += disp;
        } else ea = R[hs.modrm_rm | (hs.rex_b << 3)] + disp;
        if (ea <= target && target < ea + 16) return next - k;
    }
    return 0;
}

static LONG CALLBACK Veh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    uintptr_t ip = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    if (g_learning && (c->Dr6 & 1)) {
        c->Dr6 = 0;
        LONG n = g_nNext;
        bool known = false;
        for (LONG i = 0; i < n; i++) if (g_next[i] == ip) { known = true; break; }
        if (!known && n < 32) { g_next[n] = ip; g_start[n] = DecodeReader(ip, c, (uintptr_t)g_fov); g_nNext = n + 1; }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (t_armed && (c->Dr6 & 0x4000)) {   // single step after the reading instruction
        t_armed = false;
        c->Dr6 = 0;
        c->EFlags &= ~0x100u;
        if (g_fov) *g_fov = g_orig;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    for (int i = 0; i < g_nbp; i++) {
        if (ip == g_bp[i] && (c->Dr6 & (1ull << i))) {
            c->Dr6 = 0;
            if (g_split && g_headAim && g_headOk && g_wide > 0) {
                float* fv = (float*)(c->Rsp + 0x20);
                float* asp = (float*)(c->Rsp + 0x28);
                float* M = (float*)c->Rdx;
                if (*fv > 0.05f && *fv < 3.0f && *asp > 0.2f && *asp < 20.f && M) {
                    // rotate: rows of M times R^T (R = head rotation in camera space, orthonormal)
                    memcpy(g_lastM, M, sizeof(g_lastM));
                    if (g_rowMix) {                  // M = camera -> world: rotate the camera's axis rows, keep its position
                        float o[3][4];
                        for (int i = 0; i < 3; i++) for (int k = 0; k < 4; k++) {
                            float a = 0.f;
                            for (int j = 0; j < 3; j++) a += (g_headFlip ? g_headR[j][i] : g_headR[i][j]) * M[j * 4 + k];
                            o[i][k] = a;
                        }
                        for (int i = 0; i < 3; i++) for (int k = 0; k < 4; k++) M[i * 4 + k] = o[i][k];
                    } else
                    for (int r = 0; r < 4; r++) {
                        float x = M[r * 4 + 0], y = M[r * 4 + 1], z = M[r * 4 + 2];
                        for (int k = 0; k < 3; k++) M[r * 4 + k] = g_headFlip ? x * g_headR[0][k] + y * g_headR[1][k] + z * g_headR[2][k]
                                                                         : x * g_headR[k][0] + y * g_headR[k][1] + z * g_headR[k][2];
                    }
                    memcpy(g_lastM2, M, sizeof(g_lastM2));
                    float th = (g_headTan[0] > g_headTan[1] ? g_headTan[0] : g_headTan[1]) * g_headMargin;
                    float tv = (g_headTan[2] > g_headTan[3] ? g_headTan[2] : g_headTan[3]) * g_headMargin;
                    float tg = tanf(*fv * 0.5f);
                    if (tv < tg) tv = tg;                       // never narrower than the game's own view
                    if (th < tg * *asp) th = tg * *asp;
                    { float hw = tg * *asp * g_widenTan; if (th < hw) th = hw; }   // keep the normal horizontal widening
                    if (tv > 20.f) tv = 20.f;
                    *fv = 2.f * atanf(tv);
                    *asp = th / tv;
                }
                c->EFlags |= 0x10000u;
                InterlockedIncrement(&g_bpHits);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (g_split) {
                float* fv = (float*)(c->Rsp + 0x20);
                float* asp = (float*)(c->Rsp + 0x28);
                float w = g_widenTan;
                if (g_wide > 0 && w > 1.001f && *fv > 0.05f && *fv < 3.0f && *asp > 0.2f && *asp < 20.f) {
                    float vb = g_vertBoost < 1.f ? 1.f : g_vertBoost;
                    float t = tanf(*fv * 0.5f) * vb;
                    if (t > 20.f) t = 20.f;
                    *fv = 2.f * atanf(t);
                    *asp = *asp * w / vb;
                }
                c->EFlags |= 0x10000u;
                InterlockedIncrement(&g_bpHits);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (g_fov && g_wide > 0) { *g_fov = g_wide; t_armed = true; c->EFlags |= 0x100u; }
            c->EFlags |= 0x10000u;   // RF: run the instruction without hitting the breakpoint again
            InterlockedIncrement(&g_bpHits);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Debug registers on every thread of the process (set from a helper thread: a thread can't change its own).
struct DrSet { uintptr_t a[4]; int n; bool dataWatch; };
static DWORD WINAPI ApplyThread(void* p) {
    DrSet* s = (DrSet*)p;
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 1;
    THREADENTRY32 te = { sizeof(te) };
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
        if (!th) continue;
        if (SuspendThread(th) != (DWORD)-1) {
            CONTEXT c = {};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &c)) {
                c.Dr0 = c.Dr1 = c.Dr2 = c.Dr3 = 0;
                c.Dr7 &= ~0xFFFF00FFull;
                if (s->dataWatch && s->n > 0) {
                    c.Dr0 = s->a[0];
                    c.Dr7 |= 1ull | (3ull << 16) | (3ull << 18);   // L0, read/write, 4 bytes
                } else {
                    for (int i = 0; i < s->n; i++) {
                        (&c.Dr0)[i] = s->a[i];
                        c.Dr7 |= 1ull << (2 * i);                  // Li, execute (RW 00, LEN 00)
                    }
                }
                SetThreadContext(th, &c);
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
    return 0;
}
static void ApplyAll(const uintptr_t* a, int n, bool dataWatch) {
    DrSet s = {};
    for (int i = 0; i < n && i < 4; i++) s.a[i] = a[i];
    s.n = n > 4 ? 4 : n;
    s.dataWatch = dataWatch;
    HANDLE t = CreateThread(nullptr, 0, ApplyThread, &s, 0, nullptr);
    if (t) { WaitForSingleObject(t, 5000); CloseHandle(t); }
}

static void EnsureVeh() { if (!g_veh) g_veh = AddVectoredExceptionHandler(1, Veh); }

static void SetBreakpoints(const uintptr_t* a, int n) {
    EnsureVeh();
    g_nbp = 0;
    ApplyAll(nullptr, 0, false);
    for (int i = 0; i < n && i < 4; i++) g_bp[i] = a[i];
    g_nbp = n > 4 ? 4 : n;
    ApplyAll(g_bp, g_nbp, false);
}

static uintptr_t Base() { return (uintptr_t)GetModuleHandleW(nullptr); }

// Signature scan over swtor.exe's executable sections. Pattern: hex bytes, "??" = any. Returns every match.
static std::vector<uintptr_t> FindPattern(const char* pat) {
    std::vector<int> bytes;
    for (const char* p = pat; *p;) {
        if (*p == ' ') { p++; continue; }
        if (p[0] == '?') { bytes.push_back(-1); p += (p[1] == '?') ? 2 : 1; continue; }
        bytes.push_back((int)strtoul(std::string(p, 2).c_str(), nullptr, 16));
        p += 2;
    }
    std::vector<uintptr_t> out;
    uint8_t* base = (uint8_t*)GetModuleHandleW(nullptr);
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    size_t n = bytes.size();
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* a = base + sec->VirtualAddress;
        size_t len = sec->Misc.VirtualSize;
        for (size_t o = 0; o + n <= len && out.size() < 8; o++) {
            size_t k = 0;
            while (k < n && (bytes[k] < 0 || a[o + k] == (uint8_t)bytes[k])) k++;
            if (k == n) out.push_back((uintptr_t)(a + o));
        }
    }
    return out;
}

// The readers that feed culling (found 2026-10-31 with DebugCullSiteScan on build 7.9.1c), as signatures + offset to
// the FOV load (movss xmm, [reg+1A0h]).
struct SiteSig { const char* name; const char* pat; int off; };
// Reader A (85 C0 75 ?? F3 0F 10 8E A0 01 00 00 F3 0F 59 0D ...) widens culling a little more but also feeds mouse
// picking (taxi hover: 4 of 33 points), so it is left on the game FOV. Reader B alone: 31 of 33, most of the culling.
static const SiteSig kSites[] = {
    { "culling B", "F3 0F 11 44 24 28 F3 0F 10 87 A0 01 00 00 F3 0F 11 44 24 20 E8 ?? ?? ?? ?? 48 8B 0E 0F 57 DB 49 8B D6", 6 },
};

// ---- per-frame driver (called from Cull_Frame with the FOV address and both values)
enum { S_IDLE, S_LEARN, S_PHASES, S_ACTIVE, S_DONE };
static int g_state = S_IDLE;
static DWORD g_t0 = 0;
static std::vector<uintptr_t> g_sites;   // absolute addresses of the reading instructions
static int g_phase = -1;
static uint64_t g_drawSum = 0, g_frames = 0;
static bool g_memWide = true;

bool CullSites_MemoryWide() { return g_memWide; }
// boost off / camera lost: the breakpoints stay but write nothing
void CullSites_Pause() { g_wide = 0; g_fov = nullptr; }

static void StartPhase(int p) {
    g_phase = p;
    g_drawSum = g_frames = 0;
    g_t0 = GetTickCount();
    // phase 0: game FOV everywhere; 1: wide everywhere (memory); 2..: wide for site p-2 only
    g_memWide = (p == 1);
    if (p >= 2) { uintptr_t a = g_sites[p - 2]; SetBreakpoints(&a, 1); }
    else SetBreakpoints(nullptr, 0);
}
static const char* PhaseName(int p, char* b, size_t n) {
    if (p == 0) snprintf(b, n, "game FOV everywhere");
    else if (p == 1) snprintf(b, n, "wide FOV everywhere");
    else snprintf(b, n, "wide only at swtor.exe+0x%llx", (unsigned long long)(g_sites[p - 2] - Base()));
    return b;
}

// Split mode reads the game's FOV from the culling call itself: it needs no camera address in memory.
bool CullSites_NeedsMemory() { return !g_split || g_scan > 0 || g_siteMode == 2 || g_state == S_DONE; }
void CullSites_Frame(float* fovAddr, float orig, float wide) {
    if (orig <= 0 || (!fovAddr && CullSites_NeedsMemory())) return;
    g_fov = fovAddr; g_orig = orig; g_wide = wide;
    g_widenTan = tanf(wide * 0.5f) / tanf(orig * 0.5f);
    if (g_headAim) {
        float R[3][3], t4[4];
        bool ok = Stereo_HeadRotation(R, t4);
        if (ok) {
            // the culling camera's x axis is mirrored relative to the eye cameras (measured: yaw turned the wrong way,
            // pitch the right way): conjugate with diag(-1,1,1)
            // DebugCullHeadMirror: 0 none, 1 x, 2 y, 3 z (conjugate with a mirror of that axis)
            if (g_headMirror >= 1 && g_headMirror <= 3) {
                int a = g_headMirror - 1;
                for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) if ((i == a) != (j == a)) R[i][j] = -R[i][j];
            }
            memcpy(g_headR, R, sizeof(R)); memcpy(g_headTan, t4, sizeof(t4));
        }
        g_headOk = ok;
        static DWORD lastLog = 0;
        DWORD now = GetTickCount();
        if (ok && now - lastLog > 2000) {
            lastLog = now;
            float th = (t4[0] > t4[1] ? t4[0] : t4[1]) * g_headMargin, tv = (t4[2] > t4[3] ? t4[2] : t4[3]) * g_headMargin;
            LOGI("cullhead: R [%.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f] tan L%.2f R%.2f U%.2f D%.2f -> view %.0f x %.0f deg, mirror %d flip %d",
                 R[0][0], R[0][1], R[0][2], R[1][0], R[1][1], R[1][2], R[2][0], R[2][1], R[2][2], t4[0], t4[1], t4[2], t4[3],
                 2.f * atanf(th) * 57.3f, 2.f * atanf(tv) * 57.3f, g_headMirror, (int)g_headFlip);
            const float* a = g_lastM; const float* b = g_lastM2;
            LOGI("cullhead: M in  [%.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.1f %.1f %.1f %.2f] rowmix %d",
                 a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9],a[10],a[11],a[12],a[13],a[14],a[15], (int)g_rowMix);
            LOGI("cullhead: M out [%.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.1f %.1f %.1f %.2f]",
                 b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
        }
    }
    if (g_logDraws) {   // test aid: average draws per frame every 4 s
        static uint64_t sum = 0, n = 0; static DWORD t0 = 0;
        DWORD now = GetTickCount();
        if (!t0) t0 = now;
        sum += Trace_LastFrameDraws(); n++;
        if (now - t0 >= 4000) { LOGI("cullsite: %.0f draws/frame (split %d, head aim %d/%d, vert %.2f, widen x%.1f, bp hits %ld)", n ? (double)sum / n : 0.0, g_split, g_headAim, (int)g_headOk, g_vertBoost, g_widenTan, InterlockedExchange(&g_bpHits, 0)); sum = n = 0; t0 = now; }
    }
    DWORD now = GetTickCount();
    switch (g_state) {
    case S_IDLE:
        if (g_scan > 0) {
            EnsureVeh();
            g_nNext = 0;
            g_learning = true;
            g_memWide = true;
            uintptr_t a = (uintptr_t)fovAddr;
            ApplyAll(&a, 1, true);
            g_t0 = now;
            g_state = S_LEARN;
            LOGI("cullsite: learning who reads the camera FOV at %p", fovAddr);
        } else if (g_siteMode != 2) {
            uintptr_t a[4]; int n = 0;
            if (g_siteMode == 1) { for (uintptr_t r : g_cfgSites) if (n < 4) a[n++] = Base() + r; }
            else {
                bool ok = true;
                for (const SiteSig& sg : kSites) {
                    std::vector<uintptr_t> m = FindPattern(sg.pat);
                    if (m.size() == 1) a[n++] = m[0] + sg.off + (g_split ? 14 : 0);   // split: the call after the load
                    else { ok = false; LOGW("cullsite: signature '%s' found %zu times (need exactly 1)", sg.name, m.size()); }
                }
                if (!ok || n == 0) {
                    LOGW("cullsite: game update? culling readers not found - widening the FOV everywhere instead (clicking world objects will be off with a high boost)");
                    g_state = S_DONE;
                    break;
                }
            }
            for (int i = 0; i < n; i++) {   // bytes around each reader (for building signatures)
                uint8_t b[96]; SIZE_T got = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), (void*)(a[i] - 48), b, sizeof(b), &got)) continue;
                char hex[400]; int k = 0;
                for (int j = 0; j < 96; j++) k += snprintf(hex + k, sizeof(hex) - k, j == 48 ? "| %02x " : "%02x ", b[j]);
                LOGI("cullsite: bytes around swtor.exe+0x%llx: %s", (unsigned long long)(a[i] - Base()), hex);
            }
            g_memWide = false;
            SetBreakpoints(a, n);
            g_state = S_ACTIVE;
            if (g_split && g_headAim) LOGI("cullsite: head-aimed culling - the culling view turns with the head and covers the headset FOV x %.2f", g_headMargin);
            else if (g_split) LOGI("cullsite: split mode - the culling view gets a wider aspect (x boost / %.2f) and the game's vertical FOV x %.2f; nothing else changes", g_vertBoost, g_vertBoost);
            else LOGI("cullsite: wide FOV only for %d culling reader(s); everything else (mouse picking, sky) sees the game FOV", n);
        } else g_state = S_DONE;
        break;
    case S_LEARN:
        if (now - g_t0 < 1000) break;
        ApplyAll(nullptr, 0, true);
        g_learning = false;
        g_sites.clear();
        for (LONG i = 0; i < g_nNext; i++) {
            uintptr_t s = g_start[i];
            uint8_t b[12] = {};
            ReadProcessMemory(GetCurrentProcess(), (void*)(s ? s : g_next[i]), b, sizeof(b), nullptr);
            LOGI("cullsite: reader %ld: hit swtor.exe+0x%llx -> instruction swtor.exe+0x%llx  bytes %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                 i, (unsigned long long)(g_next[i] - Base()), (unsigned long long)(s ? s - Base() : 0),
                 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9]);
            if (s) g_sites.push_back(s);
        }
        StartPhase(0);
        g_state = S_PHASES;
        break;
    case S_PHASES: {
        uint32_t d = Trace_LastFrameDraws();
        if (now - g_t0 > 1000) { g_drawSum += d; g_frames++; }   // skip the first second
        static int dumped = -1;
        if (g_scan >= 2 && now - g_t0 > 3000 && dumped != g_phase) { dumped = g_phase; Vr_RequestDump(900 + g_phase); }
        if (now - g_t0 > 4000) {
            char n[96];
            LOGI("cullsite: phase %d (%s): %.0f draws/frame, breakpoint hits %ld", g_phase, PhaseName(g_phase, n, sizeof(n)),
                 g_frames ? (double)g_drawSum / g_frames : 0.0, InterlockedExchange(&g_bpHits, 0));
            if (g_phase + 1 < 2 + (int)g_sites.size()) StartPhase(g_phase + 1);
            else { SetBreakpoints(nullptr, 0); g_memWide = true; g_state = S_DONE; g_scan = 0; LOGI("cullsite: scan done"); }
        }
        break;
    }
    default: break;
    }
}

void CullSites_Shutdown() {
    if (g_nbp || g_learning) ApplyAll(nullptr, 0, false);
    g_nbp = 0; g_learning = false;
    if (g_fov && g_orig > 0) *g_fov = g_orig;
    if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = nullptr; }
    g_state = S_IDLE;
}

} // namespace svr
