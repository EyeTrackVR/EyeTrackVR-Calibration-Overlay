#include "platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <unistd.h>
#include <climits>
#endif

namespace fs = std::filesystem;

namespace platform {
namespace {

std::atomic<int> g_quit{0};

double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void on_quit_signal() {
    if (g_quit.fetch_add(1) >= 1) std::_Exit(130);  // second request: leave now
}

#ifdef _WIN32
BOOL WINAPI console_handler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            on_quit_signal();
            return TRUE;
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            // Windows kills the process shortly after this returns; ask the main
            // loop to tear down and give it a moment to do so.
            on_quit_signal();
            Sleep(1500);
            return TRUE;
        default:
            return FALSE;
    }
}
#else
extern "C" void posix_handler(int) { on_quit_signal(); }
#endif

}  // namespace

std::string exe_dir(const char* argv0) {
    std::error_code ec;
#ifdef _WIN32
    std::wstring buf(1024, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0) break;
        if (n < buf.size()) {
            buf.resize(n);
            return fs::path(buf).parent_path().string();
        }
        buf.resize(buf.size() * 2);
        if (buf.size() > 32768) break;
    }
#else
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        return fs::path(buf).parent_path().string();
    }
#endif
    fs::path p = fs::absolute(fs::path(argv0 ? argv0 : ""), ec).parent_path();
    return p.string();
}

std::string temp_dir() {
    std::error_code ec;
    fs::path p = fs::temp_directory_path(ec);
    return ec ? std::string(".") : p.string();
}

void install_quit_handlers() {
#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    struct sigaction sa{};
    sa.sa_handler = posix_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
    // A closed stdout pipe must not kill us mid-teardown.
    signal(SIGPIPE, SIG_IGN);
#endif
}

bool quit_requested() { return g_quit.load() > 0; }
void request_quit() { g_quit.fetch_add(1); }

ParentWatch::ParentWatch() {
#ifdef _WIN32
    const DWORD self = GetCurrentProcessId();
    DWORD parent = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            if (pe.th32ProcessID == self) {
                parent = pe.th32ParentProcessID;
                break;
            }
        }
        CloseHandle(snap);
    }
    if (parent) {
        HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent);
        if (h) {
            // Guard against PID reuse: the parent must have started before us.
            FILETIME pc{}, pe2{}, pk{}, pu{}, sc{}, se{}, sk{}, su{};
            if (GetProcessTimes(h, &pc, &pe2, &pk, &pu) && GetProcessTimes(GetCurrentProcess(), &sc, &se, &sk, &su) &&
                CompareFileTime(&pc, &sc) <= 0 && WaitForSingleObject(h, 0) == WAIT_TIMEOUT) {
                handle_ = h;
                active_ = true;
            } else {
                CloseHandle(h);
            }
        }
    }
#else
    ppid_ = (long)getppid();
    active_ = ppid_ > 1;
#endif
}

ParentWatch::~ParentWatch() {
#ifdef _WIN32
    if (handle_) CloseHandle((HANDLE)handle_);
#endif
}

bool ParentWatch::parent_alive() {
    if (!active_ || !alive_) return alive_;
    const double t = now_s();
    if (t < next_check_) return true;
    next_check_ = t + 0.5;
#ifdef _WIN32
    alive_ = WaitForSingleObject((HANDLE)handle_, 0) == WAIT_TIMEOUT;
#else
    alive_ = (long)getppid() == ppid_;
#endif
    return alive_;
}

long process_id() {
#ifdef _WIN32
    return (long)GetCurrentProcessId();
#else
    return (long)getpid();
#endif
}

std::vector<std::string> fallback_fonts(const std::string& lang) {
    std::vector<std::string> out;
#ifdef _WIN32
    char windir[MAX_PATH] = {};
    const UINT n = GetWindowsDirectoryA(windir, MAX_PATH);
    const std::string fonts = (n > 0 ? std::string(windir) : std::string("C:\\Windows")) + "\\Fonts\\";
    for (const char* f : {"segoeui.ttf", "msyh.ttc", "YuGothM.ttc", "meiryo.ttc", "malgun.ttf", "simsun.ttc",
                          "seguisym.ttf", "arial.ttf"})
        out.push_back(fonts + f);
#else
    for (const char* f : {
             "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
             "/usr/share/fonts/TTF/DejaVuSans.ttf",
             "/usr/share/fonts/dejavu/DejaVuSans.ttf",
             "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
             "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
             "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
             "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
             "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
             "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
         })
        out.push_back(f);
#endif
    if (lang == "ja") {
        std::stable_partition(out.begin(), out.end(), [](const std::string& p) {
            return p.find("YuGoth") != std::string::npos || p.find("meiryo") != std::string::npos;
        });
    }
    std::vector<std::string> existing;
    std::error_code ec;
    for (auto& p : out)
        if (fs::exists(p, ec)) existing.push_back(p);
    return existing;
}

}  // namespace platform
