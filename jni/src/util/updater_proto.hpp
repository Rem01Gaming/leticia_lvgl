#pragma once

#include <cstdio>

namespace Leticia {

/**
 * @brief Handles communication with the recovery process.
 */
class updater_proto final {
public:
    updater_proto() = default;
    ~updater_proto();

    updater_proto(const updater_proto &) = delete;
    updater_proto &operator=(const updater_proto &) = delete;

    /**
     * @brief Attaches to the recovery command pipe.
     *
     * @param fd File descriptor received from the recovery process.
     * @return true if attached successfully, false otherwise.
     */
    bool attach(int fd);

    /**
     * @brief Prints a message to the recovery console.
     *
     * @param fmt Printf style format string.
     */
    void ui_print(const char *fmt, ...) __attribute__((format(printf, 2, 3)));

    /**
     * @brief Sets the recovery progress bar.
     *
     * @param fraction Progress fraction from 0 to 1.
     * @param seconds Estimated time for the segment.
     */
    void set_progress(float fraction, float seconds);

    /**
     * @brief Closes the command pipe.
     */
    void close();

private:
    FILE *pipe_ = nullptr;
};

} // namespace Leticia

namespace Leticia {
    /**
     * @brief Sets the global updater protocol instance.
     *
     * @param p Pointer to the updater protocol instance.
     */
    void set_updater_proto(updater_proto *p);

    /**
     * @brief Clears the global updater protocol instance.
     */
    void clear_updater_proto();

    /**
     * @brief Prints a message to the recovery console using the global instance.
     *
     * @param fmt Printf style format string.
     */
    void ui_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
} // namespace Leticia
