#include <cstdio>
#include <cstring>

#include "commands.hpp"

#ifndef AGENT_INDICATOR_VERSION
#define AGENT_INDICATOR_VERSION "dev"
#endif

static void usage() {
    printf("agent-indicator %s - Claude Code sessions on the AgentIndicator v3 LED matrix\n\n"
           "  install [--no-service]  add Claude Code hooks + start the background service\n"
           "  uninstall               remove the hooks and the service\n"
           "  status [--json]         what the daemon sees right now\n"
           "  test [seconds]          show the layout test pattern on the device\n"
           "  raw <cmd...>            send a raw line to the device via the daemon (e.g. \"B 50\")\n"
           "  daemon [-v]             run the daemon in the foreground\n"
           "  hook                    (called by Claude Code; reads the event JSON on stdin)\n",
           AGENT_INDICATOR_VERSION);
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    const char* c = argv[1];
    if (!strcmp(c, "hook")) return cmd_hook(argc, argv);
    if (!strcmp(c, "daemon")) return cmd_daemon(argc, argv);
    if (!strcmp(c, "install")) return cmd_install(argc, argv);
    if (!strcmp(c, "uninstall")) return cmd_uninstall(argc, argv);
    if (!strcmp(c, "status")) return cmd_status(argc, argv);
    if (!strcmp(c, "raw")) return cmd_raw(argc, argv);
    if (!strcmp(c, "test")) {
        char secs[16];
        snprintf(secs, sizeof secs, "%s", argc > 2 ? argv[2] : "20");
        char t[] = "T";
        char* args[] = {argv[0], argv[1], t, secs, nullptr};
        return cmd_raw(4, args);
    }
    if (!strcmp(c, "--version") || !strcmp(c, "version")) { printf("%s\n", AGENT_INDICATOR_VERSION); return 0; }
    usage();
    return !strcmp(c, "help") || !strcmp(c, "--help") || !strcmp(c, "-h") ? 0 : 2;
}
