#include "updater_proto.h"

#include <stdarg.h>
#include <stdio.h>

static FILE *g_pipe = NULL;

int updater_proto_attach(int fd)
{
    if (fd < 0)
        return -1;

    g_pipe = fdopen(fd, "w");
    if (g_pipe == NULL)
        return -1;

    setlinebuf(g_pipe);
    return 0;
}

void updater_proto_ui_print(const char *fmt, ...)
{
    if (g_pipe == NULL)
        return;

    char msg[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    fprintf(g_pipe, "ui_print %s\n", msg);
    fprintf(g_pipe, "ui_print\n");
}

void updater_proto_set_progress(float fraction, float seconds)
{
    if (g_pipe == NULL)
        return;

    fprintf(g_pipe, "set_progress %f\n", (double)fraction);
    (void)seconds;
}

void updater_proto_close(void)
{
    if (g_pipe != NULL) {
        fclose(g_pipe);
        g_pipe = NULL;
    }
}
