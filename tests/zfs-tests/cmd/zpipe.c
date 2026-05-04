// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2026 CompEd Software Design srl.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/select.h>
#include <getopt.h>

#define BUF_SIZE (1024 * 1024)
#define WRITE_CHUNK 1024
#define IOMON_STATUS_MAXBYTES 142
#define IOMON_STATUS_TIMEOUT 143

unsigned long long g_total_bytes = 0;
char    g_cnt_file[1024] = {0};
int     g_cnt_active = 0;
int     g_cnt_interval = 1;
time_t  g_last_update = 0;

static void write_progress(void) {
    if (!g_cnt_active || !g_cnt_file[0]) return;
    FILE *fp = fopen(g_cnt_file, "w");
    if (fp) {
        fprintf(fp, "%llu\n", g_total_bytes);
        fclose(fp);
    }
}

static void maybe_update_counter(void) {
    if (!g_cnt_active) return;
    time_t now = time(NULL);
    if (now - g_last_update >= g_cnt_interval) {
        write_progress();
        g_last_update = now;
    }
}

static void handle_signal(int sig) {
    (void)sig;
    write_progress();
    exit(0);
}

static int parse_rate(const char *s) {
    if (!s || !*s) return 0;
    char *end;
    long long val = strtoll(s, &end, 10);
    if (val <= 0) return 0;
    if (*end == '\0') return (int)val;
    switch (*end) {
        case 'k': case 'K': val *= 1024; break;
        case 'm': case 'M': val *= 1024 * 1024; break;
        case 'g': case 'G': val *= 1024 * 1024 * 1024; break;
        default: return 0;
    }
    return (int)val;
}

static int mkdir_p(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    return 0;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Pipeline monitor for zfs send/recv. Reads from stdin, writes to stdout.\n"
        "\n"
        "Options:\n"
        "  -t, --timeout SEC         Exit with code %d after SEC seconds of runtime\n"
        "  -r, --throttle BYTES      Throttle write rate (e.g. 32k, 1M)\n"
        "  -c, --cut BYTES           Exit with code %d after transferring BYTES\n"
        "      --counter PATH        Write live byte counter to PATH every interval\n"
        "      --counter-interval S  Update counter every S seconds (default: 1)\n"
        "  -h, --help                Show this help\n",
        prog, IOMON_STATUS_TIMEOUT, IOMON_STATUS_MAXBYTES);
}

int main(int argc, char *argv[]) {
    int timeout_sec = 0;
    int write_rate = 0;
    int max_bytes = 0;
    const char *counter_path = NULL;
    int counter_interval = 1;

    static struct option long_opts[] = {
        {"timeout",          required_argument, 0, 't'},
        {"throttle",         required_argument, 0, 'r'},
        {"cut",              required_argument, 0, 'c'},
        {"counter",          required_argument, 0, 'C'},
        {"counter-interval", required_argument, 0, 'I'},
        {"help",             no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "t:r:c:h", long_opts, NULL)) != -1) {
        switch (c) {
            case 't': timeout_sec = atoi(optarg); break;
            case 'r': write_rate = parse_rate(optarg); break;
            case 'c': max_bytes  = parse_rate(optarg); break;
            case 'C': counter_path = optarg; break;
            case 'I': counter_interval = atoi(optarg); break;
            case 'h': print_usage(argv[0]); return 0;
            default:
                fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
                return 1;
        }
    }

    if (counter_path) {
        if (counter_interval <= 0) counter_interval = 1;

        snprintf(g_cnt_file, sizeof(g_cnt_file), "%s", counter_path);

        char dir[1024];
        snprintf(dir, sizeof(dir), "%s", counter_path);
        char *last_slash = strrchr(dir, '/');
        if (last_slash) {
            *last_slash = '\0';
            if (mkdir_p(dir) != 0)
                fprintf(stderr, "zpipe: cannot create counter directory %s: %s\n", dir, strerror(errno));
        }

        g_cnt_active = 1;
        g_cnt_interval = counter_interval;
    }

    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);

    unsigned char *buffer = malloc(BUF_SIZE);
    if (!buffer) return 1;

    size_t read_size = BUF_SIZE;
    if (max_bytes > 0 && (size_t)max_bytes < read_size)
        read_size = (size_t)max_bytes;

    time_t start_time = time(NULL);

    int chunk_size = (write_rate > 0 && write_rate < WRITE_CHUNK) ? write_rate : WRITE_CHUNK;
    struct timespec chunk_delay = {0, 0};
    if (write_rate > 0) {
        double delay_ns = (double)chunk_size / write_rate * 1000000000.0;
        chunk_delay.tv_sec  = (time_t)(delay_ns / 1000000000.0);
        chunk_delay.tv_nsec = (long)(delay_ns - chunk_delay.tv_sec * 1000000000.0);
    }

    maybe_update_counter();

    while (1) {
        if (timeout_sec > 0 && time(NULL) - start_time >= timeout_sec) {
            fprintf(stderr, "zpipe: timeout after %ds\n", timeout_sec);
            write_progress();
            free(buffer);
            return IOMON_STATUS_TIMEOUT;
        }

        int wait_sec = -1;
        if (g_cnt_active && g_cnt_interval > 0)
            wait_sec = g_cnt_interval;
        if (timeout_sec > 0) {
            int remaining = timeout_sec - (int)(time(NULL) - start_time);
            if (remaining < 0) remaining = 0;
            if (wait_sec < 0 || remaining < wait_sec) wait_sec = remaining;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        struct timeval tv, *ptv = NULL;
        if (wait_sec >= 0) { tv.tv_sec = wait_sec; tv.tv_usec = 0; ptv = &tv; }

        int sel = select(STDIN_FILENO + 1, &rfds, NULL, NULL, ptv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            break;
        }

        maybe_update_counter();

        if (sel == 0) continue;

        ssize_t bytes_read = read(STDIN_FILENO, buffer, read_size);
        if (bytes_read <= 0) break;

        ssize_t bytes_written = 0;
        while (bytes_written < bytes_read) {
            if (timeout_sec > 0 && (time(NULL) - start_time >= timeout_sec)) {
                fprintf(stderr, "zpipe: timeout after %ds\n", timeout_sec);
                write_progress();
                free(buffer);
                return IOMON_STATUS_TIMEOUT;
            }

            ssize_t to_write = bytes_read - bytes_written;
            if (write_rate > 0 && to_write > chunk_size)
                to_write = chunk_size;

            ssize_t res = write(STDOUT_FILENO, buffer + bytes_written, to_write);
            if (res < 0) {
                if (errno == EINTR) continue;
                free(buffer);
                return 1;
            }
            bytes_written += res;

            if (write_rate > 0 && (chunk_delay.tv_sec > 0 || chunk_delay.tv_nsec > 0))
                nanosleep(&chunk_delay, NULL);
        }

        g_total_bytes += bytes_read;

        if (max_bytes > 0 && g_total_bytes >= (unsigned long long)max_bytes) {
            fprintf(stderr, "zpipe: max-bytes limit after %llu bytes\n", g_total_bytes);
            write_progress();
            free(buffer);
            return IOMON_STATUS_MAXBYTES;
        }

        maybe_update_counter();
    }

    write_progress();
    free(buffer);
    return 0;
}
