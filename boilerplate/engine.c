#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "monitor_ioctl.h"

#define CONTAINER_ID_LEN 32
#define STACK_SIZE (1024 * 1024)
#define SOCKET_PATH "/tmp/engine.sock"

/* ===================== CONTAINER ===================== */

typedef struct container {
    char id[CONTAINER_ID_LEN];
    pid_t pid;
    struct container *next;
} container_t;

container_t *containers = NULL;

/* ===================== CHILD ===================== */

static char child_stack[STACK_SIZE];

typedef struct {
    int pipe_fd[2];
} child_args_t;

int child_fn(void *arg) {
    child_args_t *args = (child_args_t *)arg;

    close(args->pipe_fd[0]);

    dup2(args->pipe_fd[1], STDOUT_FILENO);
    dup2(args->pipe_fd[1], STDERR_FILENO);
    close(args->pipe_fd[1]);

    execl("./memory_hog", "memory_hog", NULL);

    perror("[child] exec failed");
    return 1;
}

/* ===================== MEMORY MONITOR ===================== */

void monitor_memory(pid_t pid, int client_fd,
                    int *soft_triggered,
                    int *hard_triggered) {

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[256];
    long rss_kb = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line, "VmRSS: %ld kB", &rss_kb);
            break;
        }
    }
    fclose(f);

    unsigned long rss_bytes = (unsigned long)rss_kb * 1024;

    const char *soft_msg = "[monitor] SOFT LIMIT exceeded\n";
    const char *hard_msg = "[monitor] HARD LIMIT exceeded\n";

    if (!(*soft_triggered) && rss_bytes > (20UL * 1024 * 1024)) {
        write(client_fd, soft_msg, strlen(soft_msg));
        *soft_triggered = 1;
    }

    if (!(*hard_triggered) && rss_bytes > (40UL * 1024 * 1024)) {
        write(client_fd, hard_msg, strlen(hard_msg));
        kill(pid, SIGKILL);
        *hard_triggered = 1;
    }
}

/* ===================== START ===================== */

void start_container(const char *id, int client_fd) {

    printf("[engine] starting container: %s\n", id);

    child_args_t *args = malloc(sizeof(child_args_t));
    pipe(args->pipe_fd);

    pid_t pid = clone(child_fn,
                      child_stack + STACK_SIZE,
                      SIGCHLD,
                      args);

    if (pid < 0) {
        perror("clone failed");
        return;
    }

    close(args->pipe_fd[1]);

    printf("[engine] clone OK, pid=%d\n", pid);

    container_t *c = malloc(sizeof(container_t));
    strncpy(c->id, id, CONTAINER_ID_LEN - 1);
    c->id[CONTAINER_ID_LEN - 1] = '\0';
    c->pid = pid;

    c->next = containers;
    containers = c;

    usleep(100000);

    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd >= 0) {
        struct monitor_request req = {0};
        req.pid = pid;

        strncpy(req.container_id, id, sizeof(req.container_id) - 1);

        req.soft_limit_bytes = 20UL * 1024 * 1024;
        req.hard_limit_bytes = 40UL * 1024 * 1024;

        ioctl(fd, MONITOR_REGISTER, &req);
        close(fd);
    }

    printf("[supervisor] started %s (PID %d)\n", id, pid);

    write(client_fd, "OK\n", 3);

    char buf[256];
    int n, lines = 0;

    int soft_triggered = 0;
    int hard_triggered = 0;

    while ((n = read(args->pipe_fd[0], buf, sizeof(buf))) > 0) {

        write(client_fd, buf, n);

        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n') lines++;
        }

        monitor_memory(pid, client_fd,
                       &soft_triggered,
                       &hard_triggered);

        if (hard_triggered) break;
        if (lines >= 10) break;
    }

    close(args->pipe_fd[0]);
    free(args);
}

/* ===================== PS ===================== */

void handle_ps(int client_fd) {
    char buf[256];
    int len;

    len = snprintf(buf, sizeof(buf), "ID\tPID\n");
    write(client_fd, buf, len);

    container_t *c = containers;
    while (c) {
        len = snprintf(buf, sizeof(buf), "%s\t%d\n", c->id, c->pid);
        write(client_fd, buf, len);
        c = c->next;
    }
}

/* ===================== SUPERVISOR ===================== */

void run_supervisor() {

    setbuf(stdout, NULL);

    printf("[supervisor] booting...\n");

    unlink(SOCKET_PATH);

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_PATH);

    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    chmod(SOCKET_PATH, 0777);
    listen(server_fd, 5);

    printf("[supervisor] listening on %s\n", SOCKET_PATH);

    while (1) {

        while (waitpid(-1, NULL, WNOHANG) > 0);

        int client = accept(server_fd, NULL, NULL);

        char cmd[128] = {0};
        int n = read(client, cmd, sizeof(cmd) - 1);
        if (n <= 0) {
            close(client);
            continue;
        }

        cmd[n] = '\0';

        printf("[supervisor] received: %s\n", cmd);

        if (strncmp(cmd, "start ", 6) == 0) {
            char id[32];
            sscanf(cmd + 6, "%s", id);
            start_container(id, client);
        }

        else if (strcmp(cmd, "ps") == 0) {
            handle_ps(client);
        }

        close(client);
    }
}

/* ===================== CLIENT ===================== */

void send_command(const char *cmd) {

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_PATH);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect failed");
        return;
    }

    write(fd, cmd, strlen(cmd));

    char buf[256];
    int n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        write(STDOUT_FILENO, buf, n);
    }

    close(fd);
}

/* ===================== MAIN ===================== */

int main(int argc, char *argv[]) {

    if (argc < 2) {
        printf("Usage:\n");
        printf("  ./engine supervisor\n");
        printf("  ./engine start <id>\n");
        printf("  ./engine ps\n");
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        run_supervisor();
    }

    else if (strcmp(argv[1], "start") == 0) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "start %s", argv[2]);
        send_command(cmd);
    }

    else if (strcmp(argv[1], "ps") == 0) {
        send_command("ps");
    }

    return 0;
}
