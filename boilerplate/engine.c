/*
 * engine.c - Supervised Multi-Container Runtime
 *
 * Full implementation:
 *   - Bounded‑buffer logging (producer/consumer threads)
 *   - Container isolation (PID, mount, UTS namespaces + chroot)
 *   - Supervisor CLI control via UNIX domain socket (/tmp/engine.sock)
 *   - Integration with kernel memory monitor (ioctl)
 *   - Signal handling (SIGCHLD, SIGINT, SIGTERM)
 *   - Clean teardown of all resources
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
#define CONTROL_PATH "/tmp/engine.sock"           /* Changed to match expected path */
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

/* ----- Enums and structures ----- */
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
    int stop_requested;
    char log_path[PATH_MAX];
    pthread_t producer_thread;
    int pipe_read_fd;
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
    int exit_code;          /* For CMD_RUN response */
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
    char container_id[CONTAINER_ID_LEN];
    int pipe_read_fd;
    bounded_buffer_t *log_buffer;
} producer_arg_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    volatile sig_atomic_t should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

static supervisor_ctx_t *g_ctx = NULL;   /* for signal handlers */

/* ----- Function prototypes ----- */
static void usage(const char *prog);
static int parse_mib_flag(const char *flag, const char *value, unsigned long *target_bytes);
static int parse_optional_flags(control_request_t *req, int argc, char *argv[], int start_index);
static const char *state_to_string(container_state_t state);

static int bounded_buffer_init(bounded_buffer_t *buffer);
static void bounded_buffer_destroy(bounded_buffer_t *buffer);
static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer);
static int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item);
static int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item);

static void *logging_thread(void *arg);
static void *producer_thread(void *arg);
static int child_fn(void *arg);
static int register_with_monitor(int monitor_fd, const char *container_id, pid_t host_pid,
                                 unsigned long soft, unsigned long hard);
static int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid);
static container_record_t *find_container(supervisor_ctx_t *ctx, const char *id);
static int start_container(supervisor_ctx_t *ctx, const control_request_t *req, control_response_t *resp);
static int stop_container(supervisor_ctx_t *ctx, const char *id, control_response_t *resp);
static void process_request(supervisor_ctx_t *ctx, const control_request_t *req, control_response_t *resp);
static void *control_thread(void *arg);
static void signal_handler(int sig);
static int run_supervisor(const char *rootfs);
static int send_control_request(const control_request_t *req);
static int cmd_start(int argc, char *argv[]);
static int cmd_run(int argc, char *argv[]);
static int cmd_ps(void);
static int cmd_logs(int argc, char *argv[]);
static int cmd_stop(int argc, char *argv[]);

/* ========== Step 2: Bounded Buffer Implementation ========== */

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;
    memset(buffer, 0, sizeof(*buffer));
    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc) return rc;
    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc) { pthread_mutex_destroy(&buffer->mutex); return rc; }
    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc) { pthread_cond_destroy(&buffer->not_empty); pthread_mutex_destroy(&buffer->mutex); return rc; }
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

static int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
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

static int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    if (buffer->shutting_down && buffer->count == 0) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }
    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/* ========== Logging Consumer Thread ========== */

static void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t*)arg;
    log_item_t item;
    char logpath[PATH_MAX];
    mkdir(LOG_DIR, 0755);
    while (1) {
        int ret = bounded_buffer_pop(&ctx->log_buffer, &item);
        if (ret == -1) break;
        snprintf(logpath, sizeof(logpath), "%s/%s.log", LOG_DIR, item.container_id);
        FILE *fp = fopen(logpath, "a");
        if (fp) {
            fwrite(item.data, 1, item.length, fp);
            fclose(fp);
        }
    }
    return NULL;
}

/* ========== Producer Thread for One Container ========== */

static void *producer_thread(void *arg)
{
    producer_arg_t *pa = (producer_arg_t*)arg;
    log_item_t item;
    ssize_t n;
    snprintf(item.container_id, sizeof(item.container_id), "%s", pa->container_id);
    while ((n = read(pa->pipe_read_fd, item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = n;
        bounded_buffer_push(pa->log_buffer, &item);
    }
    close(pa->pipe_read_fd);
    free(pa);
    return NULL;
}

/* ========== Step 3: Container Child (Namespaces + chroot) ========== */

static int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t*)arg;
    /* Make mount namespace private to avoid propagation to host */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == -1) {
        perror("mount private");
        return 1;
    }
    /* Change root to the container's rootfs */
    if (chroot(cfg->rootfs) == -1) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") == -1) {
        perror("chdir");
        return 1;
    }
    /* Mount /proc inside container */
    if (mount("proc", "/proc", "proc", 0, NULL) == -1) {
        perror("mount proc");
        return 1;
    }
    /* Set nice value if requested */
    if (nice(cfg->nice_value) == -1 && errno != 0) {
        perror("nice");
        return 1;
    }
    /* Redirect stdout and stderr to the pipe */
    dup2(cfg->log_write_fd, STDOUT_FILENO);
    dup2(cfg->log_write_fd, STDERR_FILENO);
    close(cfg->log_write_fd);
    /* Execute command via /bin/sh -c */
    execl("/bin/sh", "sh", "-c", cfg->command, NULL);
    perror("execl");
    return 1;
}

/* ========== Supervisor Helpers ========== */

static container_record_t *find_container(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *cur = ctx->containers;
    while (cur) {
        if (strcmp(cur->id, id) == 0)
            return cur;
        cur = cur->next;
    }
    return NULL;
}

static int start_container(supervisor_ctx_t *ctx, const control_request_t *req, control_response_t *resp)
{
    /* Check for duplicate ID */
    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container(ctx, req->container_id)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "Container ID '%s' already exists", req->container_id);
        return -1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Create pipe for container's stdout/stderr */
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "pipe failed: %s", strerror(errno));
        return -1;
    }

    /* Prepare child configuration */
    child_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.id, sizeof(cfg.id), "%s", req->container_id);
    snprintf(cfg.rootfs, sizeof(cfg.rootfs), "%s", req->rootfs);
    snprintf(cfg.command, sizeof(cfg.command), "%s", req->command);
    cfg.nice_value = req->nice_value;
    cfg.log_write_fd = pipefd[1];

    /* Allocate stack for clone */
    void *stack = malloc(STACK_SIZE);
    if (!stack) {
        close(pipefd[0]); close(pipefd[1]);
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "stack allocation failed");
        return -1;
    }
    int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD;
    pid_t child = clone(child_fn, stack + STACK_SIZE, flags, &cfg);
    free(stack);
    if (child == -1) {
        close(pipefd[0]); close(pipefd[1]);
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "clone failed: %s", strerror(errno));
        return -1;
    }
    close(pipefd[1]);   /* parent no longer writes */

    /* Create producer thread to read from pipe */
    producer_arg_t *pa = malloc(sizeof(producer_arg_t));
    if (!pa) {
        close(pipefd[0]);
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "producer arg alloc failed");
        return -1;
    }
    snprintf(pa->container_id, sizeof(pa->container_id), "%s", req->container_id);
    pa->pipe_read_fd = pipefd[0];
    pa->log_buffer = &ctx->log_buffer;
    pthread_t prod_tid;
    if (pthread_create(&prod_tid, NULL, producer_thread, pa) != 0) {
        free(pa);
        close(pipefd[0]);
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "producer thread create failed");
        return -1;
    }
    pthread_detach(prod_tid);   /* thread cleans itself when pipe closes */

    /* Register with kernel monitor */
    if (ctx->monitor_fd >= 0) {
        register_with_monitor(ctx->monitor_fd, req->container_id, child,
                              req->soft_limit_bytes, req->hard_limit_bytes);
    }

    /* Create container record */
    container_record_t *rec = calloc(1, sizeof(container_record_t));
    snprintf(rec->id, sizeof(rec->id), "%s", req->container_id);
    rec->host_pid = child;
    rec->started_at = time(NULL);
    rec->state = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->exit_code = 0;
    rec->exit_signal = 0;
    rec->stop_requested = 0;
    snprintf(rec->log_path, sizeof(rec->log_path), "%s/%s.log", LOG_DIR, req->container_id);
    rec->producer_thread = prod_tid;
    rec->pipe_read_fd = pipefd[0];

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    resp->status = 0;
    snprintf(resp->message, CONTROL_MESSAGE_LEN, "Container %s started with PID %d", req->container_id, child);
    return 0;
}

static int stop_container(supervisor_ctx_t *ctx, const char *id, control_response_t *resp)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *rec = find_container(ctx, id);
    if (!rec) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "Container %s not found", id);
        return -1;
    }
    if (rec->state != CONTAINER_RUNNING && rec->state != CONTAINER_STARTING) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "Container %s not running (state=%s)", id, state_to_string(rec->state));
        return -1;
    }
    rec->stop_requested = 1;
    pid_t pid = rec->host_pid;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (kill(pid, SIGTERM) == -1) {
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "kill(%d, SIGTERM) failed: %s", pid, strerror(errno));
        return -1;
    }
    resp->status = 0;
    snprintf(resp->message, CONTROL_MESSAGE_LEN, "SIGTERM sent to container %s", id);
    return 0;
}

/* ========== Step 4: Control Request Processing ========== */

static void process_request(supervisor_ctx_t *ctx, const control_request_t *req, control_response_t *resp)
{
    memset(resp, 0, sizeof(*resp));
    switch (req->kind) {
        case CMD_START:
            start_container(ctx, req, resp);
            break;
        case CMD_RUN:
            {
                if (start_container(ctx, req, resp) != 0)
                    break;
                /* Wait for container to exit */
                while (1) {
                    pthread_mutex_lock(&ctx->metadata_lock);
                    container_record_t *rec = find_container(ctx, req->container_id);
                    if (!rec) {
                        pthread_mutex_unlock(&ctx->metadata_lock);
                        resp->status = -1;
                        snprintf(resp->message, CONTROL_MESSAGE_LEN, "container vanished");
                        break;
                    }
                    if (rec->state == CONTAINER_EXITED || rec->state == CONTAINER_STOPPED || rec->state == CONTAINER_KILLED) {
                        resp->exit_code = rec->exit_code;
                        if (rec->exit_signal)
                            resp->exit_code = 128 + rec->exit_signal;
                        snprintf(resp->message, CONTROL_MESSAGE_LEN, "Container %s exited with code %d", req->container_id, resp->exit_code);
                        pthread_mutex_unlock(&ctx->metadata_lock);
                        break;
                    }
                    pthread_mutex_unlock(&ctx->metadata_lock);
                    usleep(100000);   /* poll every 100 ms */
                }
                resp->status = 0;
            }
            break;
        case CMD_PS:
            {
                char buf[CONTROL_MESSAGE_LEN] = {0};
                pthread_mutex_lock(&ctx->metadata_lock);
                container_record_t *cur = ctx->containers;
                while (cur) {
                    char line[128];
                    snprintf(line, sizeof(line), "%-16s %-6d %-10s %-10ld %-10ld\n",
                             cur->id, cur->host_pid, state_to_string(cur->state),
                             cur->soft_limit_bytes >> 20, cur->hard_limit_bytes >> 20);
                    strncat(buf, line, CONTROL_MESSAGE_LEN - strlen(buf) - 1);
                    cur = cur->next;
                }
                pthread_mutex_unlock(&ctx->metadata_lock);
                if (buf[0] == '\0')
                    snprintf(buf, CONTROL_MESSAGE_LEN, "No containers.\n");
                snprintf(resp->message, CONTROL_MESSAGE_LEN, "%s", buf);
                resp->status = 0;
            }
            break;
        case CMD_LOGS:
            {
                char logpath[PATH_MAX];
                snprintf(logpath, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);
                FILE *fp = fopen(logpath, "r");
                if (!fp) {
                    resp->status = -1;
                    snprintf(resp->message, CONTROL_MESSAGE_LEN, "No log file for %s", req->container_id);
                    break;
                }
                char *logdata = malloc(CONTROL_MESSAGE_LEN);
                if (!logdata) {
                    fclose(fp);
                    resp->status = -1;
                    snprintf(resp->message, CONTROL_MESSAGE_LEN, "Out of memory");
                    break;
                }
                size_t n = fread(logdata, 1, CONTROL_MESSAGE_LEN - 1, fp);
                logdata[n] = '\0';
                snprintf(resp->message, CONTROL_MESSAGE_LEN, "%s", logdata);
                free(logdata);
                fclose(fp);
                resp->status = 0;
            }
            break;
        case CMD_STOP:
            stop_container(ctx, req->container_id, resp);
            break;
        default:
            resp->status = -1;
            snprintf(resp->message, CONTROL_MESSAGE_LEN, "Unknown command");
            break;
    }
}

/* ========== Control Thread (UNIX socket server) ========== */

static void *control_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t*)arg;
    struct sockaddr_un addr;
    int client_fd;
    control_request_t req;
    control_response_t resp;

    unlink(CONTROL_PATH);
    ctx->server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx->server_fd == -1) {
        perror("socket");
        return NULL;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", CONTROL_PATH);
    if (bind(ctx->server_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("bind");
        close(ctx->server_fd);
        return NULL;
    }
    if (listen(ctx->server_fd, 5) == -1) {
        perror("listen");
        close(ctx->server_fd);
        return NULL;
    }
    fprintf(stderr, "Supervisor listening on %s\n", CONTROL_PATH);

    while (!ctx->should_stop) {
        client_fd = accept(ctx->server_fd, NULL, NULL);
        if (client_fd == -1) {
            if (ctx->should_stop) break;
            continue;
        }
        ssize_t n = read(client_fd, &req, sizeof(req));
        if (n == sizeof(req)) {
            process_request(ctx, &req, &resp);
            ssize_t w = write(client_fd, &resp, sizeof(resp));
            if (w != sizeof(resp))
                perror("write to client");
        }
        close(client_fd);
    }
    close(ctx->server_fd);
    unlink(CONTROL_PATH);
    return NULL;
}

/* ========== Step 5: Signal Handling and Cleanup ========== */

static void signal_handler(int sig)
{
    if (!g_ctx) return;
    if (sig == SIGCHLD) {
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            pthread_mutex_lock(&g_ctx->metadata_lock);
            container_record_t *cur = g_ctx->containers;
            while (cur) {
                if (cur->host_pid == pid) {
                    if (WIFEXITED(status)) {
                        cur->exit_code = WEXITSTATUS(status);
                        cur->state = CONTAINER_EXITED;
                    } else if (WIFSIGNALED(status)) {
                        cur->exit_signal = WTERMSIG(status);
                        if (cur->stop_requested && cur->exit_signal == SIGTERM)
                            cur->state = CONTAINER_STOPPED;
                        else if (!cur->stop_requested && cur->exit_signal == SIGKILL)
                            cur->state = CONTAINER_KILLED;
                        else
                            cur->state = CONTAINER_EXITED;
                    }
                    /* Unregister from kernel monitor */
                    if (g_ctx->monitor_fd >= 0)
                        unregister_from_monitor(g_ctx->monitor_fd, cur->id, pid);
                    break;
                }
                cur = cur->next;
            }
            pthread_mutex_unlock(&g_ctx->metadata_lock);
        }
    } else if (sig == SIGINT || sig == SIGTERM) {
        g_ctx->should_stop = 1;
        /* Closing the server socket will break accept() */
        if (g_ctx->server_fd >= 0)
            close(g_ctx->server_fd);
    }
}

static int run_supervisor(const char *rootfs)
{
    (void)rootfs;   /* unused parameter */
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    ctx.should_stop = 0;
    g_ctx = &ctx;          /* global pointer for signal handler */

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc) { errno = rc; perror("pthread_mutex_init"); return 1; }
    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc) { errno = rc; perror("bounded_buffer_init"); return 1; }

    /* Open kernel monitor device */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0) {
        fprintf(stderr, "Warning: could not open /dev/container_monitor (module loaded?)\n");
    }

    /* Start logging consumer thread */
    if (pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx) != 0) {
        perror("pthread_create logger");
        return 1;
    }

    /* Install signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGCHLD, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Start control thread (UNIX socket) */
    pthread_t ctrl_tid;
    if (pthread_create(&ctrl_tid, NULL, control_thread, &ctx) != 0) {
        perror("pthread_create control");
        return 1;
    }

    /* Wait for shutdown signal */
    while (!ctx.should_stop) {
        sleep(1);
    }

    /* Begin shutdown: stop accepting new logs, flush buffer */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    pthread_join(ctrl_tid, NULL);

    /* Terminate all running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *cur = ctx.containers;
    while (cur) {
        if (cur->state == CONTAINER_RUNNING || cur->state == CONTAINER_STARTING) {
            kill(cur->host_pid, SIGTERM);
            waitpid(cur->host_pid, NULL, 0);
        }
        cur = cur->next;
    }
    /* Free records */
    cur = ctx.containers;
    while (cur) {
        container_record_t *next = cur->next;
        free(cur);
        cur = next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);
    pthread_mutex_destroy(&ctx.metadata_lock);
    bounded_buffer_destroy(&ctx.log_buffer);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    g_ctx = NULL;
    return 0;
}

/* ========== Client Side IPC (UNIX socket client) ========== */

static int send_control_request(const control_request_t *req)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("socket");
        return 1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", CONTROL_PATH);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("connect (is supervisor running?)");
        close(sock);
        return 1;
    }
    if (write(sock, req, sizeof(*req)) != sizeof(*req)) {
        perror("write");
        close(sock);
        return 1;
    }
    control_response_t resp;
    if (read(sock, &resp, sizeof(resp)) != sizeof(resp)) {
        perror("read");
        close(sock);
        return 1;
    }
    close(sock);
    if (resp.status == 0) {
        printf("%s", resp.message);
        if (req->kind == CMD_RUN)
            return resp.exit_code;
        return 0;
    } else {
        fprintf(stderr, "Error: %s\n", resp.message);
        return 1;
    }
}

/* ========== Command Stubs ========== */

static int cmd_start(int argc, char *argv[])
{
    if (argc < 5) { usage(argv[0]); return 1; }
    control_request_t req = {0};
    req.kind = CMD_START;
    snprintf(req.container_id, sizeof(req.container_id), "%s", argv[2]);
    snprintf(req.rootfs, sizeof(req.rootfs), "%s", argv[3]);
    snprintf(req.command, sizeof(req.command), "%s", argv[4]);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    req.nice_value = 0;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    if (argc < 5) { usage(argv[0]); return 1; }
    control_request_t req = {0};
    req.kind = CMD_RUN;
    snprintf(req.container_id, sizeof(req.container_id), "%s", argv[2]);
    snprintf(req.rootfs, sizeof(req.rootfs), "%s", argv[3]);
    snprintf(req.command, sizeof(req.command), "%s", argv[4]);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    req.nice_value = 0;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req = {0};
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    if (argc < 3) { usage(argv[0]); return 1; }
    control_request_t req = {0};
    req.kind = CMD_LOGS;
    snprintf(req.container_id, sizeof(req.container_id), "%s", argv[2]);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    if (argc < 3) { usage(argv[0]); return 1; }
    control_request_t req = {0};
    req.kind = CMD_STOP;
    snprintf(req.container_id, sizeof(req.container_id), "%s", argv[2]);
    return send_control_request(&req);
}

/* ========== Helper Implementations ========== */

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

static int parse_mib_flag(const char *flag, const char *value, unsigned long *target_bytes)
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

static int parse_optional_flags(control_request_t *req, int argc, char *argv[], int start_index)
{
    for (int i = start_index; i < argc; i += 2) {
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i+1], &req->soft_limit_bytes) != 0) return -1;
        } else if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1], &req->hard_limit_bytes) != 0) return -1;
        } else if (strcmp(argv[i], "--nice") == 0) {
            char *end;
            long nice = strtol(argv[i+1], &end, 10);
            if (errno != 0 || end == argv[i+1] || *end != '\0' || nice < -20 || nice > 19) {
                fprintf(stderr, "Invalid value for --nice: %s\n", argv[i+1]);
                return -1;
            }
            req->nice_value = (int)nice;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return -1;
        }
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
        default: return "unknown";
    }
}

int register_with_monitor(int monitor_fd, const char *container_id, pid_t host_pid,
                          unsigned long soft_limit_bytes, unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    snprintf(req.container_id, sizeof(req.container_id), "%s", container_id);
    return ioctl(monitor_fd, MONITOR_REGISTER, &req);
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    snprintf(req.container_id, sizeof(req.container_id), "%s", container_id);
    return ioctl(monitor_fd, MONITOR_UNREGISTER, &req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]); return 1; }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run") == 0)   return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps") == 0)    return cmd_ps();
    if (strcmp(argv[1], "logs") == 0)  return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop") == 0)  return cmd_stop(argc, argv);
    usage(argv[0]);
    return 1;
}
