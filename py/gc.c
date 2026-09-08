/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2013, 2014 Damien P. George
 * Copyright (c) 2014 Paul Sokolovsky
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

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "py/gc.h"
#include "py/runtime.h"

#if MICROPY_DEBUG_VALGRIND
#include <valgrind/memcheck.h>
#endif

#if MICROPY_ENABLE_GC

#if MICROPY_DEBUG_VERBOSE // print debugging info
#define DEBUG_PRINT (1)
#define DEBUG_printf DEBUG_printf
#else // don't print debugging info
#define DEBUG_PRINT (0)
#define DEBUG_printf(...) (void)0
#endif

// make this 1 to dump the heap each time it changes
#define EXTENSIVE_HEAP_PROFILING (0)

// make this 1 to zero out swept memory to more eagerly
// detect untraced object still in use
#define CLEAR_ON_SWEEP (0)

#define WORDS_PER_BLOCK ((MICROPY_BYTES_PER_GC_BLOCK) / MP_BYTES_PER_OBJ_WORD)
#define BYTES_PER_BLOCK (MICROPY_BYTES_PER_GC_BLOCK)

// ATB = allocation table byte
// 0b00 = FREE -- free block// 0b01 = HEAD -- head of a chain of blocks
// 0b10 = TAIL -- in the tail of a chain of blocks
// 0b11 = MARK -- marked head block

#define AT_FREE (0)
#define AT_HEAD (1)
#define AT_TAIL (2)
#define AT_MARK (3)
#define AT_HEAD_DATA (5)

#define BLOCKS_PER_ATB (2)
#define ATB_MASK_0 (0x0f)
#define ATB_MASK_1 (0xf0)
//#define ATB_MASK_2 (0x30)
//#define ATB_MASK_3 (0xc0)

#define ATB_0_IS_FREE(a) (((a) & ATB_MASK_0) == 0)
#define ATB_1_IS_FREE(a) (((a) & ATB_MASK_1) == 0)
//#define ATB_2_IS_FREE(a) (((a) & ATB_MASK_2) == 0)
//#define ATB_3_IS_FREE(a) (((a) & ATB_MASK_3) == 0)

#if MICROPY_GC_SPLIT_HEAP
#define NEXT_AREA(area) ((area)->next)
#else
#define NEXT_AREA(area) (NULL)
#endif

#define BLOCK_SHIFT(block) (4 * ((block) & (BLOCKS_PER_ATB - 1)))
#define ATB_IF_ALL_WORD_IS_TAIL(area, block) (((area)->gc_alloc_table_start_word[(block) >> 3]&0x33333333)==0x22222222)
#define ATB_IF_ALL_BYTE_IS_TAIL(area, block) (((area)->gc_alloc_table_start[(block) >> 1]&0x33)==0x22)
#define ATB_GET_KIND(area, block) (((area)->gc_alloc_table_start[(block) >> 1] >> BLOCK_SHIFT(block)) & 0x3)
#define ATB_GET_KIND_FULL(area, block) (((area)->gc_alloc_table_start[(block) >> 1] >> BLOCK_SHIFT(block)) & 0xf)
#define ATB_ANY_TO_FREE_BYTE(area, block) do { area->gc_alloc_table_start[(block) >> 1] = 0; } while(0)
#define ATB_ANY_TO_FREE(area, block) do { area->gc_alloc_table_start[(block) >> 1] &= (~(0xf << BLOCK_SHIFT(block))); } while (0)
#define ATB_FREE_TO_HEAD(area, block) do { area->gc_alloc_table_start[(block)  >> 1] |= (AT_HEAD << BLOCK_SHIFT(block)); } while (0)
#define ATB_FREE_TO_TAIL(area, block) do { area->gc_alloc_table_start[(block)  >> 1] |= (AT_TAIL << BLOCK_SHIFT(block)); } while (0)
#define ATB_HEAD_TO_MARK(area, block) do { area->gc_alloc_table_start[(block)  >> 1] |= (AT_MARK << BLOCK_SHIFT(block)); } while (0)
#define ATB_MARK_TO_HEAD(area, block) do { area->gc_alloc_table_start[(block)  >> 1] &= (~(AT_TAIL << BLOCK_SHIFT(block))); } while (0)
#define ATB_FREE_TO_HEAD_DATA(area, block) do { area->gc_alloc_table_start[(block)  >> 1] |= (AT_HEAD_DATA << BLOCK_SHIFT(block)); } while (0)
// It's same as ATB_FREE_TO_HEAD
#define ATB_MARK_TO_HEAD_DATA(area, block) do { area->gc_alloc_table_start[(block)  >> 1] |= (AT_HEAD_DATA << BLOCK_SHIFT(block)); } while (0)

// ATB bit 3 is shared between two phases that never overlap: during MARK it
// means "gray" (marked but not yet scanned), during SWEEP it means "allocated
// during this sweep".  Marking clears every gray before the sweep is armed, so
// the two meanings cannot collide.
#define ATB_SET_GRAY(area, block) do { area->gc_alloc_table_start[(block) >> 1] |= (8 << BLOCK_SHIFT(block)); } while (0)
#define ATB_CLEAR_GRAY(area, block) do { area->gc_alloc_table_start[(block) >> 1] &= ~(8 << BLOCK_SHIFT(block)); } while (0)
#define ATB_IS_GRAY(area, block) (ATB_GET_KIND_FULL(area, block) & 8)

#define BLOCK_FROM_PTR(area, ptr) (((uintptr_t)(ptr) - (uintptr_t)area->gc_pool_start) / BYTES_PER_BLOCK)
#define PTR_FROM_BLOCK(area, block) (((uintptr_t)(block) * BYTES_PER_BLOCK + (uintptr_t)area->gc_pool_start))

// After the ATB, there must be a byte filled with AT_FREE so that gc_mark_tree
// cannot erroneously conclude that a block extends past the end of the GC heap
// due to bit patterns in the FTB (or first block, if finalizers are disabled)
// being interpreted as AT_TAIL.
#define ALLOC_TABLE_GAP_BYTE (1)

#if MICROPY_ENABLE_FINALISER
// FTB = finaliser table byte
// if set, then the corresponding block may have a finaliser
#define BLOCKS_PER_FTB (8)
#define FTB_GET(area, block) ((area->gc_finaliser_table_start[(block) / BLOCKS_PER_FTB] >> ((block) & 7)) & 1)
#define FTB_SET(area, block) do { area->gc_finaliser_table_start[(block) / BLOCKS_PER_FTB] |= (1 << ((block) & 7)); } while (0)
#define FTB_CLEAR(area, block) do { area->gc_finaliser_table_start[(block) / BLOCKS_PER_FTB] &= (~(1 << ((block) & 7))); } while (0)
#endif

#if MICROPY_PY_WEAKREF
// WTB = weakref table byte
// if set, then the corresponding block may have a weakref in MP_STATE_VM(mp_weakref_map).
#define BLOCKS_PER_WTB (8)
#define WTB_GET(area, block) ((area->gc_weakref_table_start[(block) / BLOCKS_PER_WTB] >> ((block) & 7)) & 1)
#define WTB_SET(area, block) do { area->gc_weakref_table_start[(block) / BLOCKS_PER_WTB] |= (1 << ((block) & 7)); } while (0)
#define WTB_CLEAR(area, block) do { area->gc_weakref_table_start[(block) / BLOCKS_PER_WTB] &= (~(1 << ((block) & 7))); } while (0)
#endif

#if MICROPY_PY_THREAD && !MICROPY_PY_THREAD_GIL
#define GC_MUTEX_INIT() mp_thread_recursive_mutex_init(&MP_STATE_MEM(gc_mutex))
#define GC_ENTER() mp_thread_recursive_mutex_lock(&MP_STATE_MEM(gc_mutex), 1)
#define GC_EXIT() mp_thread_recursive_mutex_unlock(&MP_STATE_MEM(gc_mutex))
#else
// Either no threading, or assume callers to gc_collect() hold the GIL
#define GC_MUTEX_INIT()
#define GC_ENTER()
#define GC_EXIT()
#endif

// prototypes needed by the incremental machinery below
static void gc_inc_scan_static_roots(void);

// Static functions for individual steps of the GC mark/sweep sequence
static void gc_collect_start_common(void);
static void *gc_get_ptr(void **ptrs, int i);
#if MICROPY_GC_SPLIT_HEAP
static void gc_mark_subtree(mp_state_mem_area_t *area, size_t block);
#else
static void gc_mark_subtree(size_t block);
#endif
static void gc_deal_with_stack_overflow(void);
static void gc_sweep_run_finalisers(void);
static void gc_sweep_free_blocks(void);


// TODO waste less memory; currently requires that all entries in alloc_table have a corresponding block in pool
static void gc_setup_area(mp_state_mem_area_t *area, void *start, void *end) {
    // calculate parameters for GC (T=total, A=alloc table, F=finaliser table, P=pool; all in bytes):
    // T = A + F + W + P
    //     F = A * BLOCKS_PER_ATB / BLOCKS_PER_FTB
    //     W = A * BLOCKS_PER_ATB / BLOCKS_PER_WTB
    //     P = A * BLOCKS_PER_ATB * BYTES_PER_BLOCK
    // => T = A * (1 + BLOCKS_PER_ATB / BLOCKS_PER_FTB + BLOCKS_PER_ATB / BLOCKS_PER_WTB + BLOCKS_PER_ATB * BYTES_PER_BLOCK)
    size_t total_byte_len = (byte *)end - (byte *)start;
    #if MICROPY_ENABLE_FINALISER || MICROPY_PY_WEAKREF
    area->gc_alloc_table_byte_len = (total_byte_len - ALLOC_TABLE_GAP_BYTE)
        * MP_BITS_PER_BYTE
        / (
            MP_BITS_PER_BYTE
            #if MICROPY_ENABLE_FINALISER
            + MP_BITS_PER_BYTE * BLOCKS_PER_ATB / BLOCKS_PER_FTB
            #endif
            #if MICROPY_PY_WEAKREF
            + MP_BITS_PER_BYTE * BLOCKS_PER_ATB / BLOCKS_PER_WTB
            #endif
            + MP_BITS_PER_BYTE * BLOCKS_PER_ATB * BYTES_PER_BLOCK
            );
    #else
    area->gc_alloc_table_byte_len = (total_byte_len - ALLOC_TABLE_GAP_BYTE) / (1 + MP_BITS_PER_BYTE / 2 * BYTES_PER_BLOCK);
    #endif

    area->gc_alloc_table_start = (byte *)start;
    area->gc_alloc_table_start_word = (uint32_t *)start;

    // Allocate FTB and WTB blocks if they are enabled.
    byte *next_table = area->gc_alloc_table_start + area->gc_alloc_table_byte_len + ALLOC_TABLE_GAP_BYTE;
    (void)next_table;
    #if MICROPY_ENABLE_FINALISER
    size_t gc_finaliser_table_byte_len = (area->gc_alloc_table_byte_len * BLOCKS_PER_ATB + BLOCKS_PER_FTB - 1) / BLOCKS_PER_FTB;
    area->gc_finaliser_table_start = next_table;
    next_table += gc_finaliser_table_byte_len;
    #endif
    #if MICROPY_PY_WEAKREF
    size_t gc_weakref_table_byte_len = (area->gc_alloc_table_byte_len * BLOCKS_PER_ATB + BLOCKS_PER_WTB - 1) / BLOCKS_PER_WTB;
    area->gc_weakref_table_start = next_table;
    next_table += gc_weakref_table_byte_len;
    #endif

    // Allocate the GC pool of heap blocks.
    size_t gc_pool_block_len = area->gc_alloc_table_byte_len * BLOCKS_PER_ATB;
    area->gc_pool_start = (byte *)end - gc_pool_block_len * BYTES_PER_BLOCK;
    area->gc_pool_end = end;
    assert(area->gc_pool_start >= next_table);

    // Clear ATB's, and FTB's and WTB's if they are enabled.
    memset(area->gc_alloc_table_start, 0,
        area->gc_alloc_table_byte_len + ALLOC_TABLE_GAP_BYTE
        #if MICROPY_ENABLE_FINALISER
        + gc_finaliser_table_byte_len
        #endif
        #if MICROPY_PY_WEAKREF
        + gc_weakref_table_byte_len
        #endif
        );

    area->gc_last_free_atb_index = 0;
    area->gc_last_used_block = 0;

    #if MICROPY_GC_SPLIT_HEAP
    area->next = NULL;

    // Update the global min/max region that covers all heaps
    MP_STATE_MEM(area_pool_min) = MIN(MP_STATE_MEM(area_pool_min), area->gc_pool_start);
    MP_STATE_MEM(area_pool_max) = MAX(MP_STATE_MEM(area_pool_max), area->gc_pool_end);
    #endif

    // NOTE: this block runs from gc_add() <- gc_try_add_heap() <- gc_alloc(),
    // i.e. inside the allocator whenever the heap grows an area.  As blocking
    // console output on a serial/CDC link that is tens of milliseconds with
    // the GC lock held -- keep it debug-only.
    DEBUG_printf("GC layout:\n");
    DEBUG_printf("  alloc table at %p, length " UINT_FMT " bytes, "
        UINT_FMT " blocks\n",
        area->gc_alloc_table_start, area->gc_alloc_table_byte_len,
        area->gc_alloc_table_byte_len * BLOCKS_PER_ATB);
    #if MICROPY_ENABLE_FINALISER
    DEBUG_printf("  finaliser table at %p, length " UINT_FMT " bytes, "
        UINT_FMT " blocks\n", area->gc_finaliser_table_start,
        gc_finaliser_table_byte_len,
        gc_finaliser_table_byte_len * BLOCKS_PER_FTB);
    #endif
    #if MICROPY_PY_WEAKREF
    DEBUG_printf("  weakref table at %p, length " UINT_FMT " bytes, "
        UINT_FMT " blocks\n", area->gc_weakref_table_start,
        gc_weakref_table_byte_len,
        gc_weakref_table_byte_len * BLOCKS_PER_WTB);
    #endif
    DEBUG_printf("  pool at %p, length " UINT_FMT " bytes, "
        UINT_FMT " blocks\n", area->gc_pool_start,
        gc_pool_block_len * BYTES_PER_BLOCK, gc_pool_block_len);
}

void gc_init(void *start, void *end) {
    // align end pointer on block boundary
    end = (void *)((uintptr_t)end & (~(BYTES_PER_BLOCK - 1)));
    DEBUG_printf("Initializing GC heap: %p..%p = " UINT_FMT " bytes\n", start, end, (byte *)end - (byte *)start);

    #if MICROPY_GC_SPLIT_HEAP
    // Note: min/max are deliberately swapped here, gc_setup_area() will update them to
    // the correct values for the min/max of the first actual pool region
    MP_STATE_MEM(area_pool_min) = end;
    MP_STATE_MEM(area_pool_max) = start;
    #endif

    gc_setup_area(&MP_STATE_MEM(area), start, end);

    // set last free ATB index to start of heap
    #if MICROPY_GC_SPLIT_HEAP
    MP_STATE_MEM(gc_last_free_area) = &MP_STATE_MEM(area);
    #endif

    // unlock the GC
    MP_STATE_THREAD(gc_lock_depth) = 0;

    // allow auto collection
    MP_STATE_MEM(gc_auto_collect_enabled) = 1;

    #if MICROPY_GC_ALLOC_THRESHOLD
    #if MICROPY_GC_INCREMENTAL
    // Incremental GC collects ahead of pressure via the adaptive threshold.
    MP_STATE_MEM(gc_alloc_threshold) = MICROPY_GC_INCR_TRIGGER_BLOCKS;
    #else
    // by default, maxuint for gc threshold, effectively turning gc-by-threshold off
    MP_STATE_MEM(gc_alloc_threshold) = (size_t)-1;
    #endif
    MP_STATE_MEM(gc_alloc_amount) = 0;
    #endif

    GC_MUTEX_INIT();
}

#if MICROPY_GC_SPLIT_HEAP
void gc_add(void *start, void *end) {
    // Place the area struct at the start of the area.
    mp_state_mem_area_t *area = (mp_state_mem_area_t *)start;
    start = (void *)((uintptr_t)start + sizeof(mp_state_mem_area_t));

    end = (void *)((uintptr_t)end & (~(BYTES_PER_BLOCK - 1)));
    DEBUG_printf("Adding GC heap: %p..%p = " UINT_FMT " bytes\n", start, end, (byte *)end - (byte *)start);

    // Init this area
    gc_setup_area(area, start, end);

    // Find the last registered area in the linked list
    mp_state_mem_area_t *prev_area = &MP_STATE_MEM(area);
    while (prev_area->next != NULL) {
        prev_area = prev_area->next;
    }

    // Add this area to the linked list
    prev_area->next = area;
}

#if MICROPY_GC_SPLIT_HEAP_AUTO
// Try to automatically add a heap area large enough to fulfill 'failed_alloc'.
static bool gc_try_add_heap(size_t failed_alloc) {
    // 'needed' is the size of a heap large enough to hold failed_alloc, with
    // the additional metadata overheads as calculated in gc_setup_area().
    //
    // Rather than reproduce all of that logic here, we approximate that adding
    // (13/512) is enough overhead for sufficiently large heap areas (the
    // overhead converges to 3/128, but there's some fixed overhead and some
    // rounding up of partial block sizes).
    size_t needed = failed_alloc + MAX(2048, failed_alloc * 13 / 512);

    size_t avail = gc_get_max_new_split();

    DEBUG_printf("gc_try_add_heap failed_alloc " UINT_FMT ", "
        "needed " UINT_FMT ", avail " UINT_FMT " bytes \n",
        failed_alloc,
        needed,
        avail);

    if (avail < needed) {
        // Can't fit this allocation, or system heap has nearly run out anyway
        return false;
    }

    // Deciding how much to grow the total heap by each time is tricky:
    //
    // - Grow by too small amounts, leads to heap fragmentation issues.
    //
    // - Grow by too large amounts, may lead to system heap running out of
    //   space.
    //
    // Currently, this implementation is:
    //
    // - At minimum, aim to double the total heap size each time we add a new
    //   heap.  i.e. without any large single allocations, total size will be
    //   64KB -> 128KB -> 256KB -> 512KB -> 1MB, etc
    //
    // - If the failed allocation is too large to fit in that size, the new
    //   heap is made exactly large enough for that allocation. Future growth
    //   will double the total heap size again.
    //
    // - If the new heap won't fit in the available free space, add the largest
    //   new heap that will fit (this may lead to failed system heap allocations
    //   elsewhere, but some allocation will likely fail in this circumstance!)

    // Compute total number of blocks in the current heap.
    size_t total_blocks = 0;
    for (mp_state_mem_area_t *area = &MP_STATE_MEM(area);
         area != NULL;
         area = NEXT_AREA(area)) {
        total_blocks += area->gc_alloc_table_byte_len * BLOCKS_PER_ATB;
    }

    // Compute bytes needed to build a heap with total_blocks blocks.
    size_t total_heap =
        total_blocks / BLOCKS_PER_ATB
        #if MICROPY_ENABLE_FINALISER
        + total_blocks / BLOCKS_PER_FTB
        #endif
        #if MICROPY_PY_WEAKREF
        + total_blocks / BLOCKS_PER_WTB
        #endif
        + total_blocks * BYTES_PER_BLOCK
        + ALLOC_TABLE_GAP_BYTE
        + sizeof(mp_state_mem_area_t);

    // Round up size to the nearest multiple of BYTES_PER_BLOCK.
    total_heap = (total_heap + BYTES_PER_BLOCK - 1) & (~(BYTES_PER_BLOCK - 1));

    #ifdef MICROPY_GC_MAX_NEW_AREA_SIZE
    if (total_heap > MICROPY_GC_MAX_NEW_AREA_SIZE) {
        total_heap = MICROPY_GC_MAX_NEW_AREA_SIZE;
    }
    #endif

    DEBUG_printf("total_heap " UINT_FMT " bytes\n", total_heap);

    size_t to_alloc = MIN(avail, MAX(total_heap, needed));

    mp_state_mem_area_t *new_heap = MP_PLAT_ALLOC_HEAP(to_alloc);

    DEBUG_printf("MP_PLAT_ALLOC_HEAP " UINT_FMT " = %p\n",
        to_alloc, new_heap);

    if (new_heap == NULL) {
        // This should only fail:
        // - In a threaded environment if another thread has
        //   allocated while this function ran.
        // - If there is a bug in gc_get_max_new_split().
        return false;
    }

    gc_add(new_heap, (void *)((char *)new_heap + to_alloc));

    return true;
}
#endif

#endif

void gc_lock(void) {
    // This does not need to be atomic or have the GC mutex because:
    // - each thread has its own gc_lock_depth so there are no races between threads;
    // - a hard interrupt will only change gc_lock_depth during its execution, and
    //   upon return will restore the value of gc_lock_depth.
    MP_STATE_THREAD(gc_lock_depth) += (1 << GC_LOCK_DEPTH_SHIFT);
}

void gc_unlock(void) {
    // This does not need to be atomic, See comment above in gc_lock.
    MP_STATE_THREAD(gc_lock_depth) -= (1 << GC_LOCK_DEPTH_SHIFT);
}

bool gc_is_locked(void) {
    return MP_STATE_THREAD(gc_lock_depth) != 0;
}

// See the comment in gc.h. Only ever touched between gc_collect_start_common()
// and gc_collect_end(), which (under the GIL) never overlap across threads.
static volatile bool gc_collecting_flag = false;

bool gc_is_collecting(void) {
    return gc_collecting_flag;
}

#if MICROPY_GC_SPLIT_HEAP
static mp_state_mem_area_t *gc_get_ptr_area(const void *ptr);

// Returns the area to which this arbitrary pointer belongs, or NULL if it isn't
// allocated on the GC-managed heap. Contains "fast path" inline checks for invalid
// data which isn't a pointer to the heap. Equivalent of VERIFY_PTR for the non-split-heap case.
static inline MP_ALWAYSINLINE mp_state_mem_area_t *gc_verify_ptr_get_area(const void *ptr) {
    // These inline checks are similar to VERIFY_PTR macro, below
    if ((byte *)ptr < MP_STATE_MEM(area_pool_min) || (byte *)ptr > MP_STATE_MEM(area_pool_max)) {
        return NULL;  // not in the overall pool region
    }
    if (((uintptr_t)(ptr) & (BYTES_PER_BLOCK - 1)) != 0) {
        return NULL;  // not aligned on a block boundary
    }
    return gc_get_ptr_area(ptr);
}

// Returns the area to which a pointer belongs. Assumes pointer is valid to a heap block.
static mp_state_mem_area_t *gc_get_ptr_area(const void *ptr) {
    for (mp_state_mem_area_t *area = &MP_STATE_MEM(area); area != NULL; area = NEXT_AREA(area)) {
        if (ptr >= (void *)area->gc_pool_start   // must be above start of pool
            && ptr < (void *)area->gc_pool_end) {   // must be below end of pool
            return area;
        }
    }
    return NULL;
}
#else

// ptr should be of type void*
#define VERIFY_PTR(ptr) ( \
    ((uintptr_t)(ptr) & (BYTES_PER_BLOCK - 1)) == 0          /* must be aligned on a block */ \
    && ptr >= (void *)MP_STATE_MEM(area).gc_pool_start      /* must be above start of pool */ \
    && ptr < (void *)MP_STATE_MEM(area).gc_pool_end         /* must be below end of pool */ \
    )

#endif

#if MICROPY_GC_INCREMENTAL
// ============================ incremental GC core ==========================

volatile uint8_t mp_gc_mark_active;         // 0 = idle, 1 = mark phase running
static uint8_t gc_inc_enabled = 1;          // runtime switch (gc.incremental())
static bool gc_inc_mark_only;               // set while scanning roots: mark+queue, no DFS
static bool gc_inc_in_step;                 // re-entrancy guard for step/finish
static uint32_t gc_inc_gray_sp;             // gray set stack pointer
static uint32_t gc_inc_slice_words = MICROPY_GC_INCR_SLICE_WORDS;

// Slice pacing: slices ride on gc_alloc(), but allocation-heavy code allocates
// thousands of times per frame, so slices are rate-limited by wall clock
// (gap) and individually capped in duration (cap).  Both tunable via gc.pace().
static uint32_t gc_inc_gap_us = MICROPY_GC_INCR_SLICE_GAP_US;
static uint32_t gc_inc_cap_us = MICROPY_GC_INCR_SLICE_CAP_US;
static uint32_t gc_inc_next_slice_us;
static uint8_t gc_inc_alloc_ctr; // allocator-side sampling counter (1-in-8)

#if MICROPY_GC_USTIME_AVAILABLE
static inline uint32_t gc_inc_now_us(void) {
    return (uint32_t)MICROPY_GC_PORT_USTIME();
}
static inline bool gc_inc_due(void) {
    // signed compare survives the 32-bit microsecond counter wrap (~71 min)
    return (int32_t)(gc_inc_now_us() - gc_inc_next_slice_us) >= 0;
}
static inline bool gc_inc_over_cap(uint32_t t0) {
    return (uint32_t)(gc_inc_now_us() - t0) > gc_inc_cap_us;
}
#else
static inline bool gc_inc_due(void) {
    return true;
}
static inline bool gc_inc_over_cap(uint32_t t0) {
    return false;
}
#endif

// Cycle phases for mp_gc_mark_active.
#define GC_INC_PHASE_IDLE  (0)
#define GC_INC_PHASE_MARK  (1)
#define GC_INC_PHASE_SWEEP (2)

// Sliced-sweep cursor state (see gc_inc_sweep_step).
static mp_state_mem_area_t *gc_inc_sw_area;
static size_t gc_inc_sw_free;   // free blocks tallied by the current sweep
static size_t gc_inc_sw_block;
static int gc_inc_sw_free_tail;
static size_t gc_inc_sw_last_used;
#if MICROPY_GC_SPLIT_HEAP_AUTO
static mp_state_mem_area_t *gc_inc_sw_prev_area;
#endif

// True if any of the 8 ATB nibbles covered by a word-skip has the bit-8
// "born during sweep" tag set.  Tagged nibbles must not be consumed by the
// all-tail fast skips (grown tails carry TAIL|8, which kind-masks to TAIL).
static bool gc_inc_tag_word8(const mp_state_mem_area_t *area, size_t block) {
    const uint8_t *b = &area->gc_alloc_table_start[block >> 1];
    return (b[0] | b[1] | b[2] | b[3]) & 0x88;
}
static bool gc_inc_tag_byte8(const mp_state_mem_area_t *area, size_t block) {
    return area->gc_alloc_table_start[block >> 1] & 0x88;
}

// Simple counters for visibility into the incremental collector (gc.stats()).
enum {
    GC_INC_ST_CYCLES = 0,     // cycles started
    GC_INC_ST_SLICES,         // bounded mark slices executed from gc_alloc
    GC_INC_ST_SLICE_ENDS,     // cycles whose marking finished inside a slice
    GC_INC_ST_SYNC_ENDS,      // cycles completed synchronously (gc.collect or OOM)
    GC_INC_ST_OVERFLOWS,      // gray-set overflows handled by full-table resolver
    GC_INC_ST_WORDS,          // total heap words scanned (mark work volume)
    GC_INC_ST_ABORTS,         // cycles abandoned (soft reset)
    GC_INC_ST_ROOTS_US,       // cumulative microseconds spent in root scans (STW)
    GC_INC_ST_SWEEP_US,       // cumulative microseconds spent in sweep pieces (STW)
    GC_INC_ST_MAX_SWEEP_US,   // longest single stop-the-world piece observed
    GC_INC_ST_FIN_US,         // finaliser pass (subset of sweep_us)
    GC_INC_ST_WALK_US,        // sliced table free-walk total (subset of sweep_us)
    GC_INC_ST_MAX_SLICE_US,   // longest single mark slice (per-alloc surcharge)
    GC_INC_ST_MARK_US,        // cumulative time in mark slices (the missing
                              // counterpart to sweep_us/roots_us)
    GC_INC_ST_DRAIN_US,       // final synchronous drain at end of marking (STW)
    GC_INC_ST_MAX_DRAIN_US,   // longest single final drain -- the frame killer
    GC_INC_ST_FINAL_WORDS,    // words scanned in final drains (vs WORDS total):
                              // the share of marking that did NOT go incremental
    GC_INC_ST_WALK_BLOCKS,    // ATB blocks visited by gray-overflow resolver walks
    GC_INC_ST_MAX_ALLOC_US,   // longest TOTAL GC time inside one gc_alloc()
                              // call (all retry iterations summed).  This is
                              // what a frame actually feels; the other max_*
                              // counters only bound one individual piece.
    GC_INC_ST_MAX_RETRIES,    // most collect/sweep retries in one gc_alloc()
    GC_INC_ST_ALLOC_STALL_US, // cumulative of the same, all calls
    GC_INC_ST_MAX_GRAY,       // high-water mark of the gray set, in entries.
                              // Size MICROPY_ALLOC_GC_STACK_SIZE above this to
                              // get overflows to zero (the reading is only
                              // meaningful once it is not already clipping).
    GC_INC_ST_OOM,            // TEMP: gc_alloc found no free block
    GC_INC_ST_STEP_CALLS,     // TEMP: gc_incremental_step() invocations
    GC_INC_ST_STEP_IDLE,      // TEMP: ... returned early, no cycle active
    GC_INC_ST_STEP_NOTDUE,    // TEMP: ... returned early, pacing gap not elapsed
    GC_INC_ST_THRESH_HITS,    // TEMP: gc_alloc threshold branch taken
    GC_INC_ST_START_CALLS,    // TEMP: gc_incremental_start() invocations
    GC_INC_ST_START_BUSY,     // TEMP: ... returned early (cycle already active)
    GC_INC_ST_N
};
static uint32_t gc_inc_stats[GC_INC_ST_N];

void gc_incremental_get_stats(uint32_t *out) {
    memcpy(out, gc_inc_stats, GC_INC_ST_N * sizeof(uint32_t));
}

// Runtime-tunable slice budget (words of heap scanned per gc_alloc call).
// Lower = smoother frame pacing, longer wall-clock time per cycle.
uint32_t gc_incremental_set_slice_words(uint32_t w) {
    if (w >= 16) {
        gc_inc_slice_words = w;
    }
    return gc_inc_slice_words;
}

// Slice pacing control (see MICROPY_GC_INCR_SLICE_GAP_US / _CAP_US).
// gap_us: minimum wall-clock gap between two slices (bounds GC duty cycle)
// cap_us: hard per-slice runtime cap (bounds per-allocation surcharge)
void gc_incremental_set_pace(uint32_t gap_us, uint32_t cap_us) {
    if (gap_us >= 10) {
        gc_inc_gap_us = gap_us;
    }
    if (cap_us >= 10 && cap_us <= 100000) {
        gc_inc_cap_us = cap_us;
    }
}

void gc_incremental_get_pace(uint32_t *gap_us, uint32_t *cap_us) {
    *gap_us = gc_inc_gap_us;
    *cap_us = gc_inc_cap_us;
}

bool gc_incremental_enabled(void) {
    return gc_inc_enabled;
}

void gc_incremental_set_enabled(bool on) {
    gc_inc_enabled = on ? 1 : 0;
}

bool gc_incremental_active(void) {
    return mp_gc_mark_active != 0;
}

// Push an already-marked block onto the gray set (reuses the existing
// gc_block_stack/gc_area_stack static arrays -- no new RAM).  On overflow the
// resolver fallback is flagged; it re-derives all marked blocks by a table
// walk (same fallback behaviour as the classic collector).
static void gc_inc_gray_push(mp_state_mem_area_t *area, size_t block) {
    if (gc_inc_gray_sp >= MICROPY_ALLOC_GC_STACK_SIZE) {
        MP_STATE_MEM(gc_stack_overflow) = 1;
        return;
    }
    if (gc_inc_gray_sp >= gc_inc_stats[GC_INC_ST_MAX_GRAY]) {
        gc_inc_stats[GC_INC_ST_MAX_GRAY] = gc_inc_gray_sp + 1;
    }
    MP_STATE_MEM(gc_block_stack)[gc_inc_gray_sp] = block;
    #if MICROPY_GC_SPLIT_HEAP
    MP_STATE_MEM(gc_area_stack)[gc_inc_gray_sp] = area;
    #endif
    gc_inc_gray_sp += 1;
}

static void gc_inc_mark_gray(mp_state_mem_area_t *area, size_t block) {
    ATB_HEAD_TO_MARK(area, block);
    ATB_SET_GRAY(area, block);
    gc_inc_gray_push(area, block);
}

// If ptr points at an unscanned (white/HEAD) heap object, mark it gray.
static void gc_inc_mark_head(void *ptr) {
    #if MICROPY_GC_SPLIT_HEAP
    mp_state_mem_area_t *area = gc_verify_ptr_get_area(ptr);
    if (!area) {
        return;
    }
    #else
    if (!VERIFY_PTR(ptr)) {
        return;
    }
    mp_state_mem_area_t *area = &MP_STATE_MEM(area);
    #endif
    size_t block = BLOCK_FROM_PTR(area, ptr);
    if (ATB_GET_KIND(area, block) != AT_HEAD) {
        return; // already marked, a tail block, or not an object head
    }
    gc_inc_mark_gray(area, block);
}

void gc_barrier(uintptr_t maybe_ptr) {
    gc_inc_mark_head((void *)maybe_ptr);
}

// Barrier a run of consecutive mp_obj_t slots about to be copied into a
// container.  Cheaper than N separate GC_BARRIER calls: the phase test is
// hoisted out by the GC_BARRIER_N macro.
void gc_barrier_range(const void *slots, size_t n) {
    const mp_obj_t *p = (const mp_obj_t *)slots;
    for (size_t i = 0; i < n; i++) {
        mp_obj_t v = p[i];
        if (mp_obj_is_obj(v)) {
            gc_inc_mark_head((void *)v);
        }
    }
}

void gc_gray_mark_ptr(void *ptr) {
    if (mp_gc_mark_active != GC_INC_PHASE_MARK || ptr == NULL) {
        return;
    }
    #if MICROPY_GC_SPLIT_HEAP
    mp_state_mem_area_t *area = gc_verify_ptr_get_area(ptr);
    if (!area) {
        return;
    }
    #else
    if (!VERIFY_PTR(ptr)) {
        return;
    }
    mp_state_mem_area_t *area = &MP_STATE_MEM(area);
    #endif
    size_t block = BLOCK_FROM_PTR(area, ptr);
    size_t kind = ATB_GET_KIND(area, block);
    if (kind == AT_HEAD) {
        gc_inc_mark_gray(area, block);
    } else if (kind == AT_MARK) {
        // Already marked (possibly already scanned): requeue so that slots
        // added by the realloc (grown tail blocks) get rescanned.
        ATB_SET_GRAY(area, block);
        gc_inc_gray_push(area, block);
    }
}

#endif // MICROPY_GC_INCREMENTAL

#ifdef TRACE_MARK
#error "TRACE_MARK is replaced by TRACE_MARK_R and TRACE_MARK_S"
#endif
// R for root pointer, S for subtree.
#ifndef TRACE_MARK_R
#if DEBUG_PRINT
#define TRACE_MARK_R(block, ptr) DEBUG_printf("gc_mark_r(%p)\n", ptr)
#else
#define TRACE_MARK_R(block, ptr)
#endif
#endif
#ifndef TRACE_MARK_S
#if DEBUG_PRINT
#define TRACE_MARK_S(block, ptr) DEBUG_printf("gc_mark_s(%p)\n", ptr)
#else
#define TRACE_MARK_S(block, ptr)
#endif
#endif

void gc_collect_start(void) {
    gc_collect_start_common();
    #if MICROPY_GC_ALLOC_THRESHOLD && !MICROPY_GC_INCREMENTAL
    MP_STATE_MEM(gc_alloc_amount) = 0;
    #endif
    gc_inc_scan_static_roots();
}

// Scan the static root set: mp_state_ctx root region + the pystack.
// (C stack / registers / other threads are scanned by the port via
// MICROPY_GC_PORT_STACK_ROOTS().)
static void gc_inc_scan_static_roots(void) {
    // Trace root pointers.  This relies on the root pointers being organised
    // correctly in the mp_state_ctx structure.  We scan nlr_top,
    // dict_locals, dict_globals, then the root pointer section of mp_state_vm.
    void **ptrs = (void **)(void *)&mp_state_ctx;
    size_t root_start = offsetof(mp_state_ctx_t, thread.dict_locals);
    size_t root_end = offsetof(mp_state_ctx_t, vm.qstr_last_chunk);
    gc_collect_root(ptrs + root_start / sizeof(void *), (root_end - root_start) / sizeof(void *));

    #if MICROPY_ENABLE_PYSTACK
    // Trace root pointers from the Python stack.
    ptrs = (void **)(void *)MP_STATE_THREAD(pystack_start);
    gc_collect_root(ptrs, (MP_STATE_THREAD(pystack_cur) - MP_STATE_THREAD(pystack_start)) / sizeof(void *));
    #endif
}

static void gc_collect_start_common(void) {
    GC_ENTER();
    assert((MP_STATE_THREAD(gc_lock_depth) & GC_COLLECT_FLAG) == 0);
    MP_STATE_THREAD(gc_lock_depth) |= GC_COLLECT_FLAG;
    MP_STATE_MEM(gc_stack_overflow) = 0;
    gc_collecting_flag = true;
}

void gc_collect_root(void **ptrs, size_t len) {
    #if !MICROPY_GC_SPLIT_HEAP
    mp_state_mem_area_t *area = &MP_STATE_MEM(area);
    #endif
    for (size_t i = 0; i < len; i++) {
        MICROPY_GC_HOOK_LOOP(i);
        void *ptr = gc_get_ptr(ptrs, i);
        #if MICROPY_GC_SPLIT_HEAP
        mp_state_mem_area_t *area = gc_verify_ptr_get_area(ptr);
        if (!area) {
            continue;
        }
        #else
        if (!VERIFY_PTR(ptr)) {
            continue;
        }
        #endif
        size_t block = BLOCK_FROM_PTR(area, ptr);
        if (ATB_GET_KIND(area, block) == AT_HEAD) {
            // An unmarked head: mark it, and mark all its children
            TRACE_MARK_R(ptr_block, ptr);
            ATB_HEAD_TO_MARK(area, block);
            #if MICROPY_GC_INCREMENTAL
            if (gc_inc_mark_only) {
                // Root scan of an incremental cycle: just queue for later
                // slice-wise scanning, don't descend recursively here.
                // Must tag gray as well as queue: if the gray set later
                // overflows and is dropped, the resolver walk finds blocks
                // by the gray bit, and an untagged root would be skipped --
                // its children would then never be marked.
                ATB_SET_GRAY(area, block);
                gc_inc_gray_push(area, block);
                continue;
            }
            #endif
            if((ATB_GET_KIND_FULL(area, block)&4) == 0){

            #if MICROPY_GC_SPLIT_HEAP
              gc_mark_subtree(area, block);
            #else
            gc_mark_subtree(block);
            #endif
            }else{
              //printf("root HEAD_DATA found, skip scanning\n");
            }
        }
    }
}

// Take the given block as the topmost block on the stack. Check all it's
// children: mark the unmarked child blocks and put those newly marked
// blocks on the stack. When all children have been checked, pop off the
// topmost block on the stack and repeat with that one.
#if MICROPY_GC_SPLIT_HEAP
static void gc_mark_subtree(mp_state_mem_area_t *area, size_t block)
#else
static void gc_mark_subtree(size_t block)
#endif
{
    // Start with the block passed in the argument.
    size_t sp = 0;
    for (;;) {
        #if !MICROPY_GC_SPLIT_HEAP
        mp_state_mem_area_t *area = &MP_STATE_MEM(area);
        #endif

        // work out number of consecutive blocks in the chain starting with this one
        size_t n_blocks = 0;

        if ((ATB_GET_KIND_FULL(area, block)&4)) {
          n_blocks = 0;
        }else{
        do {
          n_blocks += 1;
          //if(n_blocks > 500){ n_blocks = 2; break; }
        } while (ATB_GET_KIND(area, block + n_blocks) == AT_TAIL);
        }
        //if(n_blocks > 1000){
        //  printf("bigone %d\n", n_blocks);
        //}

        // check that the consecutive blocks didn't overflow past the end of the area
        assert(area->gc_pool_start + (block + n_blocks) * BYTES_PER_BLOCK <= area->gc_pool_end);

        // check this block's children
        void **ptrs = (void **)PTR_FROM_BLOCK(area, block);
        for (size_t i = n_blocks * (BYTES_PER_BLOCK / sizeof(void *)); i > 0; i--, ptrs++) {
            MICROPY_GC_HOOK_LOOP(i);
            void *ptr = *ptrs;
            // If this is a heap pointer that hasn't been marked, mark it and push
            // it's children to the stack.
            #if MICROPY_GC_SPLIT_HEAP
            mp_state_mem_area_t *ptr_area = gc_verify_ptr_get_area(ptr);
            if (!ptr_area) {
                // Not a heap-allocated pointer (might even be random data).
                continue;
            }
            #else
            if (!VERIFY_PTR(ptr)) {
                continue;
            }
            mp_state_mem_area_t *ptr_area = area;
            #endif
            size_t ptr_block = BLOCK_FROM_PTR(ptr_area, ptr);
            if (ATB_GET_KIND(ptr_area, ptr_block) != AT_HEAD) {
                // This block is already marked.
                continue;
            }
            // An unmarked head. Mark it, and push it on gc stack.
            TRACE_MARK_S(ptr_block, ptr);
            ATB_HEAD_TO_MARK(ptr_area, ptr_block);
            if ((ATB_GET_KIND_FULL(ptr_area, ptr_block)&4) == 0) {
            if (sp < MICROPY_ALLOC_GC_STACK_SIZE) {
                MP_STATE_MEM(gc_block_stack)[sp] = ptr_block;
                #if MICROPY_GC_SPLIT_HEAP
                MP_STATE_MEM(gc_area_stack)[sp] = ptr_area;
                #endif
                sp += 1;
            } else {
                MP_STATE_MEM(gc_stack_overflow) = 1;
                //printf("gc stack overflow\n");
            }
            }
        }
        /*
        if(n_blocks >3000){
          if((!found)){// || n_blocks > 1000){
            //printf("data: %d, %d\n", block, n_blocks);
            if ((ATB_GET_KIND_FULL(area, block)&8)) {
              //printf("data: %d, %d\n", block, n_blocks);
              ATB_MARK_TO_HEAD_DATA(area, block);
            }else{
              //printf("candidate: %d, %d\n", block, n_blocks);
              //ATB_FREE_TO_HEAD_DATA_CANDIDATE(area, block);
            }
          }else{
            //printf("ptr: %d, %d\n", block, n_blocks);
            if ((ATB_GET_KIND_FULL(area,block)&8)) {
              //ATB_REMOVE_HEAD_DATA_CANDIDATE(area, block);
            }
          }
        }
        */

        // Are there any blocks on the stack?
        if (sp == 0) {
            break; // No, stack is empty, we're done.
        }

        // pop the next block off the stack
        sp -= 1;
        block = MP_STATE_MEM(gc_block_stack)[sp];
        #if MICROPY_GC_SPLIT_HEAP
        area = MP_STATE_MEM(gc_area_stack)[sp];
        #endif
    }
}

void gc_sweep_all(void) {
    #if MICROPY_GC_INCREMENTAL
    // Called e.g. on soft reset with an incremental cycle possibly in progress:
    // drop the cycle and turn partial marks back into heads so that the sweep
    // below really frees everything.
    gc_incremental_abort();
    #endif
    gc_collect_start_common();
    gc_collect_end();
}

void gc_collect_end(void) {
    gc_deal_with_stack_overflow();
    gc_sweep_run_finalisers();
    gc_sweep_free_blocks();
    #if MICROPY_GC_SPLIT_HEAP
    MP_STATE_MEM(gc_last_free_area) = &MP_STATE_MEM(area);
    #endif
    for (mp_state_mem_area_t *area = &MP_STATE_MEM(area); area != NULL; area = NEXT_AREA(area)) {
        area->gc_last_free_atb_index = 0;
    }
    MP_STATE_THREAD(gc_lock_depth) &= ~GC_COLLECT_FLAG;
    gc_collecting_flag = false;
    GC_EXIT();
    #if MICROPY_PY_WEAKREF
    gc_weakref_sweep();
    #endif
}

static void gc_deal_with_stack_overflow(void) {
    while (MP_STATE_MEM(gc_stack_overflow)) {
        MP_STATE_MEM(gc_stack_overflow) = 0;

        // scan entire memory looking for blocks which have been marked but not their children
        for (mp_state_mem_area_t *area = &MP_STATE_MEM(area); area != NULL; area = NEXT_AREA(area)) {
            for (size_t block = 0; block < area->gc_alloc_table_byte_len * BLOCKS_PER_ATB; block++) {
                MICROPY_GC_HOOK_LOOP(block);
                // trace (again) if mark bit set
                if (ATB_GET_KIND(area, block) == AT_MARK) {
                    #if MICROPY_GC_SPLIT_HEAP
                    gc_mark_subtree(area, block);
                    #else
                    gc_mark_subtree(block);
                    #endif
                }
            }
        }
    }
}

// Run finalisers for all to-be-freed blocks
static void gc_sweep_run_finalisers(void) {
    #if MICROPY_ENABLE_FINALISER || MICROPY_PY_WEAKREF
    #if MICROPY_ENABLE_FINALISER && MICROPY_PY_WEAKREF
    MP_STATIC_ASSERT(BLOCKS_PER_FTB == BLOCKS_PER_WTB);
    #endif
    for (const mp_state_mem_area_t *area = &MP_STATE_MEM(area); area != NULL; area = NEXT_AREA(area)) {
        assert(area->gc_last_used_block <= area->gc_alloc_table_byte_len * BLOCKS_PER_ATB);
        // Small speed optimisation: skip over empty FTB blocks
        size_t ftb_end = area->gc_last_used_block / BLOCKS_PER_FTB; // index is inclusive
        for (size_t ftb_idx = 0; ftb_idx <= ftb_end; ftb_idx++) {
            #if MICROPY_ENABLE_FINALISER
            byte ftb = area->gc_finaliser_table_start[ftb_idx];
            size_t block = ftb_idx * BLOCKS_PER_FTB;
            while (ftb) {
                MICROPY_GC_HOOK_LOOP(block);
                if (ftb & 1) { // FTB_GET(area, block) shortcut
                    if (ATB_GET_KIND(area, block) == AT_HEAD) {
                        mp_obj_base_t *obj = (mp_obj_base_t *)PTR_FROM_BLOCK(area, block);
                        if (obj->type != NULL) {
                            // if the object has a type then see if it has a __del__ method
                            mp_obj_t dest[2];
                            mp_load_method_maybe(MP_OBJ_FROM_PTR(obj), MP_QSTR___del__, dest);
                            if (dest[0] != MP_OBJ_NULL) {
                                // load_method returned a method, execute it in a protected environment
                                #if MICROPY_ENABLE_SCHEDULER
                                mp_sched_lock();
                                #endif
                                mp_call_function_1_protected(dest[0], dest[1]);
                                #if MICROPY_ENABLE_SCHEDULER
                                mp_sched_unlock();
                                #endif
                            }
                        }
                        // clear finaliser flag
                        FTB_CLEAR(area, block);
                    }
                }
                ftb >>= 1;
                block++;
            }
            #endif
            #if MICROPY_PY_WEAKREF
            byte wtb = area->gc_weakref_table_start[ftb_idx];
            block = ftb_idx * BLOCKS_PER_WTB;
            while (wtb) {
                MICROPY_GC_HOOK_LOOP(block);
                if (wtb & 1) { // WTB_GET(area, block) shortcut
                    if (ATB_GET_KIND(area, block) == AT_HEAD) {
                        mp_obj_base_t *obj = (mp_obj_base_t *)PTR_FROM_BLOCK(area, block);
                        gc_weakref_about_to_be_freed(obj);
                        WTB_CLEAR(area, block);
                    }
                }
                wtb >>= 1;
                block++;
            }
            #endif
        }
    }
    #endif // MICROPY_ENABLE_FINALISER || MICROPY_PY_WEAKREF
}

// Free unmarked heads and their tails
static void gc_sweep_free_blocks(void) {
    #if MICROPY_PY_GC_COLLECT_RETVAL
    MP_STATE_MEM(gc_collected) = 0;
    #endif
    int free_tail = 0;
    #if MICROPY_GC_SPLIT_HEAP_AUTO
    mp_state_mem_area_t *prev_area = NULL;
    #endif

    for (mp_state_mem_area_t *area = &MP_STATE_MEM(area); area != NULL; area = NEXT_AREA(area)) {
      size_t last_used_block = 0;
        assert(area->gc_last_used_block <= area->gc_alloc_table_byte_len * BLOCKS_PER_ATB);

        for (size_t block = 0; block <= area->gc_last_used_block; block++) {
            MICROPY_GC_HOOK_LOOP(block);
            //If the entire byte of ATB is tail,
            if ((block & 7) == 0 && area->gc_alloc_table_start_word[block >> 3] == 0) {
                // see the matching skip in gc_inc_sweep_step()
                block += 7; // the for-loop adds the 8th
                continue;
            }
            if((block&7) == 0){
              if(ATB_IF_ALL_WORD_IS_TAIL(area, block)){
                //printf("hit\n");
                if(free_tail){
                  ATB_ANY_TO_FREE_BYTE(area, block);
                  ATB_ANY_TO_FREE_BYTE(area, block+2);
                  ATB_ANY_TO_FREE_BYTE(area, block+4);
                  ATB_ANY_TO_FREE_BYTE(area, block+6);
                }
                block+=7;
                if(!free_tail){
                  last_used_block = block;
                }
                continue;
              }
            }
            if((block&1) == 0){
              if(ATB_IF_ALL_BYTE_IS_TAIL(area, block)){
                if(free_tail) ATB_ANY_TO_FREE_BYTE(area, block);
                block++;
                if(!free_tail){
                  last_used_block = block;
                }
                continue;
              }
            }
            switch (ATB_GET_KIND(area, block)) {
                case AT_HEAD:
                    free_tail = 1;
                    DEBUG_printf("gc_sweep_free_blocks(%p)\n", (void *)PTR_FROM_BLOCK(area, block));
                    #if MICROPY_PY_GC_COLLECT_RETVAL
                    MP_STATE_MEM(gc_collected)++;
                    #endif
                    // fall through to free the head
                    MP_FALLTHROUGH

                case AT_TAIL:
                    if (free_tail) {
                      //while(ATB_GET_KIND(area, block) == AT_TAIL && block <= area->gc_last_used_block){
                        ATB_ANY_TO_FREE(area, block);
                        //block++;
                        //}
                      //block--;
                      //ATB_ANY_TO_FREE(area, block);
                        #if CLEAR_ON_SWEEP
                        memset((void *)PTR_FROM_BLOCK(area, block), 0, BYTES_PER_BLOCK);
                        #endif
                    } else {
                        last_used_block = block;
                    }
                    break;

                case AT_MARK:
                  ATB_MARK_TO_HEAD(area, block);
                    free_tail = 0;
                    last_used_block = block;
                    break;
            }
        }

        area->gc_last_used_block = last_used_block;

        #if MICROPY_GC_SPLIT_HEAP_AUTO
        // Free any empty area, aside from the first one
        if (last_used_block == 0 && prev_area != NULL) {
            DEBUG_printf("gc_sweep_free_blocks free empty area %p\n", area);
            NEXT_AREA(prev_area) = NEXT_AREA(area);
            MP_PLAT_FREE_HEAP(area);
            area = prev_area;
        }
        prev_area = area;
        #endif
    }
}

#if MICROPY_GC_INCREMENTAL
// ---------------------------------------------------------------------------
// Incremental cycle machinery.
//
// Conservative scan of one marked block chain, pushing discovered heads onto
// the gray set.  Costs `n_words` against *budget (whole chains are scanned
// atomically -- chains are small except for data blocks, whose contents are
// not scanned at all).  Same pointer test logic as gc_mark_subtree().
static bool gc_inc_final_drain; // true while the end-of-cycle STW drain runs

static void gc_inc_scan_chain(mp_state_mem_area_t *area, size_t block, uint32_t *budget) {
    size_t n_blocks = 0;
    ATB_CLEAR_GRAY(area, block); // scanning it now -> black
    if (ATB_GET_KIND_FULL(area, block) & 4) {
        // data block: contents are not scanned for pointers (same rule as the
        // classic collector's IS_DATA allocation flag)
        return;
    }
    do {
        n_blocks += 1;
    } while (ATB_GET_KIND(area, block + n_blocks) == AT_TAIL);
    assert(area->gc_pool_start + (block + n_blocks) * BYTES_PER_BLOCK <= area->gc_pool_end);

    void **ptrs = (void **)PTR_FROM_BLOCK(area, block);
    size_t n_words = n_blocks * WORDS_PER_BLOCK;
    gc_inc_stats[GC_INC_ST_WORDS] += n_words;
    if (gc_inc_final_drain) {
        gc_inc_stats[GC_INC_ST_FINAL_WORDS] += n_words;
    }
    for (size_t i = 0; i < n_words; i++, ptrs++) {
        MICROPY_GC_HOOK_LOOP(i);
        void *ptr = *ptrs;
        #if MICROPY_GC_SPLIT_HEAP
        mp_state_mem_area_t *ptr_area = gc_verify_ptr_get_area(ptr);
        if (!ptr_area) {
            continue;
        }
        #else
        if (!VERIFY_PTR(ptr)) {
            continue;
        }
        mp_state_mem_area_t *ptr_area = area;
        #endif
        size_t ptr_block = BLOCK_FROM_PTR(ptr_area, ptr);
        if (ATB_GET_KIND(ptr_area, ptr_block) != AT_HEAD) {
            // already marked / tail / interior pointer
            continue;
        }
        gc_inc_mark_gray(ptr_area, ptr_block);
    }

    if (n_words > *budget) {
        *budget = 0;
    } else {
        *budget -= n_words;
    }
}

// Drain the gray set until budget exhausted.  Returns true when marking is
// complete (gray set empty and no overflow pending).
//
// Overflow handling is itself incremental: when the gray set overflows the
// queue is dropped (all its entries are AT_MARK blocks and remain discoverable
// by a table walk) and a budgeted, cursor-based table walk takes over until
// one full clean pass completes.  Unlike the classic resolver, which repeats
// an unbounded full walk per overflow, this never restarts mid-pass, so the
// walk cost is spread across slices like normal mark work.
static mp_state_mem_area_t *gc_inc_walk_area;
static size_t gc_inc_walk_block;
static bool gc_inc_walk_pending;

static bool gc_inc_drain(uint32_t max_words) {
    uint32_t budget = max_words;
    #if MICROPY_GC_USTIME_AVAILABLE
    uint32_t t0 = gc_inc_now_us();
    #endif
    uint32_t iters = 0;
    for (;;) {
        while (gc_inc_gray_sp > 0 && budget > 0) {
            // Periodic timing check.  This must count iterations: gating on
            // gc_inc_gray_sp meant gating on the *stack depth*, which rises
            // and falls as children are pushed, so a traversal whose depth
            // never lands on a multiple of 8 at the top of the loop skipped
            // the cap entirely and the slice ran unbounded.
            if ((++iters & 7) == 0 && gc_inc_over_cap(t0)) {
                return false;
            }
            gc_inc_gray_sp -= 1;
            size_t block = MP_STATE_MEM(gc_block_stack)[gc_inc_gray_sp];
            #if MICROPY_GC_SPLIT_HEAP
            mp_state_mem_area_t *area = MP_STATE_MEM(gc_area_stack)[gc_inc_gray_sp];
            #else
            mp_state_mem_area_t *area = &MP_STATE_MEM(area);
            #endif
            gc_inc_scan_chain(area, block, &budget);
        }
        if (MP_STATE_MEM(gc_stack_overflow)) {
            // Gray set overflowed: drop it (entries are all AT_MARK blocks that
            // the walk below will rediscover) and start a full clean pass.
            MP_STATE_MEM(gc_stack_overflow) = 0;
            gc_inc_gray_sp = 0;
            gc_inc_stats[GC_INC_ST_OVERFLOWS] += 1;
            gc_inc_walk_pending = true;
            gc_inc_walk_area = &MP_STATE_MEM(area);
            gc_inc_walk_block = 0;
        }
        if (gc_inc_walk_pending) {
            // Budgeted table walk: scan every AT_MARK block from the cursor.
            // Scanning an already-scanned block is harmless (idempotent).
            bool pass_done = false;
            while (gc_inc_walk_area != NULL && budget > 0) {
                mp_state_mem_area_t *area = gc_inc_walk_area;
                size_t block = gc_inc_walk_block;
                size_t block_end = area->gc_alloc_table_byte_len * BLOCKS_PER_ATB;
                for (; block < block_end && budget > 0; block++) {
                    MICROPY_GC_HOOK_LOOP(block);
                    gc_inc_stats[GC_INC_ST_WALK_BLOCKS] += 1;
                    if ((block & 31) == 0 && gc_inc_over_cap(t0)) {
                        budget = 0; // must also end the outer walk loop
                        break;
                    }
                    if (ATB_GET_KIND(area, block) == AT_MARK && ATB_IS_GRAY(area, block)) {
                        gc_inc_scan_chain(area, block, &budget);
                    }
                }
                if (block >= block_end) {
                    #if MICROPY_GC_SPLIT_HEAP
                    gc_inc_walk_area = NEXT_AREA(area);
                    #else
                    gc_inc_walk_area = NULL;
                    #endif
                    gc_inc_walk_block = 0;
                } else {
                    gc_inc_walk_block = block;
                }
                if (MP_STATE_MEM(gc_stack_overflow)) {
                    // Overflowed again during the walk: restart a clean pass
                    // (progress is guaranteed: marking only ever grows).
                    break;
                }
            }
            if (!MP_STATE_MEM(gc_stack_overflow) && gc_inc_walk_area == NULL) {
                gc_inc_walk_pending = false;
                pass_done = true;
            }
            if (pass_done) {
                continue; // gray set may now have entries; keep draining
            }
            return false; // budget exhausted (or new overflow), resume later
        }
        return gc_inc_gray_sp == 0;
    }
}

// Scan the complete root set (statics, pystack, C stacks, other threads).
static void gc_inc_scan_all_roots(void) {
    #if MICROPY_GC_USTIME_AVAILABLE
    uint32_t t0 = (uint32_t)MICROPY_GC_PORT_USTIME();
    #endif
    gc_inc_mark_only = true;
    gc_inc_scan_static_roots();
    MICROPY_GC_PORT_STACK_ROOTS();
    gc_inc_mark_only = false;
    #if MICROPY_GC_USTIME_AVAILABLE
    gc_inc_stats[GC_INC_ST_ROOTS_US] += (uint32_t)MICROPY_GC_PORT_USTIME() - t0;
    #endif
}

// One budgeted slice of the table free-walk.  Resumable mirror of
// gc_sweep_free_blocks(): frees unmarked chains, demotes AT_MARK survivors to
// AT_HEAD, and treats blocks tagged "born during sweep" (ATB top bit, set by
// gc_alloc/gc_realloc while phase==SWEEP) as live -- also breaking any
// in-progress free_tail chain so a new object's tail blocks are never eaten.
// Returns true when every area has been fully swept.
static bool gc_inc_sweep_step(uint32_t budget_blocks) {
    int32_t budget = (int32_t)(budget_blocks > 0x40000000u ? 0x40000000u : budget_blocks);
    #if MICROPY_GC_USTIME_AVAILABLE
    uint32_t t0 = (uint32_t)MICROPY_GC_PORT_USTIME();
    #endif
    while (gc_inc_sw_area != NULL && budget > 0) {
        mp_state_mem_area_t *area = gc_inc_sw_area;
        size_t block = gc_inc_sw_block;
        int free_tail = gc_inc_sw_free_tail;
        size_t last_used = gc_inc_sw_last_used;
        while (block <= area->gc_last_used_block && budget > 0) {
            MICROPY_GC_HOOK_LOOP(block);
            if ((block & 31) == 0 && gc_inc_over_cap(t0)) {
                budget = 0; // must also end the OUTER loop (state saved below)
                break;
            }
            // 8-block skip over an all-free ATB word.  Without this the sweep
            // visits every free block individually, so a heap that has just
            // become mostly garbage (big app quit) costs a full block-by-block
            // walk of the whole heap on *every* cycle -- the dominant sweep
            // cost on a large PSRAM heap.  A zero word means 8 x AT_FREE with
            // no flag bits, so there is nothing to do but step over it.
            if ((block & 7) == 0 && area->gc_alloc_table_start_word[block >> 3] == 0) {
                block += 8;
                budget -= 8;
                gc_inc_sw_free += 8;
                continue;
            }
            // same 8-block (word) fast skip as the classic sweep
            if ((block & 7) == 0) {
                if (ATB_IF_ALL_WORD_IS_TAIL(area, block) && !gc_inc_tag_word8(area, block)) {
                    if (free_tail) {
                        ATB_ANY_TO_FREE_BYTE(area, block);
                        ATB_ANY_TO_FREE_BYTE(area, block + 2);
                        ATB_ANY_TO_FREE_BYTE(area, block + 4);
                        ATB_ANY_TO_FREE_BYTE(area, block + 6);
                        gc_inc_sw_free += 8;
                    }
                    block += 8;
                    budget -= 8;
                    if (!free_tail) {
                        last_used = block - 1;
                    }
                    continue;
                }
            }
            // same 2-block (byte) fast skip
            if ((block & 1) == 0) {
                if (ATB_IF_ALL_BYTE_IS_TAIL(area, block) && !gc_inc_tag_byte8(area, block)) {
                    if (free_tail) {
                        ATB_ANY_TO_FREE_BYTE(area, block);
                        gc_inc_sw_free += 2;
                    }
                    block += 2;
                    budget -= 2;
                    if (!free_tail) {
                        last_used = block - 1;
                    }
                    continue;
                }
            }
            if (ATB_GET_KIND_FULL(area, block) & 8) {
                // Allocated during this sweep: live for the next cycle.
                // The tag MUST be cleared here, and an AT_MARK left over from
                // a previous cycle demoted, or the block stays permanently
                // tagged: the next mark phase then skips it (kind != AT_HEAD)
                // and its children are collected while it still points at them.
                area->gc_alloc_table_start[block >> 1] &= ~(8 << BLOCK_SHIFT(block));
                if (ATB_GET_KIND(area, block) == AT_MARK) {
                    ATB_MARK_TO_HEAD(area, block);
                }
                free_tail = 0;
                last_used = block;
                block += 1;
                budget -= 1;
                continue;
            }
            switch (ATB_GET_KIND(area, block)) {
                case AT_FREE:
                    gc_inc_sw_free += 1;
                    break;
                case AT_HEAD:
                    free_tail = 1;
                    MP_FALLTHROUGH
                case AT_TAIL:
                    if (free_tail) {
                        ATB_ANY_TO_FREE(area, block);
                        gc_inc_sw_free += 1;
                    } else {
                        last_used = block;
                    }
                    break;
                case AT_MARK:
                    ATB_MARK_TO_HEAD(area, block);
                    free_tail = 0;
                    last_used = block;
                    break;
            }
            block += 1;
            budget -= 1;
        }
        gc_inc_sw_block = block;
        gc_inc_sw_free_tail = free_tail;
        gc_inc_sw_last_used = last_used;
        if (block <= area->gc_last_used_block) {
            #if MICROPY_GC_USTIME_AVAILABLE
            uint32_t dur = gc_inc_now_us() - t0;
            gc_inc_stats[GC_INC_ST_WALK_US] += dur;
            gc_inc_stats[GC_INC_ST_SWEEP_US] += dur;
            if (dur > gc_inc_stats[GC_INC_ST_MAX_SWEEP_US]) {
                gc_inc_stats[GC_INC_ST_MAX_SWEEP_US] = dur;
            }
            #endif
            return false; // budget exhausted mid-area, resume next slice
        }
        // this area is fully swept: its space is now fully available
        area->gc_last_used_block = last_used;
        area->gc_last_free_atb_index = 0;
        #if MICROPY_GC_SPLIT_HEAP
        mp_state_mem_area_t *next_area;
        #if MICROPY_GC_SPLIT_HEAP_AUTO
        if (last_used == 0 && gc_inc_sw_prev_area != NULL) {
            NEXT_AREA(gc_inc_sw_prev_area) = NEXT_AREA(area);
            // This area pointer dies here, but unlike the classic STW sweep,
            // mutators run between slices: any cached pointer to this area
            // (notably gc_last_free_area, used as the alloc starting point)
            // would let a later allocation scribble into PSRAM already given
            // back to the OS.  Make sure no such pointer survives.
            if (MP_STATE_MEM(gc_last_free_area) == area) {
                MP_STATE_MEM(gc_last_free_area) = &MP_STATE_MEM(area);
            }
            MP_PLAT_FREE_HEAP(area);
            next_area = NEXT_AREA(gc_inc_sw_prev_area);
        } else {
            gc_inc_sw_prev_area = area;
            next_area = NEXT_AREA(area);
        }
        #else
        next_area = NEXT_AREA(area);
        #endif
        gc_inc_sw_area = next_area;
        #else
        gc_inc_sw_area = NULL;
        #endif
        gc_inc_sw_block = 0;
        gc_inc_sw_free_tail = 0;
        gc_inc_sw_last_used = 0;
    }
    #if MICROPY_GC_USTIME_AVAILABLE
    uint32_t dur = gc_inc_now_us() - t0;
    gc_inc_stats[GC_INC_ST_WALK_US] += dur;
    gc_inc_stats[GC_INC_ST_SWEEP_US] += dur;
    if (dur > gc_inc_stats[GC_INC_ST_MAX_SWEEP_US]) {
        gc_inc_stats[GC_INC_ST_MAX_SWEEP_US] = dur;
    }
    #endif
    return gc_inc_sw_area == NULL;
}

// Sweep fully done: publish the new free-space state and adapt the trigger.
static void gc_inc_sweep_done_locked(void) {
    mp_gc_mark_active = GC_INC_PHASE_IDLE;
    #if MICROPY_GC_SPLIT_HEAP
    MP_STATE_MEM(gc_last_free_area) = &MP_STATE_MEM(area);
    #endif
    #if MICROPY_GC_ALLOC_THRESHOLD
    // Adaptive trigger: next cycle starts once FACTOR times the blocks
    // allocated during this window have been allocated again (clamped).
    size_t amt = MP_STATE_MEM(gc_alloc_amount);
    MP_STATE_MEM(gc_alloc_amount) = 0;
    size_t thr = MICROPY_GC_INCR_TRIGGER_FACTOR * amt;
    // Upper clamp, and it is not optional: the window that just ended usually
    // ended *because the heap ran out*, so `amt` is roughly all the free space
    // there was and FACTOR * amt exceeds the whole heap.  Without a ceiling the
    // trigger ratchets past the heap size after the first collection and never
    // fires again -- every later collection then comes from allocation failure,
    // i.e. a fully synchronous stop-the-world cycle.  A cycle also has to
    // *finish* inside the remaining free space, so cap on that.
    // The ceiling must key off *free* space, not total heap: a cycle has to
    // finish inside the space that is still available, so starting one after
    // "heap/8" of allocation is far too late once the live set is large.
    // gc_inc_sw_free is the block count this sweep just reclaimed//found free.
    size_t ceiling = gc_inc_sw_free / MICROPY_GC_INCR_TRIGGER_DIVISOR;
    if (thr > ceiling) {
        thr = ceiling;
    }
    MP_STATE_MEM(gc_alloc_threshold) = MAX(thr, (size_t)MICROPY_GC_INCR_TRIGGER_MIN);
    #endif
}

#ifndef GC_INC_VERIFY
#define GC_INC_VERIFY (0)
#endif
#if GC_INC_VERIFY
// Debug: after the final drain every live (AT_MARK) block must have been
// scanned, so no word inside one may point at an unmarked (AT_HEAD) block.
// Any hit is a store that reached a black object without a write barrier --
// the pointed-to object is about to be swept while still referenced.
// Best-effort: name the type of a heap block, for verifier output.  Object
// types are static (never in the GC heap), so a word-0 pointer that lands
// outside the heap and has an in-range qstr name is almost certainly a real
// mp_obj_base_t.  Anything else prints as "?".
static const char *gc_verify_typename(void *blk) {
    mp_obj_base_t *o = (mp_obj_base_t *)blk;
    const mp_obj_type_t *t = o->type;
    if (t == NULL || ((uintptr_t)t & (sizeof(void *) - 1)) != 0) {
        return "?";
    }
    if (gc_verify_ptr_get_area((const void *)t) != NULL) {
        return "?"; // in the heap => not a static type
    }
    qstr n = t->name;
    if (n == 0 || n >= QSTR_TOTAL()) {
        return "?";
    }
    return qstr_str(n);
}

static void gc_inc_verify(void) {
    size_t bad = 0;
    for (mp_state_mem_area_t *area = &MP_STATE_MEM(area); area != NULL; area = NEXT_AREA(area)) {
        for (size_t block = 0; block <= area->gc_last_used_block; block++) {
            if (ATB_GET_KIND(area, block) != AT_MARK || (ATB_GET_KIND_FULL(area, block) & 4)) {
                continue;
            }
            size_t nb = 0;
            do {
                nb += 1;
            } while (ATB_GET_KIND(area, block + nb) == AT_TAIL);
            void **ptrs = (void **)PTR_FROM_BLOCK(area, block);
            for (size_t i = 0; i < nb * WORDS_PER_BLOCK; i++) {
                void *p = ptrs[i];
                mp_state_mem_area_t *pa = gc_verify_ptr_get_area(p);
                if (pa && ATB_GET_KIND(pa, BLOCK_FROM_PTR(pa, p)) == AT_HEAD) {
                    if (bad < 8) {
                        printf("GC-VERIFY: live <%s> block %p word %d -> unmarked <%s> %p\n",
                            gc_verify_typename((void *)PTR_FROM_BLOCK(area, block)),
                            (void *)PTR_FROM_BLOCK(area, block), (int)i,
                            gc_verify_typename(p), p);
                    }
                    bad += 1;
                }
            }
            block += nb - 1;
        }
    }
    if (bad) {
        printf("GC-VERIFY: %d missed-barrier references\n", (int)bad);
        fflush(stdout);
        abort();
    }
}
#endif

// Marking has finished: final (atomic) root rescan, complete the drain, run
// finalisers in one stop-the-world piece (classic semantics: __del__ sees the
// pre-free heap), then arm the sliced sweep carried by later step() calls.
static void gc_inc_finish_locked(void) {
    #if MICROPY_GC_USTIME_AVAILABLE
    uint32_t tf0 = (uint32_t)MICROPY_GC_PORT_USTIME();
    #endif
    gc_inc_scan_all_roots();
    #if MICROPY_GC_USTIME_AVAILABLE
    uint32_t td0 = gc_inc_now_us();
    #endif
    gc_inc_final_drain = true;
    while (!gc_inc_drain(UINT32_MAX)) {
        // drain may cut itself off at the per-slice time cap; in the
        // synchronous finish we simply re-enter (state is resumable)
    }
    gc_inc_final_drain = false;
    #if MICROPY_GC_USTIME_AVAILABLE
    uint32_t td = gc_inc_now_us() - td0;
    gc_inc_stats[GC_INC_ST_DRAIN_US] += td;
    if (td > gc_inc_stats[GC_INC_ST_MAX_DRAIN_US]) {
        gc_inc_stats[GC_INC_ST_MAX_DRAIN_US] = td;
    }
    #endif

    #if GC_INC_VERIFY
    gc_inc_verify();
    #endif

    // (caller wraps gc_collecting_flag; the lock flag here keeps finaliser
    // code from re-entering a classic collect)
    MP_STATE_THREAD(gc_lock_depth) |= GC_COLLECT_FLAG;
    #if MICROPY_GC_USTIME_AVAILABLE
    uint32_t tfm = gc_inc_now_us(); // end of final drain, start of finalisers
    #endif
    gc_sweep_run_finalisers();
    MP_STATE_THREAD(gc_lock_depth) &= ~GC_COLLECT_FLAG;
    #if MICROPY_PY_WEAKREF
    gc_weakref_sweep();
    #endif
    #if MICROPY_GC_USTIME_AVAILABLE
    uint32_t tf1 = gc_inc_now_us();
    uint32_t dur = tf1 - tf0;                    // roots + final drain + finalisers
    gc_inc_stats[GC_INC_ST_SWEEP_US] += dur;     // all part of the STW pieces
    gc_inc_stats[GC_INC_ST_FIN_US] += tf1 - tfm; // finalisers only
    (void)dur; // max_sweep_us now means "longest sweep slice" only; the finish
               // block is attributed to roots_us / drain_us / fin_us instead
    #endif

    // Arm the sliced sweep.  Start with the allocation cursor pinned to the
    // first area: the walk below may free other areas between slices, and
    // mutators (allocations) run in those gaps.
    MP_STATE_MEM(gc_last_free_area) = &MP_STATE_MEM(area);
    gc_inc_sw_area = &MP_STATE_MEM(area);
    gc_inc_sw_block = 0;
    gc_inc_sw_free_tail = 0;
    gc_inc_sw_last_used = 0;
    gc_inc_sw_free = 0;
    #if MICROPY_GC_SPLIT_HEAP_AUTO
    gc_inc_sw_prev_area = NULL;
    #endif
    mp_gc_mark_active = GC_INC_PHASE_SWEEP;
}

void gc_incremental_start(void) {
    gc_inc_stats[GC_INC_ST_START_CALLS] += 1;
    if (mp_gc_mark_active != 0 || !gc_inc_enabled) {
        gc_inc_stats[GC_INC_ST_START_BUSY] += 1;
        return;
    }
    GC_ENTER();
    gc_inc_in_step = true; // root scan must not be re-entered by a step
    gc_inc_stats[GC_INC_ST_CYCLES] += 1;
    gc_inc_gray_sp = 0;
    gc_inc_walk_pending = false;
    gc_inc_walk_area = NULL;
    gc_inc_walk_block = 0;
    MP_STATE_MEM(gc_stack_overflow) = 0;
    mp_gc_mark_active = 1;
    gc_inc_scan_all_roots();
    // Do only ONE slice of marking here.  If that finished the job (tiny live
    // set / empty gray set), complete now; otherwise the remaining work is
    // carried by subsequent gc_alloc() calls via gc_incremental_step().
    // (Draining with an unbounded budget here would make every cycle a
    // synchronous stop-the-world collection, defeating the whole point.)
    if (gc_inc_drain(gc_inc_slice_words)) {
        gc_inc_stats[GC_INC_ST_SLICE_ENDS] += 1;
        gc_inc_in_step = false;
        gc_collecting_flag = true; // finalisers are stop-the-world
        gc_inc_finish_locked();
        gc_collecting_flag = false;
    } else {
        gc_inc_stats[GC_INC_ST_SLICES] += 1;
    }
    #if MICROPY_GC_USTIME_AVAILABLE
    gc_inc_next_slice_us = gc_inc_now_us() + gc_inc_gap_us;
    #endif
    gc_inc_in_step = false;
    GC_EXIT();
}

static void gc_inc_step_inner(bool desperate) {
    uint8_t phase = mp_gc_mark_active;
    if (phase == GC_INC_PHASE_IDLE || gc_inc_in_step) {
        return;
    }
    gc_inc_in_step = true;
    if (phase == GC_INC_PHASE_MARK) {
        // A microsecond-clock read per allocation would tax gc_alloc too
        // much, so callers only invoke the step every few allocations;
        // when they do, still honour the wall-clock gap.  desperate=1 (an
        // allocation is about to fail) bypasses the gap: free memory now.
        if (!desperate && !gc_inc_due()) {
            gc_inc_stats[GC_INC_ST_STEP_NOTDUE] += 1;
            gc_inc_in_step = false;
            return; // pace: not enough wall-clock time since last slice
        }
        #if MICROPY_GC_USTIME_AVAILABLE
        uint32_t ts0 = gc_inc_now_us();
        #endif
        bool mark_done = gc_inc_drain(desperate ? UINT32_MAX : gc_inc_slice_words);
        #if MICROPY_GC_USTIME_AVAILABLE
        uint32_t ts = gc_inc_now_us() - ts0;
        gc_inc_stats[GC_INC_ST_MARK_US] += ts;
        if (ts > gc_inc_stats[GC_INC_ST_MAX_SLICE_US]) {
            gc_inc_stats[GC_INC_ST_MAX_SLICE_US] = ts;
        }
        #endif
        if (mark_done) {
            gc_inc_stats[GC_INC_ST_SLICE_ENDS] += 1;
            gc_inc_in_step = false;
            gc_collecting_flag = true; // finalisers are stop-the-world
            gc_inc_finish_locked();
            gc_collecting_flag = false;
        } else {
            gc_inc_stats[GC_INC_ST_SLICES] += 1;
        }
        gc_inc_next_slice_us = gc_inc_now_us() + gc_inc_gap_us;
    } else { // GC_INC_PHASE_SWEEP: sliced, not flagged as "interpreter stalled"
        if (!desperate && !gc_inc_due()) {
            gc_inc_in_step = false;
            return;
        }
        gc_inc_sweep_step(desperate ? UINT32_MAX : MICROPY_GC_INCR_SWEEP_SLICE_BLOCKS);
        if (mp_gc_mark_active == GC_INC_PHASE_SWEEP && gc_inc_sw_area == NULL) {
            gc_inc_sweep_done_locked();
        }
        gc_inc_next_slice_us = gc_inc_now_us() + gc_inc_gap_us;
    }
    gc_inc_in_step = false;
}

void gc_incremental_step(bool desperate) {
    gc_inc_stats[GC_INC_ST_STEP_CALLS] += 1;
    if (mp_gc_mark_active == GC_INC_PHASE_IDLE || gc_inc_in_step) {
        gc_inc_stats[GC_INC_ST_STEP_IDLE] += 1;
        return;
    }
    // Recursive lock: no-op when we already hold it via gc_alloc, but when
    // entered from other paths (allocation failure handling) it excludes
    // allocators that do not take the GIL (bare C tasks) from ATB updates
    // racing our mark/sweep slices.  The classic collector holds this mutex
    // across the whole cycle for exactly this reason.
    GC_ENTER();
    gc_inc_step_inner(desperate);
    GC_EXIT();
}

void gc_incremental_complete(void) {
    if (mp_gc_mark_active == GC_INC_PHASE_IDLE || gc_inc_in_step) {
        return;
    }
    gc_inc_in_step = true;
    gc_inc_stats[GC_INC_ST_SYNC_ENDS] += 1;
    gc_collecting_flag = true; // whole synchronous run is a stall for the port
    GC_ENTER(); // exclude non-GIL allocators from the whole sync run
    // Run whatever phases remain straight through (used on allocation
    // failure, gc.collect() and soft-reset; each transition terminates).
    while (mp_gc_mark_active != GC_INC_PHASE_IDLE) {
        if (mp_gc_mark_active == GC_INC_PHASE_MARK) {
            while (!gc_inc_drain(gc_inc_slice_words)) {
                // keep draining past the per-slice cap (no gap wait here:
                // this whole function is already a stop-the-world run)
            }
            gc_inc_finish_locked();
        } else { // GC_INC_PHASE_SWEEP
            do {
                gc_inc_sweep_step(MICROPY_GC_INCR_SWEEP_SLICE_BLOCKS * 4);
            } while (gc_inc_sw_area != NULL);
            gc_inc_sweep_done_locked();
        }
    }
    gc_collecting_flag = false;
    GC_EXIT();
    gc_inc_in_step = false;
}

void gc_incremental_abort(void) {
    if (mp_gc_mark_active == 0) {
        return;
    }
    gc_inc_stats[GC_INC_ST_ABORTS] += 1;
    mp_gc_mark_active = 0;
    gc_inc_gray_sp = 0;
    gc_inc_in_step = false;
    gc_inc_walk_pending = false;
    gc_inc_walk_area = NULL;
    gc_inc_sw_area = NULL;
    gc_inc_sw_block = 0;
    // Turn all partial marks back into heads so the subsequent sweep_all
    // really frees everything.
    for (mp_state_mem_area_t *area = &MP_STATE_MEM(area); area != NULL; area = NEXT_AREA(area)) {
        for (size_t block = 0; block <= area->gc_last_used_block; block++) {
            if (ATB_GET_KIND(area, block) == AT_MARK) {
                ATB_MARK_TO_HEAD(area, block);
            }
            area->gc_alloc_table_start[block >> 1] &= ~(8 << BLOCK_SHIFT(block));
        }
    }
}
#endif // MICROPY_GC_INCREMENTAL

// Address sanitizer needs to know that the access to ptrs[i] must always be
// considered OK, even if it's a load from an address that would normally be
// prohibited (due to being undefined, in a red zone, etc).
#if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 8))
__attribute__((no_sanitize_address))
#endif
static void *gc_get_ptr(void **ptrs, int i) {
    #if MICROPY_DEBUG_VALGRIND
    if (!VALGRIND_CHECK_MEM_IS_ADDRESSABLE(&ptrs[i], sizeof(*ptrs))) {
        return NULL;
    }
    #endif
    return ptrs[i];
}

void gc_info(gc_info_t *info) {
    GC_ENTER();
    info->total = 0;
    info->used = 0;
    info->free = 0;
    info->max_free = 0;
    info->num_1block = 0;
    info->num_2block = 0;
    info->max_block = 0;
    for (mp_state_mem_area_t *area = &MP_STATE_MEM(area); area != NULL; area = NEXT_AREA(area)) {
        bool finish = false;
        info->total += area->gc_pool_end - area->gc_pool_start;
        for (size_t block = 0, len = 0, len_free = 0; !finish;) {
            MICROPY_GC_HOOK_LOOP(block);
            size_t kind = ATB_GET_KIND(area, block);
            switch (kind) {
                case AT_FREE:
                    info->free += 1;
                    len_free += 1;
                    len = 0;
                    break;

                case AT_HEAD:
                    info->used += 1;
                    len = 1;
                    break;

                case AT_TAIL:
                    info->used += 1;
                    len += 1;
                    break;

                case AT_MARK:
                    // shouldn't happen
                    break;
            }

            block++;
            finish = (block == area->gc_alloc_table_byte_len * BLOCKS_PER_ATB);
            // Get next block type if possible
            if (!finish) {
                kind = ATB_GET_KIND(area, block);
            }

            if (finish || kind == AT_FREE || kind == AT_HEAD) {
                if (len == 1) {
                    info->num_1block += 1;
                } else if (len == 2) {
                    info->num_2block += 1;
                }
                if (len > info->max_block) {
                    info->max_block = len;
                }
                if (finish || kind == AT_HEAD) {
                    if (len_free > info->max_free) {
                        info->max_free = len_free;
                    }
                    len_free = 0;
                }
            }
        }
    }

    info->used *= BYTES_PER_BLOCK;
    info->free *= BYTES_PER_BLOCK;

    #if MICROPY_GC_SPLIT_HEAP_AUTO
    info->max_new_split = gc_get_max_new_split();
    #endif

    GC_EXIT();
}

// Fast version of gc_info that only computes total/used/free.
void gc_info_fast(gc_info_t *info) {
    GC_ENTER();
    memset(info, 0, sizeof(*info));
    for (mp_state_mem_area_t *area = &MP_STATE_MEM(area); area != NULL; area = NEXT_AREA(area)) {
        size_t free_blocks = 0;
        info->total += area->gc_pool_end - area->gc_pool_start;
        for (size_t i = 0; i < area->gc_alloc_table_byte_len; i++) {
            uint8_t atb = area->gc_alloc_table_start[i];
            free_blocks += ((atb & 0xF) == AT_FREE) + ((atb >> 4) == AT_FREE);
        }
        info->free += free_blocks;
    }
    info->free *= BYTES_PER_BLOCK;
    info->used = info->total - info->free;
    GC_EXIT();
}

#if MICROPY_PY_WEAKREF
// Mark the GC heap pointer as having a weakref.
void gc_weakref_mark(void *ptr) {
    mp_state_mem_area_t *area;
    #if MICROPY_GC_SPLIT_HEAP
    area = gc_get_ptr_area(ptr);
    assert(area);
    #else
    assert(VERIFY_PTR(ptr));
    area = &MP_STATE_MEM(area);
    #endif

    size_t block = BLOCK_FROM_PTR(area, ptr);
    assert(ATB_GET_KIND(area, block) == AT_HEAD);

    WTB_SET(area, block);
}
#endif

void *gc_alloc(size_t n_bytes, unsigned int alloc_flags) {
    bool has_finaliser = alloc_flags & GC_ALLOC_FLAG_HAS_FINALISER;
    size_t n_blocks = ((n_bytes + BYTES_PER_BLOCK - 1) & (~(BYTES_PER_BLOCK - 1))) / BYTES_PER_BLOCK;
    DEBUG_printf("gc_alloc(" UINT_FMT " bytes -> " UINT_FMT " blocks)\n", n_bytes, n_blocks);
    //if(alloc_flags & GC_ALLOC_FLAG_IS_DATA){
    //  printf("alloc_data %d\n", n_bytes);
    //}
    // check for 0 allocation
    if (n_blocks == 0) {
        return NULL;
    }

    // check if GC is locked
    if (MP_STATE_THREAD(gc_lock_depth) > 0) {
        return NULL;
    }

    GC_ENTER();

    #if MICROPY_GC_INCREMENTAL
    // Advance any in-progress incremental cycle by one bounded slice.
    // Sampled 1-in-8 allocations: the due-check's clock read would otherwise
    // add ~50% to gc_alloc's own cost for allocation-bound code.  Allocation
    // failures take the gc_collect() path below which completes the cycle,
    // so pressure is never deferred by this sampling.
    if ((++gc_inc_alloc_ctr & 7) == 0) {
        gc_incremental_step(false);
    }
    #endif

    #if MICROPY_GC_INCREMENTAL && MICROPY_GC_USTIME_AVAILABLE
    // Per-call stall accounting: the loop below can run the collector many
    // times for one allocation, and that sum is the number a frame feels.
    bool gc_stall_timed = false;
    uint32_t gc_stall_t0 = 0;
    uint32_t gc_stall_retries = 0;
    #endif

    mp_state_mem_area_t *area;
    size_t i;
    size_t end_block;
    size_t start_block;
    size_t n_free;
    int collected = !MP_STATE_MEM(gc_auto_collect_enabled);
    #if MICROPY_GC_SPLIT_HEAP_AUTO
    bool added = false;
    #endif

    #if MICROPY_GC_ALLOC_THRESHOLD
    if (!collected && MP_STATE_MEM(gc_alloc_amount) >= MP_STATE_MEM(gc_alloc_threshold)) {
        #if MICROPY_GC_INCREMENTAL
        gc_inc_stats[GC_INC_ST_THRESH_HITS] += 1;
        #endif
        #if MICROPY_GC_INCREMENTAL
        if (gc_incremental_enabled()) {
            // Start a cycle ahead of pressure; it will complete in slices.
            // Allocation continues optimistically; on failure the cycle is
            // completed synchronously by the collect below.
            gc_incremental_start();
        } else
        #endif
        {
            GC_EXIT();
            gc_collect();
            collected = 1;
            GC_ENTER();
        }
    }
    #endif

    for (;;) {
        #if MICROPY_GC_SPLIT_HEAP
        area = MP_STATE_MEM(gc_last_free_area);
        #else
        area = &MP_STATE_MEM(area);
        #endif
        //static int last_started_atb_index = 0;
        //if(last_started_atb_index > area->gc_last_free_atb_index){
        //  printf("atb index looped\n");
        //}
        //last_started_atb_index = area->gc_last_free_atb_index;

        // look for a run of n_blocks available blocks
        for (; area != NULL; area = NEXT_AREA(area), i = 0) {
            n_free = 0;
            //printf("heap len=%d, s=%d \n", area->gc_alloc_table_byte_len, area->gc_last_free_atb_index);
            for (i = area->gc_last_free_atb_index; i < area->gc_alloc_table_byte_len; i++) {
              //for (int ct = 0; ct < area->gc_alloc_table_byte_len; ct++) {
              //i = ct + area->gc_last_free_atb_index;
              //if(i >= area->gc_alloc_table_byte_len) i-= area->gc_alloc_table_byte_len;
                MICROPY_GC_HOOK_LOOP(i);
                byte a = area->gc_alloc_table_start[i];
                // *FORMAT-OFF*
                //if (ATB_0_IS_FREE(a)) { if (++n_free >= n_blocks) { i = i * BLOCKS_PER_ATB + 0; goto found; } } else { n_free = 0; }
                //if (ATB_1_IS_FREE(a)) { if (++n_free >= n_blocks) { i = i * BLOCKS_PER_ATB + 1; goto found; } } else { n_free = 0; }
                //if (ATB_2_IS_FREE(a)) { if (++n_free >= n_blocks) { i = i * BLOCKS_PER_ATB + 2; goto found; } } else { n_free = 0; }
                //if (ATB_3_IS_FREE(a)) { if (++n_free >= n_blocks) { i = i * BLOCKS_PER_ATB + 3; goto found; } } else { n_free = 0; }
                if (ATB_0_IS_FREE(a)) { if (++n_free >= n_blocks) { i = (i<<1) + 0; goto found; } } else { n_free = 0; }
                if (ATB_1_IS_FREE(a)) { if (++n_free >= n_blocks) { i = (i<<1) + 1; goto found; } } else { n_free = 0; }
                //if (ATB_2_IS_FREE(a)) { if (++n_free >= n_blocks) { i = (i<<2) + 2; goto found; } } else { n_free = 0; }
                //if (ATB_3_IS_FREE(a)) { if (++n_free >= n_blocks) { i = (i<<2)+ 3; goto found; } } else { n_free = 0; }
                // *FORMAT-ON*
            }

            // No free blocks found on this heap. Mark this heap as
            // filled, so we won't try to find free space here again until
            // space is freed.
            #if MICROPY_GC_SPLIT_HEAP
            if (n_blocks == 1) {
                area->gc_last_free_atb_index = (i + 1) / BLOCKS_PER_ATB; // or (size_t)-1
            }
            #endif
        }

        GC_EXIT();
        // nothing found!
        if (collected) {
            #if MICROPY_GC_SPLIT_HEAP_AUTO
            if (!added && gc_try_add_heap(n_bytes)) {
                added = true;
                continue;
            }
            #endif
            return NULL;
        }
        DEBUG_printf("gc_alloc(" UINT_FMT "): no free mem, triggering GC\n", n_bytes);
        #if MICROPY_GC_INCREMENTAL
        gc_inc_stats[GC_INC_ST_OOM] += 1;
        #if MICROPY_GC_USTIME_AVAILABLE
        if (!gc_stall_timed) {
            gc_stall_timed = true;
            gc_stall_t0 = gc_inc_now_us();
        }
        gc_stall_retries += 1;
        #endif
        #endif
        #if MICROPY_GC_INCREMENTAL
        if (!gc_inc_in_step) {
            if (mp_gc_mark_active == GC_INC_PHASE_MARK) {
                // Nothing can be freed until marking is over, finalisers ran
                // and the sweep is armed: finish marking now (capped slices,
                // terminates) and fall through to the sweep treatment.
                while (mp_gc_mark_active == GC_INC_PHASE_MARK) {
                    gc_incremental_step(true);
                }
            }
            if (mp_gc_mark_active == GC_INC_PHASE_SWEEP) {
                // The sweep frees progressively: advance one (capped, larger)
                // slice of the table walk and retry the allocation instead of
                // forcing the whole cycle synchronous -- a big request gets
                // its memory as soon as the walk has freed enough of it.
                GC_ENTER(); // see gc_incremental_step(): lock ATB updates
                gc_inc_sweep_step(MICROPY_GC_INCR_SWEEP_SLICE_BLOCKS * 8);
                if (gc_inc_sw_area == NULL) {
                    gc_inc_sweep_done_locked();
                }
                GC_EXIT();
                GC_ENTER();
                continue; // retry scan; cycle not yet counted as "collected"
            }
        }
        #endif
        gc_collect();
        collected = 1;
        GC_ENTER();
    }

    // found, ending at block i inclusive
found:
    #if MICROPY_GC_INCREMENTAL && MICROPY_GC_USTIME_AVAILABLE
    if (gc_stall_timed) {
        uint32_t dur = gc_inc_now_us() - gc_stall_t0;
        gc_inc_stats[GC_INC_ST_ALLOC_STALL_US] += dur;
        if (dur > gc_inc_stats[GC_INC_ST_MAX_ALLOC_US]) {
            gc_inc_stats[GC_INC_ST_MAX_ALLOC_US] = dur;
        }
        if (gc_stall_retries > gc_inc_stats[GC_INC_ST_MAX_RETRIES]) {
            gc_inc_stats[GC_INC_ST_MAX_RETRIES] = gc_stall_retries;
        }
    }
    #endif

    // get starting and end blocks, both inclusive
    end_block = i;
    start_block = i - n_free + 1;

    // Set last free ATB index to block after last block we found, for start of
    // next scan.  To reduce fragmentation, we only do this if we were looking
    // for a single free block, which guarantees that there are no free blocks
    // before this one.  Also, whenever we free or shink a block we must check
    // if this index needs adjusting (see gc_realloc and gc_free).
    if (n_free == 1) {
      #if MICROPY_GC_SPLIT_HEAP
      MP_STATE_MEM(gc_last_free_area) = area;
      #endif
      area->gc_last_free_atb_index = (i + 1) / BLOCKS_PER_ATB;
    }

    area->gc_last_used_block = MAX(area->gc_last_used_block, end_block);

    // mark first block as used head
    if((alloc_flags & GC_ALLOC_FLAG_IS_DATA)){// || n_blocks >= 300){
      ATB_FREE_TO_HEAD_DATA(area, start_block);
      //printf("HEAD_DATA set\n");
    }else{
      ATB_FREE_TO_HEAD(area, start_block);
    }

    // mark rest of blocks as used tail
    // TODO for a run of many blocks can make this more efficient
    for (size_t bl = start_block + 1; bl <= end_block; bl++) {
        ATB_FREE_TO_TAIL(area, bl);
    }

    #if MICROPY_GC_INCREMENTAL
    if (mp_gc_mark_active == GC_INC_PHASE_SWEEP) {
        // "born during sweep" tag (ATB top bit): the sliced sweep must treat
        // this block as live, and must break any free_tail chain here so a
        // garbage chain behind it cannot swallow our new tail blocks.
        // NOTE: heads allocated with GC_ALLOC_FLAG_IS_DATA use the identical
        // nibble (the fork's unused AT_HEAD_DATA_CANDIDATE==9); treating them
        // as sweep-live is conservative (freed next cycle), never unsafe.
        area->gc_alloc_table_start[start_block >> 1] |= (8 << BLOCK_SHIFT(start_block));
    } else if (mp_gc_mark_active == GC_INC_PHASE_MARK) {
        // Allocate gray.  A block born during MARK is reachable at the final
        // root rescan only if some *re-scanned* object still points at it, but
        // a store into an already-black container is invisible to that rescan
        // (a black object is never scanned twice).  Leaving it white therefore
        // sweeps live memory whenever a long-lived heap struct has a pointer
        // field reassigned mid-cycle: m_renew of vstr->buf inside the black
        // mp_lexer_t is the canonical case, and it corrupts token text.
        //
        // Gray, not black: marking alone would keep this block alive but leave
        // it never scanned, so a *pre-existing* white object stored into it
        // right after allocation would be collected instead -- trading one
        // missed-barrier class for a worse one.  Queuing it gray guarantees it
        // is scanned at least once after its fields are filled.  Cost is one
        // cycle of floating garbage, the same trade the "born during sweep"
        // tag above already makes, plus some gray-set traffic; on overflow the
        // existing resolver walk re-derives it from the gray bit.
        gc_inc_mark_gray(area, start_block);
    }
    #endif

    // get pointer to first block
    // we must create this pointer before unlocking the GC so a collection can find it
    void *ret_ptr = (void *)(area->gc_pool_start + start_block * BYTES_PER_BLOCK);
    //printf("gc_alloc(%p, %d)\n", ret_ptr, start_block / BLOCKS_PER_ATB);

    #if MICROPY_GC_ALLOC_THRESHOLD
    MP_STATE_MEM(gc_alloc_amount) += n_blocks;
    #endif

    GC_EXIT();

    #if MICROPY_GC_CONSERVATIVE_CLEAR
    // be conservative and zero out all the newly allocated blocks
    memset((byte *)ret_ptr, 0, (end_block - start_block + 1) * BYTES_PER_BLOCK);
    #else
    // zero out the additional bytes of the newly allocated blocks
    // This is needed because the blocks may have previously held pointers
    // to the heap and will not be set to something else if the caller
    // doesn't actually use the entire block.  As such they will continue
    // to point to the heap and may prevent other blocks from being reclaimed.
    memset((byte *)ret_ptr + n_bytes, 0, (end_block - start_block + 1) * BYTES_PER_BLOCK - n_bytes);
    #endif

    #if MICROPY_ENABLE_FINALISER
    if (has_finaliser) {
        // clear type pointer in case it is never set
        ((mp_obj_base_t *)ret_ptr)->type = NULL;
        // set mp_obj flag only if it has a finaliser
        GC_ENTER();
        FTB_SET(area, start_block);
        GC_EXIT();
    }
    #else
    (void)has_finaliser;
    #endif

    #if EXTENSIVE_HEAP_PROFILING
    gc_dump_alloc_table(&mp_plat_print);
    #endif

    return ret_ptr;
}

// force the freeing of a piece of memory
// TODO: freeing here does not call finaliser
void gc_free(void *ptr) {
    // Cannot free while the GC is locked, unless we're only doing a gc sweep.
    // However free is an optimisation to reclaim the memory immediately, this
    // means it will now be left until the next collection.
    //
    // (We have the optimisation to free immediately from inside a gc sweep so
    // that finalisers can free more memory when trying to avoid MemoryError.)
    if (MP_STATE_THREAD(gc_lock_depth) & ~GC_COLLECT_FLAG) {
        return;
    }

    GC_ENTER();

    DEBUG_printf("gc_free(%p)\n", ptr);

    if (ptr == NULL) {
        // free(NULL) is a no-op
        GC_EXIT();
        return;
    }

    // get the GC block number corresponding to this pointer
    mp_state_mem_area_t *area;
    #if MICROPY_GC_SPLIT_HEAP
    area = gc_get_ptr_area(ptr);
    assert(area);
    #else
    assert(VERIFY_PTR(ptr));
    area = &MP_STATE_MEM(area);
    #endif

    size_t block = BLOCK_FROM_PTR(area, ptr);

    #if MICROPY_ENABLE_FINALISER
    // Clear the finaliser bit before the deferral below: the caller has
    // explicitly freed this object, so its finaliser must not run later even
    // if the physical free is postponed to the sweep.
    FTB_CLEAR(area, block);
    #endif

    #if MICROPY_PY_WEAKREF
    // Objects that have a weak reference should not be explicitly freed.
    assert(!WTB_GET(area, block));
    #endif

    #if MICROPY_GC_INCREMENTAL
    // An incremental cycle marks live objects while the mutator keeps running,
    // so an explicit free can land on a block that is currently AT_MARK (and
    // possibly still queued in the gray set).  Freeing it here would let the
    // block be reallocated under the queued entry, and a later drain would
    // then scan unrelated memory.  Leave it to the collector: the sweep
    // demotes it to AT_HEAD and the next cycle reclaims it.
    if (mp_gc_mark_active != GC_INC_PHASE_IDLE
        && ATB_GET_KIND(area, block) == AT_MARK
        && !(MP_STATE_THREAD(gc_lock_depth) & GC_COLLECT_FLAG)) {
        GC_EXIT();
        return;
    }
    #endif

    assert(ATB_GET_KIND(area, block) == AT_HEAD
        || (ATB_GET_KIND(area, block) == AT_MARK && (MP_STATE_THREAD(gc_lock_depth) & GC_COLLECT_FLAG)));

    #if MICROPY_GC_SPLIT_HEAP
    if (MP_STATE_MEM(gc_last_free_area) != area) {
        // We freed something but it isn't the current area. Reset the
        // last free area to the start for a rescan. Note that this won't
        // give much of a performance hit, since areas that are completely
        // filled will likely be skipped (the gc_last_free_atb_index
        // points to the last block).
        // The reason why this is necessary is because it is not possible
        // to see which area came first (like it is possible to adjust
        // gc_last_free_atb_index based on whether the freed block is
        // before the last free block).
        MP_STATE_MEM(gc_last_free_area) = &MP_STATE_MEM(area);
    }
    #endif

    // set the last_free pointer to this block if it's earlier in the heap
    if (block / BLOCKS_PER_ATB < area->gc_last_free_atb_index) {
        area->gc_last_free_atb_index = block / BLOCKS_PER_ATB;
    }

    // free head and all of its tail blocks
    do {
        ATB_ANY_TO_FREE(area, block);
        block += 1;
    } while (ATB_GET_KIND(area, block) == AT_TAIL);

    GC_EXIT();

    #if EXTENSIVE_HEAP_PROFILING
    gc_dump_alloc_table(&mp_plat_print);
    #endif
}

size_t gc_nbytes(const void *ptr) {
    GC_ENTER();

    mp_state_mem_area_t *area;
    #if MICROPY_GC_SPLIT_HEAP
    area = gc_get_ptr_area(ptr);
    #else
    if (VERIFY_PTR(ptr)) {
        area = &MP_STATE_MEM(area);
    } else {
        area = NULL;
    }
    #endif

    if (area) {
        size_t block = BLOCK_FROM_PTR(area, ptr);
        if (ATB_GET_KIND(area, block) == AT_HEAD) {
            // work out number of consecutive blocks in the chain starting with this on
            size_t n_blocks = 0;
            do {
                n_blocks += 1;
            } while (ATB_GET_KIND(area, block + n_blocks) == AT_TAIL);
            GC_EXIT();
            return n_blocks * BYTES_PER_BLOCK;
        }
    }

    // invalid pointer
    GC_EXIT();
    return 0;
}
void *gc_realloc_flags(void *ptr_in, size_t n_bytes, bool allow_move, unsigned int flag) {
    // check for pure allocation
    if (ptr_in == NULL) {
        return gc_alloc(n_bytes, flag);
    }

    // check for pure free
    if (n_bytes == 0) {
        gc_free(ptr_in);
        return NULL;
    }

    if (MP_STATE_THREAD(gc_lock_depth) > 0) {
        return NULL;
    }

    void *ptr = ptr_in;

    GC_ENTER();

    // get the GC block number corresponding to this pointer
    mp_state_mem_area_t *area;
    #if MICROPY_GC_SPLIT_HEAP
    area = gc_get_ptr_area(ptr);
    assert(area);
    #else
    assert(VERIFY_PTR(ptr));
    area = &MP_STATE_MEM(area);
    #endif
    size_t block = BLOCK_FROM_PTR(area, ptr);
    unsigned int ex_flags =0;
    if(ATB_GET_KIND_FULL(area,block)&0x4){
      ex_flags |= GC_ALLOC_FLAG_IS_DATA;
    }
    #if MICROPY_GC_INCREMENTAL
    // During an incremental cycle a live block is AT_MARK; realloc never
    // touches the head nibble, so all paths below stay correct.
    assert(ATB_GET_KIND(area, block) == AT_HEAD
        || (mp_gc_mark_active != GC_INC_PHASE_IDLE && ATB_GET_KIND(area, block) == AT_MARK));
    #else
    assert(ATB_GET_KIND(area, block) == AT_HEAD);
    #endif

    // compute number of new blocks that are requested
    size_t new_blocks = (n_bytes + BYTES_PER_BLOCK - 1) / BYTES_PER_BLOCK;

    // Get the total number of consecutive blocks that are already allocated to
    // this chunk of memory, and then count the number of free blocks following
    // it.  Stop if we reach the end of the heap, or if we find enough extra
    // free blocks to satisfy the realloc.  Note that we need to compute the
    // total size of the existing memory chunk so we can correctly and
    // efficiently shrink it (see below for shrinking code).
    size_t n_free = 0;
    size_t n_blocks = 1; // counting HEAD block
    size_t max_block = area->gc_alloc_table_byte_len * BLOCKS_PER_ATB;
    for (size_t bl = block + n_blocks; bl < max_block; bl++) {
        byte block_type = ATB_GET_KIND(area, bl);
        if (block_type == AT_TAIL) {
            n_blocks++;
            continue;
        }
        if (block_type == AT_FREE) {
            n_free++;
            if (n_blocks + n_free >= new_blocks) {
                // stop as soon as we find enough blocks for n_bytes
                break;
            }
            continue;
        }
        break;
    }

    // return original ptr if it already has the requested number of blocks
    if (new_blocks == n_blocks) {
        GC_EXIT();
        return ptr_in;
    }

    // check if we can shrink the allocated area
    if (new_blocks < n_blocks) {
        // free unneeded tail blocks
        for (size_t bl = block + new_blocks, count = n_blocks - new_blocks; count > 0; bl++, count--) {
            ATB_ANY_TO_FREE(area, bl);
        }

        #if MICROPY_GC_SPLIT_HEAP
        if (MP_STATE_MEM(gc_last_free_area) != area) {
            // See comment in gc_free.
            MP_STATE_MEM(gc_last_free_area) = &MP_STATE_MEM(area);
        }
        #endif

        // set the last_free pointer to end of this block if it's earlier in the heap
        if ((block + new_blocks) / BLOCKS_PER_ATB < area->gc_last_free_atb_index) {
          area->gc_last_free_atb_index = (block + new_blocks) / BLOCKS_PER_ATB;
        }

        GC_EXIT();

        #if EXTENSIVE_HEAP_PROFILING
        gc_dump_alloc_table(&mp_plat_print);
        #endif

        return ptr_in;
    }

    // check if we can expand in place
    if (new_blocks <= n_blocks + n_free) {
        // mark few more blocks as used tail
        size_t end_block = block + new_blocks;
        for (size_t bl = block + n_blocks; bl < end_block; bl++) {
            assert(ATB_GET_KIND(area, bl) == AT_FREE);
            ATB_FREE_TO_TAIL(area, bl);
            #if MICROPY_GC_INCREMENTAL
            if (mp_gc_mark_active == GC_INC_PHASE_SWEEP) {
                // grown tail: protect from a free_tail chain in sliced sweep
                area->gc_alloc_table_start[bl >> 1] |= (8 << BLOCK_SHIFT(bl));
            }
            #endif
        }

        area->gc_last_used_block = MAX(area->gc_last_used_block, end_block);

        #if MICROPY_GC_INCREMENTAL
        // Keep the gray set consistent with the now-longer chain: if this
        // object was already marked (possibly already scanned) during an
        // active cycle, requeue it so the new tail slots get rescanned.
        GC_GRAY_MARK_PTR(ptr_in);
        #endif

        GC_EXIT();

        #if MICROPY_GC_CONSERVATIVE_CLEAR
        // be conservative and zero out all the newly allocated blocks
        memset((byte *)ptr_in + n_blocks * BYTES_PER_BLOCK, 0, (new_blocks - n_blocks) * BYTES_PER_BLOCK);
        #else
        // zero out the additional bytes of the newly allocated blocks (see comment above in gc_alloc)
        memset((byte *)ptr_in + n_bytes, 0, new_blocks * BYTES_PER_BLOCK - n_bytes);
        #endif

        #if EXTENSIVE_HEAP_PROFILING
        gc_dump_alloc_table(&mp_plat_print);
        #endif

        return ptr_in;
    }

    #if MICROPY_ENABLE_FINALISER
    bool ftb_state = FTB_GET(area, block);
    #else
    bool ftb_state = false;
    #endif

    GC_EXIT();

    if (!allow_move) {
        // not allowed to move memory block so return failure
        return NULL;
    }

    // can't resize inplace; try to find a new contiguous chain
    void *ptr_out = gc_alloc(n_bytes, ftb_state | ex_flags);

    // check that the alloc succeeded
    if (ptr_out == NULL) {
        return NULL;
    }

    DEBUG_printf("gc_realloc(%p -> %p)\n", ptr_in, ptr_out);
    memcpy(ptr_out, ptr_in, n_blocks * BYTES_PER_BLOCK);
    #if MICROPY_GC_INCREMENTAL
    // The new buffer holds (pointer) contents copied from a live object; if a
    // cycle is active the buffer must be live for this cycle too, and it will
    // be rescanned once the caller has updated its container's pointer field
    // (no allocation happens in between, so no slice can run mid-update).
    GC_GRAY_MARK_PTR(ptr_out);
    #endif
    gc_free(ptr_in);
    return ptr_out;
}

void *gc_realloc(void *ptr_in, size_t n_bytes, bool allow_move) {
  return gc_realloc_flags(ptr_in, n_bytes, allow_move, false);
}
void *gc_realloc_data(void *ptr_in, size_t n_bytes, bool allow_move) {
  return gc_realloc_flags(ptr_in, n_bytes, allow_move,GC_ALLOC_FLAG_IS_DATA );
}



void gc_dump_info(const mp_print_t *print) {
    gc_info_t info;
    gc_info(&info);
    mp_printf(print, "GC: total: %u, used: %u, free: %u",
        (uint)info.total, (uint)info.used, (uint)info.free);
    #if MICROPY_GC_SPLIT_HEAP_AUTO
    mp_printf(print, ", max new split: %u", (uint)info.max_new_split);
    #endif
    mp_printf(print, "\n No. of 1-blocks: %u, 2-blocks: %u, max blk sz: %u, max free sz: %u\n",
        (uint)info.num_1block, (uint)info.num_2block, (uint)info.max_block, (uint)info.max_free);
}

void gc_dump_alloc_table(const mp_print_t *print) {
    GC_ENTER();
    static const size_t DUMP_BYTES_PER_LINE = 64;
    for (mp_state_mem_area_t *area = &MP_STATE_MEM(area); area != NULL; area = NEXT_AREA(area)) {
        #if !EXTENSIVE_HEAP_PROFILING
        // When comparing heap output we don't want to print the starting
        // pointer of the heap because it changes from run to run.
        mp_printf(print, "GC memory layout; from %p:", area->gc_pool_start);
        #endif
        for (size_t bl = 0; bl < area->gc_alloc_table_byte_len * BLOCKS_PER_ATB; bl++) {
            if (bl % DUMP_BYTES_PER_LINE == 0) {
                // a new line of blocks
                {
                    // check if this line contains only free blocks
                    size_t bl2 = bl;
                    while (bl2 < area->gc_alloc_table_byte_len * BLOCKS_PER_ATB && ATB_GET_KIND(area, bl2) == AT_FREE) {
                        bl2++;
                    }
                    if (bl2 - bl >= 2 * DUMP_BYTES_PER_LINE) {
                        // there are at least 2 lines containing only free blocks, so abbreviate their printing
                        mp_printf(print, "\n       (%u lines all free)", (uint)(bl2 - bl) / DUMP_BYTES_PER_LINE);
                        bl = bl2 & (~(DUMP_BYTES_PER_LINE - 1));
                        if (bl >= area->gc_alloc_table_byte_len * BLOCKS_PER_ATB) {
                            // got to end of heap
                            break;
                        }
                    }
                }
                // print header for new line of blocks
                // (the cast to uint32_t is for 16-bit ports)
                mp_printf(print, "\n%08x: ", (uint)(bl * BYTES_PER_BLOCK));
            }
            int c = ' ';
            switch (ATB_GET_KIND(area, bl)) {
                case AT_FREE:
                    c = '.';
                    break;
                /* this prints out if the object is reachable from BSS or STACK (for unix only)
                case AT_HEAD: {
                    c = 'h';
                    void **ptrs = (void**)(void*)&mp_state_ctx;
                    mp_uint_t len = offsetof(mp_state_ctx_t, vm.stack_top) / sizeof(mp_uint_t);
                    for (mp_uint_t i = 0; i < len; i++) {
                        mp_uint_t ptr = (mp_uint_t)ptrs[i];
                        if (gc_get_ptr_area(ptr) && BLOCK_FROM_PTR(ptr) == bl) {
                            c = 'B';
                            break;
                        }
                    }
                    if (c == 'h') {
                        ptrs = (void**)&c;
                        len = ((mp_uint_t)MP_STATE_THREAD(stack_top) - (mp_uint_t)&c) / sizeof(mp_uint_t);
                        for (mp_uint_t i = 0; i < len; i++) {
                            mp_uint_t ptr = (mp_uint_t)ptrs[i];
                            if (gc_get_ptr_area(ptr) && BLOCK_FROM_PTR(ptr) == bl) {
                                c = 'S';
                                break;
                            }
                        }
                    }
                    break;
                }
                */
                /* this prints the uPy object type of the head block */
                case AT_HEAD: {
                    void **ptr = (void **)(area->gc_pool_start + bl * BYTES_PER_BLOCK);
                    if (*ptr == &mp_type_tuple) {
                        c = 'T';
                    } else if (*ptr == &mp_type_list) {
                        c = 'L';
                    } else if (*ptr == &mp_type_dict) {
                        c = 'D';
                    } else if (*ptr == &mp_type_str || *ptr == &mp_type_bytes) {
                        c = 'S';
                    }
                    #if MICROPY_PY_BUILTINS_BYTEARRAY
                    else if (*ptr == &mp_type_bytearray) {
                        c = 'A';
                    }
                    #endif
                    #if MICROPY_PY_ARRAY
                    else if (*ptr == &mp_type_array) {
                        c = 'A';
                    }
                    #endif
                    #if MICROPY_PY_BUILTINS_FLOAT
                    else if (*ptr == &mp_type_float) {
                        c = 'F';
                    }
                    #endif
                    else if (*ptr == &mp_type_fun_bc) {
                        c = 'B';
                    } else if (*ptr == &mp_type_module) {
                        c = 'M';
                    } else {
                        c = 'h';
                        #if 0
                        // This code prints "Q" for qstr-pool data, and "q" for qstr-str
                        // data.  It can be useful to see how qstrs are being allocated,
                        // but is disabled by default because it is very slow.
                        for (qstr_pool_t *pool = MP_STATE_VM(last_pool); c == 'h' && pool != NULL; pool = pool->prev) {
                            if ((qstr_pool_t *)ptr == pool) {
                                c = 'Q';
                                break;
                            }
                            for (const byte **q = pool->qstrs, **q_top = pool->qstrs + pool->len; q < q_top; q++) {
                                if ((const byte *)ptr == *q) {
                                    c = 'q';
                                    break;
                                }
                            }
                        }
                        #endif
                    }
                    break;
                }
                case AT_TAIL:
                    c = '=';
                    break;
                case AT_MARK:
                    c = 'm';
                    break;
            }
            mp_printf(print, "%c", c);
        }
        mp_print_str(print, "\n");
    }
    GC_EXIT();
}

#endif // MICROPY_ENABLE_GC
