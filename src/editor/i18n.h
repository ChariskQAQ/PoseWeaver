#pragma once
// UI language. Labels are written in Chinese in the source and wrapped in tr(); in English mode tr()
// looks the text up in i18n_en.inc (text after "##" is an ImGui ID and is kept as is).
// scripts/check_i18n.py lists tr() texts that have no English entry.
#include <string>

namespace i18n {

enum class Lang { Zh, En };

void setLanguage(Lang lang);
Lang language();
Lang systemLanguage();  // Chinese when the OS UI language is Chinese, otherwise English
const char* code(Lang lang);  // "zh" / "en"
Lang fromCode(const std::string& code);

const char* translate(const char* zh);

} // namespace i18n

inline const char* tr(const char* zh) { return i18n::translate(zh); }
// The result may point into `zh` (Chinese mode, or no entry): use it before `zh` goes away.
inline const char* tr(const std::string& zh) { return i18n::translate(zh.c_str()); }
