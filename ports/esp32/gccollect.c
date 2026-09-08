/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * Development of the code in this file was sponsored by Microbric Pty Ltd
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2014 Damien P. George
 * Copyright (c) 2017 Pycom Limited
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

#include <stdio.h>

#include "py/mpconfig.h"
#include "py/mpstate.h"
#include "py/gc.h"
#include "py/mpthread.h"
#include "gccollect.h"

#if MICROPY_ENABLE_PYSTACK
#include "py/pystack.h"
#endif

#if CONFIG_IDF_TARGET_ARCH_XTENSA

#include "xtensa/hal.h"

// The level argument must be volatile to force the compiler to emit code that
// will call this function recursively, to nest the C stack.
static void gc_collect_inner(volatile unsigned int level) {
    if (level < XCHAL_NUM_AREGS / 8) {
        // Go deeper on the stack to spill more registers from the register window.
        gc_collect_inner(level + 1);
    } else {
        // Deep enough so that all registers are on the C stack, now trace the stack.
        volatile uint32_t sp = (uint32_t)esp_cpu_get_sp();
        gc_collect_root((void **)sp, ((mp_uint_t)MP_STATE_THREAD(stack_top) - sp) / sizeof(uint32_t));
    }
}

void gc_collect(void) {
    #if MICROPY_GC_INCREMENTAL
    // In incremental mode route through the incremental machinery so the
    // gray set stays consistent (a stale partial cycle would be unsafe).
    if (gc_incremental_active()) {
        gc_incremental_complete();
        return;
    }
    if (gc_incremental_enabled()) {
        gc_incremental_start();
        gc_incremental_complete();
        return;
    }
    #endif
    gc_collect_start();
    mp_port_gc_stack_roots();
    gc_collect_end();
}

// Scan all stack-related roots: own C stack + spilled registers (via the
// window-deepening recursion), own pystack, and other threads' stacks.
// Called by classic gc_collect() above and by py/gc.c at incremental cycle
// start and end (MICROPY_GC_PORT_STACK_ROOTS).
void mp_port_gc_stack_roots(void) {
    gc_collect_inner(0);
    #if MICROPY_ENABLE_PYSTACK
    // The pystack is a separate malloc'd buffer not covered by the C stack scan.
    // Scan it so that Python locals stored there are not incorrectly freed.
    gc_collect_root((void **)MP_STATE_THREAD(pystack_start),
        (MP_STATE_THREAD(pystack_cur) - MP_STATE_THREAD(pystack_start)) / sizeof(mp_uint_t));
    #endif
    #if MICROPY_PY_THREAD
    mp_thread_gc_others();
    #endif
}

#elif CONFIG_IDF_TARGET_ARCH_RISCV
#include "shared/runtime/gchelper.h"

void gc_collect(void) {
    #if MICROPY_GC_INCREMENTAL
    if (gc_incremental_active()) {
        gc_incremental_complete();
        return;
    }
    if (gc_incremental_enabled()) {
        gc_incremental_start();
        gc_incremental_complete();
        return;
    }
    #endif
    gc_collect_start();
    mp_port_gc_stack_roots();
    gc_collect_end();
}

// See comment on the xtensa variant above.
void mp_port_gc_stack_roots(void) {
    gc_helper_collect_regs_and_stack();
    #if MICROPY_ENABLE_PYSTACK
    gc_collect_root((void **)MP_STATE_THREAD(pystack_start),
        (MP_STATE_THREAD(pystack_cur) - MP_STATE_THREAD(pystack_start)) / sizeof(mp_uint_t));
    #endif
    #if MICROPY_PY_THREAD
    mp_thread_gc_others();
    #endif
}

#else
#error unknown CONFIG_IDF_TARGET_ARCH
#endif

#if MICROPY_GC_SPLIT_HEAP_AUTO

// The largest new region that is available to become Python heap is the largest
// free block in the ESP-IDF system heap.
size_t gc_get_max_new_split(void) {
    return heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
}

#endif
