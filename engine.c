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

int child_fn(void *arg) {
    (void)arg;

    // Run memory hog inside container
    execl("./memory_hog", "memory_hog", NULL);

    perror("exec failed");
    return 1;
}

/* ===================== START ===================== */

void start_container(const char *id) {

    pid_t pid = clone(child_fn,
                      child_stack + STACK_SIZE,
                      CLONE_NEWPID | SIGCHLD,
                      NULL);

    if (pid < 0) {
        perror("clone failed");
        return;
    }

    container_t *c = malloc(sizeof(container_t));
    if (!c) {
        perror("malloc failed");
        return;
    }

    strncpy(c->id, id, CONTAINER_ID_LEN - 1);
    c->id[CONTAINER_ID_LEN - 1] = '\0';
    c->pid = pid;

    c->next = containers;
    containers = c;

    /* ===== REGISTER WITH MONITOR ===== */

    int fd = open("/dev/container_monitor", O_RDWR);

    if (fd < 0) {
        perror("open monitor failed");
    } else {
        struct monitor_request req;
        memset(&req, 0, sizeof(req));

        req.pid = pid;

        strncpy(req.container_id, id, sizeof(req.container_id) - 1);
        req.container_id[sizeof(req.container_id) - 1] = '\0';

        req.soft_limit_bytes = 20UL * 1024 * 1024;  // 20MB
        req.hard_limit_bytes = 40UL * 1024 * 1024;  // 40MB

        if (ioctl(fd, MONITOR_REGISTER, &req) < 0) {
            perror("ioctl MONITOR_REGISTER failed");
        }

        close(fd);
    }

    printf("[supervisor] started %s (PID %d)\n", id, pid);
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

    printf("[supervisor] listening...\n");

    while (1) {
        int client = accept(server_fd, NULL, NULL);
        if (client < 0) continue;

        char cmd[128] = {0};
        read(client, cmd, sizeof(cmd));

        if (strncmp(cmd, "start ", 6) == 0) {
            char id[32];
            sscanf(cmd + 6, "%s", id);
            start_container(id);
            write(client, "OK\n", 3);
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
        if (argc < 3) {
            printf("Usage: ./engine start <id>\n");
            return 1;
        }
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "start %s", argv[2]);
        send_command(cmd);
    }

    else if (strcmp(argv[1], "ps") == 0) {
        send_command("ps");
    }

    return 0;
}
