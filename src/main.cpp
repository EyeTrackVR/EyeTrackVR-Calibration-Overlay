#include "config.h"
#include "fov.h"
#include "modes.h"
#include "net.h"
#include "backend.h"
#include "ovr_app.h"
#include "xr_app.h"

#include <cstdlib>
#include <memory>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <streambuf>
#include <string>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

namespace fs = std::filesystem;

#ifdef _WIN32
// Read a string registry value; empty on failure.
static std::string reg_read(HKEY root, const char* subkey, const char* value) {
    char buf[1024];
    DWORD sz = sizeof(buf);
    if (RegGetValueA(root, subkey, value, RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS)
        return std::string(buf);
    return {};
}

// Locate SteamVR's OpenXR runtime manifest across all Steam library folders.
static std::string find_steamvr_runtime_json() {
    std::vector<fs::path> steam_roots;
    for (auto v : {reg_read(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath"),
                   reg_read(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath"),
                   std::string("C:/Program Files (x86)/Steam")})
        if (!v.empty()) steam_roots.push_back(fs::path(v));

    std::vector<fs::path> libs = steam_roots;
    // libraryfolders.vdf lists additional (other-drive) library paths.
    for (const auto& root : steam_roots) {
        std::ifstream f(root / "steamapps" / "libraryfolders.vdf");
        std::string line;
        while (std::getline(f, line)) {
            size_t p = line.find("\"path\"");
            if (p == std::string::npos) continue;
            size_t a = line.find('"', p + 6);
            size_t b = (a == std::string::npos) ? a : line.find('"', a + 1);
            if (b == std::string::npos) continue;
            std::string path = line.substr(a + 1, b - a - 1);
            // VDF escapes backslashes as \\ — collapse them.
            std::string clean;
            for (size_t i = 0; i < path.size(); ++i) {
                if (path[i] == '\\' && i + 1 < path.size() && path[i + 1] == '\\') ++i;
                clean += path[i];
            }
            libs.push_back(fs::path(clean));
        }
    }
    std::error_code ec;
    for (const auto& lib : libs) {
        fs::path j = lib / "steamapps" / "common" / "SteamVR" / "steamxr_win64.json";
        if (fs::exists(j, ec)) return j.string();
    }
    return {};
}
#endif

// Mirrors everything written to std::cout into a log file as well, so that when
// the EyeTrackVR app launches this exe without a console, failures are still
// recoverable from <exe-dir>/eyetrackvr_overlay.log.
class TeeBuf : public std::streambuf {
public:
    TeeBuf(std::streambuf* a, std::streambuf* b) : a_(a), b_(b) {}
protected:
    int overflow(int c) override {
        if (c == EOF) return 0;
        const int r1 = a_ ? a_->sputc((char)c) : c;
        const int r2 = b_ ? b_->sputc((char)c) : c;
        return (r1 == EOF || r2 == EOF) ? EOF : c;
    }
    int sync() override {
        const int r1 = a_ ? a_->pubsync() : 0;
        const int r2 = b_ ? b_->pubsync() : 0;
        return (r1 || r2) ? -1 : 0;
    }
private:
    std::streambuf* a_;
    std::streambuf* b_;
};

// Resolve an asset path: try CWD-relative first, then relative to the executable
// directory (so the tool works regardless of where it is launched from).
static std::string find_asset(const char* argv0, const std::string& rel) {
    if (fs::exists(rel)) return rel;
    std::error_code ec;
    fs::path exe(argv0);
    fs::path p = exe.parent_path() / rel;
    if (fs::exists(p, ec)) return p.string();
    return rel;
}

int main(int argc, char** argv) {
    // Tee stdout to <exe-dir>/eyetrackvr_overlay.log for headless diagnosis.
    fs::path log_path = fs::path(argv[0]).parent_path() / "eyetrackvr_overlay.log";
    std::ofstream log_file(log_path);
    std::streambuf* cout_orig = std::cout.rdbuf();
    TeeBuf tee(cout_orig, log_file.rdbuf());
    if (log_file) std::cout.rdbuf(&tee);
    // Restore the original buffer before tee/log_file are destroyed at scope exit.
    struct CoutGuard {
        std::streambuf* orig;
        ~CoutGuard() { std::cout.rdbuf(orig); }
    } cout_guard{cout_orig};

    std::string mode_name = "9point";
    std::string runtime_arg;        // "" = auto, "keep" = active runtime, else a json path
    std::string backend_arg;        // "" = auto, "openvr" or "openxr" to force
    FovConfig fov;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "center")            mode_name = "center";
        else if (arg == "9point")       mode_name = "9point";
        else if (arg == "ellipse")      mode_name = "ellipse";
        else if (arg == "datacollect")  mode_name = "datacollect";
        else if (arg == "finetune")     mode_name = "finetune";
        else if (arg == "interactive")  mode_name = "interactive";
        else if (arg.rfind("--runtime=", 0) == 0) runtime_arg = arg.substr(10);
        else if (arg.rfind("--backend=", 0) == 0) backend_arg = arg.substr(10);
        else if (arg.rfind("--coverage=", 0) == 0) {
            float v = std::strtof(arg.c_str() + 11, nullptr);
            if (v > 0.0f && v <= 1.0f) fov.coverage = v;
            else std::cout << "[WARN] Ignoring " << arg << " (expected 0 < v <= 1)\n";
        } else if (arg.rfind("--max-deg=", 0) == 0) {
            float v = std::strtof(arg.c_str() + 10, nullptr);
            if (v > 0.0f && v < 90.0f) fov.max_deg = v;
            else std::cout << "[WARN] Ignoring " << arg << " (expected 0 < v < 90)\n";
        } else {
            std::cout << "[WARN] Unknown argument: " << arg << "\n";
        }
    }

    std::cout << "EyeTrackVR OpenXR Calibration Overlay — " << mode_name
              << " (udp://127.0.0.1:" << cfg::PORT_SIGNAL << ")\n";

    if (!backend_arg.empty() && backend_arg != "auto" &&
        backend_arg != "openvr" && backend_arg != "openxr") {
        std::cout << "[WARN] Unknown --backend=" << backend_arg << " (using auto)\n";
        backend_arg.clear();
    }

    UdpNet net;
    if (!net.init_send(cfg::PORT_SIGNAL)) return 1;

    std::unique_ptr<IMode> mode = make_mode(mode_name);
    if (mode->needs_recv() && !net.init_recv(cfg::PORT_CMD)) return 1;

    const std::string dot  = find_asset(argv[0], "assets/Purple_Dot.png");
    const std::string font = find_asset(argv[0], "assets/OpenSans-Regular.ttf");

    // ── Backend selection ──────────────────────────────────────────────────────
    // Prefer the OpenVR overlay backend whenever SteamVR is already running: it
    // composites over the running game instead of replacing it, so calibrating
    // never closes VRChat (or whatever is running) and exiting returns straight
    // to it. Fall back to a focused OpenXR session otherwise (e.g. a native
    // Virtual Desktop / standalone OpenXR runtime, where nothing is evicted).
    //   --backend=openvr   force the OpenVR overlay backend (errors if no SteamVR)
    //   --backend=openxr   force the OpenXR backend (note: evicts the running game)
    std::unique_ptr<IOverlayBackend> app;

    if (backend_arg != "openxr") {
        auto ovr = std::make_unique<OvrApp>();
        if (ovr->init(fov, dot.c_str(), font.c_str())) {
            app = std::move(ovr);
        } else if (backend_arg == "openvr") {
            std::cout << "[ERROR] --backend=openvr requested but SteamVR is not running.\n";
            return 1;
        } else {
            std::cout << "[INFO] Falling back to the OpenXR backend\n";
        }
    }

    if (!app) {
        // OpenXR scene session. Honour an explicit runtime override; otherwise
        // only target SteamVR when this backend was forced (--backend=openxr).
        // The auto path reaches here only when SteamVR is NOT running, so the
        // active runtime — typically the one already driving the headset (e.g.
        // Virtual Desktop) — is the right choice and we must not launch SteamVR.
        //   --runtime=keep        use the system's active runtime
        //   --runtime=<path.json> use a specific runtime manifest
        //   XR_RUNTIME_JSON set   respected as-is (no override)
#ifdef _WIN32
        const bool env_set = std::getenv("XR_RUNTIME_JSON") != nullptr;
        if (runtime_arg == "keep") {
            std::cout << "[INFO] Using the system's active OpenXR runtime (--runtime=keep)\n";
        } else if (!runtime_arg.empty()) {
            SetEnvironmentVariableA("XR_RUNTIME_JSON", runtime_arg.c_str());
            std::cout << "[INFO] Using OpenXR runtime: " << runtime_arg << "\n";
        } else if (env_set) {
            std::cout << "[INFO] Using OpenXR runtime from XR_RUNTIME_JSON\n";
        } else if (backend_arg == "openxr") {
            std::string sv = find_steamvr_runtime_json();
            if (!sv.empty()) {
                SetEnvironmentVariableA("XR_RUNTIME_JSON", sv.c_str());
                std::cout << "[INFO] Using SteamVR OpenXR runtime: " << sv << "\n";
            } else {
                std::cout << "[INFO] SteamVR OpenXR runtime not found; using active runtime\n";
            }
        } else {
            std::cout << "[INFO] Using the system's active OpenXR runtime\n";
        }
#else
        if (!runtime_arg.empty() && runtime_arg != "keep")
            setenv("XR_RUNTIME_JSON", runtime_arg.c_str(), 1);
#endif
        auto xr = std::make_unique<XrApp>();
        if (!xr->init(fov, dot.c_str(), font.c_str())) {
            xr->shutdown();
            return 1;
        }
        app = std::move(xr);
    }

    mode->on_start(net);

    while (true) {
        if (!app->poll_events()) break;
        bool cont = app->frame([&](double dt, const GazeRange& g) {
            return mode->update(dt, net, g);
        });
        if (!cont) break;
        if (mode->finished()) break;
    }

    app->shutdown();
    net.close();
    std::cout << "[INFO] Done\n";
    return 0;
}
