#include "gui/icons.hpp"

#include <cstdio>
#include <map>
#include <sys/stat.h>

namespace Leticia::gui::icons {

namespace {

constexpr const char *kSvgDir = "/tmp/leticia/svg";
constexpr const char *kWhiteFill = "#FFFFFF";

std::map<std::string, std::string> g_cache;

/**
 * Reads a whole file.
 *
 * @param file_path File path.
 * @param out Receives the content.
 * @return true on success.
 */
bool read_all(const std::string &file_path, std::string &out) {
    FILE *f = fopen(file_path.c_str(), "rb");
    if (f == nullptr)
        return false;

    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    fclose(f);
    return !out.empty();
}

/**
 * Writes the tinted copy of an icon.
 *
 * @param source_path Path of the white icon.
 * @param target_path Path of the copy.
 * @param fill Fill color as text, for example "#DD669C".
 * @return true on success.
 */
bool write_tinted(const std::string &source_path, const std::string &target_path, const char *fill) {
    std::string text;
    if (!read_all(source_path, text))
        return false;

    for (size_t pos = text.find(kWhiteFill); pos != std::string::npos; pos = text.find(kWhiteFill, pos + 7))
        text.replace(pos, 7, fill);

    FILE *f = fopen(target_path.c_str(), "wb");
    if (f == nullptr)
        return false;
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    return ok;
}

} // namespace

std::string path(const std::string &name, uint32_t rgb) {
    char tint[16];
    snprintf(tint, sizeof(tint), "#%06X", static_cast<unsigned int>(rgb & 0xFFFFFF));

    const std::string key = name + tint;
    auto cached = g_cache.find(key);
    if (cached != g_cache.end())
        return cached->second;

    const std::string base = std::string(kSvgDir) + "/" + name + ".svg";
    std::string result = "A:" + base;

    if ((rgb & 0xFFFFFF) != kWhite) {
        const std::string variant = std::string(kSvgDir) + "/" + name + "_" + (tint + 1) + ".svg";
        struct stat st;
        if (stat(variant.c_str(), &st) == 0 || write_tinted(base, variant, tint))
            result = "A:" + variant;
    }

    g_cache.emplace(key, result);
    return result;
}

} // namespace Leticia::gui::icons
