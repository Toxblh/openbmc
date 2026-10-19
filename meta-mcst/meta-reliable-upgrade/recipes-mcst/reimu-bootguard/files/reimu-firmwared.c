/*
 * reimu-firmwared: reliable upgrade state and actions for bmcweb
 *   service   xyz.openbmc_project.Reimu.Firmware
 *   object    /xyz/openbmc_project/reimu/firmware
 *   interface xyz.openbmc_project.Reimu.Firmware
 *     property Json (s)      - output of "reimu-bootguard status --json"
 *     method   Run(s) -> s   JSON {"ok": bool, "message": string}
 *              actions: verify, sync-backup, restore-primary, prepare-update
 *              (background jobs, progress in Json.job), cancel-update,
 *              commit, reboot-primary
 *
 * Copyright (c) 2026 MCST REIMU contributors. Licensed under GPLv2.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

#define TOOL "/usr/sbin/reimu-bootguard"
#define OBJ  "/xyz/openbmc_project/reimu/firmware"
#define IFC  "xyz.openbmc_project.Reimu.Firmware"
#define MAX_OUT (64 * 1024)

/* runs TOOL with the arguments, returns its exit status (-1 on failure)
 * and stdout+stderr in a malloc'ed string */
static int run_tool(char *const argv[], char **out)
{
    int pipefd[2], status = 0;
    size_t n = 0;
    ssize_t r;
    char *buf = malloc(MAX_OUT);
    if (!buf) return -1;
    buf[0] = 0;
    *out = buf;
    if (pipe(pipefd)) return -1;
    pid_t pid = fork();
    if (pid == 0)
    {
        dup2(pipefd[1], 1);
        dup2(pipefd[1], 2);
        close(pipefd[0]);
        close(pipefd[1]);
        execv(TOOL, argv);
        _exit(127);
    }
    close(pipefd[1]);
    while (n < MAX_OUT - 1 && (r = read(pipefd[0], buf + n, MAX_OUT - 1 - n)) != 0)
    {
        if (r < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        n += r;
    }
    buf[n] = 0;
    close(pipefd[0]);
    if (pid < 0 || waitpid(pid, &status, 0) != pid) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void json_escape(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++)
    {
        unsigned char c = *s;
        if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static int prop_json(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply,
                     void *ud, sd_bus_error *err)
{
    (void)bus; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    char *argv[] = { TOOL, "status", "--json", NULL };
    char *out = NULL, *line = NULL, *p;
    run_tool(argv, &out);
    /* the JSON document is the last line starting with '{', log messages
     * may precede it */
    for (p = out; p && *p; p += strcspn(p, "\n"), p += *p == '\n')
        if (*p == '{') line = p;
    if (line) line[strcspn(line, "\n")] = 0;
    int r = sd_bus_message_append(reply, "s", line ? line : "{}");
    free(out);
    return r;
}

static int method_run(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    (void)ud; (void)err;
    const char *action;
    int r = sd_bus_message_read(m, "s", &action);
    if (r < 0) return r;

    static const char *const jobs[] = { "verify", "sync-backup", "restore-primary", "prepare-update", NULL };
    static const char *const direct[] = { "cancel-update", "commit", NULL };
    char *argv[4] = { TOOL, NULL, NULL, NULL };
    int known = 0;
    for (int i = 0; jobs[i]; i++)
        if (!strcmp(action, jobs[i])) argv[1] = "job", argv[2] = (char *)action, known = 1;
    for (int i = 0; direct[i]; i++)
        if (!strcmp(action, direct[i])) argv[1] = (char *)action, known = 1;

    char *out = NULL;
    int rc;
    if (!strcmp(action, "reboot-primary"))
    {
        /* reply first: the BMC is gone right after the switch */
        pid_t pid = fork();
        if (pid == 0)
        {
            setsid();
            sleep(2);
            execl(TOOL, TOOL, "reboot-primary", (char *)NULL);
            _exit(127);
        }
        rc = pid > 0 ? 0 : 1;
        out = strdup(pid > 0 ? "rebooting from the primary chip" : "fork failed");
    }
    else if (known)
        rc = run_tool(argv, &out);
    else
    {
        rc = 1;
        out = strdup("unknown action");
    }

    /* last non-empty line, without the tool prefix */
    char *msg = out ? out : "";
    size_t n = strlen(msg);
    while (n && (msg[n - 1] == '\n' || msg[n - 1] == ' ')) msg[--n] = 0;
    char *nl = strrchr(msg, '\n');
    if (nl) msg = nl + 1;
    if (!strncmp(msg, "reimu-bootguard: ", 17)) msg += 17;

    char *res = NULL;
    size_t reslen = 0;
    FILE *f = open_memstream(&res, &reslen);
    if (!f)
    {
        free(out);
        return -ENOMEM;
    }
    fprintf(f, "{\"ok\":%s,\"message\":", rc == 0 ? "true" : "false");
    json_escape(f, *msg ? msg : (rc == 0 ? "ok" : "failed"));
    fputc('}', f);
    fclose(f);
    sd_bus_emit_properties_changed(sd_bus_message_get_bus(m), OBJ, IFC, "Json", NULL);
    r = sd_bus_reply_method_return(m, "s", res);
    free(res);
    free(out);
    return r;
}

static const sd_bus_vtable fw_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Json", "s", prop_json, 0, SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),
    SD_BUS_METHOD("Run", "s", "s", method_run, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

int main(void)
{
    sd_bus *bus = NULL;
    sd_bus_slot *slot = NULL;
    int r;

    signal(SIGCHLD, SIG_DFL);
    r = sd_bus_default_system(&bus);
    if (r < 0)
    {
        fprintf(stderr, "can't connect to system bus: %s\n", strerror(-r));
        return 1;
    }
    r = sd_bus_add_object_vtable(bus, &slot, OBJ, IFC, fw_vtable, NULL);
    if (r >= 0) r = sd_bus_add_object_manager(bus, NULL, "/xyz/openbmc_project/reimu/firmware");
    if (r >= 0) r = sd_bus_request_name(bus, IFC, 0);
    if (r < 0)
    {
        fprintf(stderr, "D-Bus setup failed: %s\n", strerror(-r));
        return 1;
    }
    for (;;)
    {
        /* reap the detached reboot-primary helper */
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;
        r = sd_bus_process(bus, NULL);
        if (r < 0) break;
        if (r > 0) continue;
        if (sd_bus_wait(bus, 5000000) < 0) break;
    }
    sd_bus_slot_unref(slot);
    sd_bus_unref(bus);
    return r < 0 ? 1 : 0;
}
