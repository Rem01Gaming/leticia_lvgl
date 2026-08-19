#include "parent_mute.h"

#include <signal.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

static pid_t g_parent_pid = 0;
static volatile sig_atomic_t g_frozen = 0;

static void resume_and_reraise(int signum)
{
    parent_mute_resume();
    signal(signum, SIG_DFL);
    raise(signum);
}

void parent_mute_resume(void)
{
    if (!g_frozen)
        return;

    g_frozen = 0;
    if (g_parent_pid > 0)
        kill(g_parent_pid, SIGCONT);
}

void parent_mute_freeze(void)
{
    g_parent_pid = getppid();
    if (g_parent_pid <= 0)
        return;

    atexit(parent_mute_resume);
    signal(SIGSEGV, resume_and_reraise);
    signal(SIGABRT, resume_and_reraise);
    signal(SIGBUS, resume_and_reraise);
    signal(SIGFPE, resume_and_reraise);
    signal(SIGILL, resume_and_reraise);

    kill(g_parent_pid, SIGSTOP);
    g_frozen = 1;
}
