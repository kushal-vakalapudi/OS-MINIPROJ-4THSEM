/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* Global supervisor context pointer for signal handlers */
static supervisor_ctx_t *g_ctx = NULL;

/* ------------------------------------------------------------------ */
/* Utility                                                              */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Bounded buffer                                                       */
/* ------------------------------------------------------------------ */

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;
    memset(buffer, 0, sizeof(*buffer));
    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0) return rc;
    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&buffer->mutex); return rc; }
    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * Push a log item into the bounded buffer.
 * Blocks if the buffer is full, returns -1 if shutting down.
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * Pop a log item from the bounded buffer.
 * Blocks while empty. Returns 0 on success, 1 when shut down and drained.
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return 1; /* done */
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Logger thread                                                        */
/* ------------------------------------------------------------------ */

/*
 * Consumer thread: drains the bounded buffer and writes to log files.
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (1) {
        int rc = bounded_buffer_pop(&ctx->log_buffer, &item);
        if (rc != 0)
            break; /* shutdown and drained */

        /* Open log file in append mode */
        int fd = open(item.container_id[0] ? item.container_id : "unknown",
                      O_WRONLY | O_CREAT | O_APPEND, 0644);

        /* Find log path from metadata */
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        char log_path[PATH_MAX] = {0};
        while (c) {
            if (strcmp(c->id, item.container_id) == 0) {
                strncpy(log_path, c->log_path, sizeof(log_path) - 1);
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (fd >= 0) close(fd);

        if (log_path[0]) {
            fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0) {
                ssize_t written = 0;
                ssize_t total = (ssize_t)item.length;
                while (written < total) {
                    ssize_t n = write(fd, item.data + written, (size_t)(total - written));
                    if (n <= 0) break;
                    written += n;
                }
                close(fd);
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Producer: reads from pipe and pushes to bounded buffer              */
/* ------------------------------------------------------------------ */

typedef struct {
    int read_fd;
    char container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buffer;
} producer_arg_t;

static void *producer_thread(void *arg)
{
    producer_arg_t *p = (producer_arg_t *)arg;
    log_item_t item;
    ssize_t n;

    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, p->container_id, CONTAINER_ID_LEN - 1);

    while ((n = read(p->read_fd, item.data, LOG_CHUNK_SIZE - 1)) > 0) {
        item.length = (size_t)n;
        item.data[n] = '\0';
        bounded_buffer_push(p->buffer, &item);
        memset(item.data, 0, sizeof(item.data));
    }

    close(p->read_fd);
    free(p);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Container child entrypoint                                           */
/* ------------------------------------------------------------------ */

int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Redirect stdout and stderr to the log pipe */
    if (cfg->log_write_fd >= 0) {
        dup2(cfg->log_write_fd, STDOUT_FILENO);
        dup2(cfg->log_write_fd, STDERR_FILENO);
        close(cfg->log_write_fd);
    }

    /* Set UTS hostname to container id */
    if (sethostname(cfg->id, strlen(cfg->id)) != 0)
        perror("sethostname");

    /* chroot into rootfs */
    if (chroot(cfg->rootfs) != 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") != 0) {
        perror("chdir");
        return 1;
    }

    /* Mount /proc */
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        /* Non-fatal: /proc may already be mounted */
    }

    /* Apply nice value */
    if (cfg->nice_value != 0) {
        if (setpriority(PRIO_PROCESS, 0, cfg->nice_value) != 0)
            perror("setpriority");
    }

    fprintf(stdout, "Inside container! Setting up environment...\n");
    fflush(stdout);

    /* Execute the command */
    char *argv[] = { cfg->command, NULL };
    char *envp[] = {
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "HOME=/root",
        "TERM=xterm",
        NULL
    };

    fprintf(stdout, "Container ready. Running command: %s\n", cfg->command);
    fflush(stdout);

    execve(cfg->command, argv, envp);
    perror("execve");
    return 1;
}

/* ------------------------------------------------------------------ */
/* Kernel monitor registration                                          */
/* ------------------------------------------------------------------ */

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Metadata helpers                                                     */
/* ------------------------------------------------------------------ */

static container_record_t *find_container(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *c = ctx->containers;
    while (c) {
        if (strcmp(c->id, id) == 0) return c;
        c = c->next;
    }
    return NULL;
}

static container_record_t *alloc_container(supervisor_ctx_t *ctx,
                                           const control_request_t *req)
{
    container_record_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;

    strncpy(c->id, req->container_id, CONTAINER_ID_LEN - 1);
    c->state = CONTAINER_STARTING;
    c->started_at = time(NULL);
    c->soft_limit_bytes = req->soft_limit_bytes;
    c->hard_limit_bytes = req->hard_limit_bytes;
    snprintf(c->log_path, sizeof(c->log_path), "%s/%s.log", LOG_DIR, c->id);

    /* Prepend to list */
    c->next = ctx->containers;
    ctx->containers = c;
    return c;
}

/* ------------------------------------------------------------------ */
/* SIGCHLD handler: reap children, update metadata                     */
/* ------------------------------------------------------------------ */

static void sigchld_handler(int sig)
{
    (void)sig;
    if (!g_ctx) return;

    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *c = g_ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                if (WIFEXITED(status)) {
                    c->state = CONTAINER_EXITED;
                    c->exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    c->exit_signal = WTERMSIG(status);
                    c->state = (c->exit_signal == SIGKILL)
                                   ? CONTAINER_KILLED
                                   : CONTAINER_STOPPED;
                }
                /* Unregister from kernel monitor */
                if (g_ctx->monitor_fd >= 0)
                    unregister_from_monitor(g_ctx->monitor_fd, c->id, pid);
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }
}

static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx) g_ctx->should_stop = 1;
}

/* ------------------------------------------------------------------ */
/* Launch a container                                                   */
/* ------------------------------------------------------------------ */

static int launch_container(supervisor_ctx_t *ctx,
                             const control_request_t *req,
                             char *response_msg,
                             size_t response_len)
{
    /* Create log directory */
    mkdir(LOG_DIR, 0755);

    /* Check for duplicate */
    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container(ctx, req->container_id)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        snprintf(response_msg, response_len,
                 "Container '%s' already exists", req->container_id);
        return -1;
    }

    container_record_t *record = alloc_container(ctx, req);
    if (!record) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        snprintf(response_msg, response_len, "Out of memory");
        return -1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Create pipe for logging */
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        snprintf(response_msg, response_len, "pipe() failed: %s", strerror(errno));
        return -1;
    }

    /* Set up child config */
    child_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        close(pipefd[0]); close(pipefd[1]);
        snprintf(response_msg, response_len, "Out of memory");
        return -1;
    }
    strncpy(cfg->id, req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs, req->rootfs, PATH_MAX - 1);
    strncpy(cfg->command, req->command, CHILD_COMMAND_LEN - 1);
    cfg->nice_value = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    /* Allocate clone stack */
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        free(cfg);
        close(pipefd[0]); close(pipefd[1]);
        snprintf(response_msg, response_len, "Out of memory");
        return -1;
    }

    int clone_flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(child_fn, stack + STACK_SIZE, clone_flags, cfg);
    free(stack);

    if (pid < 0) {
        free(cfg);
        close(pipefd[0]); close(pipefd[1]);
        snprintf(response_msg, response_len, "clone() failed: %s", strerror(errno));
        return -1;
    }

    /* Close write end in supervisor */
    close(pipefd[1]);

    /* Update metadata */
    pthread_mutex_lock(&ctx->metadata_lock);
    record->host_pid = pid;
    record->state = CONTAINER_RUNNING;
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Register with kernel monitor */
    if (ctx->monitor_fd >= 0) {
        register_with_monitor(ctx->monitor_fd, req->container_id, pid,
                              req->soft_limit_bytes, req->hard_limit_bytes);
    }

    /* Start producer thread for this container's pipe */
    producer_arg_t *parg = malloc(sizeof(*parg));
    if (parg) {
        parg->read_fd = pipefd[0];
        strncpy(parg->container_id, req->container_id, CONTAINER_ID_LEN - 1);
        parg->buffer = &ctx->log_buffer;
        pthread_t pt;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&pt, &attr, producer_thread, parg);
        pthread_attr_destroy(&attr);
    } else {
        close(pipefd[0]);
    }

    snprintf(response_msg, response_len, "Started %s (PID %d)", req->container_id, pid);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Supervisor: handle one client request                               */
/* ------------------------------------------------------------------ */

static void handle_request(supervisor_ctx_t *ctx, int client_fd)
{
    control_request_t req;
    control_response_t resp;
    ssize_t n;

    memset(&resp, 0, sizeof(resp));

    n = recv(client_fd, &req, sizeof(req), 0);
    if (n <= 0) {
        close(client_fd);
        return;
    }

    switch (req.kind) {

    case CMD_START: {
        int rc = launch_container(ctx, &req, resp.message, sizeof(resp.message));
        resp.status = (rc == 0) ? 0 : 1;
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }

    case CMD_RUN: {
        int rc = launch_container(ctx, &req, resp.message, sizeof(resp.message));
        resp.status = (rc == 0) ? 0 : 1;
        send(client_fd, &resp, sizeof(resp), 0);

        if (rc == 0) {
            /* Wait for the container to finish */
            pthread_mutex_lock(&ctx->metadata_lock);
            container_record_t *c = find_container(ctx, req.container_id);
            pid_t pid = c ? c->host_pid : -1;
            pthread_mutex_unlock(&ctx->metadata_lock);

            if (pid > 0) {
                int status;
                waitpid(pid, &status, 0);
                pthread_mutex_lock(&ctx->metadata_lock);
                c = find_container(ctx, req.container_id);
                if (c) {
                    if (WIFEXITED(status)) {
                        c->state = CONTAINER_EXITED;
                        c->exit_code = WEXITSTATUS(status);
                    } else if (WIFSIGNALED(status)) {
                        c->exit_signal = WTERMSIG(status);
                        c->state = CONTAINER_KILLED;
                    }
                }
                pthread_mutex_unlock(&ctx->metadata_lock);
                snprintf(resp.message, sizeof(resp.message),
                         "Container %s finished", req.container_id);
                resp.status = 0;
                send(client_fd, &resp, sizeof(resp), 0);
            }
        }
        break;
    }

    case CMD_PS: {
        /* Send one response per container, then a terminator */
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c) {
            snprintf(resp.message, sizeof(resp.message),
                     "%-16s %-8d %-10s %ld",
                     c->id, c->host_pid,
                     state_to_string(c->state),
                     (long)c->started_at);
            resp.status = 0;
            send(client_fd, &resp, sizeof(resp), 0);
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        /* Terminator */
        memset(&resp, 0, sizeof(resp));
        resp.status = 99; /* sentinel: end of list */
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }

    case CMD_LOGS: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = find_container(ctx, req.container_id);
        char log_path[PATH_MAX] = {0};
        if (c) strncpy(log_path, c->log_path, sizeof(log_path) - 1);
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (!log_path[0]) {
            snprintf(resp.message, sizeof(resp.message),
                     "Container '%s' not found", req.container_id);
            resp.status = 1;
            send(client_fd, &resp, sizeof(resp), 0);
            break;
        }

        FILE *f = fopen(log_path, "r");
        if (!f) {
            snprintf(resp.message, sizeof(resp.message),
                     "No logs yet for '%s'", req.container_id);
            resp.status = 1;
            send(client_fd, &resp, sizeof(resp), 0);
            break;
        }

        char line[CONTROL_MESSAGE_LEN];
        while (fgets(line, sizeof(line), f)) {
            resp.status = 0;
            strncpy(resp.message, line, sizeof(resp.message) - 1);
            send(client_fd, &resp, sizeof(resp), 0);
        }
        fclose(f);
        /* Terminator */
        memset(&resp, 0, sizeof(resp));
        resp.status = 99;
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }

    case CMD_STOP: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = find_container(ctx, req.container_id);
        pid_t pid = c ? c->host_pid : -1;
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (pid <= 0) {
            snprintf(resp.message, sizeof(resp.message),
                     "Container '%s' not found", req.container_id);
            resp.status = 1;
        } else {
            kill(pid, SIGTERM);
            usleep(200000);
            kill(pid, SIGKILL);
            pthread_mutex_lock(&ctx->metadata_lock);
            c = find_container(ctx, req.container_id);
            if (c) c->state = CONTAINER_STOPPED;
            pthread_mutex_unlock(&ctx->metadata_lock);
            snprintf(resp.message, sizeof(resp.message),
                     "Stopped %s", req.container_id);
            resp.status = 0;
        }
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }

    default:
        snprintf(resp.message, sizeof(resp.message), "Unknown command");
        resp.status = 1;
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }

    close(client_fd);
}

/* ------------------------------------------------------------------ */
/* Supervisor main loop                                                 */
/* ------------------------------------------------------------------ */

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) { errno = rc; perror("pthread_mutex_init"); return 1; }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc; perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* Open kernel monitor (optional — don't fail if not loaded) */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "Warning: cannot open /dev/container_monitor: %s\n",
                strerror(errno));

    /* Create UNIX domain socket */
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    unlink(CONTROL_PATH);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(ctx.server_fd); return 1;
    }
    if (listen(ctx.server_fd, 8) < 0) {
        perror("listen"); close(ctx.server_fd); return 1;
    }

    /* Signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* Start logger thread */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc; perror("pthread_create logger");
        close(ctx.server_fd); return 1;
    }

    fprintf(stdout, "Supervisor started with rootfs: %s\n", rootfs);
    fprintf(stdout, "Supervisor listening on %s\n", CONTROL_PATH);
    fflush(stdout);

    /* Make server socket non-blocking so we can check should_stop */
    int flags = fcntl(ctx.server_fd, F_GETFL, 0);
    fcntl(ctx.server_fd, F_SETFL, flags | O_NONBLOCK);

    /* Event loop */
    while (!ctx.should_stop) {
        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(50000);
                continue;
            }
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        handle_request(&ctx, client_fd);
    }

    fprintf(stdout, "Supervisor shutting down...\n");

    /* Stop all running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *c = ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING || c->state == CONTAINER_STARTING) {
            kill(c->host_pid, SIGKILL);
        }
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Wait briefly for children */
    sleep(1);

    /* Shutdown logger */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    /* Free metadata */
    pthread_mutex_lock(&ctx.metadata_lock);
    c = ctx.containers;
    while (c) {
        container_record_t *next = c->next;
        free(c);
        c = next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Cleanup */
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    close(ctx.server_fd);
    unlink(CONTROL_PATH);

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);

    fprintf(stdout, "Supervisor exited cleanly.\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Client-side: send a request to the supervisor                       */
/* ------------------------------------------------------------------ */

static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is supervisor running?)");
        close(fd);
        return 1;
    }

    if (send(fd, req, sizeof(*req), 0) < 0) {
        perror("send"); close(fd); return 1;
    }

    /* Read response(s) */
    control_response_t resp;
    int exit_code = 0;

    if (req->kind == CMD_PS) {
        /* Print header */
        printf("%-16s %-8s %-10s %s\n", "ID", "PID", "STATE", "STARTED");
        while (recv(fd, &resp, sizeof(resp), 0) > 0) {
            if (resp.status == 99) break;
            printf("%s\n", resp.message);
        }
    } else if (req->kind == CMD_LOGS) {
        while (recv(fd, &resp, sizeof(resp), 0) > 0) {
            if (resp.status == 99) break;
            if (resp.status != 0) { fprintf(stderr, "%s\n", resp.message); exit_code = 1; break; }
            printf("%s", resp.message);
        }
    } else if (req->kind == CMD_RUN) {
        /* First response: started */
        if (recv(fd, &resp, sizeof(resp), 0) > 0) {
            if (resp.status != 0) {
                fprintf(stderr, "%s\n", resp.message);
                exit_code = 1;
            } else {
                printf("%s\n", resp.message);
                /* Wait for finish response */
                if (recv(fd, &resp, sizeof(resp), 0) > 0)
                    printf("%s\n", resp.message);
            }
        }
    } else {
        if (recv(fd, &resp, sizeof(resp), 0) > 0) {
            printf("%s\n", resp.message);
            if (resp.status != 0) exit_code = 1;
        }
    }

    close(fd);
    return exit_code;
}

/* ------------------------------------------------------------------ */
/* CLI command handlers                                                 */
/* ------------------------------------------------------------------ */

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) { fprintf(stderr, "Usage: %s logs <id>\n", argv[0]); return 1; }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) { fprintf(stderr, "Usage: %s stop <id>\n", argv[0]); return 1; }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
