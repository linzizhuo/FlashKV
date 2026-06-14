#include "server.h"
#include "log.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static struct Server *g_s = NULL;

static void onSignal(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        LOG_INFO("received signal %d, shutting down...", sig);
        if (g_s) g_s->stop = 1;
    }
}

int main(int argc, char **argv) {
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    int port = SERVER_PORT;
    if (argc > 1) port = atoi(argv[1]);

    g_s = serverCreate(port);
    if (!g_s) {
        LOG_ERROR("failed to create server");
        return 1;
    }

    serverRun(g_s);
    serverDestroy(g_s);
    return 0;
}
