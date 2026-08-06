#ifndef LOGGER_H
#define LOGGER_H

#include <stdarg.h>
#include <stdbool.h>
#include <types.h>

#ifndef LOGGER_API
#  ifdef _WIN32
#    if defined(BUILDING_LOGGER) || defined(frametee_EXPORTS)
#      define LOGGER_API __declspec(dllexport)
#    else
#      define LOGGER_API __declspec(dllimport)
#    endif
#  else
#    define LOGGER_API extern
#  endif
#endif

typedef enum { LOG_LEVEL_INFO,
               LOG_LEVEL_WARNING,
               LOG_LEVEL_ERROR } log_level_t;
#ifdef __cplusplus
extern "C" {
#endif
LOGGER_API void logger_init(void);
LOGGER_API void logger_log(log_level_t level, const char *source, const char *format, ...);
#ifdef __cplusplus
}
#endif
#define log_info(source, ...) logger_log(LOG_LEVEL_INFO, source, __VA_ARGS__)
#define log_warn(source, ...) logger_log(LOG_LEVEL_WARNING, source, __VA_ARGS__)
#define log_error(source, ...) logger_log(LOG_LEVEL_ERROR, source, __VA_ARGS__)

#endif // LOGGER_H
