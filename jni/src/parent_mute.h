#ifndef PARENT_MUTE_H
#define PARENT_MUTE_H

/**
 * @brief Send SIGSTOP to the process that exec'd us (recovery itself).
 *        Freezes every thread in that process, including its own GUI
 *        redraw loop, so it stops fighting us for the framebuffer.
 *        Registers an atexit handler so resume always runs, even if
 *        main() returns early or exit() is called from elsewhere.
 *        Also installs fatal signal handlers so a crash in this binary
 *        cannot leave recovery permanently frozen.
 */
void parent_mute_freeze(void);

/**
 * @brief Send SIGCONT to the previously frozen parent, if any.
 *        Safe to call multiple times, only acts once.
 */
void parent_mute_resume(void);

#endif
