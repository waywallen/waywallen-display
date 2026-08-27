#define _POSIX_C_SOURCE 200809L

#include "log_file.h"

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

static char g_temp_root[256];

static void setup_temp_state_dir(void) {
    ww_log_file_reset_for_test();
    strcpy(g_temp_root, "/tmp/ww_log_test_XXXXXX");
    assert(mkdtemp(g_temp_root) != NULL);
    setenv("XDG_STATE_HOME", g_temp_root, 1);
    unsetenv("HOME");
}

static void teardown_logging(void) {
    ww_log_file_shutdown();
    ww_log_file_reset_for_test();
    unsetenv("XDG_STATE_HOME");
    unsetenv("HOME");
}

static int count_display_logs(const char* log_dir) {
    int            count = 0;
    char           path[512];
    const int      n = snprintf(path, sizeof(path), "%s", log_dir);
    DIR*           dir;
    struct dirent* entry;

    if (n <= 0 || (size_t)n >= sizeof(path)) {
        return 0;
    }

    dir = opendir(path);
    if (dir == NULL) {
        return 0;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, WW_LOG_FILE_PREFIX "_r", strlen(WW_LOG_FILE_PREFIX "_r")) == 0) {
            count++;
        }
    }

    closedir(dir);
    return count;
}

static bool read_latest_log(char* out, size_t out_len) {
    char log_dir[512];
    snprintf(log_dir, sizeof(log_dir), "%s/waywallen/logs", g_temp_root);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm day;
    localtime_r(&ts.tv_sec, &day);

    char path[512];
    snprintf(path,
             sizeof(path),
             "%s/" WW_LOG_FILE_PREFIX "_r%04d-%02d-%02d.log",
             log_dir,
             day.tm_year + 1900,
             day.tm_mon + 1,
             day.tm_mday);

    FILE* file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }

    const size_t read = fread(out, 1, out_len - 1U, file);
    out[read]         = '\0';
    fclose(file);
    return read > 0;
}

static void test_log_dir_xdg_state_home(void) {
    setup_temp_state_dir();
    ww_log_file_set_tag("test-tag");
    ww_log_file_append_for_level(WAYWALLEN_LOG_INFO, "hello from test");
    ww_log_file_flush_now_for_test();

    char contents[4096];
    assert(read_latest_log(contents, sizeof(contents)));
    assert(strstr(contents, "INFO") != NULL);
    assert(strstr(contents, "[test-tag]") != NULL);
    assert(strstr(contents, "hello from test") != NULL);
    teardown_logging();
}

static void test_log_dir_home_fallback(void) {
    setup_temp_state_dir();
    unsetenv("XDG_STATE_HOME");

    char home_path[512];
    snprintf(home_path, sizeof(home_path), "%s/home", g_temp_root);
    mkdir(home_path, 0755);
    setenv("HOME", home_path, 1);

    ww_log_file_append_for_level(WAYWALLEN_LOG_WARN, "home fallback");
    ww_log_file_flush_now_for_test();

    char            log_path[512];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm day;
    localtime_r(&ts.tv_sec, &day);
    snprintf(log_path,
             sizeof(log_path),
             "%s/.local/state/waywallen/logs/" WW_LOG_FILE_PREFIX "_r%04d-%02d-%02d.log",
             home_path,
             day.tm_year + 1900,
             day.tm_mon + 1,
             day.tm_mday);

    FILE* file = fopen(log_path, "r");
    assert(file != NULL);
    char line[256];
    assert(fgets(line, sizeof(line), file) != NULL);
    assert(strstr(line, "WARN") != NULL);
    assert(strstr(line, "home fallback") != NULL);
    fclose(file);

    teardown_logging();
}

static void test_log_level_filter(void) {
    setup_temp_state_dir();
    ww_log_file_append_for_level(WAYWALLEN_LOG_DEBUG, "debug should not appear");
    ww_log_file_flush_now_for_test();

    char contents[4096];
    if (read_latest_log(contents, sizeof(contents))) {
        assert(strstr(contents, "debug should not appear") == NULL);
    }

    teardown_logging();
}

static void test_tag_optional(void) {
    setup_temp_state_dir();
    ww_log_file_set_tag(NULL);
    ww_log_file_append_for_level(WAYWALLEN_LOG_ERROR, "untagged error");
    ww_log_file_flush_now_for_test();

    char contents[4096];
    assert(read_latest_log(contents, sizeof(contents)));
    assert(strstr(contents, "ERROR") != NULL);
    assert(strstr(contents, "untagged error") != NULL);
    assert(strstr(contents, "[") == NULL);
    teardown_logging();
}

static void test_retention_keep_seven(void) {
    char retention_root[256];
    strcpy(retention_root, "/tmp/ww_log_retention_XXXXXX");
    assert(mkdtemp(retention_root) != NULL);

    char log_dir[512];
    snprintf(log_dir, sizeof(log_dir), "%s/logs", retention_root);
    mkdir(log_dir, 0755);

    for (int day = 1; day <= 10; day++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/" WW_LOG_FILE_PREFIX "_r2026-08-%02d.log", log_dir, day);
        FILE* file = fopen(path, "w");
        assert(file != NULL);
        fprintf(file, "day %d\n", day);
        fclose(file);

        struct utimbuf times;
        times.actime  = (time_t)day;
        times.modtime = (time_t)day;
        utime(path, &times);
    }

    ww_log_file_run_retention_for_test(log_dir);
    assert(count_display_logs(log_dir) == WW_LOG_FILE_RETENTION_COUNT);
}

static void test_shutdown_flushes_buffer(void) {
    setup_temp_state_dir();
    ww_log_file_append_for_level(WAYWALLEN_LOG_INFO, "shutdown flush");
    ww_log_file_shutdown();

    char contents[4096];
    assert(read_latest_log(contents, sizeof(contents)));
    assert(strstr(contents, "shutdown flush") != NULL);

    ww_log_file_reset_for_test();
    unsetenv("XDG_STATE_HOME");
}

static void test_trylock_drop(void) {
    setup_temp_state_dir();
    ww_log_file_append_for_level(WAYWALLEN_LOG_INFO, "seed line");
    ww_log_file_flush_now_for_test();

    ww_log_file_test_lock();
    ww_log_file_append_for_level(WAYWALLEN_LOG_INFO, "dropped line");
    ww_log_file_test_unlock();

    ww_log_file_flush_now_for_test();

    char contents[4096];
    assert(read_latest_log(contents, sizeof(contents)));
    assert(strstr(contents, "seed line") != NULL);
    assert(strstr(contents, "dropped line") == NULL);
    teardown_logging();
}

static void test_ring_overflow_drops_oldest(void) {
    setup_temp_state_dir();

    for (unsigned i = 0; i < WW_LOG_FILE_RING_CAPACITY + 16U; i++) {
        char message[64];
        snprintf(message, sizeof(message), "overflow line %u", i);
        ww_log_file_append_for_level(WAYWALLEN_LOG_INFO, message);
    }

    ww_log_file_flush_now_for_test();

    char contents[256U * WW_LOG_FILE_LINE_MAX];
    assert(read_latest_log(contents, sizeof(contents)));
    assert(strstr(contents, "overflow line 0") == NULL);
    assert(strstr(contents, "overflow line 271") != NULL);
    teardown_logging();
}

static void test_stderr_fallback_on_thread_failure(void) {
    setup_temp_state_dir();
    ww_log_file_test_force_stderr_fallback(true);
    ww_log_file_set_tag("stderr-tag");

    int pipefd[2];
    assert(pipe(pipefd) == 0);

    const int saved_stderr = dup(STDERR_FILENO);
    assert(saved_stderr >= 0);
    assert(dup2(pipefd[1], STDERR_FILENO) == STDERR_FILENO);
    close(pipefd[1]);

    ww_log_file_append_for_level(WAYWALLEN_LOG_WARN, "stderr fallback message");

    assert(dup2(saved_stderr, STDERR_FILENO) == STDERR_FILENO);
    close(saved_stderr);

    char          contents[4096];
    const ssize_t n = read(pipefd[0], contents, sizeof(contents) - 1U);
    close(pipefd[0]);
    assert(n > 0);
    contents[n] = '\0';

    assert(strstr(contents, "WARN") != NULL);
    assert(strstr(contents, "[stderr-tag]") != NULL);
    assert(strstr(contents, "stderr fallback message") != NULL);

    teardown_logging();
}

int main(void) {
    test_retention_keep_seven();
    test_log_dir_xdg_state_home();
    test_log_dir_home_fallback();
    test_log_level_filter();
    test_tag_optional();
    test_shutdown_flushes_buffer();
    test_trylock_drop();
    test_ring_overflow_drops_oldest();
    test_stderr_fallback_on_thread_failure();
    return 0;
}
