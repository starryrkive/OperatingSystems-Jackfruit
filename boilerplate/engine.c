#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/resource.h>

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

/*
 * Child process runs memory_hog
 */
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

/* ===================== CONTAINER PROFILE ===================== */

int assign_profile(const char *id, char *label) {

    int sum = 0;
    for (int i = 0; id[i]; i++) {
        sum += id[i];
    }

    int type = sum % 3;

    if (type == 0) {
        strcpy(label, "FAST");
        return -5;
    } 
    else if (type == 1) {
        strcpy(label, "NORMAL");
        return 0;
    } 
    else {
        strcpy(label, "SLOW");
        return 10;
    }
}

/* ===================== CPU POLICY ===================== */

void apply_cpu_policy(pid_t pid, int nice_val) {
    setpriority(PRIO_PROCESS, pid, nice_val);
}

/* ===================== START ===================== */

void start_container(const char *id, int client_fd) {

    printf("[engine] starting container: %s\n", id);

    child_args_t *args = malloc(sizeof(child_args_t));
    if (!args) return;

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

    /* Assign dynamic profile */
    char profile[16];
    int nice_val = assign_profile(id, profile);

    apply_cpu_policy(pid, nice_val);

    /* Store container */
    container_t *c = malloc(sizeof(container_t));
    if (!c) return;

    strncpy(c->id, id, CONTAINER_ID_LEN - 1);
    c->id[CONTAINER_ID_LEN - 1] = '\0';
    c->pid = pid;

    c->next = containers;
    containers = c;

    printf("[supervisor] started %s (PID %d)\n", id, pid);

    /* Send response */
    write(client_fd, "OK\n", 3);

    char info[128];
    int len = snprintf(info, sizeof(info),
        "[%s] profile=%s nice=%d\n",
        id, profile, nice_val);

    write(client_fd, info, len);

    /* Read logs and stop after 10 allocations */
    char buf[256];
    int n;
    int allocations = 0;

    while ((n = read(args->pipe_fd[0], buf, sizeof(buf))) > 0) {

        write(client_fd, buf, n);

        if (strstr(buf, "allocation=")) {
            allocations++;
        }

        if (allocations >= 10) {
            write(client_fd,
                  "\n[engine] stopped after 10 allocations\n",
                  45);

            kill(pid, SIGKILL);  // terminate container
            break;
        }
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
    if (server_fd < 0) {
        perror("socket failed");
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));

    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        close(server_fd);
        return;
    }

    chmod(SOCKET_PATH, 0777);

    if (listen(server_fd, 5) < 0) {
        perror("listen failed");
        close(server_fd);
        return;
    }

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
            sscanf(cmd + 6, "%31s", id);
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
    if (fd < 0) {
        perror("socket failed");
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));

    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect failed");
        close(fd);
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
            printf("Missing container id\n");
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
