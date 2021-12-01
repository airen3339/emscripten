/*
 * Copyright 2015 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 */

#define _GNU_SOURCE
#include "../internal/libc.h"
#include "../internal/pthread_impl.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <termios.h>
#include <threads.h>
#include <unistd.h>
#include <utime.h>

#include <emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/stack.h>

void __pthread_testcancel();

int emscripten_pthread_attr_gettransferredcanvases(const pthread_attr_t* a, const char** str) {
  *str = a->_a_transferredcanvases;
  return 0;
}

int emscripten_pthread_attr_settransferredcanvases(pthread_attr_t* a, const char* str) {
  a->_a_transferredcanvases = str;
  return 0;
}

int sched_get_priority_max(int policy) {
  // Web workers do not actually support prioritizing threads,
  // but mimic values that Linux apparently reports, see
  // http://man7.org/linux/man-pages/man2/sched_get_priority_min.2.html
  if (policy == SCHED_FIFO || policy == SCHED_RR)
    return 99;
  else
    return 0;
}

int sched_get_priority_min(int policy) {
  // Web workers do not actually support prioritizing threads,
  // but mimic values that Linux apparently reports, see
  // http://man7.org/linux/man-pages/man2/sched_get_priority_min.2.html
  if (policy == SCHED_FIFO || policy == SCHED_RR)
    return 1;
  else
    return 0;
}

int pthread_mutexattr_getprioceiling(const pthread_mutexattr_t *restrict attr, int *restrict prioceiling)
{
  // Not supported either in Emscripten or musl, return a faked value.
  if (prioceiling) *prioceiling = 99;
  return 0;
}

int pthread_mutexattr_setprioceiling(pthread_mutexattr_t *attr, int prioceiling)
{
  // Not supported either in Emscripten or musl, return an error.
  return EPERM;
}

static uint32_t dummyZeroAddress = 0;

void emscripten_thread_sleep(double msecs) {
  double now = emscripten_get_now();
  double target = now + msecs;

  __pthread_testcancel(); // pthreads spec: sleep is a cancellation point, so must test if this
                          // thread is cancelled during the sleep.
  emscripten_current_thread_process_queued_calls();

  // If we have less than this many msecs left to wait, busy spin that instead.
  const double minimumTimeSliceToSleep = 0.1;

  // runtime thread may need to run proxied calls, so sleep in very small slices to be responsive.
  const double maxMsecsSliceToSleep = emscripten_is_main_runtime_thread() ? 1 : 100;

  emscripten_conditional_set_current_thread_status(
    EM_THREAD_STATUS_RUNNING, EM_THREAD_STATUS_SLEEPING);
  now = emscripten_get_now();
  while (now < target) {
    // Keep processing the main loop of the calling thread.
    __pthread_testcancel(); // pthreads spec: sleep is a cancellation point, so must test if this
                            // thread is cancelled during the sleep.
    emscripten_current_thread_process_queued_calls();

    now = emscripten_get_now();
    double msecsToSleep = target - now;
    if (msecsToSleep > maxMsecsSliceToSleep)
      msecsToSleep = maxMsecsSliceToSleep;
    if (msecsToSleep >= minimumTimeSliceToSleep)
      emscripten_futex_wait(&dummyZeroAddress, 0, msecsToSleep);
    now = emscripten_get_now();
  };

  emscripten_conditional_set_current_thread_status(
    EM_THREAD_STATUS_SLEEPING, EM_THREAD_STATUS_RUNNING);
}

// Allocator and deallocator for em_queued_call objects.
static em_queued_call* em_queued_call_malloc() {
  em_queued_call* call = (em_queued_call*)malloc(sizeof(em_queued_call));
  assert(call); // Not a programming error, but use assert() in debug builds to catch OOM scenarios.
  if (call) {
    call->operationDone = 0;
    call->functionPtr = 0;
    call->satelliteData = 0;
    call->heapAllocated = 1;
  }
  return call;
}

// Collect a list of varargs into the `call` structure according to the `sig`.
// Assume arguments before `start` have already been initialized and are not
// included in `args`.
static void init_em_queued_call_args(em_queued_call* call,
                                     EM_FUNC_SIGNATURE sig,
                                     int start,
                                     va_list args) {
  int numArguments = EM_FUNC_SIG_NUM_FUNC_ARGUMENTS(sig);
  EM_FUNC_SIGNATURE argumentsType = sig & EM_FUNC_SIG_ARGUMENTS_TYPE_MASK;
  for (int i = 0; i < numArguments; ++i) {
    if (i >= start) {
      switch ((argumentsType & EM_FUNC_SIG_ARGUMENT_TYPE_SIZE_MASK)) {
        case EM_FUNC_SIG_PARAM_I:
          call->args[i].i = va_arg(args, int);
          break;
        case EM_FUNC_SIG_PARAM_I64:
          call->args[i].i64 = va_arg(args, int64_t);
          break;
        case EM_FUNC_SIG_PARAM_F:
          call->args[i].f = (float)va_arg(args, double);
          break;
        case EM_FUNC_SIG_PARAM_D:
          call->args[i].d = va_arg(args, double);
          break;
      }
    }
    argumentsType >>= EM_FUNC_SIG_ARGUMENT_TYPE_SIZE_SHIFT;
  }
}

static em_queued_call* create_em_queued_call(
  EM_FUNC_SIGNATURE sig, void* func, void* satellite, int start, va_list args) {
  em_queued_call* q = em_queued_call_malloc();
  if (!q) {
    return NULL;
  }
  q->functionEnum = sig;
  q->functionPtr = func;
  q->satelliteData = satellite;
  init_em_queued_call_args(q, sig, start, args);
  return q;
}

static void em_queued_call_free(em_queued_call* call) {
  if (call) {
    free(call->satelliteData);
    if (call->heapAllocated) {
      free(call);
    }
  }
}

void emscripten_async_waitable_close(em_queued_call* call) {
  assert(call->operationDone);
  em_queued_call_free(call);
}

extern double emscripten_receive_on_main_thread_js(int functionIndex, int numCallArgs, double* args);
extern int _emscripten_notify_thread_queue(pthread_t targetThreadId, pthread_t mainThreadId);
extern int __pthread_create_js(struct pthread *thread, const pthread_attr_t *attr, void *(*start_routine) (void *), void *arg);

static void _do_call(void* arg) {
  em_queued_call* q = (em_queued_call*)arg;
  // C function pointer
  assert(EM_FUNC_SIG_NUM_FUNC_ARGUMENTS(q->functionEnum) <= EM_QUEUED_CALL_MAX_ARGS);
  switch (q->functionEnum) {
    case EM_PROXIED_PTHREAD_CREATE:
      q->returnValue.i =
        __pthread_create_js(q->args[0].vp, q->args[1].vp, q->args[2].vp, q->args[3].vp);
      break;
    case EM_PROXIED_CREATE_CONTEXT:
      q->returnValue.i = emscripten_webgl_create_context(q->args[0].cp, q->args[1].vp);
      break;
    case EM_PROXIED_RESIZE_OFFSCREENCANVAS:
      q->returnValue.i =
        emscripten_set_canvas_element_size(q->args[0].cp, q->args[1].i, q->args[2].i);
      break;
    case EM_PROXIED_JS_FUNCTION:
      q->returnValue.d =
        emscripten_receive_on_main_thread_js((int)(size_t)q->functionPtr, q->args[0].i, &q->args[1].d);
      break;
    case EM_FUNC_SIG_V:
      ((em_func_v)q->functionPtr)();
      break;
    case EM_FUNC_SIG_VI:
      ((em_func_vi)q->functionPtr)(q->args[0].i);
      break;
    case EM_FUNC_SIG_VF:
      ((em_func_vf)q->functionPtr)(q->args[0].f);
      break;
    case EM_FUNC_SIG_VII:
      ((em_func_vii)q->functionPtr)(q->args[0].i, q->args[1].i);
      break;
    case EM_FUNC_SIG_VIF:
      ((em_func_vif)q->functionPtr)(q->args[0].i, q->args[1].f);
      break;
    case EM_FUNC_SIG_VFF:
      ((em_func_vff)q->functionPtr)(q->args[0].f, q->args[1].f);
      break;
    case EM_FUNC_SIG_VIII:
      ((em_func_viii)q->functionPtr)(q->args[0].i, q->args[1].i, q->args[2].i);
      break;
    case EM_FUNC_SIG_VIIF:
      ((em_func_viif)q->functionPtr)(q->args[0].i, q->args[1].i, q->args[2].f);
      break;
    case EM_FUNC_SIG_VIFF:
      ((em_func_viff)q->functionPtr)(q->args[0].i, q->args[1].f, q->args[2].f);
      break;
    case EM_FUNC_SIG_VFFF:
      ((em_func_vfff)q->functionPtr)(q->args[0].f, q->args[1].f, q->args[2].f);
      break;
    case EM_FUNC_SIG_VIIII:
      ((em_func_viiii)q->functionPtr)(q->args[0].i, q->args[1].i, q->args[2].i, q->args[3].i);
      break;
    case EM_FUNC_SIG_VIIFI:
      ((em_func_viifi)q->functionPtr)(q->args[0].i, q->args[1].i, q->args[2].f, q->args[3].i);
      break;
    case EM_FUNC_SIG_VIFFF:
      ((em_func_vifff)q->functionPtr)(q->args[0].i, q->args[1].f, q->args[2].f, q->args[3].f);
      break;
    case EM_FUNC_SIG_VFFFF:
      ((em_func_vffff)q->functionPtr)(q->args[0].f, q->args[1].f, q->args[2].f, q->args[3].f);
      break;
    case EM_FUNC_SIG_VIIIII:
      ((em_func_viiiii)q->functionPtr)(
        q->args[0].i, q->args[1].i, q->args[2].i, q->args[3].i, q->args[4].i);
      break;
    case EM_FUNC_SIG_VIFFFF:
      ((em_func_viffff)q->functionPtr)(
        q->args[0].i, q->args[1].f, q->args[2].f, q->args[3].f, q->args[4].f);
      break;
    case EM_FUNC_SIG_VIIIIII:
      ((em_func_viiiiii)q->functionPtr)(
        q->args[0].i, q->args[1].i, q->args[2].i, q->args[3].i, q->args[4].i, q->args[5].i);
      break;
    case EM_FUNC_SIG_VIIIIIII:
      ((em_func_viiiiiii)q->functionPtr)(q->args[0].i, q->args[1].i, q->args[2].i, q->args[3].i,
        q->args[4].i, q->args[5].i, q->args[6].i);
      break;
    case EM_FUNC_SIG_VIIIIIIII:
      ((em_func_viiiiiiii)q->functionPtr)(q->args[0].i, q->args[1].i, q->args[2].i, q->args[3].i,
        q->args[4].i, q->args[5].i, q->args[6].i, q->args[7].i);
      break;
    case EM_FUNC_SIG_VIIIIIIIII:
      ((em_func_viiiiiiiii)q->functionPtr)(q->args[0].i, q->args[1].i, q->args[2].i, q->args[3].i,
        q->args[4].i, q->args[5].i, q->args[6].i, q->args[7].i, q->args[8].i);
      break;
    case EM_FUNC_SIG_VIIIIIIIIII:
      ((em_func_viiiiiiiiii)q->functionPtr)(q->args[0].i, q->args[1].i, q->args[2].i,
        q->args[3].i, q->args[4].i, q->args[5].i, q->args[6].i, q->args[7].i, q->args[8].i,
        q->args[9].i);
      break;
    case EM_FUNC_SIG_VIIIIIIIIIII:
      ((em_func_viiiiiiiiiii)q->functionPtr)(q->args[0].i, q->args[1].i, q->args[2].i,
        q->args[3].i, q->args[4].i, q->args[5].i, q->args[6].i, q->args[7].i, q->args[8].i,
        q->args[9].i, q->args[10].i);
      break;
    case EM_FUNC_SIG_I:
      q->returnValue.i = ((em_func_i)q->functionPtr)();
      break;
    case EM_FUNC_SIG_II:
      q->returnValue.i = ((em_func_ii)q->functionPtr)(q->args[0].i);
      break;
    case EM_FUNC_SIG_III:
      q->returnValue.i = ((em_func_iii)q->functionPtr)(q->args[0].i, q->args[1].i);
      break;
    case EM_FUNC_SIG_IIII:
      q->returnValue.i = ((em_func_iiii)q->functionPtr)(q->args[0].i, q->args[1].i, q->args[2].i);
      break;
    case EM_FUNC_SIG_IIIII:
      q->returnValue.i =
        ((em_func_iiiii)q->functionPtr)(q->args[0].i, q->args[1].i, q->args[2].i, q->args[3].i);
      break;
    case EM_FUNC_SIG_IIIIII:
      q->returnValue.i = ((em_func_iiiiii)q->functionPtr)(
        q->args[0].i, q->args[1].i, q->args[2].i, q->args[3].i, q->args[4].i);
      break;
    case EM_FUNC_SIG_IIIIIII:
      q->returnValue.i = ((em_func_iiiiiii)q->functionPtr)(
        q->args[0].i, q->args[1].i, q->args[2].i, q->args[3].i, q->args[4].i, q->args[5].i);
      break;
    case EM_FUNC_SIG_IIIIIIII:
      q->returnValue.i = ((em_func_iiiiiiii)q->functionPtr)(q->args[0].i, q->args[1].i,
        q->args[2].i, q->args[3].i, q->args[4].i, q->args[5].i, q->args[6].i);
      break;
    case EM_FUNC_SIG_IIIIIIIII:
      q->returnValue.i = ((em_func_iiiiiiiii)q->functionPtr)(q->args[0].i, q->args[1].i,
        q->args[2].i, q->args[3].i, q->args[4].i, q->args[5].i, q->args[6].i, q->args[7].i);
      break;
    case EM_FUNC_SIG_IIIIIIIIII:
      q->returnValue.i =
        ((em_func_iiiiiiiiii)q->functionPtr)(q->args[0].i, q->args[1].i, q->args[2].i,
          q->args[3].i, q->args[4].i, q->args[5].i, q->args[6].i, q->args[7].i, q->args[8].i);
      break;
    default:
      assert(0 && "Invalid Emscripten pthread _do_call opcode!");
  }
}

static void do_call_and_free_queued_call(void* arg) {
  em_queued_call* q = (em_queued_call*)arg;
  _do_call(q);
  em_queued_call_free(q);
}

#define CALL_QUEUE_SIZE 128

// Shared data synchronized by call_queue_lock.
typedef struct CallQueueEntry {
  void (*func)(void*);
  void* arg;
} CallQueueEntry;

typedef struct CallQueue {
  void* target_thread;
  CallQueueEntry* call_queue;
  int call_queue_head;
  int call_queue_tail;
  struct CallQueue* next;
} CallQueue;

// Currently global to the queue, but this can be improved to be per-queue specific. (TODO: with
// lockfree list operations on callQueue_head, or removing the list by moving this data to
// pthread_t)
static pthread_mutex_t call_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static CallQueue* callQueue_head = 0;

// Not thread safe, call while having call_queue_lock obtained.
static CallQueue* GetQueue(void* target) {
  assert(target);
  CallQueue* q = callQueue_head;
  while (q && q->target_thread != target)
    q = q->next;
  return q;
}

// Not thread safe, call while having call_queue_lock obtained.
static CallQueue* GetOrAllocateQueue(void* target) {
  CallQueue* q = GetQueue(target);
  if (q)
    return q;

  q = (CallQueue*)malloc(sizeof(CallQueue));
  q->target_thread = target;
  q->call_queue = 0;
  q->call_queue_head = 0;
  q->call_queue_tail = 0;
  q->next = 0;
  if (callQueue_head) {
    CallQueue* last = callQueue_head;
    while (last->next)
      last = last->next;
    last->next = q;
  } else {
    callQueue_head = q;
  }
  return q;
}

EMSCRIPTEN_RESULT emscripten_wait_for_call_v(em_queued_call* call, double timeoutMSecs) {
  // TODO: deprecate this and prefer using
  // `emscripten_dispatch_to_thread_async_as_sync` or adding a new `waitable`
  // variant of `emscripten_dispatch_to_thread`.
  int r;
  int done = atomic_load(&call->operationDone);
  if (!done) {
    double now = emscripten_get_now();
    double waitEndTime = now + timeoutMSecs;
    emscripten_set_current_thread_status(EM_THREAD_STATUS_WAITPROXY);
    while (!done && now < waitEndTime) {
      r = emscripten_futex_wait(&call->operationDone, 0, waitEndTime - now);
      done = atomic_load(&call->operationDone);
      now = emscripten_get_now();
    }
    emscripten_set_current_thread_status(EM_THREAD_STATUS_RUNNING);
  }
  if (done)
    return EMSCRIPTEN_RESULT_SUCCESS;
  else
    return EMSCRIPTEN_RESULT_TIMED_OUT;
}

EMSCRIPTEN_RESULT emscripten_wait_for_call_i(
  em_queued_call* call, double timeoutMSecs, int* outResult) {
  EMSCRIPTEN_RESULT res = emscripten_wait_for_call_v(call, timeoutMSecs);
  if (res == EMSCRIPTEN_RESULT_SUCCESS && outResult)
    *outResult = call->returnValue.i;
  return res;
}

static struct pthread __main_pthread;

pthread_t emscripten_main_browser_thread_id(void) { return &__main_pthread; }

void emscripten_async_run_in_main_thread(em_queued_call* call) {
  emscripten_dispatch_to_thread_ptr(
    emscripten_main_browser_thread_id(), do_call_and_free_queued_call, call);
}

void emscripten_sync_run_in_main_thread(em_queued_call* call) {
  emscripten_dispatch_to_thread_sync_ptr(
    emscripten_main_browser_thread_id(), do_call_and_free_queued_call, call);
}

void* emscripten_sync_run_in_main_thread_0(int function) {
  em_queued_call q = {function};
  q.returnValue.vp = 0;
  emscripten_sync_run_in_main_thread(&q);
  return q.returnValue.vp;
}

void* emscripten_sync_run_in_main_thread_1(int function, void* arg1) {
  em_queued_call q = {function};
  q.args[0].vp = arg1;
  q.returnValue.vp = 0;
  emscripten_sync_run_in_main_thread(&q);
  return q.returnValue.vp;
}

void* emscripten_sync_run_in_main_thread_2(
  int function, void* arg1, void* arg2) {
  em_queued_call q = {function};
  q.args[0].vp = arg1;
  q.args[1].vp = arg2;
  q.returnValue.vp = 0;
  emscripten_sync_run_in_main_thread(&q);
  return q.returnValue.vp;
}

void* emscripten_sync_run_in_main_thread_3(
  int function, void* arg1, void* arg2, void* arg3) {
  em_queued_call q = {function};
  q.args[0].vp = arg1;
  q.args[1].vp = arg2;
  q.args[2].vp = arg3;
  q.returnValue.vp = 0;
  emscripten_sync_run_in_main_thread(&q);
  return q.returnValue.vp;
}

void* emscripten_sync_run_in_main_thread_4(
  int function, void* arg1, void* arg2, void* arg3, void* arg4) {
  em_queued_call q = {function};
  q.args[0].vp = arg1;
  q.args[1].vp = arg2;
  q.args[2].vp = arg3;
  q.args[3].vp = arg4;
  q.returnValue.vp = 0;
  emscripten_sync_run_in_main_thread(&q);
  return q.returnValue.vp;
}

void* emscripten_sync_run_in_main_thread_5(
  int function, void* arg1, void* arg2, void* arg3, void* arg4, void* arg5) {
  em_queued_call q = {function};
  q.args[0].vp = arg1;
  q.args[1].vp = arg2;
  q.args[2].vp = arg3;
  q.args[3].vp = arg4;
  q.args[4].vp = arg5;
  q.returnValue.vp = 0;
  emscripten_sync_run_in_main_thread(&q);
  return q.returnValue.vp;
}

void* emscripten_sync_run_in_main_thread_6(
  int function, void* arg1, void* arg2, void* arg3, void* arg4, void* arg5, void* arg6) {
  em_queued_call q = {function};
  q.args[0].vp = arg1;
  q.args[1].vp = arg2;
  q.args[2].vp = arg3;
  q.args[3].vp = arg4;
  q.args[4].vp = arg5;
  q.args[5].vp = arg6;
  q.returnValue.vp = 0;
  emscripten_sync_run_in_main_thread(&q);
  return q.returnValue.vp;
}

void* emscripten_sync_run_in_main_thread_7(int function, void* arg1,
  void* arg2, void* arg3, void* arg4, void* arg5, void* arg6, void* arg7) {
  em_queued_call q = {function};
  q.args[0].vp = arg1;
  q.args[1].vp = arg2;
  q.args[2].vp = arg3;
  q.args[3].vp = arg4;
  q.args[4].vp = arg5;
  q.args[5].vp = arg6;
  q.args[6].vp = arg7;
  q.returnValue.vp = 0;
  emscripten_sync_run_in_main_thread(&q);
  return q.returnValue.vp;
}

void emscripten_current_thread_process_queued_calls() {
  // #if PTHREADS_DEBUG == 2
  //  EM_ASM(console.error('thread ' + _pthread_self() + ':
  //emscripten_current_thread_process_queued_calls(), ' + new Error().stack));
  // #endif

  static thread_local bool thread_is_processing_queued_calls = false;

  // It is possible that when processing a queued call, the control flow leads back to calling this
  // function in a nested fashion! Therefore this scenario must explicitly be detected, and
  // processing the queue must be avoided if we are nesting, or otherwise the same queued calls
  // would be processed again and again.
  if (thread_is_processing_queued_calls)
    return;
  // This must be before pthread_mutex_lock(), since pthread_mutex_lock() can call back to this
  // function.
  thread_is_processing_queued_calls = true;

  pthread_mutex_lock(&call_queue_lock);
  CallQueue* q = GetQueue(pthread_self());
  if (!q) {
    pthread_mutex_unlock(&call_queue_lock);
    thread_is_processing_queued_calls = false;
    return;
  }

  int head = q->call_queue_head;
  int tail = q->call_queue_tail;
  while (head != tail) {
    // Assume that the call is heavy, so unlock access to the call queue while it is being
    // performed.
    pthread_mutex_unlock(&call_queue_lock);
    q->call_queue[head].func(q->call_queue[head].arg);
    pthread_mutex_lock(&call_queue_lock);

    head = (head + 1) % CALL_QUEUE_SIZE;
    q->call_queue_head = head;
    tail = q->call_queue_tail;
  }
  pthread_mutex_unlock(&call_queue_lock);

  // If the queue was full and we had waiters pending to get to put data to queue, wake them up.
  emscripten_futex_wake((void*)&q->call_queue_head, 0x7FFFFFFF);

  thread_is_processing_queued_calls = false;
}

// At times when we disallow the main thread to process queued calls, this will
// be set to 0.
int _emscripten_allow_main_runtime_queued_calls = 1;

void emscripten_main_thread_process_queued_calls() {
  assert(emscripten_is_main_runtime_thread());
  if (!_emscripten_allow_main_runtime_queued_calls)
    return;

  emscripten_current_thread_process_queued_calls();
}

int emscripten_sync_run_in_main_runtime_thread_(EM_FUNC_SIGNATURE sig, void* func_ptr, ...) {
  em_queued_call q = {sig, func_ptr};
  va_list args;
  va_start(args, func_ptr);
  init_em_queued_call_args(&q, sig, 0, args);
  va_end(args);
  emscripten_sync_run_in_main_thread(&q);
  return q.returnValue.i;
}

double emscripten_run_in_main_runtime_thread_js(int index, int num_args, int64_t* buffer, int sync) {
  em_queued_call q = {};
  em_queued_call *c;
  if (sync) {
    q.operationDone = 0;
    q.satelliteData = 0;
    c = &q;
  } else {
    c = em_queued_call_malloc();
  }
  c->functionEnum = EM_PROXIED_JS_FUNCTION;
  // Index not needed to ever be more than 32-bit.
  c->functionPtr = (void*)(size_t)index;
  assert(num_args+1 <= EM_QUEUED_JS_CALL_MAX_ARGS);
  // The types are only known at runtime in these calls, so we store values that
  // must be able to contain any valid JS value, including a 64-bit BigInt if
  // BigInt support is enabled. We store to an i64, which can contain both a
  // BigInt and a JS Number which is a 64-bit double.
  c->args[0].i = num_args;
  for (int i = 0; i < num_args; i++) {
    c->args[i+1].i64 = buffer[i];
  }

  if (sync) {
    emscripten_sync_run_in_main_thread(&q);
    // TODO: support BigInt return values somehow.
    return q.returnValue.d;
  } else {
    // 'async' runs are fire and forget, where the caller detaches itself from the call object after
    // returning here, and it is the callee's responsibility to free up the memory after the call
    // has been performed.
    emscripten_async_run_in_main_thread(c);
    return 0;
  }
}

void emscripten_async_run_in_main_runtime_thread_(EM_FUNC_SIGNATURE sig, void* func_ptr, ...) {
  va_list args;
  va_start(args, func_ptr);
  emscripten_dispatch_to_thread_args(
    emscripten_main_browser_thread_id(), sig, func_ptr, NULL, args);
  va_end(args);
}

em_queued_call* emscripten_async_waitable_run_in_main_runtime_thread_(
  EM_FUNC_SIGNATURE sig, void* func_ptr, ...) {
  int numArguments = EM_FUNC_SIG_NUM_FUNC_ARGUMENTS(sig);
  em_queued_call* q = em_queued_call_malloc();
  if (!q)
    return NULL;
  q->functionEnum = sig;
  q->functionPtr = func_ptr;
  va_list args;
  va_start(args, func_ptr);
  init_em_queued_call_args(q, sig, 0, args);
  va_end(args);
  // 'async waitable' runs are waited on by the caller, so the call object needs
  // to remain alive for the caller to access it after the operation is done.
  if (!emscripten_dispatch_to_thread_ptr(
        emscripten_main_browser_thread_id(), _do_call, q)) {
    em_queued_call_free(q);
    return NULL;
  }
  return q;
}

// Return 1 if the call was successfully dispatched (or executed if already on
// the target thread) and 0 otherwise.
int emscripten_dispatch_to_thread_ptr(pthread_t target_thread,
                                      void (*func)(void*),
                                      void* arg) {
  // Can't be a null pointer here, and can't be
  // EM_CALLBACK_THREAD_CONTEXT_MAIN_BROWSER_THREAD either.
  assert(target_thread);
  if (target_thread == EM_CALLBACK_THREAD_CONTEXT_MAIN_BROWSER_THREAD) {
    target_thread = emscripten_main_browser_thread_id();
  }

  // If we are the target recipient of this message, we can just call the
  // operation directly.
  if (target_thread == EM_CALLBACK_THREAD_CONTEXT_CALLING_THREAD ||
      target_thread == pthread_self()) {
    func(arg);
    return 1;
  }

  // Add the operation to the call queue of the target thread.
  pthread_mutex_lock(&call_queue_lock);
  CallQueue* q = GetOrAllocateQueue(target_thread);
  if (!q->call_queue) {
    // Shared data synchronized by call_queue_lock.
    q->call_queue = malloc(sizeof(CallQueueEntry) * CALL_QUEUE_SIZE);
  }

  int head = q->call_queue_head;
  int tail = q->call_queue_tail;
  int new_tail = (tail + 1) % CALL_QUEUE_SIZE;

  // Check whether the queue is full.
  if (new_tail == head) {
    if (target_thread == emscripten_main_browser_thread_id()) {
      // If queue of the main browser thread is full, then we wait. Never drop
      // messages for the main browser thread.
      while (new_tail == head) {
        pthread_mutex_unlock(&call_queue_lock);
        emscripten_futex_wait((void*)&q->call_queue_head, head, INFINITY);
        pthread_mutex_lock(&call_queue_lock);
        head = q->call_queue_head;
        tail = q->call_queue_tail;
        new_tail = (tail + 1) % CALL_QUEUE_SIZE;
      }
    } else {
      // We're not targeting the main thread, so we're ok dropping the message.
      return 0;
    }
  }

  // Push the new data onto the queue.
  q->call_queue[tail].func = func;
  q->call_queue[tail].arg = arg;
  q->call_queue_tail = new_tail;

  pthread_mutex_unlock(&call_queue_lock);

  // If the call queue was empty, notify the thread to start processing queued
  // commands in case it was idle in its event loop. Otherwise, the thread must
  // already have been notified by whoever previously inserted the first
  // element.
  if (head == tail) {
    return _emscripten_notify_thread_queue(target_thread,
                                           emscripten_main_browser_thread_id());
  } else {
    return 1;
  }
}

int emscripten_dispatch_to_thread_args(pthread_t target_thread,
                                       EM_FUNC_SIGNATURE sig,
                                       void* func,
                                       void* satellite,
                                       va_list args) {
  em_queued_call* q;
  if ((q = create_em_queued_call(sig, func, satellite, 0, args))) {
    return emscripten_dispatch_to_thread_ptr(
      target_thread, do_call_and_free_queued_call, q);
  }
  return 0;
}

int emscripten_dispatch_to_thread_(pthread_t target_thread,
                                   EM_FUNC_SIGNATURE sig,
                                   void* func,
                                   void* satellite,
                                   ...) {
  va_list args;
  va_start(args, satellite);
  int ret = emscripten_dispatch_to_thread_args(
    target_thread, sig, func, satellite, args);
  va_end(args);
  return ret;
}

int emscripten_dispatch_to_thread_async_ptr(pthread_t target_thread,
                                            void (*func)(void*),
                                            void* arg) {
  // If already on the target thread, schedule an asynchronous execution.
  // Otherwise dispatch as normal.
  if (pthread_equal(target_thread, pthread_self())) {
    emscripten_async_call(func, arg, 0);
    return 1;
  } else {
    return emscripten_dispatch_to_thread_ptr(target_thread, func, arg);
  }
}

int emscripten_dispatch_to_thread_async_args(pthread_t target_thread,
                                             EM_FUNC_SIGNATURE sig,
                                             void* func,
                                             void* satellite,
                                             va_list args) {
  em_queued_call* q;
  if ((q = create_em_queued_call(sig, func, satellite, 0, args))) {
    return emscripten_dispatch_to_thread_async_ptr(
      target_thread, do_call_and_free_queued_call, q);
  }
  return 0;
}

int emscripten_dispatch_to_thread_async_(pthread_t target_thread,
                                         EM_FUNC_SIGNATURE sig,
                                         void* func,
                                         void* satellite,
                                         ...) {
  va_list args;
  va_start(args, satellite);
  int ret = emscripten_dispatch_to_thread_async_args(
    target_thread, sig, func, satellite, args);
  va_end(args);
  return ret;
}

typedef struct em_sync_ctx {
  void (*func)(void*);
  void* arg;
} em_sync_ctx;

// Helper for performing the user-provided function then synchronously calling
// `emscripten_async_as_sync_ptr_finish`. This lets us reuse the waiting logic
// from `emscripten_dispatch_to_thread_async_as_sync` without unnecessarily
// exposing the `em_async_as_sync_ctx` to the user code.
static void do_sync_call(em_async_as_sync_ctx* ctx, void* arg) {
  em_sync_ctx* sync = (em_sync_ctx*)arg;
  sync->func(sync->arg);
  emscripten_async_as_sync_ptr_finish(ctx);
}

// Dispatch `func` to `target_thread` and wait until it has finished executing.
// Return 1 if the work was completed or 0 if it was not successfully
// dispatched.
int emscripten_dispatch_to_thread_sync_ptr(pthread_t target_thread,
                                           void (*func)(void*),
                                           void* arg) {
  em_sync_ctx ctx = {func, arg};
  return emscripten_dispatch_to_thread_async_as_sync_ptr(
    target_thread, do_sync_call, &ctx);
}

int emscripten_dispatch_to_thread_sync_args(pthread_t target_thread,
                                            EM_FUNC_SIGNATURE sig,
                                            void* func,
                                            void* satellite,
                                            va_list args) {
  em_queued_call* q;
  if ((q = create_em_queued_call(sig, func, satellite, 0, args))) {
    return emscripten_dispatch_to_thread_sync_ptr(
      target_thread, do_call_and_free_queued_call, q);
  }
  return 0;
}

int emscripten_dispatch_to_thread_sync_(pthread_t target_thread,
                                        EM_FUNC_SIGNATURE sig,
                                        void* func,
                                        void* satellite,
                                        ...) {
  va_list args;
  va_start(args, satellite);
  int ret = emscripten_dispatch_to_thread_sync_args(
    target_thread, sig, func, satellite, args);
  va_end(args);
  return ret;
}

struct em_async_as_sync_ctx {
  // The function being dispatched and its argument.
  void (*func)(struct em_async_as_sync_ctx*, void*);
  void* arg;
  // Allow the dispatching thread to wait for the work to be finished.
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  // Set to 1 when the work is finished.
  int done;
};

// Helper for exposing the `em_async_as_sync_ctx` to the user-provided async
// work function.
static void do_async_as_sync_call(void* arg) {
  em_async_as_sync_ctx* ctx = (em_async_as_sync_ctx*)arg;
  ctx->func(ctx, ctx->arg);
}

// Dispatch `func` to `target_thread` and wait until
// `emscripten_async_as_sync_ptr_finish` is called on the
// `em_async_as_sync_ctx*` passed to `func`, possibly at some point after `func`
// returns. Return 1 if the work was completed or 0 if it was not successfully
// dispatched.
int emscripten_dispatch_to_thread_async_as_sync_ptr(
  pthread_t target_thread,
  void (*func)(em_async_as_sync_ctx*, void*),
  void* arg) {
  // Initialize the context that will be used to wait for the result of the work
  // on the original thread.
  em_async_as_sync_ctx ctx;
  ctx.func = func;
  ctx.arg = arg;
  pthread_mutex_init(&ctx.mutex, NULL);
  pthread_cond_init(&ctx.cond, NULL);
  ctx.done = 0;

  // Schedule `func` to run on the target thread.
  int dispatched = emscripten_dispatch_to_thread_ptr(
    target_thread, do_async_as_sync_call, &ctx);

  if (!dispatched) {
    pthread_mutex_destroy(&ctx.mutex);
    pthread_cond_destroy(&ctx.cond);
    return 0;
  }

  // Wait for the work to be marked done by
  // `emscripten_async_as_sync_ptr_finish`.
  pthread_mutex_lock(&ctx.mutex);
  while (!ctx.done) {
    // A thread cannot both perform asynchronous work and synchronously wait for
    // that work to be finished. If we were proxying to the current thread, the
    // work must have been synchronous and should already be done.
    assert(!pthread_equal(target_thread, pthread_self()));
    pthread_cond_wait(&ctx.cond, &ctx.mutex);
  }
  pthread_mutex_unlock(&ctx.mutex);

  // The work has been finished. Clean up and return.
  pthread_mutex_destroy(&ctx.mutex);
  pthread_cond_destroy(&ctx.cond);
  return 1;
}

// Helper for injecting a `em_async_as_sync_ctx` argument into an
// `em_queued_call` and calling it.
static void do_set_ctx_and_call(em_async_as_sync_ctx* ctx, void* arg) {
  em_queued_call* q = (em_queued_call*)arg;
  // Set the first argument to be the `ctx` pointer.
#ifdef __wasm32__
  q->args[0].i = (int)ctx;
#else
#ifdef __wasm64__
  q->args[0].i64 = (int64_t)ctx;
#else
#error "expected either __wasm32__ or __wasm64__"
#endif
#endif

  // `q` is only used to kick off the async work, but its satellite data might
  // need to live for the entirety of the async work, so we need to defer
  // freeing `q` until after the async work has been completed.
  _do_call(q);
}

int emscripten_dispatch_to_thread_async_as_sync_args(pthread_t target_thread,
                                                     EM_FUNC_SIGNATURE sig,
                                                     void* func,
                                                     void* satellite,
                                                     va_list args) {
  // Leave argument 0 uninitialized; it will later be filled in with the pointer
  // to the `em_async_as_sync_ctx`.
  em_queued_call* q;
  if ((q = create_em_queued_call(sig, func, satellite, 1, args))) {
    return emscripten_dispatch_to_thread_async_as_sync_ptr(
      target_thread, do_set_ctx_and_call, q);
  }
  return 0;
}

int emscripten_dispatch_to_thread_async_as_sync_(pthread_t target_thread,
                                                 EM_FUNC_SIGNATURE sig,
                                                 void* func,
                                                 void* satellite,
                                                 ...) {
  va_list args;
  va_start(args, satellite);
  int ret = emscripten_dispatch_to_thread_async_as_sync_args(
    target_thread, sig, func, satellite, args);
  va_end(args);
  return ret;
}

void emscripten_async_as_sync_ptr_finish(em_async_as_sync_ctx* ctx) {
  // Mark this work as done and wake up the invoking thread.
  pthread_mutex_lock(&ctx->mutex);
  ctx->done = 1;
  pthread_mutex_unlock(&ctx->mutex);
  pthread_cond_signal(&ctx->cond);
}

void emscripten_async_as_sync_finish(em_async_as_sync_ctx* ctx) {
  em_queued_call_free((em_queued_call*)ctx->arg);
  emscripten_async_as_sync_ptr_finish(ctx);
}

// Stores the memory address that the main thread is waiting on, if any. If
// the main thread is waiting, we wake it up before waking up any workers.
EMSCRIPTEN_KEEPALIVE void* _emscripten_main_thread_futex;

void __emscripten_init_main_thread_js(void* tb);

static void *dummy_tsd[1] = { 0 };
weak_alias(dummy_tsd, __pthread_tsd_main);

// See system/lib/README.md for static constructor ordering.
__attribute__((constructor(48)))
void __emscripten_init_main_thread(void) {
  __emscripten_init_main_thread_js(&__main_pthread);

  // The pthread struct has a field that points to itself - this is used as
  // a magic ID to detect whether the pthread_t structure is 'alive'.
  __main_pthread.self = &__main_pthread;
  __main_pthread.stack = (void*)emscripten_stack_get_base();
  __main_pthread.stack_size = emscripten_stack_get_base() - emscripten_stack_get_end();
  __main_pthread.detach_state = DT_JOINABLE;
  // pthread struct robust_list head should point to itself.
  __main_pthread.robust_list.head = &__main_pthread.robust_list.head;
  // Main thread ID is always 1.  It can't be 0 because musl assumes
  // tid is always non-zero.
  __main_pthread.tid = getpid();
  __main_pthread.locale = &libc.global_locale;
  // TODO(sbc): Implement circular list of threads
  //__main_pthread.next = __main_pthread.prev = &__main_pthread;
  __main_pthread.tsd = (void **)__pthread_tsd_main;
}
