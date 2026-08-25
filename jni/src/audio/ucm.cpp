#include "ucm.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ucm {

// ============================================================================
// parse_error::description
// ============================================================================

const char *parse_error::description() const noexcept {
    switch (code) {
        case parse_error_code::none: return "no error";
        case parse_error_code::unterminated_string: return "unterminated quoted string";
        case parse_error_code::malformed_section: return "malformed section header";
        case parse_error_code::unknown_section_kind: return "unknown section kind";
        case parse_error_code::unknown_directive: return "unknown directive";
        case parse_error_code::missing_control_name: return "directive is missing a quoted control name";
        case parse_error_code::missing_value: return "directive is missing a value";
        case parse_error_code::duplicate_card_section: return "more than one [card:...] section";
        case parse_error_code::too_many_verbs: return "too many verb sections for a fixed-capacity config";
        case parse_error_code::too_many_devices: return "too many device sections within one verb";
        case parse_error_code::too_many_directives: return "too many directives within one section";
        case parse_error_code::too_many_conflicts: return "too many conflicts declared for one device";
        case parse_error_code::out_of_memory: return "could not read the config file into memory";
        case parse_error_code::invalid_channels: return "channels must be a positive integer";
        case parse_error_code::invalid_pcm_device: return "playback_pcm/capture_pcm must be a non-negative integer";
        case parse_error_code::channels_outside_device: return "channels is only valid inside a [verb:x/device:y] section";
        case parse_error_code::conflicts_outside_device: return "conflicts is only valid inside a [verb:x/device:y] section";
        case parse_error_code::pcm_device_outside_verb: return "playback_pcm/capture_pcm is only valid directly inside a [verb:x] section, not inside a device";
    }
    return "unknown error";
}

namespace {

// ============================================================================
// Small string helpers, deliberately not touching <string>/<vector>
// ============================================================================

void copy_bounded(char *dest, size_type dest_size, const char *src, size_type src_len) noexcept {
    size_type n = src_len < dest_size - 1 ? src_len : dest_size - 1;
    memcpy(dest, src, n);
    dest[n] = '\0';
}

const char *skip_spaces(const char *p) noexcept {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/**
 * @brief Trims trailing spaces/tabs/CR in place by writing a new nul
 * before the first trailing whitespace run.
 */
void rstrip(char *line) noexcept {
    size_type len = strlen(line);
    while (len > 0) {
        char c = line[len - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            line[--len] = '\0';
        } else {
            break;
        }
    }
}

/**
 * @brief Parses a `"quoted string"` starting at *p (which must point at
 * the opening quote). Advances *p past the closing quote on success.
 */
bool parse_quoted(const char **p, char *out, size_type out_size) noexcept {
    const char *cursor = *p;
    if (*cursor != '"') return false;
    cursor++;

    size_type len = 0;
    while (*cursor != '"') {
        if (*cursor == '\0') return false;
        if (len + 1 < out_size) out[len++] = *cursor;
        cursor++;
    }
    out[len] = '\0';
    cursor++; // past closing quote

    *p = cursor;
    return true;
}

bool parse_long(const char *p, long *out) noexcept {
    char *end = nullptr;
    errno = 0;
    long v = strtol(p, &end, 10);
    if (end == p || errno == ERANGE) return false;
    *out = v;
    return true;
}

} // namespace

// ============================================================================
// device::conflicts_with / verb::find_device / config::find_verb
// ============================================================================

bool device::conflicts_with(const char *other_device_name) const noexcept {
    for (size_type i = 0; i < conflict_count; i++) {
        if (strcmp(conflicts[i], other_device_name) == 0) return true;
    }
    return false;
}

const device *verb::find_device(const char *device_name) const noexcept {
    for (size_type i = 0; i < device_count; i++) {
        if (strcmp(devices[i].name, device_name) == 0) return &devices[i];
    }
    return nullptr;
}

const verb *config::find_verb(const char *verb_name) const noexcept {
    for (size_type i = 0; i < verb_count; i++) {
        if (strcmp(verbs[i].name, verb_name) == 0) return &verbs[i];
    }
    return nullptr;
}

// ============================================================================
// config::parse
// ============================================================================

namespace {

/**
 * @brief Tracks which section the parser is currently inside, so plain
 * `key = value` and `directive "ctl" = value` lines land in the right place.
 */
struct parse_cursor final {
    config *cfg = nullptr;
    verb *current_verb = nullptr;
    device *current_device = nullptr; // null while inside a bare [verb:X] section
    bool in_card_section = false;
};

/**
 * @brief Appends one directive to whichever section is currently active
 * (a device section if one is open, otherwise the enclosing verb).
 */
parse_error_code push_directive(parse_cursor &cur, const directive &d) noexcept {
    if (cur.current_verb == nullptr) return parse_error_code::malformed_section;

    if (cur.current_device != nullptr) {
        device &dev = *cur.current_device;
        if (dev.directive_count >= max_directives_per_section())
            return parse_error_code::too_many_directives;
        dev.directives[dev.directive_count++] = d;
    } else {
        verb &v = *cur.current_verb;
        if (v.directive_count >= max_directives_per_section())
            return parse_error_code::too_many_directives;
        v.directives[v.directive_count++] = d;
    }
    return parse_error_code::none;
}

/**
 * @brief Parses a `[card:name]`, `[verb:name]`, or `[verb:name/device:name]`
 * header. @p body points just past the '[' with the trailing ']' already
 * located by the caller and replaced with '\0'.
 */
parse_error_code parse_section_header(parse_cursor &cur, char *body) noexcept {
    // card:<name>
    if (strncmp(body, "card:", 5) == 0) {
        if (cur.cfg->card_name[0] != '\0') return parse_error_code::duplicate_card_section;
        copy_bounded(cur.cfg->card_name, sizeof(cur.cfg->card_name), body + 5, strlen(body + 5));
        cur.current_verb = nullptr;
        cur.current_device = nullptr;
        cur.in_card_section = true;
        return parse_error_code::none;
    }

    cur.in_card_section = false;

    // verb:<name> or verb:<name>/device:<name>
    if (strncmp(body, "verb:", 5) != 0) return parse_error_code::unknown_section_kind;

    char *verb_start = body + 5;
    char *slash = strchr(verb_start, '/');

    char verb_name[32];
    if (slash != nullptr) {
        copy_bounded(verb_name, sizeof(verb_name), verb_start, static_cast<size_type>(slash - verb_start));
    } else {
        copy_bounded(verb_name, sizeof(verb_name), verb_start, strlen(verb_start));
    }

    // Find or create the verb.
    verb *v = nullptr;
    for (size_type i = 0; i < cur.cfg->verb_count; i++) {
        if (strcmp(cur.cfg->verbs[i].name, verb_name) == 0) {
            v = &cur.cfg->verbs[i];
            break;
        }
    }
    if (v == nullptr) {
        if (cur.cfg->verb_count >= max_verbs()) return parse_error_code::too_many_verbs;
        v = &cur.cfg->verbs[cur.cfg->verb_count++];
        copy_bounded(v->name, sizeof(v->name), verb_name, strlen(verb_name));
    }
    cur.current_verb = v;
    cur.current_device = nullptr;

    if (slash == nullptr) return parse_error_code::none; // bare [verb:X]

    // .../device:<name>
    char *device_part = slash + 1;
    if (strncmp(device_part, "device:", 7) != 0) return parse_error_code::malformed_section;
    char *device_name_str = device_part + 7;
    if (*device_name_str == '\0') return parse_error_code::malformed_section;

    device *d = nullptr;
    for (size_type i = 0; i < v->device_count; i++) {
        if (strcmp(v->devices[i].name, device_name_str) == 0) {
            d = &v->devices[i];
            break;
        }
    }
    if (d == nullptr) {
        if (v->device_count >= max_devices_per_verb()) return parse_error_code::too_many_devices;
        d = &v->devices[v->device_count++];
        copy_bounded(d->name, sizeof(d->name), device_name_str, strlen(device_name_str));
    }
    cur.current_device = d;
    return parse_error_code::none;
}

/**
 * @brief Parses one `enable "ctl" [= value]`, `disable "ctl"`, or
 * `value "ctl" = value` line and appends it to the active section.
 */
parse_error_code parse_directive_line(parse_cursor &cur, const char *line) noexcept {
    directive_kind kind;
    const char *p = line;

    if (strncmp(p, "enable", 6) == 0 && (p[6] == ' ' || p[6] == '\t')) {
        kind = directive_kind::enable;
        p += 6;
    } else if (strncmp(p, "disable", 7) == 0 && (p[7] == ' ' || p[7] == '\t')) {
        kind = directive_kind::disable;
        p += 7;
    } else if (strncmp(p, "value", 5) == 0 && (p[5] == ' ' || p[5] == '\t')) {
        kind = directive_kind::value;
        p += 5;
    } else {
        return parse_error_code::unknown_directive;
    }

    p = skip_spaces(p);

    directive d;
    d.kind = kind;
    if (*p != '"' || !parse_quoted(&p, d.control_name, sizeof(d.control_name)))
        return parse_error_code::missing_control_name;

    p = skip_spaces(p);
    if (*p == '=') {
        p = skip_spaces(p + 1);
        if (*p == '"') {
            if (!parse_quoted(&p, d.text_value, sizeof(d.text_value)))
                return parse_error_code::unterminated_string;
            d.val_kind = value_kind::text;
        } else {
            long v = 0;
            if (!parse_long(p, &v)) return parse_error_code::missing_value;
            d.int_value = v;
            d.val_kind = value_kind::integer;
        }
    } else if (kind == directive_kind::value) {
        return parse_error_code::missing_value;
    } else {
        d.val_kind = value_kind::none; // bare enable/disable of a boolean-ish control
    }

    return push_directive(cur, d);
}

/**
 * @brief Parses a bare `key = value` line, valid only directly inside a
 * `[card:...]` section (currently just `name = "..."`).
 */
parse_error_code parse_card_field(parse_cursor &cur, const char *line) noexcept {
    if (strncmp(line, "name", 4) != 0) return parse_error_code::unknown_directive;
    const char *p = skip_spaces(line + 4);
    if (*p != '=') return parse_error_code::missing_value;
    p = skip_spaces(p + 1);

    char text[64];
    if (*p != '"' || !parse_quoted(&p, text, sizeof(text)))
        return parse_error_code::missing_value;

    copy_bounded(cur.cfg->card_display_name, sizeof(cur.cfg->card_display_name), text, strlen(text));
    return parse_error_code::none;
}

/**
 * @brief Recognizes `channels = N`, `conflicts = "Name"`, `playback_pcm = N`,
 * and `capture_pcm = N` lines and, if the line is one of these four keys,
 * applies it to whichever section is currently open. Lines that aren't
 * shaped like one of these keys are left alone (*out_recognized set to
 * false) so the caller falls through to directive parsing instead; this
 * is unambiguous because directive lines always start with
 * enable/disable/value, never with a bare `key =`.
 */
parse_error_code parse_property_field(parse_cursor &cur, const char *line, bool *out_recognized) noexcept {
    *out_recognized = false;

    const char *eq = strchr(line, '=');
    if (eq == nullptr) return parse_error_code::none;

    char key[32];
    copy_bounded(key, sizeof(key), line, static_cast<size_type>(eq - line));
    rstrip(key);

    const char *p = skip_spaces(eq + 1);

    if (strcmp(key, "channels") == 0) {
        *out_recognized = true;
        if (cur.current_device == nullptr) return parse_error_code::channels_outside_device;

        long v = 0;
        if (!parse_long(p, &v) || v <= 0) return parse_error_code::invalid_channels;
        cur.current_device->channels = static_cast<size_type>(v);
        return parse_error_code::none;
    }

    if (strcmp(key, "conflicts") == 0) {
        *out_recognized = true;
        if (cur.current_device == nullptr) return parse_error_code::conflicts_outside_device;

        char text[32];
        if (*p != '"' || !parse_quoted(&p, text, sizeof(text)))
            return parse_error_code::missing_value;

        device &dev = *cur.current_device;
        if (dev.conflict_count >= max_conflicts_per_device())
            return parse_error_code::too_many_conflicts;
        copy_bounded(dev.conflicts[dev.conflict_count], sizeof(dev.conflicts[dev.conflict_count]), text, strlen(text));
        dev.conflict_count++;
        return parse_error_code::none;
    }

    if (strcmp(key, "playback_pcm") == 0 || strcmp(key, "capture_pcm") == 0) {
        *out_recognized = true;
        // Must be directly inside [verb:X], not [verb:X/device:Y].
        if (cur.current_verb == nullptr || cur.current_device != nullptr)
            return parse_error_code::pcm_device_outside_verb;

        long v = 0;
        if (!parse_long(p, &v) || v < 0) return parse_error_code::invalid_pcm_device;

        if (strcmp(key, "playback_pcm") == 0)
            cur.current_verb->playback_pcm_device = static_cast<size_type>(v);
        else
            cur.current_verb->capture_pcm_device = static_cast<size_type>(v);
        return parse_error_code::none;
    }

    return parse_error_code::none; // not one of ours, let directive parsing try it
}

} // namespace

parse_error config::parse(const char *text) noexcept {
    *this = config();

    parse_cursor cur;
    cur.cfg = this;

    char line_buf[256];
    unsigned int line_no = 0;

    const char *p = text;
    while (*p != '\0') {
        const char *line_start = p;
        while (*p != '\0' && *p != '\n') p++;
        size_type len = static_cast<size_type>(p - line_start);
        if (*p == '\n') p++;
        line_no++;

        copy_bounded(line_buf, sizeof(line_buf), line_start, len);
        rstrip(line_buf);

        const char *trimmed = skip_spaces(line_buf);
        if (*trimmed == '\0' || *trimmed == ';' || *trimmed == '#') continue;

        if (*trimmed == '[') {
            char *close = strchr(const_cast<char *>(trimmed), ']');
            if (close == nullptr) return {parse_error_code::malformed_section, line_no};
            *close = '\0';

            parse_error_code code = parse_section_header(cur, const_cast<char *>(trimmed) + 1);
            if (code != parse_error_code::none) return {code, line_no};
            continue;
        }

        parse_error_code code;
        if (cur.in_card_section) {
            code = parse_card_field(cur, trimmed);
        } else {
            bool recognized = false;
            code = parse_property_field(cur, trimmed, &recognized);
            if (!recognized) code = parse_directive_line(cur, trimmed);
        }
        if (code != parse_error_code::none) return {code, line_no};
    }

    return {parse_error_code::none, 0};
}

parse_error config::parse_file(const char *path) noexcept {
    FILE *f = fopen(path, "rb");
    if (f == nullptr) return {parse_error_code::out_of_memory, 0};

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return {parse_error_code::out_of_memory, 0};
    }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return {parse_error_code::out_of_memory, 0};
    }

    char *buf = static_cast<char *>(malloc(static_cast<size_t>(size) + 1));
    if (buf == nullptr) {
        fclose(f);
        return {parse_error_code::out_of_memory, 0};
    }

    size_t read = fread(buf, 1, static_cast<size_t>(size), f);
    fclose(f);
    buf[read] = '\0';

    parse_error err = parse(buf);
    free(buf);
    return err;
}

// ============================================================================
// apply
// ============================================================================

namespace {

/**
 * @brief Drives one directive against whichever control it names,
 * dispatching on the control's actual kernel element type rather than
 * trusting the config to have gotten it right.
 * @return true if the control existed and the write succeeded, or the
 *         control simply doesn't exist on this card (skipped, not fatal).
 *         false only on a hard I/O failure from the ioctl itself.
 */
bool apply_one(const ModernAlsa::mixer &mx, const directive &d, int *out_error) noexcept {
    const ModernAlsa::mixer_ctl *ctl = mx.get_ctl_by_name(d.control_name);
    if (ctl == nullptr) return true; // not present on this card, silently skip

    ModernAlsa::result r;

    if (ctl->is_boolean()) {
        bool on = (d.kind == directive_kind::disable) ? false : (d.val_kind == value_kind::integer) ? (d.int_value != 0) :
                                                                                                      true;
        r = ctl->set_all_bools(on);
    } else if (ctl->is_enum()) {
        if (d.val_kind == value_kind::text) {
            r = ctl->set_enum_by_name(d.text_value);
        } else {
            unsigned int item = (d.kind == directive_kind::disable) ? 0 : (d.val_kind == value_kind::integer) ? static_cast<unsigned int>(d.int_value) :
                                                                                                                1;
            r = ctl->set_all_enum_indices(item);
        }
    } else if (ctl->is_integer()) {
        long v = (d.kind == directive_kind::disable) ? 0 : (d.val_kind == value_kind::integer) ? d.int_value :
                                                                                                 1;
        r = ctl->set_all_values(v);
    } else {
        return true; // bytes/int64/unknown types are out of scope for UCM directives
    }

    if (r.failed()) {
        *out_error = r.error;
        return false;
    }
    return true;
}

bool apply_sequence(const ModernAlsa::mixer &mx, const directive *directives, size_type count, int *out_error) noexcept {
    for (size_type i = 0; i < count; i++) {
        if (!apply_one(mx, directives[i], out_error)) return false;
    }
    return true;
}

/**
 * @brief Runs @p directives with enable/disable flipped, skipping
 * directive_kind::value entries (no natural inverse). Used to tear down
 * a previously-active conflicting device before a new one comes up.
 */
bool apply_sequence_inverted(const ModernAlsa::mixer &mx, const directive *directives, size_type count, int *out_error) noexcept {
    for (size_type i = 0; i < count; i++) {
        if (directives[i].kind == directive_kind::value) continue; // no natural inverse, skip

        directive flipped = directives[i];
        flipped.kind = (flipped.kind == directive_kind::enable) ? directive_kind::disable : directive_kind::enable;
        if (!apply_one(mx, flipped, out_error)) return false;
    }
    return true;
}

} // namespace

result apply(const ModernAlsa::mixer &mx, const config &cfg, const char *verb_name, const char *device_name, const char *previous_device) noexcept {
    const verb *v = cfg.find_verb(verb_name);
    if (v == nullptr) return {ENOENT};

    int err = 0;

    // Tear down a conflicting previously-active device first, if the
    // caller told us about one and it's actually declared as a conflict.
    if (previous_device != nullptr && device_name != nullptr &&
        strcmp(previous_device, device_name) != 0) {
        const device *prev = v->find_device(previous_device);
        const device *next = v->find_device(device_name);

        bool conflict = false;
        if (prev != nullptr && next != nullptr) {
            conflict = next->conflicts_with(previous_device) || prev->conflicts_with(device_name);
        }

        if (conflict) {
            if (!apply_sequence_inverted(mx, prev->directives, prev->directive_count, &err))
                return {err};
        }
    }

    if (!apply_sequence(mx, v->directives, v->directive_count, &err)) return {err};

    if (device_name == nullptr) return {0};

    const device *d = v->find_device(device_name);
    if (d == nullptr) return {ENOENT};

    if (!apply_sequence(mx, d->directives, d->directive_count, &err)) return {err};

    return {0};
}

} // namespace ucm
