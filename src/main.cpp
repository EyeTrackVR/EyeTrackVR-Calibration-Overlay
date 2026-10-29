#include "backend.h"
#include "config.h"
#include "fov.h"
#include "i18n.h"
#include "modes.h"
#include "net.h"
#include "ovr_app.h"
#include "platform.h"
#include "presenter.h"
#include "session.h"
#include "sim_app.h"
#include "text.h"
#include "xr_app.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <streambuf>
#include <string>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
// Read a string registry value; empty on failure.
std::string reg_read(HKEY root, const char* subkey, const char* value) {
    char buf[1024];
    DWORD sz = sizeof(buf);
    if (RegGetValueA(root, subkey, value, RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS)
        return std::string(buf);
    return {};
}

// Locate SteamVR's OpenXR runtime manifest across all Steam library folders.
std::string find_steamvr_runtime_json() {
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
// recoverable from <exe-dir>/eyetrackvr_overlay.log. File lines get an elapsed
// time prefix and are flushed per line, so the log survives a hard kill.
class TeeBuf : public std::streambuf {
public:
    TeeBuf(std::streambuf* console, std::ofstream* file) : con_(console), file_(file) {
        t0_ = std::chrono::steady_clock::now();
    }
protected:
    int overflow(int c) override {
        if (c == EOF) return 0;
        if (con_) con_->sputc((char)c);
        if (file_ && *file_) {
            if (bol_) {
                const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
                char ts[24];
                std::snprintf(ts, sizeof(ts), "[%8.3f] ", t);
                file_->write(ts, (std::streamsize)std::strlen(ts));
                bol_ = false;
            }
            file_->put((char)c);
            if (c == '\n') { bol_ = true; file_->flush(); }
        }
        return c;
    }
    int sync() override {
        if (con_) con_->pubsync();
        if (file_ && *file_) file_->flush();
        return 0;
    }
private:
    std::streambuf* con_;
    std::ofstream*  file_;
    bool            bol_ = true;
    std::chrono::steady_clock::time_point t0_;
};

std::string find_file(const std::string& exe_dir, const std::string& rel) {
    std::error_code ec;
    for (const fs::path& p : {fs::path(exe_dir) / rel, fs::path(rel)})
        if (fs::exists(p, ec)) return p.string();
    return (fs::path(exe_dir) / rel).string();
}

bool parse_float(const std::string& arg, size_t prefix, float lo, float hi, bool hi_inclusive, float& out) {
    char* end = nullptr;
    const float v = std::strtof(arg.c_str() + prefix, &end);
    const bool ok = end && *end == '\0' && end != arg.c_str() + prefix && v > lo &&
                    (hi_inclusive ? v <= hi : v < hi);
    if (ok) out = v;
    else std::cout << "[WARN] Ignoring " << arg << " (expected " << lo << " < v "
                   << (hi_inclusive ? "<= " : "< ") << hi << ")\n";
    return ok;
}

bool parse_int(const std::string& arg, size_t prefix, long lo, long hi, long& out) {
    char* end = nullptr;
    const long v = std::strtol(arg.c_str() + prefix, &end, 10);
    const bool ok = end && *end == '\0' && end != arg.c_str() + prefix && v >= lo && v <= hi;
    if (ok) out = v;
    else std::cout << "[WARN] Ignoring " << arg << " (expected " << lo << ".." << hi << ")\n";
    return ok;
}

void print_usage() {
    std::cout <<
        "EyeTrackVR-Overlay [9point|center|ellipse|datacollect|finetune|interactive]\n"
        "    [--coverage=0.70] [--max-deg=30] [--max-deg-up=15] [--max-deg-down=40]\n"
        "    [--center-deg=0] [--backend=auto|openvr|openxr|sim] [--runtime=keep|<path.json>]\n"
        "  simulator: [--sim-hmd=beyond2|index|quest3|pimax|vive|canted|narrow|generic]\n"
        "    [--sim-fast] [--sim-seconds=N] [--sim-dump=<dir>] [--sim-dump-every=N] [--sim-debug]\n"
        "  misc: [--lang=en|de|es|ja|zh] [--list-langs] [--signal-port=2112] [--cmd-port=2113]\n"
        "    [--no-parent-watch] [--version]\n";
}

} // namespace

int main(int argc, char** argv) {
    platform::install_quit_handlers();

    const std::string exe_dir = platform::exe_dir(argc > 0 ? argv[0] : nullptr);

    // Informational flags answer before the log is opened, so querying the
    // binary (the app's tests ask --list-langs) never truncates the last run's log.
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--help" || arg == "-h") { print_usage(); return 0; }
        if (arg == "--version")             { std::cout << cfg::VERSION << "\n"; return 0; }
        if (arg == "--list-langs") {
            for (const auto& l : i18n::languages()) std::cout << l << "\n";
            return 0;
        }
    }

    // Tee stdout to <exe-dir>/eyetrackvr_overlay.log (or the temp dir if the
    // install directory is read-only) for headless diagnosis.
    fs::path log_path = fs::path(exe_dir) / "eyetrackvr_overlay.log";
    std::ofstream log_file(log_path, std::ios::out | std::ios::trunc);
    if (!log_file) {
        log_path = fs::path(platform::temp_dir()) / "eyetrackvr_overlay.log";
        log_file.open(log_path, std::ios::out | std::ios::trunc);
    }
    std::streambuf* cout_orig = std::cout.rdbuf();
    TeeBuf tee(cout_orig, log_file ? &log_file : nullptr);
    std::cout.rdbuf(&tee);
    // Restore the original buffer before tee/log_file are destroyed at scope exit.
    struct CoutGuard {
        std::streambuf* orig;
        ~CoutGuard() { std::cout.flush(); std::cout.rdbuf(orig); }
    } cout_guard{cout_orig};

    std::string mode_name = "9point";
    std::string runtime_arg;        // "" = auto, "keep" = active runtime, else a json path
    std::string backend_arg;        // "" = auto, "openvr" / "openxr" / "sim" to force
    FovConfig   fov;
    SimOptions  sim;
    long        signal_port = cfg::PORT_SIGNAL, cmd_port = cfg::PORT_CMD;
    bool        parent_watch = true;
    std::string lang_arg = "en";    // the app passes its UI language

    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        long n = 0;
        if (is_mode_name(arg))                         mode_name = arg;
        else if (arg.rfind("--lang=", 0) == 0)         lang_arg = arg.substr(7);
        else if (arg.rfind("--runtime=", 0) == 0)      runtime_arg = arg.substr(10);
        else if (arg.rfind("--backend=", 0) == 0)      backend_arg = arg.substr(10);
        else if (arg.rfind("--coverage=", 0) == 0)     parse_float(arg, 11, 0.0f, 1.0f, true, fov.coverage);
        else if (arg.rfind("--max-deg=", 0) == 0)      parse_float(arg, 10, 0.0f, 90.0f, false, fov.max_deg);
        else if (arg.rfind("--max-deg-up=", 0) == 0)   parse_float(arg, 13, 0.0f, 90.0f, false, fov.max_deg_up);
        else if (arg.rfind("--max-deg-down=", 0) == 0) parse_float(arg, 15, 0.0f, 90.0f, false, fov.max_deg_down);
        else if (arg.rfind("--center-deg=", 0) == 0)   parse_float(arg, 13, -45.0f, 45.0f, false, fov.center_deg);
        else if (arg.rfind("--sim-hmd=", 0) == 0)      sim.hmd = arg.substr(10);
        else if (arg.rfind("--sim-dump=", 0) == 0)     sim.dump_dir = arg.substr(11);
        else if (arg.rfind("--sim-dump-every=", 0) == 0) { if (parse_int(arg, 17, 1, 100000, n)) sim.dump_every = (int)n; }
        else if (arg.rfind("--sim-seconds=", 0) == 0)  { float s = 0; if (parse_float(arg, 14, 0.0f, 86400.0f, true, s)) sim.max_seconds = s; }
        else if (arg == "--sim-fast")                  sim.fast = true;
        else if (arg == "--sim-debug")                 sim.debug = true;
        else if (arg.rfind("--sim-eye-px=", 0) == 0)   { if (parse_int(arg, 13, 64, 4096, n)) sim.eye_px = (int)n; }
        else if (arg.rfind("--signal-port=", 0) == 0)  parse_int(arg, 14, 1, 65535, signal_port);
        else if (arg.rfind("--cmd-port=", 0) == 0)     parse_int(arg, 11, 1, 65535, cmd_port);
        else if (arg == "--no-parent-watch")           parent_watch = false;
        else std::cout << "[WARN] Unknown argument: " << arg << "\n";
    }
    if (!sim.dump_dir.empty() && sim.dump_every == 0) sim.dump_every = 9;

    std::cout << "EyeTrackVR Calibration Overlay " << cfg::VERSION << " — " << mode_name
              << " (udp://127.0.0.1:" << signal_port << ")\n";
    std::cout << "[INFO] Log: " << log_path.string() << "\n";
    {
        const std::string lang = i18n::set_language(lang_arg);
        std::cout << "[INFO] Language: " << lang;
        if (lang != lang_arg) std::cout << " (requested " << lang_arg << ")";
        std::cout << "\n";
    }

    if (!backend_arg.empty() && backend_arg != "auto" && backend_arg != "openvr" &&
        backend_arg != "openxr" && backend_arg != "sim") {
        std::cout << "[WARN] Unknown --backend=" << backend_arg << " (using auto)\n";
        backend_arg.clear();
    }

    UdpNet net;
    if (!net.init_send((uint16_t)signal_port)) return 1;

    std::unique_ptr<IMode> mode = make_mode(mode_name);
    if (mode->needs_recv() && !net.init_recv((uint16_t)cmd_port)) return 1;

    // Instruction text: bundled Open Sans, with system fonts for anything it lacks.
    TextRenderer text;
    const std::string font = find_file(exe_dir, "assets/OpenSans-Regular.ttf");
    if (!text.load(font, platform::fallback_fonts(i18n::language())))
        std::cout << "[WARN] No usable font; instructions will not be shown\n";

    // ── Backend selection ──────────────────────────────────────────────────────
    // Prefer the OpenVR overlay backend whenever SteamVR is already running: it
    // composites over the running game instead of replacing it, so calibrating
    // never closes VRChat (or whatever is running) and exiting returns straight
    // to it. Fall back to a focused OpenXR session otherwise (e.g. a native
    // Virtual Desktop / standalone OpenXR runtime, where nothing is evicted).
    //   --backend=openvr   force the OpenVR overlay backend (errors if no SteamVR)
    //   --backend=openxr   force the OpenXR backend (note: evicts the running game,
    //                      unless the runtime supports overlay sessions)
    //   --backend=sim      headless simulator (tests, layout previews)
    // On Linux, OpenVR is often xrizer/OpenComposite with incomplete overlay
    // support, so an OpenXR runtime with overlay sessions is tried first there.
    std::unique_ptr<IOverlayBackend> app;

    if (backend_arg == "sim") {
        auto s = std::make_unique<SimApp>(sim);
        if (!s->init(fov)) return 1;
        app = std::move(s);
    }

#ifndef _WIN32
    // Before the overlay check, so that it checks this runtime.
    if (!runtime_arg.empty() && runtime_arg != "keep") {
        setenv("XR_RUNTIME_JSON", runtime_arg.c_str(), 1);
        std::cout << "[INFO] Using OpenXR runtime: " << runtime_arg << "\n";
    }
    if (!app && (backend_arg.empty() || backend_arg == "auto") && XrApp::runtime_supports_overlay()) {
        std::cout << "[INFO] The OpenXR runtime supports overlay sessions; using the OpenXR backend\n";
        auto xr = std::make_unique<XrApp>();
        if (xr->init(fov)) {
            app = std::move(xr);
        } else {
            xr->shutdown();
        }
    }
#endif

    if (!app && backend_arg != "openxr") {
        auto ovr = std::make_unique<OvrApp>();
        if (ovr->init(fov)) {
            app = std::move(ovr);
        } else if (backend_arg == "openvr") {
            std::cout << "[ERROR] --backend=openvr requested but the OpenVR overlay is unavailable.\n";
            return 1;
        } else if (ovr->steamvr_running()) {
            // SteamVR is up but the overlays could not be set up. Falling back to
            // an OpenXR scene session here would close the running game.
            std::cout << "[ERROR] SteamVR is running but the calibration overlay could not be set up;\n"
                         "        not falling back to OpenXR (that would close the running game).\n"
                         "        Use --backend=openxr to force it.\n";
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
#endif
        auto xr = std::make_unique<XrApp>();
        if (!xr->init(fov)) {
            xr->shutdown();
            return 1;
        }
        app = std::move(xr);
    }

    // Interactive runs are driven by the EyeTrackVR app; if it disappears the
    // overlay must not keep the user stuck behind a grey backdrop.
    platform::ParentWatch watch;
    const bool watch_parent = parent_watch && mode->needs_recv() && watch.active();
    bool parent_gone = false;

    Presenter presenter(&text);
    Session session(*mode, net, presenter);
    bool backend_ended = false;

    while (true) {
        if (!app->poll_events()) { backend_ended = true; break; }
        if (platform::quit_requested()) session.request_quit();
        if (watch_parent && !parent_gone && !watch.parent_alive()) {
            parent_gone = true;
            std::cout << "[WARN] The launching app exited; closing the overlay\n";
            session.request_quit();
        }
        const bool cont = app->frame([&](double dt, const ViewGeometry& g) {
            return session.step(dt, g);
        });
        if (!cont) { backend_ended = true; break; }
        if (session.done()) break;
    }

    if (backend_ended && !session.done())
        std::cout << "[WARN] The VR runtime ended the session before calibration finished\n";
    app->shutdown();
    net.close();
    std::cout << "[INFO] Done\n";
    return (backend_ended && !session.done()) ? 2 : 0;
}
