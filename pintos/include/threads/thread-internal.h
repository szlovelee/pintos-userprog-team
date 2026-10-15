/* thread.c 내부 함수 중 timer.c에서만 쓰는 함수의 선언.
   thread.c와 devices/timer.c만 include한다. */

#ifndef THREADS_THREAD_INTERNAL_H
#define THREADS_THREAD_INTERNAL_H

#include <stdint.h>

/* timer_sleep()에서만 호출한다.
   인터럽트가 켜진 스레드 문맥에서 불려야 한다. */
void thread_sleep (int64_t wakeup_tick);

/* timer_interrupt()에서만 호출한다.
   인터럽트 핸들러 안에서 불려야 한다. */
void thread_wakeup (int64_t current_tick);

#endif 
/* threads/thread-internal.h */