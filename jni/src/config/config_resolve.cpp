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
 * @brief Extracts a file from a zip archive using unzip.
 *
 * @param zip_path Path to the zip file.
 * @param entry_name Name of the file to extract.
 * @param out String to store the extracted content.
 * @return true if extracted successfully, false otherwise.
 */
bool run_unzip_extract(const std::string &zip_path, const char *entry_name, std::string &out) {
    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) {
        Leticia::ui_print("config: failed to create pipe: %s", strerror(errno));
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

    out.clear();
    char buf[4096];
    ssize_t n;
    while ((n = read(pipe_fd[0], buf, sizeof(buf))) > 0) {
        out.append(buf, static_cast<size_t>(n));
        if (out.size() > kMaxConfigBytes) {
            Leticia::ui_print("config: %s exceeds %zu bytes, aborting read", entry_name, kMaxConfigBytes);
            close(pipe_fd[0]);
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
            return false;
        }
    }
    close(pipe_fd[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        Leticia::ui_print("config: waitpid failed: %s", strerror(errno));
        return false;
    }

    if (!WIFEXITED(status)) {
        Leticia::ui_print("config: unzip terminated abnormally");
        return false;
    }

    int exit_code = WEXITSTATUS(status);
    if (exit_code == 127) {
        Leticia::ui_print("config: 'unzip' not found on PATH");
        return false;
    }
    if (exit_code != 0) {
        if (exit_code != 11)
            Leticia::ui_print("config: unzip exited with status %d", exit_code);
        return false;
    }

    return !out.empty();
}

} // namespace

bool resolve_config_text(const std::string &zip_path, const char *env_var, const std::string &sibling_suffix, const char *zip_entry, const char *log_label, std::string &out_text) {
    const char *env_override = getenv(env_var);
    if (env_override != nullptr) {
        if (read_whole_file(env_override, out_text)) {
            Leticia::ui_print("config: %s loaded from %s", log_label, env_override);
            return true;
        }
        Leticia::ui_print("config: %s=%s set but unreadable", env_var, env_override);
    }

    if (zip_path.empty())
        return false;

    std::string sibling_path = zip_path + sibling_suffix;
    if (read_whole_file(sibling_path, out_text)) {
        Leticia::ui_print("config: %s loaded from %s", log_label, sibling_path.c_str());
        return true;
    }

    if (run_unzip_extract(zip_path, zip_entry, out_text)) {
        Leticia::ui_print("config: %s loaded from %s inside %s", log_label, zip_entry, zip_path.c_str());
        return true;
    }

    return false;
}

} // namespace Leticia
