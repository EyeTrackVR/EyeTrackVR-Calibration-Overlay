#pragma once
#include <string>
#include <vector>

// Small OS abstraction: paths, quit signals and the parent-process watchdog.
namespace platform {

// Directory containing the running executable (not argv[0]-relative, so it is
// right however the overlay was launched). Falls back to argv0's directory.
std::string exe_dir(const char* argv0);

// Writable directory for the log when the exe directory is read-only.
std::string temp_dir();

// Install Ctrl+C / SIGTERM / console-close handlers. The first request sets
// quit_requested() so the app can fade out and tear down cleanly (overlays
// destroyed, OpenXR session ended); a second one exits immediately.
void install_quit_handlers();
bool quit_requested();
void request_quit();

// Detects the launching process going away (e.g. the EyeTrackVR app crashed or
// was killed), so an interactive overlay never strands the user in a grey void.
class ParentWatch {
public:
    ParentWatch();
    ~ParentWatch();
    bool active() const { return active_; }
    // Cheap; call every frame (internally rate-limited).
    bool parent_alive();

private:
    bool active_ = false;
#ifdef _WIN32
    void* handle_ = nullptr;  // process handle
#else
    long ppid_ = 0;  // parent pid
#endif
    double next_check_ = 0.0;
    bool alive_ = true;
};

// Current process id (used to make OpenVR overlay keys unique per instance).
long process_id();

// System fonts tried for glyphs the bundled font lacks (CJK, etc.). Han
// characters are shared between Chinese and Japanese but drawn differently, so
// for lang "ja" the Japanese fonts come before the Chinese ones.
std::vector<std::string> fallback_fonts(const std::string& lang = "");

}  // namespace platform
