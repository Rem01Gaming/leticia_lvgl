#pragma once

#include <cstddef>

#include "ModernAlsa.hpp"

/**
 * @brief A lightweight, dependency-free INI-like config parser and applier for ALSA use cases, built on ModernAlsa::mixer.
 */
namespace ModernUcm {

using ModernAlsa::generic_result;
using ModernAlsa::result;
using ModernAlsa::size_type;

// ============================================================================
// Sentinel values
// ============================================================================

static constexpr size_type invalid_index() noexcept {
    return static_cast<size_type>(-1);
}

// ============================================================================
// parse_error
// ============================================================================

/**
 * @brief Reason a config file failed to parse.
 */
enum class parse_error_code {
    none,
    unterminated_string,
    malformed_section,
    unknown_section_kind,
    unknown_directive,
    missing_control_name,
    missing_value,
    duplicate_card_section,
    too_many_verbs,
    too_many_devices,
    too_many_directives,
    out_of_memory,
};

struct parse_error final {
    parse_error_code code = parse_error_code::none;
    unsigned int line = 0;

    constexpr bool failed() const noexcept {
        return code != parse_error_code::none;
    }

    const char *description() const noexcept;
};

// ============================================================================
// directive_kind / value_kind
// ============================================================================

enum class directive_kind {
    enable,   ///< Sets the control to an "on" state (bool true, or value = 1).
    disable,  ///< Sets the control to an "off" state (bool false, or value = 0).
    value,    ///< Sets the control to an explicit integer or enum-name value.
};

enum class value_kind {
    none,     ///< No literal given (used by plain enable/disable of a boolean).
    integer,
    text,     ///< Quoted string, used for set_enum_by_name.
};

/**
 * @brief One `directive "control name" = value` line inside a device or verb section.
 */
struct directive final {
    directive_kind kind = directive_kind::enable;
    char control_name[64] = {};
    value_kind val_kind = value_kind::none;
    long int_value = 0;
    char text_value[64] = {};
};

// ============================================================================
// device / verb
// ============================================================================

static constexpr size_type max_directives_per_section() noexcept {
    return 16;
}
static constexpr size_type max_devices_per_verb() noexcept {
    return 8;
}
static constexpr size_type max_verbs() noexcept {
    return 16;
}

/**
 * @brief One `[verb:X/device:Y]` section: a named audio path within a verb,
 * e.g. Speaker, Headphones, HDMI.
 */
struct device final {
    char name[32] = {};
    directive directives[max_directives_per_section()];
    size_type directive_count = 0;
};

/**
 * @brief One `[verb:X]` section: a use case, e.g. HiFi, Voice Call, Low Latency.
 * Holds a default directive sequence applied on every device switch within
 * this verb, plus a fixed set of named devices.
 */
struct verb final {
    char name[32] = {};
    directive directives[max_directives_per_section()];
    size_type directive_count = 0;
    device devices[max_devices_per_verb()];
    size_type device_count = 0;

    const device *find_device(const char *device_name) const noexcept;
};

// ============================================================================
// config
// ============================================================================

/**
 * @brief A fully parsed UCM config: one card identity plus its verbs.
 * Fixed-capacity, no heap allocation, safe to keep as a stack or static object.
 */
class config final {
public:
    char card_name[64] = {};         ///< Identifier from [card:<this>], used to look the config up.
    char card_display_name[64] = {}; ///< Optional human-readable `name = "..."` field, for logs/UI.
    verb verbs[max_verbs()];
    size_type verb_count = 0;

    /**
     * @brief Parses @p text in place; does not keep a pointer to it afterward.
     * @param text Null-terminated config source.
     */
    parse_error parse(const char *text) noexcept;

    /**
     * @brief Reads and parses a config file from disk.
     * @return line 0 with an I/O-failure code if the file couldn't be read.
     */
    parse_error parse_file(const char *path) noexcept;

    const verb *find_verb(const char *verb_name) const noexcept;
};

// ============================================================================
// apply
// ============================================================================

/**
 * @brief Runs a verb's default directives, then a device's directives,
 * against @p mx. Unknown controls are skipped; a hard I/O failure stops
 * the sequence and is returned.
 * @param verb_name   Verb to activate, e.g. "HiFi".
 * @param device_name Device within the verb, e.g. "Speaker"; nullptr to
 *        apply only the verb's default sequence.
 * @return ENOENT if the verb or device isn't found in @p cfg.
 */
result apply(const ModernAlsa::mixer &mx, const config &cfg, const char *verb_name,
             const char *device_name) noexcept;

} // namespace ModernUcm
