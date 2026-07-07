#ifndef COMPAT_THREADS_H
#define COMPAT_THREADS_H

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#include <stdlib.h>

// Types
typedef HANDLE pthread_t;
typedef CRITICAL_SECTION pthread_mutex_t;

// For pthread_once
typedef INIT_ONCE pthread_once_t;
#define PTHREAD_ONCE_INIT INIT_ONCE_STATIC_INIT

typedef void* pthread_attr_t;
typedef void* pthread_mutexattr_t;

// Helper to convert pthread start routine signature
typedef void *(*pthread_start_routine_t)(void *);

typedef struct {
    pthread_start_routine_t func;
    void *arg;
} pthread_win32_args_t;

static unsigned __stdcall pthread_win32_thread_proc(void *arg) {
    pthread_win32_args_t *args = (pthread_win32_args_t *)arg;
    pthread_start_routine_t func = args->func;
    void *func_arg = args->arg;
    free(args);
    func(func_arg);
    return 0;
}

static inline int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                  void *(*start_routine)(void *), void *arg) {
    (void)attr;
    pthread_win32_args_t *args = malloc(sizeof(pthread_win32_args_t));
    if (!args) return -1;
    args->func = start_routine;
    args->arg = arg;
    
    HANDLE hThread = (HANDLE)_beginthreadex(NULL, 0, pthread_win32_thread_proc, args, 0, NULL);
    if (hThread == NULL) {
        free(args);
        return -1;
    }
    *thread = hThread;
    return 0;
}

static inline int pthread_join(pthread_t thread, void **retval) {
    if (thread == NULL) return 0;
    WaitForSingleObject(thread, INFINITE);
    if (retval) *retval = NULL;
    CloseHandle(thread);
    return 0;
}

static inline int pthread_detach(pthread_t thread) {
    if (thread == NULL) return 0;
    CloseHandle(thread);
    return 0;
}

static inline int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
    (void)attr;
    InitializeCriticalSection(mutex);
    return 0;
}

static inline int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    DeleteCriticalSection(mutex);
    return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t *mutex) {
    EnterCriticalSection(mutex);
    return 0;
}

static inline int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    LeaveCriticalSection(mutex);
    return 0;
}

// Dummy functions for attribute configuration since CRITICAL_SECTION is always recursive on Windows
#define PTHREAD_MUTEX_RECURSIVE 0
static inline int pthread_mutexattr_init(pthread_mutexattr_t *attr) { (void)attr; return 0; }
static inline int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type) { (void)attr; (void)type; return 0; }
static inline int pthread_mutexattr_destroy(pthread_mutexattr_t *attr) { (void)attr; return 0; }

// pthread_once implementation using Windows InitOnceExecuteOnce
static inline BOOL CALLBACK pthread_win32_init_once_callback(PINIT_ONCE InitOnce, PVOID Parameter, PVOID *Context) {
    (void)InitOnce;
    (void)Context;
    void (*init_routine)(void) = (void (*)(void))Parameter;
    init_routine();
    return TRUE;
}

static inline int pthread_once(pthread_once_t *once_control, void (*init_routine)(void)) {
    InitOnceExecuteOnce(once_control, pthread_win32_init_once_callback, (PVOID)init_routine, NULL);
    return 0;
}

#else
// Non-Windows: standard pthread
#include <pthread.h>
#endif

#endif // COMPAT_THREADS_H
