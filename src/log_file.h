#ifndef WW_LOG_FILE_H
#define WW_LOG_FILE_H

#include "waywallen_display.h"

#include <stdbool.h>
#include <stddef.h>

#define WW_LOG_FILE_PREFIX             "waywallen_display"
#define WW_LOG_FILE_RETENTION_COUNT    7
#define WW_LOG_FILE_FLUSH_INTERVAL_SEC 2
#define WW_LOG_FILE_RING_CAPACITY      256
#define WW_LOG_FILE_LINE_MAX           640

void ww_log_file_set_tag(const char* tag);

void ww_log_file_append_for_level(waywallen_log_level_t level, const char* message);

void ww_log_file_shutdown(void);

void ww_log_file_flush_now_for_test(void);

void ww_log_file_run_retention_for_test(const char* log_dir);

void ww_log_file_reset_for_test(void);

void ww_log_file_test_lock(void);

void ww_log_file_test_unlock(void);

void ww_log_file_test_force_stderr_fallback(bool enable);

#endif
