#include "editor/i18n.h"

#include <cstring>
#include <string_view>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#endif

namespace i18n {

namespace {
Lang g_lang = Lang::Zh;

struct Entry {
    const char* zh;
    const char* en;
};
const Entry kEnglish[] = {
#include "editor/i18n_en.inc"
};

const std::unordered_map<std::string_view, const char*>& englishTable() {
    static const std::unordered_map<std::string_view, const char*> table = [] {
        std::unordered_map<std::string_view, const char*> t;
        for (const Entry& e : kEnglish) t.emplace(e.zh, e.en);
        return t;
    }();
    return table;
}
} // namespace

void setLanguage(Lang lang) { g_lang = lang; }
Lang language() { return g_lang; }

Lang systemLanguage() {
#ifdef _WIN32
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE ? Lang::Zh : Lang::En;
#else
    const char* l = std::getenv("LANG");
    return l && std::strncmp(l, "zh", 2) == 0 ? Lang::Zh : Lang::En;
#endif
}

const char* code(Lang lang) { return lang == Lang::En ? "en" : "zh"; }
Lang fromCode(const std::string& c) { return c == "en" ? Lang::En : Lang::Zh; }

const char* translate(const char* zh) {
    if (g_lang == Lang::Zh || !zh) return zh;
    const char* id = std::strstr(zh, "##");
    std::string_view key = id ? std::string_view(zh, size_t(id - zh)) : std::string_view(zh);
    auto& table = englishTable();
    auto it = table.find(key);
    if (it == table.end()) return zh;
    if (!id) return it->second;
    // Label with an ImGui ID: English label + the original "##..." part (cached, the set is bounded).
    static std::unordered_map<std::string, std::string> withId;
    auto c = withId.find(zh);
    if (c == withId.end()) c = withId.emplace(zh, std::string(it->second) + id).first;
    return c->second.c_str();
}

} // namespace i18n
