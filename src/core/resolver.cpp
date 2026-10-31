// Update-proof address resolution.
//
// Order of preference for every game address the mod needs:
//   1. Exported name (RemoteRenderer.dll exports its whole API)          -> Resolver_Export
//   2. Byte signature with wildcards, must match exactly once in .text   -> Resolver_Sig
//      optionally followed by a RIP-relative / call target resolve and an offset
// Results are cached in swtorvr\offsets.txt, one section per module build:
//
//   [swtor.exe 0x021d8000 0x6ac6a73e]
//   CameraController.Update = rva:0x01a2b3c0        # sig
//   [RemoteRenderer.dll 0x002a1000 0x6ac6a5a0]
//   ...
//
// The file is plain text and may be edited by hand: when a scan fails after a game patch, an entry can be filled in
// manually for that build and the mod uses it (still sanity-checked by the caller). A section whose size/timestamp
// does not match the running module is ignored, so stale entries can never be applied to a different build.
#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include "core.h"
#include "common/log.h"

namespace svr {

struct ModInfo { HMODULE mod = nullptr; uint8_t* base = nullptr; size_t size = 0; uint32_t ts = 0; uint8_t* text = nullptr; size_t textSize = 0; std::string key; };

static std::map<std::string, std::map<std::string, std::string>> g_cache;   // section key -> name -> value
static bool g_cacheLoaded = false, g_cacheDirty = false;

static std::wstring CachePath() { return std::wstring(g_api->modDir) + L"offsets.txt"; }

static bool GetMod(const char* name, ModInfo& mi) {
    HMODULE m = name ? GetModuleHandleA(name) : GetModuleHandleW(nullptr);
    if (!m) return false;
    MODULEINFO info = {};
    GetModuleInformation(GetCurrentProcess(), m, &info, sizeof(info));
    auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)m + ((IMAGE_DOS_HEADER*)m)->e_lfanew);
    mi.mod = m;
    mi.base = (uint8_t*)m;
    mi.size = info.SizeOfImage;
    mi.ts = nt->FileHeader.TimeDateStamp;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (memcmp(sec->Name, ".text", 5) == 0) { mi.text = mi.base + sec->VirtualAddress; mi.textSize = sec->Misc.VirtualSize; break; }
    }
    char k[160];
    snprintf(k, sizeof(k), "%s 0x%08zx 0x%08x", name ? name : "swtor.exe", mi.size, mi.ts);
    mi.key = k;
    return mi.text != nullptr;
}

static std::string Trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static void LoadCache() {
    if (g_cacheLoaded) return;
    g_cacheLoaded = true;
    FILE* f = _wfopen(CachePath().c_str(), L"r");
    if (!f) return;
    char line[512];
    std::string section;
    while (fgets(line, sizeof(line), f)) {
        std::string l = line;
        size_t c = l.find('#');
        if (c != std::string::npos) l = l.substr(0, c);
        l = Trim(l);
        if (l.empty()) continue;
        if (l[0] == '[') { section = Trim(l.substr(1, l.find(']') - 1)); continue; }
        size_t eq = l.find('=');
        if (eq == std::string::npos || section.empty()) continue;
        g_cache[section][Trim(l.substr(0, eq))] = Trim(l.substr(eq + 1));
    }
    fclose(f);
}

void Resolver_SaveCache() {
    if (!g_cacheDirty) return;
    FILE* f = _wfopen(CachePath().c_str(), L"w");
    if (!f) return;
    fputs("# SWTOR VR address cache. One [module size timestamp] section per game build.\n"
          "# Values: rva:0x... (module-relative). Written by the scanner; may be edited by hand for a new build.\n", f);
    for (auto& s : g_cache) {
        fprintf(f, "\n[%s]\n", s.first.c_str());
        for (auto& kv : s.second) fprintf(f, "%s = %s\n", kv.first.c_str(), kv.second.c_str());
    }
    fclose(f);
    g_cacheDirty = false;
}

// Pattern: "48 8B 05 ?? ?? ?? ?? 48 85 C0" ('?' or '??' = wildcard).
static bool ParsePattern(const char* p, std::vector<int>& out) {
    out.clear();
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '?') { out.push_back(-1); p++; if (*p == '?') p++; continue; }
        unsigned v;
        if (sscanf(p, "%2x", &v) != 1) return false;
        out.push_back((int)v);
        p += 2;
    }
    return !out.empty();
}

static std::vector<uint8_t*> Scan(uint8_t* start, size_t size, const std::vector<int>& pat, int maxHits) {
    std::vector<uint8_t*> hits;
    size_t n = pat.size();
    if (size < n) return hits;
    // first non-wildcard byte as anchor for speed
    size_t a = 0;
    while (a < n && pat[a] < 0) a++;
    if (a == n) return hits;
    uint8_t first = (uint8_t)pat[a];
    for (size_t i = 0; i + n <= size; i++) {
        uint8_t* p = (uint8_t*)memchr(start + i + a, first, size - n - i + 1);
        if (!p) break;
        i = (size_t)(p - start) - a;
        bool ok = true;
        for (size_t j = 0; j < n; j++)
            if (pat[j] >= 0 && start[i + j] != (uint8_t)pat[j]) { ok = false; break; }
        if (ok) { hits.push_back(start + i); if ((int)hits.size() >= maxHits) break; }
    }
    return hits;
}

// name: key in the cache. module: nullptr = swtor.exe. pattern: see ParsePattern.
// resolve: 0 = address of match + offset; 'r' = RIP-relative disp32 at match+offset (target = next insn + disp)
// for 'r', insnEnd is the offset of the end of the instruction relative to the disp32 field (usually 4).
void* Resolver_Sig(const char* name, const char* module, const char* pattern, int offset, char resolve, int insnEnd) {
    ModInfo mi;
    if (!GetMod(module, mi)) { LOGW("resolver: module %s not loaded", module ? module : "swtor.exe"); return nullptr; }
    LoadCache();
    auto sec = g_cache.find(mi.key);
    if (sec != g_cache.end()) {
        auto it = sec->second.find(name);
        if (it != sec->second.end() && it->second.rfind("rva:", 0) == 0) {
            uintptr_t rva = (uintptr_t)strtoull(it->second.c_str() + 4, nullptr, 0);
            if (rva && rva < mi.size) { LOGI("resolver: %-32s cached  %s+0x%llx", name, module ? module : "swtor.exe", (unsigned long long)rva); return mi.base + rva; }
        }
    }
    std::vector<int> pat;
    if (!ParsePattern(pattern, pat)) { LOGE("resolver: bad pattern for %s", name); return nullptr; }
    DWORD t0 = GetTickCount();
    auto hits = Scan(mi.text, mi.textSize, pat, 2);
    if (hits.size() != 1) {
        LOGW("resolver: %-32s signature matched %zu times (need exactly 1) - feature disabled; fill it in offsets.txt [%s]",
             name, hits.size(), mi.key.c_str());
        return nullptr;
    }
    uint8_t* p = hits[0] + offset;
    if (resolve == 'r') {
        int32_t disp;
        memcpy(&disp, p, 4);
        p = p + insnEnd + disp;
    }
    uintptr_t rva = (uintptr_t)(p - mi.base);
    char v[32];
    snprintf(v, sizeof(v), "rva:0x%08llx", (unsigned long long)rva);
    g_cache[mi.key][name] = v;
    g_cacheDirty = true;
    LOGI("resolver: %-32s scanned %s+0x%llx (%lu ms)", name, module ? module : "swtor.exe", (unsigned long long)rva, GetTickCount() - t0);
    return p;
}

void* Resolver_Export(const char* module, const char* exportName) {
    HMODULE m = GetModuleHandleA(module);
    return m ? (void*)GetProcAddress(m, exportName) : nullptr;
}

} // namespace svr
