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

#include <stdio.h>

#include "py/mpstate.h"
#include "py/gc.h"

#include "shared/runtime/gchelper.h"

#if MICROPY_ENABLE_GC

void gc_collect(void) {
    #if MICROPY_GC_INCREMENTAL
    // Mirror of ports/esp32/gccollect.c: keep the gray set consistent by
    // routing collections through the incremental machinery when enabled.
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

// Own C stack + spilled registers, plus every other thread's stack.  Also
// called by py/gc.c at incremental cycle start and end.
void mp_port_gc_stack_roots(void) {
    gc_helper_collect_regs_and_stack();
    #if MICROPY_PY_THREAD
    mp_thread_gc_others();
    #endif
}

#endif // MICROPY_ENABLE_GC

#if MICROPY_GC_SPLIT_HEAP_AUTO
// The esp32 port returns the largest free system-heap block here.  The host has
// no such limit, so impose one: without it the heap grows until the OS kills
// the process instead of raising MemoryError.  Called with the GC lock held, so
// the area list is walked directly rather than via gc_info().
size_t gc_get_max_new_split(void) {
    size_t total = 0;
    for (mp_state_mem_area_t *a = &MP_STATE_MEM(area); a != NULL; a = a->next) {
        total += a->gc_pool_end - a->gc_pool_start;
    }
    if (total >= MICROPY_GC_TOTAL_HEAP_LIMIT) {
        return 0;
    }
    return MICROPY_GC_MAX_NEW_AREA_SIZE;
}
#endif
