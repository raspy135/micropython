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

#include <assert.h>
#include <string.h>

#include "py/mpstate.h"
#include "py/obj.h"
#include "py/gc.h"

#if MICROPY_PY_GC && MICROPY_ENABLE_GC

// collect(): run a garbage collection
static mp_obj_t py_gc_collect(void) {
    gc_collect();
    #if MICROPY_PY_GC_COLLECT_RETVAL
    return MP_OBJ_NEW_SMALL_INT(MP_STATE_MEM(gc_collected));
    #else
    return mp_const_none;
    #endif
}
MP_DEFINE_CONST_FUN_OBJ_0(gc_collect_obj, py_gc_collect);

// disable(): disable the garbage collector
static mp_obj_t gc_disable(void) {
    MP_STATE_MEM(gc_auto_collect_enabled) = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(gc_disable_obj, gc_disable);

// enable(): enable the garbage collector
static mp_obj_t gc_enable(void) {
    MP_STATE_MEM(gc_auto_collect_enabled) = 1;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(gc_enable_obj, gc_enable);

static mp_obj_t gc_isenabled(void) {
    return mp_obj_new_bool(MP_STATE_MEM(gc_auto_collect_enabled));
}
MP_DEFINE_CONST_FUN_OBJ_0(gc_isenabled_obj, gc_isenabled);

// mem_free(): return the number of bytes of available heap RAM
static mp_obj_t gc_mem_free(void) {
    gc_info_t info;
    gc_info(&info);
    #if MICROPY_GC_SPLIT_HEAP_AUTO
    // Include max_new_split value here as a more useful heuristic
    return MP_OBJ_NEW_SMALL_INT(info.free + info.max_new_split);
    #else
    return MP_OBJ_NEW_SMALL_INT(info.free);
    #endif
}
MP_DEFINE_CONST_FUN_OBJ_0(gc_mem_free_obj, gc_mem_free);

// mem_alloc(): return the number of bytes of heap RAM that are allocated
static mp_obj_t gc_mem_alloc(void) {
    gc_info_t info;
    gc_info(&info);
    return MP_OBJ_NEW_SMALL_INT(info.used);
}
MP_DEFINE_CONST_FUN_OBJ_0(gc_mem_alloc_obj, gc_mem_alloc);

#if MICROPY_GC_ALLOC_THRESHOLD
static mp_obj_t gc_threshold(size_t n_args, const mp_obj_t *args) {
    if (n_args == 0) {
        if (MP_STATE_MEM(gc_alloc_threshold) == (size_t)-1) {
            return MP_OBJ_NEW_SMALL_INT(-1);
        }
        return mp_obj_new_int(MP_STATE_MEM(gc_alloc_threshold) * MICROPY_BYTES_PER_GC_BLOCK);
    }
    mp_int_t val = mp_obj_get_int(args[0]);
    if (val < 0) {
        MP_STATE_MEM(gc_alloc_threshold) = (size_t)-1;
    } else {
        MP_STATE_MEM(gc_alloc_threshold) = val / MICROPY_BYTES_PER_GC_BLOCK;
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gc_threshold_obj, 0, 1, gc_threshold);
#endif

#if MICROPY_GC_INCREMENTAL
// gc.incremental() -> current state (bool)
// gc.incremental(on) -> set, returns previous state
// When disabled, collections fall back to the classic single-pass
// stop-the-world algorithm (an already-running cycle still finishes safely).
static mp_obj_t gc_incremental(size_t n_args, const mp_obj_t *args) {
    bool prev = gc_incremental_enabled();
    if (n_args == 1) {
        gc_incremental_set_enabled(mp_obj_is_true(args[0]));
    }
    return mp_obj_new_bool(prev);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gc_incremental_obj, 0, 1, gc_incremental);

// gc.stats() -> dict with incremental-collector counters:
//   cycles       - incremental cycles started
//   slices       - bounded mark slices executed during allocations
//   slice_ends   - cycles whose marking completed inside a slice (no pause)
//   sync_ends    - cycles completed synchronously (gc.collect() / memory pressure)
//   overflows    - gray-set overflows resolved by a full table walk
//   words        - total heap words scanned so far (mark work volume)
//   aborts       - cycles abandoned by soft reset
//   roots_us     - microseconds in stop-the-world root scans
//   sweep_us     - microseconds in all stop-the-world sweep/finaliser pieces
//   max_sweep_us - longest single stop-the-world piece (the frame-killer metric)
//   fin_us       - finaliser pass time (subset of sweep_us)
//   walk_us      - sliced free-walk time (sweep is sliced; each piece is
//                  budgeted, only finalisers remain fully stop-the-world)
//   max_slice_us - longest single mark slice (worst per-allocation surcharge)
//   drain_us     - end-of-cycle synchronous drain, total (stop-the-world)
//   max_drain_us - longest single end-of-cycle drain: the frame-killer metric
//   final_words  - words marked in the end-of-cycle drain.  final_words/words
//                  is the share of marking that did NOT happen incrementally;
//                  near 1.0 means the collector is stop-the-world in practice.
//   walk_blocks  - ATB blocks visited by gray-overflow resolver walks.  Large
//                  vs words means the gray set is too small.
//   max_gray     - high-water mark of the gray set in entries.  Build with
//                  MICROPY_ALLOC_GC_STACK_SIZE comfortably above this and
//                  overflows go to zero.  Only meaningful when overflows == 0;
//                  while overflowing, the reading is clipped at the array size.
static mp_obj_t gc_stats(void) {
    static const char *const names[29] = {"cycles", "slices", "slice_ends", "sync_ends", "overflows", "words", "aborts", "roots_us", "sweep_us", "max_sweep_us", "fin_us", "walk_us", "max_slice_us", "mark_us", "drain_us", "max_drain_us", "final_words", "walk_blocks", "max_alloc_us", "max_retries", "alloc_stall_us", "max_gray", "oom", "step_calls", "step_idle", "step_notdue", "thresh_hits", "start_calls", "start_busy"};
    uint32_t st[29];
    gc_incremental_get_stats(st);
    mp_obj_dict_t *d = MP_OBJ_TO_PTR(mp_obj_new_dict(29));
    for (size_t i = 0; i < 29; i++) {
        mp_obj_dict_store(MP_OBJ_FROM_PTR(d), mp_obj_new_str(names[i], strlen(names[i])), mp_obj_new_int(st[i]));
    }
    return MP_OBJ_FROM_PTR(d);
}
MP_DEFINE_CONST_FUN_OBJ_0(gc_stats_obj, gc_stats);

// gc.slice() -> current slice budget (heap words scanned per allocation)
// gc.slice(n) -> set it; both forms return the value in effect after the call.
// Smaller = smoother frame pacing (shorter per-allocation worst-case cost),
// longer wall-clock time for one collection cycle. Typical: 64..512.
static mp_obj_t gc_slice(size_t n_args, const mp_obj_t *args) {
    uint32_t cur = gc_incremental_set_slice_words(n_args == 1 ? (uint32_t)mp_obj_get_int(args[0]) : 0);
    return mp_obj_new_int(cur);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gc_slice_obj, 0, 1, gc_slice);

// gc.pace() -> (gap_us, cap_us)
// gc.pace(gap_us, cap_us) -> sets them, returns the values in effect.
//   gap_us: minimum wall-clock time between two slices; this is the GC duty
//           cycle limiter (duty ~= cap/(cap+gap)).  Default 500.
//   cap_us: hard wall-clock cap for a single slice; bounds the worst-case
//           surcharge of any one gc_alloc call.  Default 80.
// Raise gap for smoother frames / slower cycles; lower it if cycles fall
// behind allocation pressure (gc.stats() sync_ends climbing).
static mp_obj_t gc_pace(size_t n_args, const mp_obj_t *args) {
    if (n_args == 2) {
        gc_incremental_set_pace((uint32_t)mp_obj_get_int(args[0]), (uint32_t)mp_obj_get_int(args[1]));
    }
    uint32_t gap, cap;
    gc_incremental_get_pace(&gap, &cap);
    mp_obj_t tup[2] = {mp_obj_new_int(gap), mp_obj_new_int(cap)};
    return mp_obj_new_tuple(2, tup);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gc_pace_obj, 0, 2, gc_pace);
#endif

static const mp_rom_map_elem_t mp_module_gc_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_gc) },
    { MP_ROM_QSTR(MP_QSTR_collect), MP_ROM_PTR(&gc_collect_obj) },
    { MP_ROM_QSTR(MP_QSTR_disable), MP_ROM_PTR(&gc_disable_obj) },
    { MP_ROM_QSTR(MP_QSTR_enable), MP_ROM_PTR(&gc_enable_obj) },
    { MP_ROM_QSTR(MP_QSTR_isenabled), MP_ROM_PTR(&gc_isenabled_obj) },
    { MP_ROM_QSTR(MP_QSTR_mem_free), MP_ROM_PTR(&gc_mem_free_obj) },
    { MP_ROM_QSTR(MP_QSTR_mem_alloc), MP_ROM_PTR(&gc_mem_alloc_obj) },
    #if MICROPY_GC_ALLOC_THRESHOLD
    { MP_ROM_QSTR(MP_QSTR_threshold), MP_ROM_PTR(&gc_threshold_obj) },
    #endif
    #if MICROPY_GC_INCREMENTAL
    { MP_ROM_QSTR(MP_QSTR_incremental), MP_ROM_PTR(&gc_incremental_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&gc_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_slice), MP_ROM_PTR(&gc_slice_obj) },
    { MP_ROM_QSTR(MP_QSTR_pace), MP_ROM_PTR(&gc_pace_obj) },
    #endif
};

static MP_DEFINE_CONST_DICT(mp_module_gc_globals, mp_module_gc_globals_table);

const mp_obj_module_t mp_module_gc = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp_module_gc_globals,
};

MP_REGISTER_MODULE(MP_QSTR_gc, mp_module_gc);

#endif
