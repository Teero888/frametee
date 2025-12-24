#ifndef LOGGER_H
#define LOGGER_H

#include <stdarg.h>
#include <stdbool.h>
#include <types.h>

typedef enum { LOG_LEVEL_INFO,
               LOG_LEVEL_WARNING,
               LOG_LEVEL_ERROR } log_level_t;

void logger_init(void);

void logger_log(log_level_t level, const char *source, const char *format, ...);

#define log_info(source, ...) logger_log(LOG_LEVEL_INFO, source, __VA_ARGS__)
#define log_warn(source, ...) logger_log(LOG_LEVEL_WARNING, source, __VA_ARGS__)
#define log_error(source, ...) logger_log(LOG_LEVEL_ERROR, source, __VA_ARGS__)

#endif // LOGGER_H
