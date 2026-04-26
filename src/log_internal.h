/*
 * Internal logging shim. Backends call ww_log() to route messages
 * through the user-installed waywallen_display_set_log_callback().
 */

#ifndef WW_LOG_INTERNAL_H
#define WW_LOG_INTERNAL_H

#include "waywallen_display.h"

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((format(printf, 2, 3), visibility("hidden")))
void ww_log(waywallen_log_level_t level, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* WW_LOG_INTERNAL_H */
