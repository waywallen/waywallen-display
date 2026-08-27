#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "log_file.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char     lines[WW_LOG_FILE_RING_CAPACITY][WW_LOG_FILE_LINE_MAX];
    size_t   lengths[WW_LOG_FILE_RING_CAPACITY];
    unsigned head;
    unsigned tail;
    unsigned count;
} ww_log_ring_t;

typedef struct {
    char   path[512];
    time_t mtime;
} ww_log_file_entry_t;

static ww_log_ring_t   s_ring;
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_cond  = PTHREAD_COND_INITIALIZER;
static pthread_t       s_flush_thread;
static bool            s_flush_thread_running;
static bool            s_shutdown_requested;
static bool            s_flush_now_requested;
static bool            s_logging_enabled;
static bool            s_log_dir_resolved;
static char            s_log_dir[512];
static int             s_current_fd = -1;
static int             s_current_year;
static int             s_current_month;
static int             s_current_mday;
static char            s_tag[64];
static bool            s_thread_started;
static bool            s_atexit_registered;
static bool            s_shutdown_done;
static bool            s_test_wait_flush;
static bool            s_stderr_fallback;
static bool            s_test_force_stderr_fallback;

static const char* level_name(waywallen_log_level_t level) {
    switch (level) {
    case WAYWALLEN_LOG_INFO: return "INFO";
    case WAYWALLEN_LOG_WARN: return "WARN";
    case WAYWALLEN_LOG_ERROR: return "ERROR";
    default: return "INFO";
    }
}

static bool level_writes_to_file(waywallen_log_level_t level) {
    return level >= WAYWALLEN_LOG_INFO && level <= WAYWALLEN_LOG_ERROR;
}

static bool resolve_log_dir(char* out, size_t out_len) {
    const char* xdg = getenv("XDG_STATE_HOME");
    if (xdg != NULL && xdg[0] != '\0') {
        const int n = snprintf(out, out_len, "%s/waywallen/logs", xdg);
        return n > 0 && (size_t)n < out_len;
    }

    const char* home = getenv("HOME");
    if (home != NULL && home[0] != '\0') {
        const int n = snprintf(out, out_len, "%s/.local/state/waywallen/logs", home);
        return n > 0 && (size_t)n < out_len;
    }

    return false;
}

static int format_log_line(waywallen_log_level_t level, const char* tag, const char* message,
                           char* out, size_t out_len) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return -1;
    }

    struct tm tm_local;
    if (localtime_r(&ts.tv_sec, &tm_local) == NULL) {
        return -1;
    }

    const long  millis = ts.tv_nsec / 1000000L;
    const char* lvl    = level_name(level);

    if (tag != NULL && tag[0] != '\0') {
        return snprintf(out,
                        out_len,
                        "%04d-%02d-%02d %02d:%02d:%02d.%03ld %s [%s] %s\n",
                        tm_local.tm_year + 1900,
                        tm_local.tm_mon + 1,
                        tm_local.tm_mday,
                        tm_local.tm_hour,
                        tm_local.tm_min,
                        tm_local.tm_sec,
                        millis,
                        lvl,
                        tag,
                        message);
    }

    return snprintf(out,
                    out_len,
                    "%04d-%02d-%02d %02d:%02d:%02d.%03ld %s %s\n",
                    tm_local.tm_year + 1900,
                    tm_local.tm_mon + 1,
                    tm_local.tm_mday,
                    tm_local.tm_hour,
                    tm_local.tm_min,
                    tm_local.tm_sec,
                    millis,
                    lvl,
                    message);
}

static void ring_enqueue(const char* line, size_t len) {
    if (len == 0 || len >= WW_LOG_FILE_LINE_MAX) {
        return;
    }

    const unsigned idx = s_ring.head;
    s_ring.head        = (s_ring.head + 1U) % WW_LOG_FILE_RING_CAPACITY;

    if (s_ring.count < WW_LOG_FILE_RING_CAPACITY) {
        s_ring.count++;
    } else {
        s_ring.tail = (s_ring.tail + 1U) % WW_LOG_FILE_RING_CAPACITY;
    }

    memcpy(s_ring.lines[idx], line, len);
    s_ring.lines[idx][len] = '\0';
    s_ring.lengths[idx]    = len;
}

static unsigned ring_drain(char batch_lines[][WW_LOG_FILE_LINE_MAX], size_t batch_lengths[],
                           unsigned batch_capacity) {
    const unsigned n = s_ring.count < batch_capacity ? s_ring.count : batch_capacity;
    for (unsigned i = 0; i < n; i++) {
        const unsigned idx = (s_ring.tail + i) % WW_LOG_FILE_RING_CAPACITY;
        memcpy(batch_lines[i], s_ring.lines[idx], s_ring.lengths[idx] + 1U);
        batch_lengths[i] = s_ring.lengths[idx];
    }

    s_ring.head  = 0;
    s_ring.tail  = 0;
    s_ring.count = 0;
    return n;
}

static bool ensure_log_dir(void) {
    if (s_log_dir_resolved) {
        return s_logging_enabled;
    }

    s_logging_enabled  = resolve_log_dir(s_log_dir, sizeof(s_log_dir));
    s_log_dir_resolved = true;
    if (! s_logging_enabled) {
        return false;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s", s_log_dir);

    for (char* cursor = path + 1; *cursor != '\0'; cursor++) {
        if (*cursor != '/') {
            continue;
        }
        *cursor = '\0';
        if (mkdir(path, 0755) != 0 && errno != EEXIST) {
            s_logging_enabled = false;
            return false;
        }
        *cursor = '/';
    }

    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        s_logging_enabled = false;
        return false;
    }

    return true;
}

static bool build_daily_path(char* out, size_t out_len, const struct tm* day) {
    const int n = snprintf(out,
                           out_len,
                           "%s/" WW_LOG_FILE_PREFIX "_r%04d-%02d-%02d.log",
                           s_log_dir,
                           day->tm_year + 1900,
                           day->tm_mon + 1,
                           day->tm_mday);
    return n > 0 && (size_t)n < out_len;
}

static bool open_daily_file(const struct tm* day) {
    char path[512];
    if (! build_daily_path(path, sizeof(path), day)) {
        return false;
    }

    const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        return false;
    }

    if (s_current_fd >= 0) {
        close(s_current_fd);
    }

    s_current_fd    = fd;
    s_current_year  = day->tm_year + 1900;
    s_current_month = day->tm_mon + 1;
    s_current_mday  = day->tm_mday;
    return true;
}

static bool ensure_daily_file(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return false;
    }

    struct tm day;
    if (localtime_r(&ts.tv_sec, &day) == NULL) {
        return false;
    }

    const int year  = day.tm_year + 1900;
    const int month = day.tm_mon + 1;
    const int mday  = day.tm_mday;

    if (s_current_fd >= 0 && year == s_current_year && month == s_current_month &&
        mday == s_current_mday) {
        return true;
    }

    return open_daily_file(&day);
}

static int compare_entries_desc(const void* a, const void* b) {
    const ww_log_file_entry_t* left  = (const ww_log_file_entry_t*)a;
    const ww_log_file_entry_t* right = (const ww_log_file_entry_t*)b;
    if (left->mtime > right->mtime) {
        return -1;
    }
    if (left->mtime < right->mtime) {
        return 1;
    }
    return 0;
}

static void run_retention_in_dir(const char* log_dir) {
    DIR* dir = opendir(log_dir);
    if (dir == NULL) {
        return;
    }

    ww_log_file_entry_t entries[128];
    size_t              count = 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, WW_LOG_FILE_PREFIX "_r", strlen(WW_LOG_FILE_PREFIX "_r")) != 0) {
            continue;
        }
        const size_t name_len = strlen(entry->d_name);
        if (name_len < strlen(".log") ||
            strcmp(entry->d_name + name_len - strlen(".log"), ".log") != 0) {
            continue;
        }
        if (count >= sizeof(entries) / sizeof(entries[0])) {
            continue;
        }

        char      full_path[512];
        const int n = snprintf(full_path, sizeof(full_path), "%s/%s", log_dir, entry->d_name);
        if (n <= 0 || (size_t)n >= sizeof(full_path)) {
            continue;
        }

        struct stat st;
        if (stat(full_path, &st) != 0) {
            continue;
        }

        memcpy(entries[count].path, full_path, (size_t)n + 1U);
        entries[count].mtime = st.st_mtime;
        count++;
    }

    closedir(dir);

    if (count <= WW_LOG_FILE_RETENTION_COUNT) {
        return;
    }

    qsort(entries, count, sizeof(entries[0]), compare_entries_desc);

    for (size_t i = (size_t)WW_LOG_FILE_RETENTION_COUNT; i < count; i++) {
        unlink(entries[i].path);
    }
}

static void write_batch(char batch_lines[][WW_LOG_FILE_LINE_MAX], size_t batch_lengths[],
                        unsigned batch_count) {
    if (batch_count == 0) {
        return;
    }

    if (! ensure_log_dir() || ! ensure_daily_file() || s_current_fd < 0) {
        return;
    }

    for (unsigned i = 0; i < batch_count; i++) {
        const size_t len = batch_lengths[i];
        if (len == 0) {
            continue;
        }
        ssize_t written = 0;
        while (written < (ssize_t)len) {
            const ssize_t rc =
                write(s_current_fd, batch_lines[i] + (size_t)written, len - (size_t)written);
            if (rc <= 0) {
                return;
            }
            written += rc;
        }
    }

    run_retention_in_dir(s_log_dir);
}

static void flush_batch(void) {
    char     batch_lines[WW_LOG_FILE_RING_CAPACITY][WW_LOG_FILE_LINE_MAX];
    size_t   batch_lengths[WW_LOG_FILE_RING_CAPACITY];
    unsigned batch_count;

    pthread_mutex_lock(&s_mutex);
    batch_count = ring_drain(batch_lines, batch_lengths, WW_LOG_FILE_RING_CAPACITY);
    pthread_mutex_unlock(&s_mutex);

    write_batch(batch_lines, batch_lengths, batch_count);
}

static void* flush_thread_main(void* arg) {
    (void)arg;

#if defined(__linux__)
    (void)pthread_setname_np(pthread_self(), "ww-log-flush");
#endif
    (void)nice(19);

    while (true) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += WW_LOG_FILE_FLUSH_INTERVAL_SEC;

        pthread_mutex_lock(&s_mutex);
        while (! s_shutdown_requested && ! s_flush_now_requested) {
            const int rc = pthread_cond_timedwait(&s_cond, &s_mutex, &deadline);
            if (rc == ETIMEDOUT) {
                break;
            }
        }

        const bool shutting_down = s_shutdown_requested;
        s_flush_now_requested    = false;
        pthread_mutex_unlock(&s_mutex);

        flush_batch();

        pthread_mutex_lock(&s_mutex);
        if (s_test_wait_flush) {
            s_test_wait_flush = false;
            pthread_cond_broadcast(&s_cond);
        }
        pthread_mutex_unlock(&s_mutex);

        if (shutting_down) {
            break;
        }
    }

    return NULL;
}

static void register_atexit(void) {
    if (! s_atexit_registered) {
        s_atexit_registered = true;
        atexit(ww_log_file_shutdown);
    }
}

static void start_flush_thread(void) {
    register_atexit();

    if (s_test_force_stderr_fallback) {
        s_stderr_fallback = true;
        return;
    }

    const int rc = pthread_create(&s_flush_thread, NULL, flush_thread_main, NULL);
    if (rc != 0) {
        s_stderr_fallback = true;
        return;
    }

    s_flush_thread_running = true;
    s_thread_started       = true;
}

void ww_log_file_set_tag(const char* tag) {
    pthread_mutex_lock(&s_mutex);
    if (tag == NULL) {
        s_tag[0] = '\0';
    } else {
        snprintf(s_tag, sizeof(s_tag), "%s", tag);
    }
    pthread_mutex_unlock(&s_mutex);
}

void ww_log_file_append_for_level(waywallen_log_level_t level, const char* message) {
    if (! level_writes_to_file(level) || message == NULL) {
        return;
    }

    if (pthread_mutex_trylock(&s_mutex) != 0) {
        return;
    }

    if (! s_thread_started && ! s_stderr_fallback) {
        start_flush_thread();
    }

    char tag_copy[sizeof(s_tag)];
    memcpy(tag_copy, s_tag, sizeof(tag_copy));

    const bool use_stderr = s_stderr_fallback;

    char      line[WW_LOG_FILE_LINE_MAX];
    const int formatted = format_log_line(level, tag_copy, message, line, sizeof(line));
    if (formatted <= 0 || (size_t)formatted >= sizeof(line)) {
        pthread_mutex_unlock(&s_mutex);
        return;
    }

    const size_t len = (size_t)formatted;

    if (use_stderr) {
        fwrite(line, 1, len, stderr);
        fflush(stderr);
        pthread_mutex_unlock(&s_mutex);
        return;
    }

    ring_enqueue(line, len);
    pthread_cond_signal(&s_cond);
    pthread_mutex_unlock(&s_mutex);
}

void ww_log_file_flush_now_for_test(void) {
    if (! s_flush_thread_running) {
        return;
    }

    pthread_mutex_lock(&s_mutex);
    s_flush_now_requested = true;
    s_test_wait_flush     = true;
    pthread_cond_signal(&s_cond);

    struct timespec wait_until;
    clock_gettime(CLOCK_REALTIME, &wait_until);
    wait_until.tv_sec += 5;

    while (s_test_wait_flush) {
        const int rc = pthread_cond_timedwait(&s_cond, &s_mutex, &wait_until);
        if (rc == ETIMEDOUT) {
            s_test_wait_flush = false;
            break;
        }
    }
    pthread_mutex_unlock(&s_mutex);
}

void ww_log_file_run_retention_for_test(const char* log_dir) {
    if (log_dir == NULL) {
        return;
    }
    run_retention_in_dir(log_dir);
}

void ww_log_file_reset_for_test(void) {
    s_shutdown_done              = false;
    s_shutdown_requested         = false;
    s_flush_thread_running       = false;
    s_flush_now_requested        = false;
    s_test_wait_flush            = false;
    s_log_dir_resolved           = false;
    s_logging_enabled            = false;
    s_log_dir[0]                 = '\0';
    s_current_fd                 = -1;
    s_current_year               = 0;
    s_current_month              = 0;
    s_current_mday               = 0;
    s_thread_started             = false;
    s_atexit_registered          = false;
    s_stderr_fallback            = false;
    s_test_force_stderr_fallback = false;
    s_tag[0]                     = '\0';
    s_ring.head                  = 0;
    s_ring.tail                  = 0;
    s_ring.count                 = 0;
}

void ww_log_file_test_lock(void) { pthread_mutex_lock(&s_mutex); }

void ww_log_file_test_unlock(void) { pthread_mutex_unlock(&s_mutex); }

void ww_log_file_test_force_stderr_fallback(bool enable) { s_test_force_stderr_fallback = enable; }

void ww_log_file_shutdown(void) {
    if (s_shutdown_done || ! s_flush_thread_running) {
        return;
    }

    s_shutdown_done = true;

    pthread_mutex_lock(&s_mutex);
    s_shutdown_requested = true;
    pthread_cond_signal(&s_cond);
    pthread_mutex_unlock(&s_mutex);

    pthread_join(s_flush_thread, NULL);
    s_flush_thread_running = false;

    flush_batch();

    if (s_current_fd >= 0) {
        fsync(s_current_fd);
        close(s_current_fd);
        s_current_fd = -1;
    }
}
