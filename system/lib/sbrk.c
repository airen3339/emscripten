/*
 * Copyright 2019 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 *
*/

#ifndef EMSCRIPTEN_NO_ERRNO
#include <errno.h>
#endif
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#if __EMSCRIPTEN_PTHREADS__ // for error handling, see below
#include <stdio.h>
#include <stdlib.h>
#endif

#ifdef __EMSCRIPTEN_TRACING__
#include <emscripten/em_asm.h>
#endif

#include <emscripten/heap.h>

#ifndef EMSCRIPTEN_NO_ERRNO
#define SET_ERRNO() { errno = ENOMEM; }
#else
#define SET_ERRNO()
#endif

// FIXME just use  = &sbrk_val  here. That is simpler, but currently it has a
// code size cost for tiny programs that don't use malloc. We link in malloc by
// default, and depend on metadce to remove it when not used, which we can do,
// but a static assignment here would mean a nonzero value in a memory segment,
// which metadce cannot remove (it can remove code, but not data in segments).
// The current code allows tiny programs without malloc to have no data segments
// at all, while programs that use malloc may suffer a few more bytes of code
// size, but that's negligible compared to malloc itself.
// TODO: when we stop including malloc by default we can use the simpler way.
static intptr_t sbrk_val = 0;

intptr_t* emscripten_get_sbrk_ptr() {
  extern size_t __heap_base;
  intptr_t* sbrk_ptr = &sbrk_val;
#if __EMSCRIPTEN_PTHREADS__
  if (__c11_atomic_load((_Atomic(intptr_t)*)sbrk_ptr, __ATOMIC_SEQ_CST) == 0) {
    __c11_atomic_store((_Atomic(intptr_t)*)sbrk_ptr, (intptr_t)&__heap_base, __ATOMIC_SEQ_CST);
  }
#else
  if (sbrk_val == 0) {
    sbrk_val = (intptr_t)&__heap_base;
  }
#endif
  return &sbrk_val;
}

void *sbrk(intptr_t increment) {
  uintptr_t old_size;
  // Enforce preserving a minimal 4-byte alignment for sbrk.
  increment = (increment + 3) & ~3;
#if __EMSCRIPTEN_PTHREADS__
  // Our default dlmalloc uses locks around each malloc/free, so no additional
  // work is necessary to keep things threadsafe, but we also make sure sbrk
  // itself is threadsafe so alternative allocators work. We do that by looping
  // and retrying if we hit interference with another thread.
  intptr_t expected;
  while (1) {
#endif // __EMSCRIPTEN_PTHREADS__
    intptr_t* sbrk_ptr = emscripten_get_sbrk_ptr();
#if __EMSCRIPTEN_PTHREADS__
    intptr_t old_brk = __c11_atomic_load((_Atomic(intptr_t)*)sbrk_ptr, __ATOMIC_SEQ_CST);
#else
    intptr_t old_brk = *sbrk_ptr;
#endif
    intptr_t new_brk = old_brk + increment;
    // Check for a 32-bit overflow, which would indicate that we are trying to
    // allocate over 4GB, which is never possible in wasm32.
    if (increment > 0 && (uint32_t)new_brk <= (uint32_t)old_brk) {
      goto Error;
    }
#ifdef __wasm__
    old_size = __builtin_wasm_memory_size(0) * WASM_PAGE_SIZE;
#else
    old_size = emscripten_get_heap_size();
#endif
    if (new_brk > old_size) {
      // Try to grow memory.
      if (!emscripten_resize_heap(new_brk)) {
        goto Error;
      }
    }
#if __EMSCRIPTEN_PTHREADS__
    // Attempt to update the dynamic top to new value. Another thread may have
    // beat this one to the update, in which case we will need to start over
    // by iterating the loop body again.
    expected = old_brk;
    __c11_atomic_compare_exchange_strong(
        (_Atomic(intptr_t)*)sbrk_ptr,
        &expected, new_brk,
        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    if (expected != old_brk) {
      continue;
    }
#else // __EMSCRIPTEN_PTHREADS__
    *sbrk_ptr = new_brk;
#endif // __EMSCRIPTEN_PTHREADS__

#ifdef __EMSCRIPTEN_TRACING__
    EM_ASM({if (typeof emscriptenMemoryProfiler !== 'undefined') emscriptenMemoryProfiler.onSbrkGrow($0, $1)}, old_brk, old_brk + increment );
#endif
    return (void*)old_brk;

#if __EMSCRIPTEN_PTHREADS__
  }
#endif // __EMSCRIPTEN_PTHREADS__

Error:
  SET_ERRNO();
  return (void*)-1;
}

int brk(intptr_t ptr) {
#if __EMSCRIPTEN_PTHREADS__
  // FIXME
  printf("brk() is not theadsafe yet, https://github.com/emscripten-core/emscripten/issues/10006");
  abort();
#endif
  intptr_t last = (intptr_t)sbrk(0);
  if (sbrk(ptr - last) == (void*)-1) {
    return -1;
  }
  return 0;
}
