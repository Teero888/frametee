#include "logger.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

// ANSI color codes for console output
#define RESET "\x1B[0m"
#define GRAY "\x1B[90m"
#define YELLOW "\x1B[33m"
#define RED "\x1B[31m"
#define BLUE "\x1B[34m"

static const char *level_strings[] = {"INFO", "WARN", "ERROR"};
static const char *level_colors[] = {BLUE, YELLOW, RED};
static bool g_logger_quiet = false;
// (each thread's own: an operation is told what it logged itself, not what workers did meanwhile)
// (C99: the compilers' own keywords)
#if defined(_MSC_VER) && !defined(__clang__)
#define LOGGER_THREAD_LOCAL __declspec(thread)
#else
#define LOGGER_THREAD_LOCAL __thread
#endif
static LOGGER_THREAD_LOCAL char g_first_error[512];
static LOGGER_THREAD_LOCAL char g_warnings[2048];

void logger_init(void) {
#ifdef _WIN32
  // enable virtual terminal processing for ANSI color codes on Windows
  HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
  if (hOut != INVALID_HANDLE_VALUE) {
    DWORD dwMode = 0;
    if (GetConsoleMode(hOut, &dwMode)) {
      dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
      SetConsoleMode(hOut, dwMode);
    }
  }
#endif
}

void logger_set_quiet(bool quiet) {
  g_logger_quiet = quiet;
}

const char *logger_first_error(void) { return g_first_error; }

const char *logger_warnings(void) { return g_warnings; }

void logger_clear_first_error(void) {
  g_first_error[0] = '\0';
  g_warnings[0] = '\0';
}

void logger_log(log_level_t level, const char *source, const char *format, ...) {
  if (level == LOG_LEVEL_ERROR && !g_first_error[0]) {
    va_list args;
    va_start(args, format);
    vsnprintf(g_first_error, sizeof(g_first_error), format, args);
    va_end(args);
  } else if (level == LOG_LEVEL_WARNING) {
    size_t used = strlen(g_warnings);
    if (used && used + 1 < sizeof(g_warnings)) g_warnings[used++] = '\n', g_warnings[used] = '\0';
    va_list args;
    va_start(args, format);
    if (used < sizeof(g_warnings)) vsnprintf(g_warnings + used, sizeof(g_warnings) - used, format, args);
    va_end(args);
  }
  if (g_logger_quiet && level < LOG_LEVEL_ERROR) return;

  time_t timer;
  char time_buffer[26];
  struct tm *tm_info;

  time(&timer);
  tm_info = localtime(&timer);
  strftime(time_buffer, 26, "%H:%M:%S", tm_info);

  char source_with_brackets[32];
  snprintf(source_with_brackets, sizeof(source_with_brackets), "[%s]", source);

  fprintf(stdout, "%s[%s] %s[%-4s]%s %-17s ", GRAY, time_buffer, level_colors[level], level_strings[level], RESET, source_with_brackets);

  va_list args;
  va_start(args, format);
  vfprintf(stdout, format, args);
  va_end(args);

  fprintf(stdout, "\n");
  fflush(stdout);
}
