#ifndef LOGGER_H
#define LOGGER_H

#include <stdarg.h>
#include <stdbool.h>
#include <types.h>

#ifndef LOGGER_API
#ifdef _WIN32
#if defined(BUILDING_LOGGER) || defined(frametee_EXPORTS)
#define LOGGER_API __declspec(dllexport)
#else
#define LOGGER_API __declspec(dllimport)
#endif
#else
#define LOGGER_API extern
#endif
#endif

typedef enum { LOG_LEVEL_INFO,
               LOG_LEVEL_WARNING,
               LOG_LEVEL_ERROR } log_level_t;
#ifdef __cplusplus
extern "C" {
#endif
LOGGER_API void logger_init(void);
LOGGER_API void logger_set_quiet(bool quiet);
LOGGER_API void logger_log(log_level_t level, const char *source, const char *format, ...);
// The text of the first error the calling thread logged since logger_clear_first_error, "" for none:
// what went wrong in an operation that can only fail with a log line, for showing it to the user (the
// errors after it are mostly that one again, seen from further out).
LOGGER_API const char *logger_first_error(void);
// The warnings it logged since then, a line each (as many as fit): what an operation that went
// through wants the user to know.
LOGGER_API const char *logger_warnings(void);
LOGGER_API void logger_clear_first_error(void);
#ifdef __cplusplus
}
#endif
#define log_info(source, ...) logger_log(LOG_LEVEL_INFO, source, __VA_ARGS__)
#define log_warn(source, ...) logger_log(LOG_LEVEL_WARNING, source, __VA_ARGS__)
#define log_error(source, ...) logger_log(LOG_LEVEL_ERROR, source, __VA_ARGS__)

#endif // LOGGER_H
