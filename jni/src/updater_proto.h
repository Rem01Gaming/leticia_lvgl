#pragma once

/**
 * @brief Attach to the command pipe recovery passed as argv[2].
 * @param fd File descriptor number, as received from the recovery process.
 * @return 0 on success, -1 if the fd is invalid.
 */
int updater_proto_attach(int fd);

/**
 * @brief Send a ui_print line so it also shows on recovery's own log/console.
 * @param fmt printf style format string.
 */
void updater_proto_ui_print(const char *fmt, ...);

/**
 * @brief Report install progress to recovery's native progress bar.
 * @param fraction 0.0 to 1.0.
 * @param seconds Estimated time for this segment, matches edify's set_progress semantics.
 */
void updater_proto_set_progress(float fraction, float seconds);

/**
 * @brief Close the pipe if it was attached.
 */
void updater_proto_close(void);
