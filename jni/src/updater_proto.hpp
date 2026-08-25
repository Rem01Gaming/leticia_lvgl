#pragma once

#include <cstdio>

namespace Leticia {

/**
 * @brief Wraps the line based protocol recovery speaks over the pipe fd it
 *        passes us in argv[2], so ui_print/set_progress lines reach its own
 *        log/console.
 */
class updater_proto final {
public:
    updater_proto() = default;
    ~updater_proto();

    updater_proto(const updater_proto &) = delete;
    updater_proto &operator=(const updater_proto &) = delete;

    /**
     * @brief Attach to the command pipe recovery passed as argv[2].
     * @param fd File descriptor number, as received from the recovery process.
     * @return true on success, false if the fd is invalid.
     */
    bool attach(int fd);

    /**
     * @brief Send a ui_print line so it also shows on recovery's own log/console.
     * @param fmt printf style format string.
     */
    void ui_print(const char *fmt, ...) __attribute__((format(printf, 2, 3)));

    /**
     * @brief Report install progress to recovery's native progress bar.
     * @param fraction 0.0 to 1.0.
     * @param seconds Estimated time for this segment, matches edify's set_progress semantics.
     */
    void set_progress(float fraction, float seconds);

    /**
     * @brief Close the pipe if it was attached.
     */
    void close();

private:
    FILE *pipe_ = nullptr;
};

} // namespace Leticia

// Global helpers so non-main code can emit messages that reach recovery's
// console when an updater_proto has been attached by main.
namespace Leticia {
void set_updater_proto(updater_proto *p);
void clear_updater_proto();
void ui_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
} // namespace Leticia
