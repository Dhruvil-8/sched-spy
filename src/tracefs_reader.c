#include "tracefs_reader.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <poll.h>
#include <unistd.h>
#else
#include <io.h>
#define close _close
#endif

static const char *trace_events[] = {
    "sched/sched_wakeup",
    "sched/sched_wakeup_new",
    "sched/sched_switch",
    "sched/sched_migrate_task",
    NULL
};

static void copy_comm(char dst[SCHED_SPY_COMM_LEN], const char *src) {
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= SCHED_SPY_COMM_LEN) {
        n = SCHED_SPY_COMM_LEN - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static long parse_state(const char *state) {
    if (!state || !state[0]) {
        return 0;
    }
    if (isdigit((unsigned char)state[0])) {
        return strtol(state, NULL, 0);
    }
    return state[0] == 'R' ? 0 : 1;
}

static int parse_prefix(const char *line, RawEvent *event, const char **payload) {
    const char *lb = strchr(line, '[');
    const char *rb = lb ? strchr(lb, ']') : NULL;
    const char *colon = rb ? strchr(rb, ':') : NULL;
    if (!lb || !rb || !colon) {
        return -1;
    }

    event->cpu = atoi(lb + 1);

    const char *p = rb + 1;
    while (*p && !isdigit((unsigned char)*p)) {
        p++;
    }
    char *end = NULL;
    double ts = strtod(p, &end);
    if (!end || end == p) {
        return -1;
    }
    event->timestamp_ns = (uint64_t)(ts * 1000000000.0);
    *payload = colon + 1;
    while (**payload == ' ') {
        (*payload)++;
    }
    return 0;
}

int parse_trace_line(const char *line, RawEvent *event) {
    memset(event, 0, sizeof(*event));
    event->cpu = -1;
    event->target_cpu = -1;
    event->orig_cpu = -1;
    event->dest_cpu = -1;

    const char *payload = NULL;
    if (parse_prefix(line, event, &payload) != 0) {
        return -1;
    }

    if (strncmp(payload, "sched_switch:", 13) == 0) {
        char prev_comm[64], next_comm[64], state[64];
        int prev_pid, prev_prio, next_pid, next_prio;
        int n = sscanf(payload + 13,
                       " prev_comm=%63s prev_pid=%d prev_prio=%d prev_state=%63s ==> next_comm=%63s next_pid=%d next_prio=%d",
                       prev_comm,
                       &prev_pid,
                       &prev_prio,
                       state,
                       next_comm,
                       &next_pid,
                       &next_prio);
        if (n != 7) {
            return -1;
        }
        event->type = RAW_SCHED_SWITCH;
        copy_comm(event->prev_comm, prev_comm);
        event->prev_pid = prev_pid;
        event->prev_prio = prev_prio;
        event->prev_state = parse_state(state);
        copy_comm(event->next_comm, next_comm);
        event->next_pid = next_pid;
        event->next_prio = next_prio;
        return 0;
    }

    if (strncmp(payload, "sched_wakeup_new:", 17) == 0 || strncmp(payload, "sched_wakeup:", 13) == 0) {
        int is_new = strncmp(payload, "sched_wakeup_new:", 17) == 0;
        const char *fields = payload + (is_new ? 17 : 13);
        char comm[64];
        int pid, prio, success, target_cpu;
        int n = sscanf(fields, " comm=%63s pid=%d prio=%d success=%d target_cpu=%d",
                       comm, &pid, &prio, &success, &target_cpu);
        if (n != 5) {
            n = sscanf(fields, " comm=%63s pid=%d prio=%d target_cpu=%d",
                       comm, &pid, &prio, &target_cpu);
            if (n != 4) {
                return -1;
            }
        }
        event->type = is_new ? RAW_SCHED_WAKEUP_NEW : RAW_SCHED_WAKEUP;
        copy_comm(event->comm, comm);
        event->pid = pid;
        event->prio = prio;
        event->target_cpu = target_cpu;
        return 0;
    }

    if (strncmp(payload, "sched_migrate_task:", 19) == 0) {
        char comm[64];
        int pid, prio, orig_cpu, dest_cpu;
        int n = sscanf(payload + 19,
                       " comm=%63s pid=%d prio=%d orig_cpu=%d dest_cpu=%d",
                       comm,
                       &pid,
                       &prio,
                       &orig_cpu,
                       &dest_cpu);
        if (n != 5) {
            return -1;
        }
        event->type = RAW_SCHED_MIGRATE;
        copy_comm(event->comm, comm);
        event->pid = pid;
        event->prio = prio;
        event->orig_cpu = orig_cpu;
        event->dest_cpu = dest_cpu;
        return 0;
    }

    return -1;
}

static int write_enable_file(const char *tracefs_path, const char *event, int enable) {
#ifdef _WIN32
    (void)tracefs_path;
    (void)event;
    (void)enable;
    errno = ENOSYS;
    return -1;
#else
    char path[512];
    snprintf(path, sizeof(path), "%s/events/%s/enable", tracefs_path, event);
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    const char value = enable ? '1' : '0';
    int rc = write(fd, &value, 1) == 1 ? 0 : -1;
    close(fd);
    return rc;
#endif
}

int tracefs_enable_events(const char *tracefs_path, int enable, char *err, size_t err_len) {
    for (int i = 0; trace_events[i]; i++) {
        if (write_enable_file(tracefs_path, trace_events[i], enable) != 0) {
            if (err && err_len) {
                snprintf(err, err_len, "%s/events/%s/enable: %s", tracefs_path, trace_events[i], strerror(errno));
            }
            return -1;
        }
    }
    return 0;
}

int tracefs_reader_open(TracefsReader *reader, const char *tracefs_path, char *err, size_t err_len) {
    memset(reader, 0, sizeof(*reader));
    snprintf(reader->tracefs_path, sizeof(reader->tracefs_path), "%s", tracefs_path);

#ifdef _WIN32
    (void)err;
    (void)err_len;
    reader->fd = -1;
    errno = ENOSYS;
    return -1;
#else
    char path[512];
    snprintf(path, sizeof(path), "%s/trace_pipe", tracefs_path);
    reader->fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (reader->fd < 0) {
        if (err && err_len) {
            snprintf(err, err_len, "%s: %s", path, strerror(errno));
        }
        return -1;
    }
    return 0;
#endif
}

void tracefs_reader_close(TracefsReader *reader) {
    if (reader && reader->fd >= 0) {
        close(reader->fd);
        reader->fd = -1;
    }
}

#ifndef _WIN32
static int pop_line(TracefsReader *reader, char *line, size_t line_len) {
    for (size_t i = 0; i < reader->buffer_len; i++) {
        if (reader->buffer[i] == '\n') {
            size_t n = i < line_len - 1 ? i : line_len - 1;
            memcpy(line, reader->buffer, n);
            line[n] = '\0';
            memmove(reader->buffer, reader->buffer + i + 1, reader->buffer_len - i - 1);
            reader->buffer_len -= i + 1;
            return 1;
        }
    }
    return 0;
}
#endif

int tracefs_reader_next(TracefsReader *reader, RawEvent *event, int timeout_ms) {
#ifdef _WIN32
    (void)reader;
    (void)event;
    (void)timeout_ms;
    errno = ENOSYS;
    return -1;
#else
    char line[4096];
    for (;;) {
        while (pop_line(reader, line, sizeof(line))) {
            if (parse_trace_line(line, event) == 0) {
                return 1;
            }
        }

        struct pollfd pfd;
        pfd.fd = reader->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int rc = poll(&pfd, 1, timeout_ms);
        if (rc == 0) {
            return 0;
        }
        if (rc < 0) {
            return errno == EINTR ? 0 : -1;
        }
        if (!(pfd.revents & POLLIN)) {
            return 0;
        }

        if (reader->buffer_len == sizeof(reader->buffer)) {
            reader->buffer_len = 0;
        }
        ssize_t n = read(reader->fd,
                         reader->buffer + reader->buffer_len,
                         sizeof(reader->buffer) - reader->buffer_len);
        if (n > 0) {
            reader->buffer_len += (size_t)n;
        } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
            return -1;
        }
    }
#endif
}
