#include "device_config.hpp"
#include "updater_proto.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Leticia {

namespace {

constexpr const char *kZipEntryName = "config/device.conf";
constexpr const char *kAlsaZipEntryName = "config/alsa.conf";
constexpr size_t kMaxDeviceConfigBytes = 64 * 1024;

bool read_whole_file(const std::string &path, std::string &out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (f == nullptr)
        return false;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return false;
    }
    rewind(f);

    out.resize(static_cast<size_t>(size));
    size_t read = size > 0 ? fread(&out[0], 1, static_cast<size_t>(size), f) : 0;
    fclose(f);

    return read == static_cast<size_t>(size);
}

void parse_ini(const std::string &text, device_config_t &out) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? text.size() : eol + 1;

        // Trim whitespace and CR.
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

        if (key == "backlight_path") {
            out.backlight_path = val;
        } else if (key == "max_brightness") {
            out.max_brightness = atoi(val.c_str());
        } else if (key == "screen_blank_supported") {
            out.screen_blank_supported = (val == "1" || val == "true" || val == "yes");
        } else if (key == "alsa_card") {
            out.alsa_card = static_cast<unsigned int>(atoi(val.c_str()));
        }
    }
}

// Runs `unzip -p <zip_path> <entry_name>` and captures stdout, i.e. the
// decompressed entry contents (works whether the entry is STORED or
// DEFLATEd -- unzip handles both transparently, unlike a hand-rolled
// reader). Uses fork/exec with an argv array rather than popen(), so
// zip_path/entry_name are never interpolated into a shell command line.
bool run_unzip_extract(const std::string &zip_path, const char *entry_name, std::string &out) {
    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) {
        perror("device_config: pipe");
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("device_config: fork");
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return false;
    }

    if (pid == 0) {
        // Child: stdout -> pipe, stderr -> /dev/null (unzip is chatty
        // about "caution: filename not matched" etc; we log our own
        // diagnostic on failure instead).
        close(pipe_fd[0]);
        dup2(pipe_fd[1], STDOUT_FILENO);
        close(pipe_fd[1]);

        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        execlp("unzip", "unzip", "-p", zip_path.c_str(), entry_name, static_cast<char *>(nullptr));
        _exit(127); // exec failed (unzip not on PATH, etc)
    }

    // Parent.
    close(pipe_fd[1]);

    out.clear();
    char buf[4096];
    ssize_t n;
    while ((n = read(pipe_fd[0], buf, sizeof(buf))) > 0) {
        out.append(buf, static_cast<size_t>(n));
        if (out.size() > kMaxDeviceConfigBytes) {
            Leticia::ui_print("device_config: %s exceeds %zu bytes, aborting read", entry_name, kMaxDeviceConfigBytes);
            close(pipe_fd[0]);
            // Drain and reap the child so it doesn't become a zombie, but
            // don't block on it since it may still be writing.
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
            return false;
        }
    }
    close(pipe_fd[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("device_config: waitpid");
        return false;
    }

    if (!WIFEXITED(status)) {
        Leticia::ui_print("device_config: unzip terminated abnormally");
        return false;
    }

    int exit_code = WEXITSTATUS(status);
    if (exit_code == 127) {
        Leticia::ui_print("device_config: 'unzip' not found on PATH");
        return false;
    }
    // unzip -p: 0 = success. 11 = "no matching files found" (entry not
    // present -- normal case for a device with no embedded config).
    // Anything else is treated as a real failure worth logging.
    if (exit_code != 0) {
        if (exit_code != 11)
            Leticia::ui_print("device_config: unzip exited with status %d", exit_code);
        return false;
    }

    return !out.empty();
}

/**
 * @brief Shared 3-tier lookup used by both load_device_config() and
 * load_alsa_ucm_config_text(): env override, then a sibling file next to
 * the zip, then a zip-embedded entry. Returns raw text; parsing is the
 * caller's job, since the two config formats differ (plain key=value vs
 * ucm's INI-like grammar).
 */
bool resolve_config_text(const std::string &zip_path, const char *env_var, const std::string &sibling_suffix,
                          const char *zip_entry, const char *log_label, std::string &out_text) {
    const char *env_override = getenv(env_var);
    if (env_override != nullptr) {
        if (read_whole_file(env_override, out_text)) {
            Leticia::ui_print("device_config: %s loaded from %s", log_label, env_override);
            return true;
        }
        Leticia::ui_print("device_config: %s=%s set but unreadable", env_var, env_override);
    }

    if (zip_path.empty())
        return false;

    std::string sibling_path = zip_path + sibling_suffix;
    if (read_whole_file(sibling_path, out_text)) {
        Leticia::ui_print("device_config: %s loaded from %s", log_label, sibling_path.c_str());
        return true;
    }

    if (run_unzip_extract(zip_path, zip_entry, out_text)) {
        Leticia::ui_print("device_config: %s loaded from %s inside %s", log_label, zip_entry, zip_path.c_str());
        return true;
    }

    return false;
}

} // namespace

bool load_device_config(const std::string &zip_path, device_config_t &out) {
    std::string text;
    if (!resolve_config_text(zip_path, "LETICIA_DEVICE_CONFIG", ".leticia.conf", kZipEntryName, "device config", text))
        return false;

    parse_ini(text, out);
    return true;
}

bool load_alsa_ucm_config_text(const std::string &zip_path, std::string &out_text) {
    return resolve_config_text(zip_path, "LETICIA_ALSA_CONFIG", ".leticia.alsa.conf", kAlsaZipEntryName,
                                "alsa ucm config", out_text);
}

} // namespace Leticia
