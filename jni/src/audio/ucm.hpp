#pragma once

#include <cstddef>

#include "ModernAlsa.hpp"

/**
 * @brief A lightweight, dependency-free parser and applier for a small
 * INI-like ALSA use-case configuration format, built on top of ModernAlsa::mixer.
 *
 * Grammar:
 *
 *   [card:mt6768]
 *   name = "MT6768 Sia81xx"
 *
 *   [verb:HiFi]
 *   playback_pcm = 0            ; optional, see below
 *   enable "ADDA_DL_CH1 DL2_CH1"
 *
 *   [verb:HiFi/device:Speaker]
 *   channels = 1                ; optional, see below
 *   conflicts = "Headphones"    ; optional, see below
 *   enable "Ext_Speaker_Amp Switch"
 *   enable "SpkrLeft Sia81xx Power" = 1
 *
 *   [verb:HiFi/device:Headphones]
 *   conflicts = "Speaker"
 *   enable "HPL Mux" = 2
 *
 * `playback_pcm` / `capture_pcm` (verb-level only): the ALSA PCM device
 * index (the second number in "hw:card,device") this verb streams
 * through, e.g. front-end 0 for a deep-buffer/primary node. Deliberately
 * verb-level, not per-device: switching between Speaker and Headphones
 * within one verb changes mixer routing only, not which PCM node is
 * open, at least on every board this has been checked against so far.
 * Unset (the default) means the caller already knows which PCM device
 * to open and doesn't need this config to say so.
 *
 * `channels` (device-level only): a fixed physical channel count for
 * this specific path, e.g. `channels = 1` for a mono speaker wired to a
 * single amp. This is NOT a negotiated hw_params capability query result
 * -- the kernel's own ModernAlsa::pcm_params::test_config() is the only
 * authoritative source for what a PCM device actually accepts, and
 * should always be checked live rather than trusted from a static
 * config. This field exists because "how many physical output channels
 * does an open PCM stream get folded down into by the amp" is a board
 * fact the kernel has no query for -- a 2-channel open will happily
 * succeed against a mono speaker, it just plays both channels into one
 * driver. 0 (the default) means unspecified; the caller's own default
 * applies.
 *
 * Rate and sample format are deliberately NOT part of this grammar.
 * Both are runtime hw_params negotiation outcomes that can vary by
 * kernel patch level independent of what a config file claims, and a
 * combination that individually looks valid on each axis can still be
 * jointly rejected -- see ModernAlsa::pcm_params::test_config()'s own
 * documentation. Query live, always.
 *
 * `conflicts` (device-level only, repeatable): names of other devices
 * in the same verb that must be torn down before this device is
 * brought up, e.g. Speaker and Headphones on a board where both drive
 * through the same physical amp path. See apply()'s previous_device
 * parameter for how this is actually enforced; the config only
 * declares which devices are mutually exclusive, apply() does the work.
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
 * @brief Reason a config file failed to parse, paired with the 1-based line number.
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
    enable,  ///< Sets the control to an "on" state (bool true, or value = 1).
    disable, ///< Sets the control to an "off" state (bool false, or value = 0).
    value,   ///< Sets the control to an explicit integer or enum-name value.
};

enum class value_kind {
    none, ///< No literal given (used by plain enable/disable of a boolean).
    integer,
    text, ///< Quoted string, used for set_enum_by_name.
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
static constexpr size_type max_conflicts_per_device() noexcept {
    return 4;
}

/**
 * @brief One `[verb:X/device:Y]` section: a named audio path within a verb,
 * e.g. Speaker, Headphones, HDMI.
 */
struct device final {
    char name[32] = {};
    directive directives[max_directives_per_section()];
    size_type directive_count = 0;

    /// Fixed physical channel count for this path, 0 = unspecified. See
    /// the file-level doc comment; this is a board fact, not a hw_params query result.
    size_type channels = 0;

    /// Names of other devices in the same verb this one is exclusive with.
    char conflicts[max_conflicts_per_device()][32] = {};
    size_type conflict_count = 0;

    bool conflicts_with(const char *other_device_name) const noexcept;
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

    /// ALSA PCM device index this verb streams through, or invalid_index()
    /// if the config doesn't declare one. See the file-level doc comment.
    size_type playback_pcm_device = invalid_index();
    size_type capture_pcm_device = invalid_index();

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
     * @brief Parses @p text in place. Does not take ownership or keep a
     * pointer to @p text after returning; the buffer may be freed once
     * this call completes.
     * @param text Null-terminated config source.
     */
    parse_error parse(const char *text) noexcept;

    /**
     * @brief Reads and parses a config file from disk.
     * @return Parse error with line 0 and a code describing the I/O
     *         failure if the file itself could not be read; ordinary
     *         parse_error otherwise.
     */
    parse_error parse_file(const char *path) noexcept;

    const verb *find_verb(const char *verb_name) const noexcept;
};

// ============================================================================
// apply
// ============================================================================

/**
 * @brief Runs a verb's default directives, then a specific device's
 * directives, against @p mx. Directives that reference a control the
 * mixer doesn't have are skipped; the first hard I/O failure on a
 * control that does exist stops the sequence and is returned.
 *
 * If @p previous_device is non-null and names a device in the same verb
 * that conflicts with @p device_name (declared via `conflicts = "..."`
 * in either device, checked both directions), that previous device's
 * own directives are run first with enable/disable flipped -- turning
 * its enable-control-X into a disable-control-X -- before the new
 * device's sequence runs. directive_kind::value entries have no natural
 * inverse and are skipped during this auto-teardown; if a `value`
 * directive genuinely needs undoing when switching away from a device,
 * write that as an explicit directive in the new device's own section,
 * the same way earlier hand-written configs did for every control.
 *
 * The library itself holds no session state -- it does not remember
 * which device was last applied. Callers that want automatic conflict
 * teardown must track "currently active device" themselves and pass it
 * as @p previous_device on the next call.
 *
 * @param mx        Open mixer for the card this config targets.
 * @param verb_name Verb to activate, e.g. "HiFi".
 * @param device_name Device within that verb, e.g. "Speaker". May be
 *        nullptr to apply only the verb's default sequence.
 * @param previous_device Device name the caller believes is currently
 *        active in this verb, or nullptr if none / unknown.
 * @return ENOENT if the verb or device name isn't found in @p cfg.
 */
result apply(const ModernAlsa::mixer &mx, const config &cfg, const char *verb_name, const char *device_name, const char *previous_device = nullptr) noexcept;

} // namespace ucm
