#include "user_config.hpp"
#include "config_resolve.hpp"

namespace Leticia {

namespace {

constexpr const char *kUserConfigName = "config/user.conf";

void parse_ini(const std::string &text, user_config_t &out) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? text.size() : eol + 1;

        size_t start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos)
            continue;
        size_t end = line.find_last_not_of(" \t\r");
        line = line.substr(start, end - start + 1);

        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        size_t kstart = key.find_first_not_of(" \t");
        size_t kend = key.find_last_not_of(" \t");
        if (kstart == std::string::npos)
            continue;
        key = key.substr(kstart, kend - kstart + 1);

        size_t vstart = val.find_first_not_of(" \t");
        size_t vend = val.find_last_not_of(" \t");
        val = (vstart == std::string::npos) ? std::string() : val.substr(vstart, vend - vstart + 1);

        if (key == "timezone") {
            out.timezone = val;
            continue;
        }

        if (key == "music_scan_path") {
            out.music_scan_path = val;
            continue;
        }
    }
}

} // namespace

bool load_user_config(const std::string &zip_path, user_config_t &out) {
    std::string text;
    if (!resolve_config_text(zip_path, "LETICIA_USER_CONFIG", ".leticia.user.conf", kUserConfigName, "user config", text))
        return false;

    parse_ini(text, out);
    return true;
}

} // namespace Leticia
