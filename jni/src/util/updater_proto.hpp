#pragma once

namespace Leticia {

/**
 * Owns the command pipe to the recovery process.
 *
 * Every write is non-blocking. The recovery process stops while the UI runs,
 * so nobody reads the pipe. A blocking write would freeze the calling thread.
 */
class updater_proto final {
public:
    updater_proto() = default;
    ~updater_proto();

    updater_proto(const updater_proto &) = delete;
    updater_proto &operator=(const updater_proto &) = delete;

    /**
     * Attaches to the recovery command pipe and makes it non-blocking.
     *
     * @param fd File descriptor received from the recovery process.
     * @return true if the descriptor is valid, false otherwise.
     */
    bool attach(int fd);

    /**
     * Closes the command pipe.
     */
    void close();

    /**
     * Writes one complete protocol message without blocking.
     *
     * The message is dropped if the pipe is full or closed.
     *
     * @param data Message bytes.
     * @param size Number of bytes in the message. Must not exceed 4096.
     */
    void write_message(const char *data, int size) const;

private:
    int fd_ = -1;
};

/**
 * Sets the global updater protocol instance.
 *
 * @param p Pointer to the updater protocol instance.
 */
void set_updater_proto(updater_proto *p);

/**
 * Clears the global updater protocol instance.
 */
void clear_updater_proto();

/**
 * Prints a message to the recovery console. Safe to call from any thread.
 *
 * @param fmt Printf style format string.
 */
void ui_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/**
 * Prints an error to the recovery console. Safe to call from any thread.
 *
 * @param fmt Printf style format string.
 */
void ui_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/**
 * Prints a warning to the recovery console. Safe to call from any thread.
 *
 * @param fmt Printf style format string.
 */
void ui_warning(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/**
 * Sets the recovery progress bar.
 *
 * @param fraction Progress fraction from 0 to 1.
 * @param seconds Estimated time for the segment.
 */
void ui_set_progress(float fraction, float seconds);

} // namespace Leticia
