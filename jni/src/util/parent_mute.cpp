#include "parent_mute.hpp"

#include <csignal>
#include <cstdlib>
#include <sys/types.h>
#include <unistd.h>

namespace Leticia {

namespace {

volatile sig_atomic_t g_parent_pid = 0;
volatile sig_atomic_t g_frozen = 0;

/**
 * Sends SIGCONT to the frozen parent. Safe in a signal handler.
 */
void do_resume() {
    if (!g_frozen)
        return;

    g_frozen = 0;
    if (g_parent_pid > 0)
        kill(static_cast<pid_t>(g_parent_pid), SIGCONT);
}

/**
 * Resumes the parent and then re-raises the fatal signal with the default action.
 *
 * @param signum Signal number that triggered the handler.
 */
void resume_and_reraise(int signum) {
    do_resume();
    signal(signum, SIG_DFL);
    raise(signum);
}

} // namespace

parent_mute::~parent_mute() {
    resume();
}

void parent_mute::freeze() {
    pid_t parent = getppid();
    if (parent <= 1)
        return;

    g_parent_pid = parent;
    atexit(do_resume);
    signal(SIGSEGV, resume_and_reraise);
    signal(SIGABRT, resume_and_reraise);
    signal(SIGBUS, resume_and_reraise);
    signal(SIGFPE, resume_and_reraise);
    signal(SIGILL, resume_and_reraise);

    kill(parent, SIGSTOP);
    g_frozen = 1;
}

void parent_mute::resume() {
    do_resume();
}

void parent_mute::emergency_resume() {
    do_resume();
}

} // namespace Leticia
