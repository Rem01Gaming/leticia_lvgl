#include "font_manager.hpp"

#include "config/config_resolve.hpp"
#include "util/updater_proto.hpp"

#include <map>
#include <tuple>

namespace Leticia::font_manager {

namespace {

constexpr const char *kUprightZipEntry = "fonts/GoogleSans-VariableFont.ttf";
constexpr const char *kItalicZipEntry = "fonts/GoogleSans-Italic-VariableFont.ttf";

/* FreeType's own glyph cache (LV_FREETYPE_CACHE_FT_GLYPH_CNT) covers a
 * single set of frequently-used glyphs shared across every created font,
 * not the lv_font_t objects themselves -- each distinct (size, weight,
 * style) is its own font instance that must be created and freed
 * explicitly, hence this cache. */
using font_key = std::tuple<int32_t, int32_t, bool>;

std::string g_upright_path;
std::string g_italic_path;
std::map<font_key, lv_font_t *> g_font_cache;
bool g_initialized = false;

bool resolve_google_sans_paths(const std::string &zip_path) {
    if (!resolve_config_file_path(zip_path, "LETICIA_FONT_GOOGLESANS", kUprightZipEntry,
                                  "Google Sans (upright)", g_upright_path)) {
        Leticia::ui_print("font_manager: failed to resolve Google Sans upright font");
        return false;
    }

    if (!resolve_config_file_path(zip_path, "LETICIA_FONT_GOOGLESANS_ITALIC", kItalicZipEntry,
                                  "Google Sans (italic)", g_italic_path)) {
        Leticia::ui_print("font_manager: failed to resolve Google Sans italic font");
        return false;
    }

    return true;
}

} // namespace

bool init(const std::string &zip_path) {
    if (g_initialized) {
        Leticia::ui_print("font_manager: init() called twice, ignoring");
        return true;
    }

    if (!resolve_google_sans_paths(zip_path))
        return false;

    /* lv_init() already initializes FreeType if LV_USE_FREETYPE is 1 in lv_conf.h.
     * A second call here is a no-op that returns LV_RESULT_INVALID; we ignore the
     * result and rely on the test-font probe below to verify functionality. */
    (void)lv_freetype_init(LV_FREETYPE_CACHE_FT_GLYPH_CNT);

    /* Fail fast on a corrupt or unreadable TTF rather than deferring the
     * error to the first get_font() call, since every screen assumes
     * get_font() cannot fail once init() has succeeded. */
    const lv_font_t *probe = get_font(16, weight::regular, false);
    if (probe == nullptr) {
        Leticia::ui_print("font_manager: failed to create a test font from %s", g_upright_path.c_str());
        lv_freetype_uninit();
        return false;
    }

    g_initialized = true;
    Leticia::ui_print("font_manager: Google Sans ready (upright=%s, italic=%s)", g_upright_path.c_str(),
                      g_italic_path.c_str());
    return true;
}

void deinit() {
    if (!g_initialized)
        return;

    for (auto &[key, font] : g_font_cache)
        lv_freetype_font_delete(font);
    g_font_cache.clear();

    lv_freetype_uninit();
    g_initialized = false;
}

const lv_font_t *get_font(int32_t size_px, weight w, bool italic) {
    font_key key{size_px, static_cast<int32_t>(w), italic};

    auto it = g_font_cache.find(key);
    if (it != g_font_cache.end())
        return it->second;

    lv_font_info_t info;
    lv_freetype_init_font_info(&info);
    info.name = (italic ? g_italic_path : g_upright_path).c_str();
    info.size = static_cast<uint32_t>(size_px);
    info.weight = static_cast<int32_t>(w);
    info.kerning = LV_FONT_KERNING_NORMAL;
    if (italic)
        info.style = LV_FREETYPE_FONT_STYLE_ITALIC;

    lv_font_t *font = lv_freetype_font_create_with_info(&info);
    if (font == nullptr) {
        Leticia::ui_print("font_manager: failed to create font (size=%d, weight=%d, italic=%d)", static_cast<int>(size_px),
                          static_cast<int>(w), static_cast<int>(italic));
        return nullptr;
    }

    g_font_cache.emplace(key, font);
    return font;
}

} // namespace Leticia::font_manager
