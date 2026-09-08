/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2013, 2014 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifndef MICROPY_INCLUDED_PY_GC_H
#define MICROPY_INCLUDED_PY_GC_H

#include <stdbool.h>
#include <stddef.h>
#include "py/mpprint.h"

void gc_init(void *start, void *end);

#if MICROPY_GC_SPLIT_HEAP
// Used to add additional memory areas to the heap.
void gc_add(void *start, void *end);

#if MICROPY_GC_SPLIT_HEAP_AUTO
// Port must implement this function to return the maximum available block of
// RAM to allocate a new heap area into using MP_PLAT_ALLOC_HEAP.
size_t gc_get_max_new_split(void);
#endif // MICROPY_GC_SPLIT_HEAP_AUTO
#endif // MICROPY_GC_SPLIT_HEAP

// These lock/unlock functions can be nested.
// They can be used to prevent the GC from allocating/freeing.
void gc_lock(void);
void gc_unlock(void);
bool gc_is_locked(void);

// True for the whole span between gc_collect_start() and gc_collect_end()
// (i.e. while a mark-and-sweep is actually in progress). Safe to poll from
// another FreeRTOS task/core: under MICROPY_PY_THREAD_GIL only one thread
// ever runs a collection at a time, so this is a simple non-nesting flag.
// Used by port code (e.g. the BLE/USB HID key-repeat timer) that needs to
// know the interpreter is stalled and shouldn't advance time-based state.
bool gc_is_collecting(void);

// A given port must implement gc_collect by using the other collect functions.
void gc_collect(void);
void gc_collect_start(void);
void gc_collect_root(void **ptrs, size_t len);
void gc_collect_end(void);

// Use this function to sweep the whole heap and run all finalisers
void gc_sweep_all(void);

// These functions are used to manage weakrefs.
void gc_weakref_mark(void *ptr);
void gc_weakref_about_to_be_freed(void *ptr);
void gc_weakref_sweep(void);

enum {
  GC_ALLOC_FLAG_HAS_FINALISER = 1,
  GC_ALLOC_FLAG_IS_DATA = 2,
};

void *gc_alloc(size_t n_bytes, unsigned int alloc_flags);
void gc_free(void *ptr); // does not call finaliser
size_t gc_nbytes(const void *ptr);
void *gc_realloc(void *ptr, size_t n_bytes, bool allow_move);
void *gc_realloc_data(void *ptr, size_t n_bytes, bool allow_move);

#if MICROPY_GC_INCREMENTAL
// ---------------------------------------------------------------------------
// Tri-colour incremental GC (Dijkstra incremental marking, mark-on-store
// write barrier).
//
// A cycle is: scan roots -> drain gray set in bounded slices (advanced from
// gc_alloc) -> atomic root rescan -> sweep (still stop-the-world, as before).
// mp_gc_mark_active is the cycle phase: 0 = idle, 1 = marking (write
// barriers live), 2 = sweeping (barriers off; new allocations are tagged
// "born during sweep" in the ATB and the table walk treats them as live).
extern volatile uint8_t mp_gc_mark_active; // 0 = idle, 1 = marking, 2 = sweeping

// Barrier slow path: mark `maybe_ptr` (a heap object head) and push to gray.
void gc_barrier(uintptr_t maybe_ptr);
// Mark an arbitrary heap-allocated buffer gray (used by table-relocation code
// in map.c etc. whose contents are pointer slots).
void gc_gray_mark_ptr(void *ptr);

// Runtime enable/disable.  Disabling only stops *starting* new incremental
// cycles; an already-running cycle always finishes with barriers active.
bool gc_incremental_enabled(void);
void gc_incremental_set_enabled(bool on);
// True while a cycle's mark phase is in progress.
bool gc_incremental_active(void);
// Start an incremental cycle if idle (root scan, bounded STW).
void gc_incremental_start(void);
// Advance the active cycle by one bounded mark slice; completes the cycle
// (atomic rescan + sweep) if marking finishes within the slice.
void gc_incremental_step(bool desperate);
// Run the active cycle to completion synchronously.
void gc_incremental_complete(void);
// Abandon an in-progress cycle, unmarking everything (used by gc_sweep_all
// on soft reset, where the whole heap is about to be freed anyway).
void gc_incremental_abort(void);
// Fill 10 counters: cycles, slices, slice_ends, sync_ends, overflows, words,
// aborts, roots_us, sweep_us, max_sweep_us.
void gc_incremental_get_stats(uint32_t *out);
// Runtime-tunable slice budget (heap words scanned per gc_alloc call).
// Returns previous value; pass 0 to query without changing.
uint32_t gc_incremental_set_slice_words(uint32_t w);
void gc_incremental_set_pace(uint32_t gap_us, uint32_t cap_us);
void gc_incremental_get_pace(uint32_t *gap_us, uint32_t *cap_us);

// Write barrier for a value being stored into a heap container.
// Zero cost (one likely-not-taken byte test) when no cycle is active.
#define GC_BARRIER(obj) \
    do { mp_obj_t _barrier_v = (obj); \
         if (mp_gc_mark_active == 1 && mp_obj_is_obj(_barrier_v)) { \
             gc_barrier((uintptr_t)_barrier_v); \
         } \
    } while (0)
// Mark a freshly (re)allocated pointer-table buffer as live for this cycle.
#define GC_GRAY_MARK_PTR(ptr) \
    do { if (mp_gc_mark_active == 1) { gc_gray_mark_ptr(ptr); } } while (0)

// Barrier a run of `n` consecutive mp_obj_t slots that are about to be copied
// into a container (mp_seq_* bulk moves, slice assignment, extend).
void gc_barrier_range(const void *slots, size_t n);
#define GC_BARRIER_N(slots, n) \
    do { if (mp_gc_mark_active == 1) { gc_barrier_range((slots), (n)); } } while (0)

#else // MICROPY_GC_INCREMENTAL
#define GC_BARRIER(obj)
#define GC_GRAY_MARK_PTR(ptr)
#define GC_BARRIER_N(slots, n)
#endif // MICROPY_GC_INCREMENTAL

// ---------------------------------------------------------------------------
// Write-barrier completion (see ports/esp32/GC_BARRIER_TODO.md).
//
// GC_WB / GC_WB_N / GC_WB_PTR are the barriers added while completing the
// store-site coverage.  They are spelled differently from the original
// GC_BARRIER only so the new coverage can be switched off in one place for
// A/B testing:
//
//     make CFLAGS_EXTRA=-DMICROPY_GC_BARRIER_FULL=0
//
// builds with just the handful of original barriers, which is the
// known-unsound configuration -- use it to compare cost, never to ship.
#ifndef MICROPY_GC_BARRIER_FULL
#define MICROPY_GC_BARRIER_FULL (MICROPY_GC_INCREMENTAL)
#endif

#if MICROPY_GC_BARRIER_FULL
#define GC_WB(obj)            GC_BARRIER(obj)
#define GC_WB_N(slots, n)     GC_BARRIER_N(slots, n)
#define GC_WB_PTR(ptr)        GC_GRAY_MARK_PTR(ptr)
#else
#define GC_WB(obj)
#define GC_WB_N(slots, n)
#define GC_WB_PTR(ptr)
#endif

typedef struct _gc_info_t {
    size_t total;
    size_t used;
    size_t free;
    size_t max_free;
    size_t num_1block;
    size_t num_2block;
    size_t max_block;
    #if MICROPY_GC_SPLIT_HEAP_AUTO
    size_t max_new_split;
    #endif
} gc_info_t;

void gc_info(gc_info_t *info);
void gc_info_fast(gc_info_t *info);
void gc_dump_info(const mp_print_t *print);
void gc_dump_alloc_table(const mp_print_t *print);
/*
void gc_mark_start(void);
void gc_mark_end(void);
*/
#endif // MICROPY_INCLUDED_PY_GC_H
