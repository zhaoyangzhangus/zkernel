#pragma once

#include <pthread.h>
#include <time.h>

/* C11 线程接口是原生 pthread 原语的一层轻量源码兼容封装。底层对象仍采用原生表示形式。 */
typedef pthread_t thrd_t;
typedef pthread_mutex_t mtx_t;
typedef pthread_cond_t cnd_t;
typedef pthread_once_t once_flag;
typedef pthread_key_t tss_t;
typedef int (*thrd_start_t)(void *argument);
typedef void (*tss_dtor_t)(void *value);

#define thrd_success 0
#define thrd_busy 1
#define thrd_error 2
#define thrd_nomem 3
#define thrd_timedout 4

#define mtx_plain 0
#define mtx_recursive 1
#define mtx_timed 2
#define ONCE_FLAG_INIT PTHREAD_ONCE_INIT

#ifndef thread_local
#define thread_local _Thread_local
#endif

int thrd_create(thrd_t *thread, thrd_start_t function, void *argument);
int thrd_equal(thrd_t left, thrd_t right);
thrd_t thrd_current(void);
int thrd_sleep(const struct timespec *duration, struct timespec *remaining);
void thrd_yield(void);
_Noreturn void thrd_exit(int result);
int thrd_join(thrd_t thread, int *result);
int thrd_detach(thrd_t thread);

int mtx_init(mtx_t *mutex, int type);
void mtx_destroy(mtx_t *mutex);
int mtx_lock(mtx_t *mutex);
int mtx_timedlock(mtx_t *mutex, const struct timespec *time_point);
int mtx_trylock(mtx_t *mutex);
int mtx_unlock(mtx_t *mutex);

int cnd_init(cnd_t *condition);
void cnd_destroy(cnd_t *condition);
int cnd_wait(cnd_t *condition, mtx_t *mutex);
int cnd_timedwait(cnd_t *condition, mtx_t *mutex,
                  const struct timespec *time_point);
int cnd_signal(cnd_t *condition);
int cnd_broadcast(cnd_t *condition);

void call_once(once_flag *flag, void (*function)(void));

int tss_create(tss_t *key, tss_dtor_t destructor);
void tss_delete(tss_t key);
int tss_set(tss_t key, void *value);
void *tss_get(tss_t key);
