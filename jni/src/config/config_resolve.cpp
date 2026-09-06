#include "config_resolve.hpp"
#include "util/updater_proto.hpp"

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

constexpr size_t kMaxConfigBytes = 64 * 1024;

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

/**
 * @brief Checks whether a file exists and is non-empty.
 */
bool file_has_content(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (f == nullptr)
        return false;

    int c = fgetc(f);
    fclose(f);
    return c != EOF;
}

/**
 * @brief Common logic for running unzip -p and handling the output.
 *
 * @param zip_path Path to zip.
 * @param entry_name Entry to extract.
 * @param handle_output Callback taking the read end of the pipe.
 * @return true if unzip exited with 0 and callback returned true.
 */
template <typename F>
bool run_unzip_pipe(const std::string &zip_path, const char *entry_name, F &&handle_output) {
    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) {
        Leticia::ui_print("config: pipe failed: %s", strerror(errno));
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        Leticia::ui_print("config: fork failed: %s", strerror(errno));
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return false;
    }

    if (pid == 0) {
        close(pipe_fd[0]);
        dup2(pipe_fd[1], STDOUT_FILENO);
        close(pipe_fd[1]);

        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        execlp("unzip", "unzip", "-p", zip_path.c_str(), entry_name, static_cast<char *>(nullptr));
        _exit(127);
    }

    close(pipe_fd[1]);
    bool ok = handle_output(pipe_fd[0]);
    close(pipe_fd[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        Leticia::ui_print("config: waitpid failed: %s", strerror(errno));
        return false;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
            Leticia::ui_print("config: 'unzip' not found on PATH");
        } else if (WIFEXITED(status) && WEXITSTATUS(status) != 11) {
            Leticia::ui_print("config: unzip -p %s failed (exit=%d)", entry_name, WEXITSTATUS(status));
        }
        return false;
    }

    return ok;
}

bool run_unzip_extract(const std::string &zip_path, const char *entry_name, std::string &out) {
    return run_unzip_pipe(zip_path, entry_name, [&](int fd) {
        out.clear();
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            out.append(buf, static_cast<size_t>(n));
            if (out.size() > kMaxConfigBytes) {
                Leticia::ui_print("config: %s exceeds %zu bytes, aborting read", entry_name, kMaxConfigBytes);
                return false;
            }
        }
        return !out.empty();
    });
}

bool run_unzip_extract_to_file(const std::string &zip_path, const char *entry_name, const std::string &out_path) {
    return run_unzip_pipe(zip_path, entry_name, [&](int fd) {
        FILE *f = fopen(out_path.c_str(), "wb");
        if (!f) {
            Leticia::ui_print("config: failed to open %s for writing: %s", out_path.c_str(), strerror(errno));
            return false;
        }

        char buf[16384];
        ssize_t n;
        bool write_ok = true;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            if (fwrite(buf, 1, n, f) != static_cast<size_t>(n)) {
                Leticia::ui_print("config: write to %s failed: %s", out_path.c_str(), strerror(errno));
                write_ok = false;
                break;
            }
        }
        fclose(f);
        return write_ok && file_has_content(out_path);
    });
}

/**
 * @brief Generic tier-based resolver helper.
 */
template <typename F1, typename F2>
bool resolve_tiers(const std::string &zip_path, const char *env_var, const std::string &sibling_suffix, const char *zip_entry,
                  const char *log_label, F1 on_file, F2 on_zip) {
    const char *env_override = getenv(env_var);
    if (env_override != nullptr) {
        if (on_file(env_override)) {
            Leticia::ui_print("config: %s loaded from %s", log_label, env_override);
            return true;
        }
        Leticia::ui_print("config: %s=%s set but unreadable", env_var, env_override);
    }

    if (zip_path.empty())
        return false;

    std::string sibling_path = zip_path + sibling_suffix;
    if (on_file(sibling_path)) {
        Leticia::ui_print("config: %s loaded from %s", log_label, sibling_path.c_str());
        return true;
    }

    if (on_zip(zip_path)) {
        Leticia::ui_print("config: %s loaded from %s inside %s", log_label, zip_entry, zip_path.c_str());
        return true;
    }

    return false;
}

} // namespace

bool resolve_config_text(const std::string &zip_path, const char *env_var, const std::string &sibling_suffix,
                        const char *zip_entry, const char *log_label, std::string &out_text) {
    return resolve_tiers(
            zip_path, env_var, sibling_suffix, zip_entry, log_label,
            [&out_text](const std::string &p) { return read_whole_file(p, out_text); },
            [zip_entry, &out_text](const std::string &z) { return run_unzip_extract(z, zip_entry, out_text); });
}

bool resolve_config_file_path(const std::string &zip_path, const char *env_var, const std::string &sibling_suffix,
                             const char *zip_entry, const char *log_label, std::string &out_resolved_path) {
    return resolve_tiers(
            zip_path, env_var, sibling_suffix, zip_entry, log_label,
            [&out_resolved_path](const std::string &p) {
                if (file_has_content(p)) {
                    out_resolved_path = p;
                    return true;
                }
                return false;
            },
            [sibling_suffix, zip_entry, &out_resolved_path](const std::string &z) {
                /* Use /tmp for extractions to avoid issues with read-only
                 * source media or long paths. */
                std::string entry_hash = std::to_string(std::hash<std::string>{}(zip_entry));
                std::string extracted_path = "/tmp/leticia_res_" + entry_hash + ".bin";

                if (run_unzip_extract_to_file(z, zip_entry, extracted_path)) {
                    out_resolved_path = extracted_path;
                    return true;
                }
                return false;
            });
}

} // namespace Leticia
