#include "parent_mute.hpp"

#include <csignal>
#include <cstdlib>
#include <sys/types.h>
#include <unistd.h>

namespace Leticia {

namespace {

/* Signal handlers and atexit() only accept free functions, so the frozen
 * pid must live at namespace scope rather than as a member: there is only
 * ever one parent to unfreeze regardless of how many parent_mute objects
 * exist, and a crash handler has no object to call through anyway. */
pid_t g_parent_pid = 0;
volatile sig_atomic_t g_frozen = 0;

void do_resume()
{
    if (!g_frozen)
        return;

    g_frozen = 0;
    if (g_parent_pid > 0)
        kill(g_parent_pid, SIGCONT);
}

void resume_and_reraise(int signum)
{
    do_resume();
    signal(signum, SIG_DFL);
    raise(signum);
}

} // namespace

parent_mute::~parent_mute()
{
    resume();
}

void parent_mute::freeze()
{
    g_parent_pid = getppid();
    if (g_parent_pid <= 0)
        return;

    atexit(do_resume);
    signal(SIGSEGV, resume_and_reraise);
    signal(SIGABRT, resume_and_reraise);
    signal(SIGBUS, resume_and_reraise);
    signal(SIGFPE, resume_and_reraise);
    signal(SIGILL, resume_and_reraise);

    kill(g_parent_pid, SIGSTOP);
    g_frozen = 1;
}

void parent_mute::resume()
{
    do_resume();
}

} // namespace Leticia
