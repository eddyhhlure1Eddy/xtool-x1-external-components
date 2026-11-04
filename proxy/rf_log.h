/*
 * rf_log.h - logging for rf_prepare_task / rf_restore_task implementations.
 * rf_log() is provided by proxy.c: it appends one line (timestamp, pid, tid, "hook: "
 * prefix, then the printf-formatted text) to relief_wrapper.log next to the proxy DLL.
 * Thread safe; text is written as given (use UTF-8). Lines are cut at about 4000 bytes.
 */
#ifndef RF_LOG_H
#define RF_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

void rf_log(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* RF_LOG_H */
