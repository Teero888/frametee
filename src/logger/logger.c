#include "logger.h"
#include <stdio.h>
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

void logger_log(log_level_t level, const char *source, const char *format, ...) {
  time_t timer;
  char time_buffer[26];
  struct tm *tm_info;

  time(&timer);
  tm_info = localtime(&timer);
  strftime(time_buffer, 26, "%H:%M:%S", tm_info);

  char source_with_brackets[32];
  snprintf(source_with_brackets, sizeof(source_with_brackets), "[%s]", source);

  fprintf(stdout, "%s[%s] %s[%-4s]%s %-17s ", GRAY, time_buffer, level_colors[level], level_strings[level],
          RESET, source_with_brackets);

  va_list args;
  va_start(args, format);
  vfprintf(stdout, format, args);
  va_end(args);

  fprintf(stdout, "\n");
  fflush(stdout);
}
