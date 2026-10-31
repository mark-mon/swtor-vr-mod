// "Save logs for sending" (menu): copies the current core + loader logs (which stay open while the game runs) to
// LogCopyDir (default swtorvr\logs\) as swtorvr_core_<date>_<time>.log / swtorvr_loader_<...>.log, plus the plain
// names swtorvr_core.log / swtorvr_loader.log (overwritten) so the newest copy is always at a fixed path.
#include <windows.h>
#include <string>
#include <vector>
#include "core.h"
#include "common/log.h"
#include "common/config.h"

namespace svr {

static std::wstring g_copyDir;

void LogSave_LoadSettings(const xrb::Config& c) {
    std::string d = c.GetStr("LogCopyDir", "");
    g_copyDir.assign(d.begin(), d.end());
}

static bool ReadShared(const std::wstring& path, std::vector<char>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    bool ok = out.empty() || (ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size());
    CloseHandle(h);
    return ok;
}
static bool WriteAllBytes(const std::wstring& path, const std::vector<char>& d) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = d.empty() || WriteFile(h, d.data(), (DWORD)d.size(), &w, nullptr);
    CloseHandle(h);
    return ok;
}

static std::wstring CopyDir() {
    std::wstring dir = g_copyDir.empty() ? std::wstring(g_api->modDir) + L"logs\\" : g_copyDir;
    if (dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
    return dir;
}

// Each "Save logs for sending" leaves dated copies; without this the folder grows forever. On a fresh game start
// (not a core reload) the dated copies from earlier runs are deleted. The plain-named copies (swtorvr_core.log, ...)
// stay, so the last saved set is still there to send. Only our own file names are touched.
void Log_CleanOldCopies() {
    std::wstring dir = CopyDir();
    int n = 0;
    for (const wchar_t* name : { L"swtorvr_core_", L"swtorvr_loader_", L"xrhost_" }) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + name + L"????????_??????.log").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && DeleteFileW((dir + fd.cFileName).c_str())) n++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (n) LOGI("logs: removed %d dated log copies from earlier runs in %ls", n, dir.c_str());
}

bool Log_SaveSnapshot() {
    LOGI("logs: saving a copy for sending");
    xrb::LogFlush();
    std::wstring mod = g_api->modDir;
    std::wstring dir = CopyDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t stamp[64];
    swprintf(stamp, 64, L"_%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    int n = 0;
    for (const wchar_t* name : { L"swtorvr_core", L"swtorvr_loader", L"xrhost" }) {
        std::vector<char> d;
        if (!ReadShared(mod + name + L".log", d)) continue;
        if (WriteAllBytes(dir + name + stamp + L".log", d)) n++;
        WriteAllBytes(dir + name + L".log", d);
    }
    LOGI("logs: %d log(s) copied to %ls", n, dir.c_str());
    return n > 0;
}

} // namespace svr
