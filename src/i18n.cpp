#include "i18n.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace i18n {
namespace {

constexpr size_t N = (size_t)Str::Count;

struct Catalog {
    const char* code;
    std::array<const char*, N> s;  // order matches enum Str
};

// Keep these in step with the app's lang/*.toml set (EyeTrackApp/lang). Tone
// follows the app's catalogs: informal "du" (de) and "tú" (es).
const Catalog kCatalogs[] = {
    {"en",
     {
         "Follow the dot",
         "Keep your head still",
         "Almost done",
         "Keep following the dot",
         "Hold your gaze",
         "Look at the center of the dot",
         "Calibration complete",
         "Squint",
         "Keep squinting and follow the dot",
         "Collection complete",
         "Look at the center of each dot",
         "Fine-tune complete",
         "Follow the dot",
         "Widen your eyes",
         "Raise eyebrows fully",
         "Raise eyebrows halfway",
         "Lower eyebrows fully",
         "Lower eyebrows halfway",
         "Look at each dot as it appears",
         "Look at the center",
         "Shift your headset around",
     }},
    {"de",
     {
         "Folge dem Punkt",
         "Halte den Kopf still",
         "Fast fertig",
         "Folge weiter dem Punkt",
         "Blick halten",
         "Schau auf die Mitte des Punkts",
         "Kalibrierung abgeschlossen",
         "Augen zusammenkneifen",
         "Kneif weiter und folge dem Punkt",
         "Erfassung abgeschlossen",
         "Schau auf die Mitte jedes Punkts",
         "Feinabstimmung abgeschlossen",
         "Folge dem Punkt",
         "Augen weit öffnen",
         "Augenbrauen ganz heben",
         "Augenbrauen halb heben",
         "Augenbrauen ganz senken",
         "Augenbrauen halb senken",
         "Schau auf jeden Punkt, sobald er erscheint",
         "Schau in die Mitte",
         "Verschiebe dein Headset in alle Richtungen",
     }},
    {"es",
     {
         "Sigue el punto",
         "Mantén la cabeza quieta",
         "Casi listo",
         "Sigue mirando el punto",
         "Mantén la mirada",
         "Mira el centro del punto",
         "Calibración completada",
         "Entrecierra los ojos",
         "Sigue entrecerrando los ojos y sigue el punto",
         "Recopilación completada",
         "Mira el centro de cada punto",
         "Ajuste fino completado",
         "Sigue el punto",
         "Abre bien los ojos",
         "Levanta las cejas del todo",
         "Levanta las cejas a medias",
         "Baja las cejas del todo",
         "Baja las cejas a medias",
         "Mira cada punto cuando aparezca",
         "Mira al centro",
         "Mueve el visor en todas direcciones",
     }},
    {"ja",
     {
         "点を目で追ってください",
         "頭を動かさないでください",
         "もう少しです",
         "そのまま点を追ってください",
         "視線を固定してください",
         "点の中心を見てください",
         "キャリブレーション完了",
         "目を細めてください",
         "目を細めたまま点を追ってください",
         "収集完了",
         "各点の中心を見てください",
         "微調整完了",
         "点を目で追ってください",
         "目を大きく開いてください",
         "眉を最大まで上げてください",
         "眉を半分まで上げてください",
         "眉を最大まで下げてください",
         "眉を半分まで下げてください",
         "表示された点を順に見てください",
         "中央を見てください",
         "ヘッドセットを色々な方向にずらしてください",
     }},
    {"zh",
     {
         "用眼睛跟随圆点",     "保持头部不动",   "即将完成",       "继续跟随圆点",       "保持注视",
         "注视圆点的中心",     "校准完成",       "眯起眼睛",       "保持眯眼并跟随圆点", "采集完成",
         "注视每个圆点的中心", "微调完成",       "跟随圆点",       "睁大眼睛",           "将眉毛完全抬起",
         "将眉毛抬起一半",     "将眉毛完全压低", "将眉毛压低一半", "注视每个出现的圆点", "注视中心",
         "向各个方向移动头显",
     }},
};

const Catalog* g_active = &kCatalogs[0];

}  // namespace

std::string set_language(const std::string& code) {
    // "zh-CN" / "pt_BR" / "DE" -> base language, lower case.
    std::string base;
    for (char c : code) {
        if (c == '-' || c == '_' || c == '.') break;
        base.push_back((char)std::tolower((unsigned char)c));
    }
    g_active = &kCatalogs[0];
    for (const Catalog& c : kCatalogs)
        if (base == c.code) {
            g_active = &c;
            break;
        }
    return g_active->code;
}

std::string language() { return g_active->code; }

std::vector<std::string> languages() {
    std::vector<std::string> out;
    for (const Catalog& c : kCatalogs) out.push_back(c.code);
    return out;
}

const char* tr(Str s) {
    const size_t i = std::min((size_t)s, N - 1);
    const char* v = g_active->s[i];
    return (v && *v) ? v : kCatalogs[0].s[i];
}

std::string tr2(Str title, Str sub) { return std::string(tr(title)) + "\n" + tr(sub); }

}  // namespace i18n
