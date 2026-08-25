#pragma once

#include <cstddef>

#include "ModernAlsa.hpp"

/**
 * @brief Parser and applier for ALSA use case configuration.
 */
namespace ucm {

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
 * @brief Error codes for configuration parsing.
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
    too_many_conflicts,
    out_of_memory,
    invalid_channels,
    invalid_pcm_device,
    channels_outside_device,
    conflicts_outside_device,
    pcm_device_outside_verb,
};

/**
 * @brief Represents a configuration parsing error.
 */
struct parse_error final {
    parse_error_code code = parse_error_code::none;
    unsigned int line = 0;

    /**
     * @brief Checks if the parsing failed.
     *
     * @return true if failed, false otherwise.
     */
    constexpr bool failed() const noexcept {
        return code != parse_error_code::none;
    }

    /**
     * @brief Gets a description of the error.
     *
     * @return Error description string.
     */
    const char *description() const noexcept;
};

// ============================================================================
// directive_kind / value_kind
// ============================================================================

/**
 * @brief Kinds of configuration directives.
 */
enum class directive_kind {
    enable,  /**< Sets the control to an on state. */
    disable, /**< Sets the control to an off state. */
    value,   /**< Sets the control to an explicit value. */
};

/**
 * @brief Kinds of configuration values.
 */
enum class value_kind {
    none,    /**< No value. */
    integer, /**< Integer value. */
    text,    /**< String value. */
};

/**
 * @brief Represents a single configuration directive.
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
static constexpr size_type max_conflicts_per_device() noexcept {
    return 4;
}

/**
 * @brief Represents an audio device within a verb.
 */
struct device final {
    char name[32] = {};
    directive directives[max_directives_per_section()];
    size_type directive_count = 0;

    /**
     * @brief Physical channel count for this path.
     */
    size_type channels = 0;

    /**
     * @brief Names of conflicting devices.
     */
    char conflicts[max_conflicts_per_device()][32] = {};
    size_type conflict_count = 0;

    /**
     * @brief Checks if this device conflicts with another.
     *
     * @param other_device_name Name of the other device.
     * @return true if conflicting, false otherwise.
     */
    bool conflicts_with(const char *other_device_name) const noexcept;
};

/**
 * @brief Represents an audio use case verb.
 */
struct verb final {
    char name[32] = {};
    directive directives[max_directives_per_section()];
    size_type directive_count = 0;
    device devices[max_devices_per_verb()];
    size_type device_count = 0;

    /**
     * @brief ALSA PCM device indices for this verb.
     */
    size_type playback_pcm_device = invalid_index();
    size_type capture_pcm_device = invalid_index();

    /**
     * @brief Finds a device within this verb by name.
     *
     * @param device_name Name of the device.
     * @return Pointer to the device or nullptr if not found.
     */
    const device *find_device(const char *device_name) const noexcept;
};

// ============================================================================
// config
// ============================================================================

/**
 * @brief Represents a fully parsed UCM configuration.
 */
class config final {
public:
    /**
     * @brief ALSA card identifier.
     */
    char card_name[64] = {};
    /**
     * @brief Human readable card name.
     */
    char card_display_name[64] = {};
    verb verbs[max_verbs()];
    size_type verb_count = 0;

    /**
     * @brief Parses configuration text.
     *
     * @param text Null terminated configuration source.
     * @return Parse error result.
     */
    parse_error parse(const char *text) noexcept;

    /**
     * @brief Parses a configuration file.
     *
     * @param path Path to the configuration file.
     * @return Parse error result.
     */
    parse_error parse_file(const char *path) noexcept;

    /**
     * @brief Finds a verb by name.
     *
     * @param verb_name Name of the verb.
     * @return Pointer to the verb or nullptr if not found.
     */
    const verb *find_verb(const char *verb_name) const noexcept;
};

// ============================================================================
// apply
// ============================================================================

/**
 * @brief Applies configuration directives to the mixer.
 *
 * @param mx Open mixer instance.
 * @param cfg Parsed configuration.
 * @param verb_name Name of the verb to activate.
 * @param device_name Name of the device to activate.
 * @param previous_device Name of the previously active device.
 * @return Operation result.
 */
result apply(const ModernAlsa::mixer &mx, const config &cfg, const char *verb_name, const char *device_name, const char *previous_device = nullptr) noexcept;

} // namespace ucm
