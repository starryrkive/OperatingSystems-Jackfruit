
#include <sys/ioctl.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "monitor_ioctl.h"

#define STACK_SIZE 65536

static char child_stack[STACK_SIZE];

typedef struct container {
    char id[64];
    pid_t pid;
    struct container *next;
} container_t;

container_t *containers = NULL;

typedef struct {
    int pipe_fd[2];
} child_args_t;

/* ===================== CHILD ===================== */

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

    if (!(*soft_triggered) && rss_bytes > (20UL * 1024 * 1024)) {
        write(client_fd, "[monitor] SOFT LIMIT exceeded\n", 32);
        *soft_triggered = 1;
    }

    if (!(*hard_triggered) && rss_bytes > (40UL * 1024 * 1024)) {
        write(client_fd, "[monitor] HARD LIMIT exceeded\n", 32);
        kill(pid, SIGKILL);
        *hard_triggered = 1;
    }
}

/* ===================== START ===================== */

void start_container(const char *id, int client_fd) {

    printf("[engine] starting container: %s\n", id);

    child_args_t *args = malloc(sizeof(child_args_t));
    if (!args) {
        perror("malloc failed");
        return;
    }

    if (pipe(args->pipe_fd) < 0) {
        perror("pipe failed");
        free(args);
        return;
    }

    pid_t pid = clone(child_fn,
                      child_stack + STACK_SIZE,
                      SIGCHLD,
                      args);

    if (pid < 0) {
        perror("clone failed");
        free(args);
        return;
    }

    close(args->pipe_fd[1]);

    printf("[engine] clone OK, pid=%d\n", pid);

    container_t *c = malloc(sizeof(container_t));
    if (!c) {
        perror("malloc failed");
        return;
    }

    strncpy(c->id, id, sizeof(c->id) - 1);
    c->id[sizeof(c->id) - 1] = '\0';
    c->pid = pid;

    c->next = containers;
    containers = c;

    usleep(100000);

    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd >= 0) {
        struct monitor_request req = {0};
        req.pid = pid;

        strncpy(req.container_id, id, sizeof(req.container_id) - 1);
        req.container_id[sizeof(req.container_id) - 1] = '\0';

        req.soft_limit_bytes = 20UL * 1024 * 1024;
        req.hard_limit_bytes = 40UL * 1024 * 1024;

        if (ioctl(fd, MONITOR_REGISTER, &req) < 0) {
            perror("ioctl MONITOR_REGISTER failed");
        }

        close(fd);
    } else {
        perror("open monitor failed");
    }

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
    container_t *curr = containers;

    while (curr) {
        char line[128];
        int len = snprintf(line, sizeof(line),
                           "%s (PID %d)\n",
                           curr->id, curr->pid);
        write(client_fd, line, len);
        curr = curr->next;
    }
}

/* ===================== SUPERVISOR ===================== */

#define SOCKET_PATH "/tmp/containerd.sock"

void run_supervisor() {

    setbuf(stdout, NULL);

    printf("[supervisor] booting...\n");

    unlink(SOCKET_PATH);

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket failed");
        exit(1);
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_PATH);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        exit(1);
    }

    chmod(SOCKET_PATH, 0777);
    listen(server_fd, 5);

    printf("[supervisor] listening on %s\n", SOCKET_PATH);

    while (1) {

        while (waitpid(-1, NULL, WNOHANG) > 0);

        int client = accept(server_fd, NULL, NULL);
        if (client < 0) continue;

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
