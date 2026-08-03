#pragma once

#include <string>

namespace xpbd::app {


enum class Lang {
    En,
    ZhCn,
};


inline constexpr Lang kDefaultLang = Lang::ZhCn;


inline constexpr const char* kAppVersion = "1.0.0-cpp";


inline constexpr const char* kAuthorOriginal = "ranjitmutum";


inline constexpr const char* kAuthorCpp = "卡门线";

inline constexpr const char* kGithubUrl = "https://github.com/ranjitmutum/xpbd_baker";

void initI18n();
void setLang(Lang lang);
[[nodiscard]] Lang currentLang();
[[nodiscard]] const char* langName(Lang lang);





[[nodiscard]] const char* tr(const char* key);


[[nodiscard]] const char* trf(const char* key, ...);

}
