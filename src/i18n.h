#pragma once
#include <string>
#include <vector>

// ── Instruction-text localisation ─────────────────────────────────────────────
//
// Built-in catalogs for every language the EyeTrackVR app ships (its
// EyeTrackApp/lang/*.toml files). The app passes its active UI language as
// --lang=<code> when it launches the overlay, so both always speak the same
// language. Unknown codes fall back to English; region suffixes are ignored
// ("zh-CN" / "zh_TW" -> "zh").
//
// Only text the overlay owns lives here. In interactive mode the app sends its
// own (already translated) instruction text with command 50.

namespace i18n {

enum class Str {
    FollowTitle,    // "Follow the dot"
    FollowSub,      // "Keep your head still"
    AlmostDone,     // "Almost done"
    KeepFollowing,  // "Keep following the dot"
    HoldTitle,      // "Hold your gaze"
    HoldSub,        // "Look at the center of the dot"
    CalibrationComplete,
    SquintTitle,  // "Squint"
    SquintSub,    // "Keep squinting and follow the dot"
    CollectionComplete,
    EachDotCenter,  // "Look at the center of each dot"
    FinetuneComplete,
    FollowLower,  // subtitle under a pass title: "Follow the dot"
    WidenTitle,
    BrowsUpFull,
    BrowsUpHalf,
    BrowsDownFull,
    BrowsDownHalf,
    EachDotAppears,  // "Look at each dot as it appears"
    CenterTitle,     // "Look at the center"
    ShiftHeadset,    // "Shift your headset around"
    Count
};

// Select the active language. Returns the code actually used ("en" when the
// requested one is unknown).
std::string set_language(const std::string& code);
std::string language();

// Supported language codes, English first.
std::vector<std::string> languages();

// Translated string for the active language (English if a slot is missing).
const char* tr(Str s);

// "title\nsubtitle" convenience for the instruction panel.
std::string tr2(Str title, Str sub);

}  // namespace i18n
