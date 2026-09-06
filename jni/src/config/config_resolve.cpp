#include "config_resolve.hpp"
#include "util/updater_proto.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>

namespace Leticia {

namespace {

constexpr const char *kExtractionRoot = "/tmp/leticia";

const char *kNeedToExtract[] = {
    "fonts",
    "config",
    "svg"
};

bool file_has_content(const std::string &path) {
    struct stat st;
    return (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0);
}

template <typename F1>
bool resolve_tiers(const std::string &zip_path, const char *env_var, const char *zip_entry,
                  const char *log_label, F1 on_file) {
    (void)zip_path;
    const char *env_override = getenv(env_var);
    if (env_override != nullptr) {
        if (on_file(env_override)) {
            Leticia::ui_print("config: %s loaded from override %s", log_label, env_override);
            return true;
        }
        Leticia::ui_print("config: %s=%s set but unreadable", env_var, env_override);
    }

    std::string extracted_path = std::string(kExtractionRoot) + "/" + zip_entry;
    if (on_file(extracted_path)) {
        Leticia::ui_print("config: %s loaded from %s", log_label, extracted_path.c_str());
        return true;
    }

    return false;
}

} // namespace

bool init_resources(const std::string &zip_path) {
    Leticia::ui_print("config: nuking %s and re-extracting fresh resources", kExtractionRoot);

    /* system() is the most robust way to handle rm -rf and recursive unzip
     * in this restricted updater environment. */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", kExtractionRoot, kExtractionRoot);
    if (system(cmd) != 0) {
        Leticia::ui_print("config: failed to prepare %s", kExtractionRoot);
        return false;
    }

    if (zip_path.empty()) {
        Leticia::ui_print("config: no zip_path provided, starting with empty %s", kExtractionRoot);
        return true;
    }

    std::string patterns;
    for (const char *dir : kNeedToExtract) {
        patterns += "'";
        patterns += dir;
        patterns += "/*' ";
    }

    snprintf(cmd, sizeof(cmd), "unzip -o -q %s %s -d %s", zip_path.c_str(), patterns.c_str(), kExtractionRoot);
    int ret = system(cmd);
    if (ret != 0) {
        Leticia::ui_print("config: unzip failed (exit=%d) for %s", ret, zip_path.c_str());
        return false;
    }

    return true;
}

bool resolve_config_file_path(const std::string &zip_path, const char *env_var, const char *zip_entry,
                             const char *log_label, std::string &out_resolved_path) {
    return resolve_tiers(
            zip_path, env_var, zip_entry, log_label,
            [&out_resolved_path](const std::string &p) {
                if (file_has_content(p)) {
                    out_resolved_path = p;
                    return true;
                }
                return false;
            });
}

} // namespace Leticia
