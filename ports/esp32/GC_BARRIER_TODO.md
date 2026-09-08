# GC write-barrier completion checklist

Generated 2026-09-02 from the working tree, branch `gc-incremental`.
Regenerate any time; the checkboxes are the only hand-maintained part.

## Why this file exists

`MICROPY_GC_INCREMENTAL` marks the heap in slices while the interpreter keeps
running. That is only sound with a **Dijkstra mark-on-store write barrier on
every store of a heap reference into an already-scanned (black) object**.
Roughly ten sites are instrumented today; the rest are below.

Until this list is done, the collector can free a live object. It is
timing-dependent, so it presents as random corruption minutes later, not as a
reproducible crash at the offending line.

## Context: collector state this list assumes

Uncommitted work on `gc-incremental` that must be in place for the list below
to make sense. If a fresh session finds these missing, the tree has been
reverted and the barrier work is premature.

**Correctness fixes (were causing memory corruption):**

1. `gc_inc_sweep_step` — the "born during sweep" ATB tag (nibble bit 3) was
   never cleared, so blocks allocated during any sweep became permanently
   `AT_MARK`, were skipped by every later mark phase, and their children were
   freed while still referenced. Now cleared, and a stale `AT_MARK` demoted.
2. `gc_free` — an explicit free can land on an `AT_MARK` block while a cycle is
   marking (tripped the existing assert). Now deferred to the sweep, with
   `FTB_CLEAR` moved above the deferral so an explicitly freed object does not
   get its finaliser run later.
3. `gc_collect_root` — the incremental branch queued root blocks without
   tagging them gray, so a gray-set overflow dropped every root. Fixed.

**Pacing fixes (were causing the frame drops):**

4. Adaptive trigger had no upper clamp; it ratcheted past the heap size after
   the first collection and never fired again, so every collection came from
   allocation failure and ran fully stop-the-world. Now clamped to
   `free_blocks / MICROPY_GC_INCR_TRIGGER_DIVISOR` (default 4), with the sweep
   tallying free blocks as it walks.
5. Gray/black distinction in the ATB, reusing nibble bit 3 (free during MARK,
   since the sweep-born tag only means anything during SWEEP). The overflow
   resolver now rescans only genuine grays instead of the whole live set:
   2.7x less mark work at small gray-set sizes, and identical when no overflow
   occurs. This is why `MICROPY_ALLOC_GC_STACK_SIZE` can stay small.
6. `gc_inc_drain` slice time cap gated on `gc_inc_gray_sp & 7` — the gray stack
   *depth*, not an iteration count — so it was frequently never checked and
   slices ran unbounded (measured 8016 us against an 80 us cap). Now counts
   iterations.
7. 8-block skip over all-free ATB words in both the sliced and classic sweeps.
   Without it a heap that just became mostly garbage was walked block by block
   every cycle.
8. `gc_setup_area` / `gc_add` heap-layout `printf`s made `DEBUG_printf`. They
   ran from inside `gc_alloc` on every area growth: ~250 bytes of blocking
   console output with the GC lock held.

**Instrumentation** (`gc.stats()`): `max_slice_us`, `mark_us`, `drain_us`,
`max_drain_us`, `final_words`, `walk_blocks`, `max_gray`, plus temporary
counters `oom` / `step_calls` / `step_idle` / `step_notdue` / `thresh_hits` /
`start_calls` / `start_busy` marked TEMP in `py/gc.c` — delete those seven when
the tuning work is finished.

`max_gray` reads back exactly `MICROPY_ALLOC_GC_STACK_SIZE` when it is
clipping; only a reading below the array size is a real measurement.

## The rule

Barrier a store when **all** of these hold:

1. The destination is inside a **GC-heap object** (not the C stack, not the
   pystack, not static/BSS, not a `malloc`'d non-GC buffer). The C stack and
   pystack are rescanned atomically at end of cycle, so they need nothing.
2. The destination object may **already be black**.

   > **CORRECTION (found the hard way).** "It was freshly allocated, so it is
   > white" is only safe if **no allocation happens between the object's
   > allocation and the store**. The classic broken shape is:
   >
   > ```c
   > o = mp_obj_malloc(mp_obj_set_t, &mp_type_set);  // white
   > o->set.table = m_new0(mp_obj_t, n);             // <-- this m_new0 can
   >                                                 //     drive a mark slice
   >                                                 //     that blackens `o`
   > ```
   >
   > The slice inside `m_new0` can scan `o` while `table` is still garbage,
   > so `o` goes black with no reference to the table, and the store that
   > follows is missed. This was a real defect in `mp_set_init`,
   > `mp_set_rehash` and `mp_map_init`, and it is why the generated
   > "fresh target (no barrier)" classification below **cannot be trusted**
   > for any `x->field = <something that allocates>`.
   >
   > Rule of thumb: **`x->field = m_new/m_renew/mp_obj_new_*(...)` always
   > needs `GC_WB_PTR(x->field)` after it**, no matter how fresh `x` looks.
3. The stored value is a **real heap pointer**. Small ints, qstrs,
   `MP_OBJ_NULL`, `MP_OBJ_SENTINEL`, `mp_const_*` and ROM objects need nothing
   (`GC_BARRIER` already tests `mp_obj_is_obj`, so this is a clarity point,
   not a correctness one).

Removing a reference never needs a barrier. Dijkstra only cares about the
value being stored, never the one overwritten.

## How to apply one

```c
#include "py/gc.h"   // usually already present via py/obj.h

GC_BARRIER(value);        // immediately BEFORE the store
dest->field = value;
```

`GC_BARRIER` compiles to nothing when `MICROPY_GC_INCREMENTAL` is 0, and to a
single predictable byte test (`mp_gc_mark_active == 1`) when no cycle is
running. Put it before the store, and never between a `mp_map_lookup` and the
slot write in a way that could allocate.

For a freshly (re)allocated buffer of pointer slots that is published into an
existing container by a raw store — map tables, list `items` arrays — use
`GC_GRAY_MARK_PTR(buf)` instead; see `mp_map_rehash` in `py/map.c` for the
pattern already in place.

## Do the chokepoints first

The raw site count is large but the leverage is very concentrated. Field
histogram across all residual sites:

| field | sites | what it is |
|---|---|---|
| `items` | 185 | list / tuple / deque / namedtuple element arrays |
| `state` | 47 | **VM value stack + locals** — see Structural below |
| `data` / `buf` | 38 | mostly byte buffers — likely false positives, triage |
| `value` / `table` | 15 | map slots |
| everything else | ~120 | one-offs |

So: instrument the container helpers, not their callers.

- [ ] **`py/obj.h` `mp_seq_*` macros** (`mp_seq_copy`, `mp_seq_cat`,
      `mp_seq_replace_slice_*`). Barrier inside the macro and every bulk
      element move in the tree is covered at once. Biggest single win.
- [ ] **`py/objlist.c`** — route every `items[...] =` through one static
      inline setter that barriers. `append`/`insert`/`store` are already
      done; `extend`, the slice paths, `sort` and `reverse` are not.
- [ ] **`py/map.c` `mp_map_lookup`** — keys are barriered; the *value* store
      happens at each caller. Add a `mp_map_set_value(elem, v)` helper and
      convert the `->value =` callers to it.
- [x] **`py/objdeque.c`**, **`py/objset.c`** — done.

### Done in the first pass

- [x] `GC_WB` / `GC_WB_N` / `GC_WB_PTR` macros + `MICROPY_GC_BARRIER_FULL=0`
      escape hatch (`py/gc.h`), and `gc_barrier_range()` (`py/gc.c`).
- [x] `mp_seq_copy` / `mp_seq_cat` bulk moves (`py/obj.h`).
- [x] `py/objlist.c` — `list_set()` helper; `extend` bulk path.
- [x] `py/map.c` — `mp_map_set_value()` helper; set-table inserts;
      `mp_map_init` / `mp_set_init` / `mp_set_rehash` table publishes.
- [x] Converted `->value =` callers: `objdict.c`, `objtype.c`, `objobject.c`,
      `objmodule.c`, `parse.c`.
- [x] `py/objdeque.c`, `py/objset.c`, `py/objarray.c`, `py/objexcept.c`.
- [x] `py/objgenerator.c` — re-gray the generator across execution
      (partial; see Structural above).
- [x] **esp32 port** (`ports/esp32/`): `machine_timer.c`, `machine_uart.c`,
      `modaudio.c`, `moddeck.c`, `modespnow.c`, `modsocket.c`, `modesp32.c`,
      `network_ppp.c`, `network_wlan.c`, `vfs_utils.c`, `esp32_rmt.c`.
      Applied as `X = expr; GC_WB(X);` -- barrier *after* the store, reading
      back the lvalue, which avoids double-evaluating the RHS and is safe
      because nothing allocates in between.

## Structural — not fixable by a store-site barrier

- [ ] **Generator frames (`state`, 47 sites, mostly `py/vm.c`).**
      A generator's `mp_code_state_t` is embedded in the heap-allocated
      generator object, so every `PUSH()`, `SET_TOP()`, `sp[n] =` and
      `STORE_FAST` executing inside a generator is an unbarriered store into a
      heap object. The same `py/vm.c` lines are safe in a normal call (frame
      on the C stack / pystack, rescanned) and unsafe in a generator, so no
      list of store sites fixes it.
      Options, cheapest first:
        a. Re-gray the generator object on every resume/yield boundary
           (`GC_GRAY_MARK_PTR(gen)` in `mp_obj_gen_resume`) so its frame is
           rescanned each time it runs. Coarse but correct and ~2 lines.
        b. Barrier the VM store macros, paying the cost in all frames.
        c. Move generator frames out of the GC heap.
      **(a) is almost certainly the right answer — do it before the long tail.**
- [ ] **Native / viper emitted code** stores directly and never routes through
      any of these paths. Audit `py/emitnative.c` or accept that
      `@micropython.native` is unsound under incremental GC and gate it.

## Known false-positive classes in the list below

The list is generated by matching assignments to fields whose declared type is
`mp_obj_t` / `mp_obj_*_t *` / `mp_map_elem_t *`. It over-matches:

- `vstr->buf`, `objstr` `->data`, `objarray` `->items` on a byte/int array —
  raw data, no pointers, no barrier needed.
- Compile-time-only structures (`py/compile.c`, `py/parse.c`, `py/emit*.c`)
  where values are small ints or qstrs.
- Anything unreachable in your build (`extmod/` modules you do not enable).

Triage against the three-part rule; do not apply blindly.

## Verification

```
cd ports/unix && make DEBUG=1 BUILD=build-ver \
  CFLAGS_EXTRA="-DGC_INC_VERIFY=1 -DMICROPY_GC_INCR_TRIGGER_BLOCKS=64 \
               -DMICROPY_GC_INCR_SLICE_WORDS=16 -DMICROPY_GC_INCR_SLICE_GAP_US=0"
cd ../../tests
for f in basics/*.py misc/*.py float/*.py extmod/*.py; do \
  ../ports/unix/build-ver/micropython $f 2>&1 | grep -q GC-VERIFY && echo $f; done
```

`gc_inc_verify()` in `py/gc.c` asserts, after the final drain, that no live
(`AT_MARK`) block holds a word pointing at an unmarked (`AT_HEAD`) block. The
drain scans exactly those words, so **a hit is always a genuine missed
barrier — no false positives.**

The verifier names the container and the lost object, e.g.

```
GC-VERIFY: live <set> block 0x... word 3 -> unmarked <?> 0x...
```

`<set>` word 3 is `set->table`; `<?>` means the block has no `mp_obj_base_t`
(a raw buffer: map table, items array, ...). Word index x sizeof(void*) gives
the byte offset into the struct, which identifies the field.

Progress: **24 files -> 12** after the first pass. Target is 0.
Also run `ports/esp32/gc_torture.py` after each batch.

### Still tripping (as of the first pass)

```
basics/array_micropython.py    live <array> word 3 -> <?>        (items buffer)
basics/frozenset_binop.py      live <?>     word 16 -> <list>
basics/set_binop.py            live <?>     word 14 -> <list>
basics/string_format_modulo_int.py  live <?> word 18 -> <str>
basics/string_format_sep.py    live <?>     word 12 -> <str>
misc/rge_sm.py                 live <?>     word 7  -> <?>
float/float_format.py          live <?>     word 8  -> <str>
float/math_domain.py           live <?>     word 11 -> <tuple>
float/math_domain_special.py   live <?>     word 11 -> <tuple>
float/string_format_modulo2.py live <?>     word 17 -> <float>
float/string_format_modulo2_intbig.py  live <?> word 17 -> <float>
extmod/asyncio_fair.py         live <StopIteration> word 2 -> <?>
```

The `<?>` containers are raw pointer buffers. To identify one, print its size
in blocks alongside the address in `gc_inc_verify()` -- a map table is a run of
key/value pairs, an items array is a flat run.

---

# Site checklist

Residual sites after filtering out const RHS, freshly-allocated targets, and
sites already instrumented.

## py/ core — do this first (237 sites)

### `py/profile.c` (82)

- [ ] `py/profile.c:220` — `args->event = MP_OBJ_NEW_QSTR(MP_QSTR_call);`
- [ ] `py/profile.c:224` — `code_state->frame->callback = mp_obj_is_callable(top) ? top : MP_OBJ_NULL;`
- [ ] `py/profile.c:273` — `args->event = MP_OBJ_NEW_QSTR(MP_QSTR_exception);`
- [ ] `py/profile.c:285` — `args->event = MP_OBJ_NEW_QSTR(MP_QSTR_line);`
- [ ] `py/profile.c:292` — `args->event = MP_OBJ_NEW_QSTR(MP_QSTR_return);`
- [ ] `py/profile.c:302` — `args->event = MP_OBJ_NEW_QSTR(MP_QSTR_opcode);`
- [ ] `py/profile.c:379` — `instruction->arg = num;`
- [ ] `py/profile.c:386` — `instruction->arg = qst;`
- [ ] `py/profile.c:387` — `instruction->argobj = MP_OBJ_NEW_QSTR(qst);`
- [ ] `py/profile.c:393` — `instruction->arg = unum;`
- [ ] `py/profile.c:394` — `instruction->argobj = obj;`
- [ ] `py/profile.c:404` — `instruction->arg = unum;`
- [ ] `py/profile.c:410` — `instruction->arg = unum;`
- [ ] `py/profile.c:416` — `instruction->arg = qst;`
- [ ] `py/profile.c:417` — `instruction->argobj = MP_OBJ_NEW_QSTR(qst);`
- [ ] `py/profile.c:423` — `instruction->arg = qst;`
- [ ] `py/profile.c:424` — `instruction->argobj = MP_OBJ_NEW_QSTR(qst);`
- [ ] `py/profile.c:430` — `instruction->arg = qst;`
- [ ] `py/profile.c:431` — `instruction->argobj = MP_OBJ_NEW_QSTR(qst);`
- [ ] `py/profile.c:437` — `instruction->arg = qst;`
- [ ] `py/profile.c:438` — `instruction->argobj = MP_OBJ_NEW_QSTR(qst);`
- [ ] `py/profile.c:444` — `instruction->arg = qst;`
- [ ] `py/profile.c:445` — `instruction->argobj = MP_OBJ_NEW_QSTR(qst);`
- [ ] `py/profile.c:459` — `instruction->arg = unum;`
- [ ] `py/profile.c:465` — `instruction->arg = unum;`
- [ ] `py/profile.c:471` — `instruction->arg = qst;`
- [ ] `py/profile.c:472` — `instruction->argobj = MP_OBJ_NEW_QSTR(qst);`
- [ ] `py/profile.c:478` — `instruction->arg = qst;`
- [ ] `py/profile.c:479` — `instruction->argobj = MP_OBJ_NEW_QSTR(qst);`
- [ ] `py/profile.c:485` — `instruction->arg = qst;`
- [ ] `py/profile.c:486` — `instruction->argobj = MP_OBJ_NEW_QSTR(qst);`
- [ ] `py/profile.c:496` — `instruction->arg = unum;`
- [ ] `py/profile.c:502` — `instruction->arg = unum;`
- [ ] `py/profile.c:508` — `instruction->arg = qst;`
- [ ] `py/profile.c:509` — `instruction->argobj = MP_OBJ_NEW_QSTR(qst);`
- [ ] `py/profile.c:515` — `instruction->arg = qst;`
- [ ] `py/profile.c:516` — `instruction->argobj = MP_OBJ_NEW_QSTR(qst);`
- [ ] `py/profile.c:542` — `instruction->arg = unum;`
- [ ] `py/profile.c:548` — `instruction->arg = unum;`
- [ ] `py/profile.c:554` — `instruction->arg = unum;`
- [ ] `py/profile.c:560` — `instruction->arg = unum;`
- [ ] `py/profile.c:566` — `instruction->arg = unum;`
- [ ] `py/profile.c:572` — `instruction->arg = unum;`
- [ ] `py/profile.c:582` — `instruction->arg = unum;`
- [ ] `py/profile.c:588` — `instruction->arg = unum;`
- [ ] `py/profile.c:594` — `instruction->arg = unum;`
- [ ] `py/profile.c:616` — `instruction->arg = unum;`
- [ ] `py/profile.c:622` — `instruction->arg = unum;`
- [ ] `py/profile.c:628` — `instruction->arg = unum;`
- [ ] `py/profile.c:634` — `instruction->arg = unum;`
- [ ] `py/profile.c:644` — `instruction->arg = unum;`
- [ ] `py/profile.c:651` — `instruction->arg = unum;`
- [ ] `py/profile.c:658` — `instruction->arg = unum;`
- [ ] `py/profile.c:664` — `instruction->arg = unum;`
- [ ] `py/profile.c:670` — `instruction->arg = unum;`
- [ ] `py/profile.c:676` — `instruction->arg = unum;`
- [ ] `py/profile.c:677` — `instruction->argobj = mp_obj_new_int_from_ull((uint64_t)ptr);`
- [ ] `py/profile.c:683` — `instruction->arg = unum;`
- [ ] `py/profile.c:684` — `instruction->argobj = mp_obj_new_int_from_ull((uint64_t)ptr);`
- [ ] `py/profile.c:691` — `instruction->arg = unum;`
- [ ] `py/profile.c:692` — `instruction->argobj = mp_obj_new_int_from_ull((uint64_t)ptr);`
- [ ] `py/profile.c:693` — `instruction->argobjex_cache = MP_OBJ_NEW_SMALL_INT(n_closed_over);`
- [ ] `py/profile.c:701` — `instruction->arg = unum;`
- [ ] `py/profile.c:702` — `instruction->argobj = mp_obj_new_int_from_ull((uint64_t)ptr);`
- [ ] `py/profile.c:703` — `instruction->argobjex_cache = MP_OBJ_NEW_SMALL_INT(n_closed_over);`
- [ ] `py/profile.c:710` — `instruction->arg = unum & 0xff;`
- [ ] `py/profile.c:711` — `instruction->argobjex_cache = MP_OBJ_NEW_SMALL_INT((unum >> 8) & 0xff);`
- [ ] `py/profile.c:717` — `instruction->arg = unum & 0xff;`
- [ ] `py/profile.c:718` — `instruction->argobjex_cache = MP_OBJ_NEW_SMALL_INT((unum >> 8) & 0xff);`
- [ ] `py/profile.c:724` — `instruction->arg = unum & 0xff;`
- [ ] `py/profile.c:725` — `instruction->argobjex_cache = MP_OBJ_NEW_SMALL_INT((unum >> 8) & 0xff);`
- [ ] `py/profile.c:731` — `instruction->arg = unum & 0xff;`
- [ ] `py/profile.c:732` — `instruction->argobjex_cache = MP_OBJ_NEW_SMALL_INT((unum >> 8) & 0xff);`
- [ ] `py/profile.c:762` — `instruction->arg = qst;`
- [ ] `py/profile.c:763` — `instruction->argobj = MP_OBJ_NEW_QSTR(qst);`
- [ ] `py/profile.c:769` — `instruction->arg = qst;`
- [ ] `py/profile.c:770` — `instruction->argobj = MP_OBJ_NEW_QSTR(qst);`
- [ ] `py/profile.c:780` — `instruction->arg = (mp_int_t)ip[-1] - MP_BC_LOAD_CONST_SMALL_INT_MULTI - 16;`
- [ ] `py/profile.c:783` — `instruction->arg = (mp_uint_t)ip[-1] - MP_BC_LOAD_FAST_MULTI;`
- [ ] `py/profile.c:786` — `instruction->arg = (mp_uint_t)ip[-1] - MP_BC_STORE_FAST_MULTI;`
- [ ] `py/profile.c:789` — `instruction->arg = (mp_uint_t)ip[-1] - MP_BC_UNARY_OP_MULTI;`
- [ ] `py/profile.c:793` — `instruction->arg = op;`

### `py/vm.c` (17)

- [ ] `py/vm.c:127` — `exc_sp->handler = ip + ulab; \`
- [ ] `py/vm.c:128` — `exc_sp->val_sp = MP_TAGPTR_MAKE(sp, ((with_or_finally) << 1)); \`
- [ ] `py/vm.c:466` — `ce->type = type;`
- [ ] `py/vm.c:468` — `ce->func = *(sp - 1);`
- [ ] `py/vm.c:818` — `code_state->sp = sp;`
- [ ] `py/vm.c:1002` — `code_state->sp = sp;`
- [ ] `py/vm.c:1039` — `code_state->sp = sp;`
- [ ] `py/vm.c:1083` — `code_state->sp = sp;`
- [ ] `py/vm.c:1124` — `code_state->sp = sp;`
- [ ] `py/vm.c:1193` — `code_state->sp = sp;`
- [ ] `py/vm.c:1209` — `*code_state->sp = res;`
- [ ] `py/vm.c:1252` — `code_state->sp = sp;`
- [ ] `py/vm.c:1263` — `code_state->sp = sp; // Save sp because it's needed if mp_resume raises StopIteration`
- [ ] `py/vm.c:1545` — `code_state->state[0] = obj;`
- [ ] `py/vm.c:1634` — `*code_state->sp = mp_obj_exception_get_value(MP_OBJ_FROM_PTR(nlr.ret_val));`
- [ ] `py/vm.c:1698` — `code_state->sp = sp;`
- [ ] `py/vm.c:1723` — `code_state->state[0] = MP_OBJ_FROM_PTR(nlr.ret_val); // put exception here because sp is invalid`

### `py/objlist.c` (10)

- [ ] `py/objlist.c:192` — `self->items = m_renew(mp_obj_t, self->items, self->alloc, self->len + len_adj);`
- [ ] `py/objlist.c:232` — `self->items = m_renew(mp_obj_t, self->items, self->alloc, self->alloc * 2);`
- [ ] `py/objlist.c:249` — `self->items = m_renew(mp_obj_t, self->items, self->alloc, self->len + arg->len + 4);`
- [ ] `py/objlist.c:275` — `self->items = m_renew(mp_obj_t, self->items, self->alloc, self->alloc / 2);`
- [ ] `py/objlist.c:355` — `self->items = m_renew(mp_obj_t, self->items, self->alloc, LIST_MIN_ALLOC);`
- [ ] `py/objlist.c:397` — `self->items[i] = self->items[i - 1];`
- [ ] `py/objlist.c:421` — `self->items[i] = self->items[len - i - 1];`
- [ ] `py/objlist.c:422` — `self->items[len - i - 1] = a;`
- [ ] `py/objlist.c:474` — `o->items = m_new(mp_obj_t, o->alloc);`
- [ ] `py/objlist.c:541` — `o->list = list;`

### `py/objarray.c` (9)

- [ ] `py/objarray.c:112` — `o->items = m_new_data(byte, typecode_size * o->len);`
- [ ] `py/objarray.c:115` — `o->items = m_new_data(byte, typecode_size * o->len);`
- [ ] `py/objarray.c:118` — `o->items = m_new(byte, typecode_size * o->len);`
- [ ] `py/objarray.c:249` — `self->items = other->items;`
- [ ] `py/objarray.c:408` — `self->items = m_renew_data(byte, self->items, item_sz * self->len, item_sz * (self->len + add_cnt));`
- [ ] `py/objarray.c:441` — `self->items = m_renew_data(byte, self->items, (self->len + self->free) * sz, (self->len + len) * sz);`
- [ ] `py/objarray.c:529` — `o->items = m_renew_data(byte, o->items, (o->len + o->free) * item_sz, (o->len + len_adj) * item_sz);`
- [ ] `py/objarray.c:602` — `bufinfo->buf = o->items;`
- [ ] `py/objarray.c:611` — `bufinfo->buf = (uint8_t *)bufinfo->buf + (size_t)o->memview_offset * sz;`

### `py/objtype.c` (9)

- [ ] `py/objtype.c:93` — `self->subobj[0] = MP_OBJ_TYPE_GET_SLOT(native_base, make_new)(native_base, n_args - 1, kw_args->used, args + 1`
- [ ] `py/objtype.c:138` — `assert(lookup->dest[0] == MP_OBJ_NULL);`
- [ ] `py/objtype.c:139` — `assert(lookup->dest[1] == MP_OBJ_NULL);`
- [ ] `py/objtype.c:373` — `o->subobj[0] = MP_OBJ_TYPE_GET_SLOT(native_base, make_new)(native_base, n_args, n_kw, args);`
- [ ] `py/objtype.c:1002` — `tail->next = m_new_obj(setname_list_t);`
- [ ] `py/objtype.c:1004` — `tail->call[3] = name;`
- [ ] `py/objtype.c:1015` — `head->call[2] = owner;`
- [ ] `py/objtype.c:1225` — `o->name = name;`
- [ ] `py/objtype.c:1303` — `elem->value = static_class_method_make_new(&mp_type_staticmethod, 1, 0, &elem->value);`

### `py/compile.c` (8)

- [ ] `py/compile.c:224` — `elem->value = MP_OBJ_NEW_SMALL_INT(0);`
- [ ] `py/compile.c:274` — `comp->compile_error = mp_obj_new_exception_msg(&mp_type_SyntaxError, msg);`
- [ ] `py/compile.c:324` — `s->next = scope;`
- [ ] `py/compile.c:2849` — `comp->compile_error = mp_obj_new_exception_msg_varg(&mp_type_ViperTypeError, MP_ERROR_TEXT("unknown type '%q'"`
- [ ] `py/compile.c:3503` — `for (scope_t *s = comp->scope_head; s != NULL && comp->compile_error == MP_OBJ_NULL; s = s->next) {`
- [ ] `py/compile.c:3528` — `for (scope_t *s = comp->scope_head; s != NULL && comp->compile_error == MP_OBJ_NULL; s = s->next) {`
- [ ] `py/compile.c:3539` — `for (scope_t *s = comp->scope_head; s != NULL && comp->compile_error == MP_OBJ_NULL; s = s->next) {`
- [ ] `py/compile.c:3579` — `comp->compile_error = mp_obj_new_exception_msg(&mp_type_NotImplementedError, MP_ERROR_TEXT("cannot emit native`

### `py/objexcept.c` (8)

- [ ] `py/objexcept.c:142` — `o->args = (mp_obj_tuple_t *)&mp_const_empty_tuple_obj;`
- [ ] `py/objexcept.c:146` — `o->args = (mp_obj_tuple_t *)&mp_const_empty_tuple_obj;`
- [ ] `py/objexcept.c:151` — `o_str->data = buf;`
- [ ] `py/objexcept.c:251` — `o_exc->args = o_tuple;`
- [ ] `py/objexcept.c:421` — `o_str->data = (const byte *)msg;`
- [ ] `py/objexcept.c:454` — `pr->buf = new_buf;`
- [ ] `py/objexcept.c:509` — `o_str->data = (const byte *)fmt;`
- [ ] `py/objexcept.c:526` — `o_str->data = exc_pr.buf;`

### `py/objstr.c` (8)

- [ ] `py/objstr.c:224` — `o->data = str_data;`
- [ ] `py/objstr.c:269` — `o->data = str_data;`
- [ ] `py/objstr.c:742` — `res->items[idx] = mp_obj_new_str_of_type(self_type, beg, last - beg);`
- [ ] `py/objstr.c:745` — `res->items[idx--] = mp_obj_new_str_of_type(self_type, s + sep_len, last - s - sep_len);`
- [ ] `py/objstr.c:2065` — `bufinfo->buf = (void *)str_data;`
- [ ] `py/objstr.c:2076` — `str->data = data;`
- [ ] `py/objstr.c:2448` — `o->str = str;`
- [ ] `py/objstr.c:2471` — `o->str = str;`

### `py/map.c` (7)

- [ ] `py/map.c:95` — `map->table = m_new0(mp_map_elem_t, map->alloc);`
- [ ] `py/map.c:109` — `map->table = (mp_map_elem_t *)table;`
- [ ] `py/map.c:147` — `map->table = new_table;`
- [ ] `py/map.c:211` — `elem->value = value;`
- [ ] `py/map.c:225` — `map->table = m_renew(mp_map_elem_t, map->table, map->used, map->alloc);`
- [ ] `py/map.c:341` — `set->table = m_new0(mp_obj_t, set->alloc);`
- [ ] `py/map.c:349` — `set->table = m_new0(mp_obj_t, set->alloc);`

### `py/pairheap.c` (6)

- [ ] `py/pairheap.c:50` — `heap1->child_last->next = heap2;`
- [ ] `py/pairheap.c:53` — `heap2->next = NEXT_MAKE_RIGHTMOST_PARENT(heap1);`
- [ ] `py/pairheap.c:56` — `heap1->next = heap2->child;`
- [ ] `py/pairheap.c:60` — `heap1->next = NEXT_MAKE_RIGHTMOST_PARENT(heap2);`
- [ ] `py/pairheap.c:139` — `n->next = node;`
- [ ] `py/pairheap.c:142` — `node->next = next;`

### `py/vstr.c` (6)

- [ ] `py/vstr.c:47` — `vstr->buf = m_new_data(char, vstr->alloc);`
- [ ] `py/vstr.c:61` — `vstr->buf = buf;`
- [ ] `py/vstr.c:67` — `print->data = vstr;`
- [ ] `py/vstr.c:103` — `vstr->buf = new_buf;`
- [ ] `py/vstr.c:117` — `vstr->buf = new_buf;`
- [ ] `py/vstr.c:138` — `vstr->buf[vstr->len] = '\0';`

### `py/objdeque.c` (4)

- [ ] `py/objdeque.c:111` — `self->items[self->i_put] = arg;`
- [ ] `py/objdeque.c:137` — `self->items[self->i_get] = arg;`
- [ ] `py/objdeque.c:220` — `self->items[index_val] = value;`
- [ ] `py/objdeque.c:307` — `o->deque = deque;`

### `py/objgetitemiter.c` (4)

- [ ] `py/objgetitemiter.c:44` — `self->args[2] = MP_OBJ_NEW_SMALL_INT(MP_OBJ_SMALL_INT_VALUE(self->args[2]) + 1);`
- [ ] `py/objgetitemiter.c:71` — `o->args[0] = args[0];`
- [ ] `py/objgetitemiter.c:72` — `o->args[1] = args[1];`
- [ ] `py/objgetitemiter.c:73` — `o->args[2] = MP_OBJ_NEW_SMALL_INT(0);`

### `py/runtime.c` (4)

- [ ] `py/runtime.c:570` — `tuple->items[0] = MP_OBJ_NEW_SMALL_INT(mp_small_int_floor_divide(lhs_val, rhs_val));`
- [ ] `py/runtime.c:571` — `tuple->items[1] = MP_OBJ_NEW_SMALL_INT(mp_small_int_modulo(lhs_val, rhs_val));`
- [ ] `py/runtime.c:927` — `out_args->fun = fun;`
- [ ] `py/runtime.c:928` — `out_args->args = args2;`

### `py/bc.c` (3)

- [ ] `py/bc.c:235` — `elem->value = kwargs[2 * i + 1];`
- [ ] `py/bc.c:324` — `code_state->sp = &code_state->state[0] - 1;`
- [ ] `py/bc.c:342` — `code_state->sp = &code_state->state[0] - 1;`

### `py/emitinlinerv32.c` (3)

- [ ] `py/emitinlinerv32.c:78` — `*emit->error_slot = mp_obj_new_exception_msg(&mp_type_SyntaxError, msg);`
- [ ] `py/emitinlinerv32.c:82` — `*emit->error_slot = exc;`
- [ ] `py/emitinlinerv32.c:102` — `emit->error_slot = error_slot;`

### `py/emitinlinethumb.c` (3)

- [ ] `py/emitinlinethumb.c:78` — `*emit->error_slot = mp_obj_new_exception_msg(&mp_type_SyntaxError, msg);`
- [ ] `py/emitinlinethumb.c:82` — `*emit->error_slot = exc;`
- [ ] `py/emitinlinethumb.c:102` — `emit->error_slot = error_slot;`

### `py/emitinlinextensa.c` (3)

- [ ] `py/emitinlinextensa.c:47` — `*emit->error_slot = mp_obj_new_exception_msg(&mp_type_SyntaxError, msg);`
- [ ] `py/emitinlinextensa.c:51` — `*emit->error_slot = exc;`
- [ ] `py/emitinlinextensa.c:71` — `emit->error_slot = error_slot;`

### `py/objnamedtuple.c` (3)

- [ ] `py/objnamedtuple.c:139` — `tuple->items[id] = args[i + 1];`
- [ ] `py/objnamedtuple.c:149` — `o->fields[i] = mp_obj_str_get_qstr(fields[i]);`
- [ ] `py/objnamedtuple.c:159` — `type->name = name;`

### `py/objslice.c` (3)

- [ ] `py/objslice.c:186` — `result->start = start;`
- [ ] `py/objslice.c:187` — `result->stop = stop;`
- [ ] `py/objslice.c:188` — `result->step = step;`

### `py/parse.c` (3)

- [ ] `py/parse.c:849` — `assert(elem->value == MP_OBJ_NULL);`
- [ ] `py/parse.c:850` — `elem->value = value;`
- [ ] `py/parse.c:925` — `tuple->items[--i] = mp_parse_node_convert_to_obj(pn);`

### `py/sequence.c` (3)

- [ ] `py/sequence.c:58` — `indexes->stop = indexes->start;`
- [ ] `py/sequence.c:60` — `indexes->stop = indexes->start + 1;`
- [ ] `py/sequence.c:63` — `return indexes->step == 1;`

### `py/malloc.c` (2)

- [ ] `py/malloc.c:326` — `node->next = MP_STATE_VM(m_tracked_head);`
- [ ] `py/malloc.c:359` — `node->prev->next = node->next;`

### `py/modstruct.c` (2)

- [ ] `py/modstruct.c:169` — `res->items[i++] = item;`
- [ ] `py/modstruct.c:173` — `res->items[i++] = item;`

### `py/modsys.c` (2)

- [ ] `py/modsys.c:234` — `t->items[0] = MP_OBJ_FROM_PTR(mp_obj_get_type(cur_exc));`
- [ ] `py/modsys.c:235` — `t->items[1] = cur_exc;`

### `py/objcode.c` (2)

- [ ] `py/objcode.c:65` — `consts->items[const_no++] = code;`
- [ ] `py/objcode.c:206` — `o->lnotab = raw_code_lnotab(rc);`

### `py/objdict.c` (2)

- [ ] `py/objdict.c:502` — `o->dict = view->dict;`
- [ ] `py/objdict.c:595` — `o->dict = self_in;`

### `py/objrange.c` (2)

- [ ] `py/objrange.c:59` — `o->stop = stop;`
- [ ] `py/objrange.c:60` — `o->step = step;`

### `py/reader.c` (2)

- [ ] `py/reader.c:65` — `reader->data = rm;`
- [ ] `py/reader.c:143` — `reader->data = rp;`

### `py/ringbuf.c` (2)

- [ ] `py/ringbuf.c:70` — `r->buf[r->iput] = (v >> 8) & 0xff;`
- [ ] `py/ringbuf.c:71` — `r->buf[iput_a] = v & 0xff;`

### `py/emitcommon.c` (1)

- [ ] `py/emitcommon.c:39` — `elem->value = MP_OBJ_NEW_SMALL_INT(emit->qstr_map.used - 1);`

### `py/emitnative.c` (1)

- [ ] `py/emitnative.c:180` — `*emit->error_slot = mp_obj_new_exception_msg_varg(&mp_type_ViperTypeError, __VA_ARGS__); \`

### `py/modthread.c` (1)

- [ ] `py/modthread.c:262` — `th_args->fun = args[0];`

### `py/nativeglue.c` (1)

- [ ] `py/nativeglue.c:180` — `iter->buf[0] = obj;`

### `py/nlr.c` (1)

- [ ] `py/nlr.c:56` — `node->fun = fun;`

### `py/objboundmeth.c` (1)

- [ ] `py/objboundmeth.c:108` — `return mp_obj_new_bool(lhs->self == rhs->self && lhs->meth == rhs->meth);`

### `py/objgenerator.c` (1)

- [ ] `py/objgenerator.c:340` — `self->pend_exc = exc_in;`

### `py/objint_longlong.c` (1)

- [ ] `py/objint_longlong.c:141` — `self->val = -self->val;`

### `py/objmodule.c` (1)

- [ ] `py/objmodule.c:140` — `el->value = MP_OBJ_FROM_PTR(o);`

### `py/objstringio.c` (1)

- [ ] `py/objstringio.c:72` — `o->vstr->buf = m_new_data(char, o->vstr->len);`

### `py/objstrunicode.c` (1)

- [ ] `py/objstrunicode.c:275` — `o->str = str;`

### `py/objtuple.c` (1)

- [ ] `py/objtuple.c:293` — `o->tuple = MP_OBJ_TO_PTR(o_in);`

### `py/objzip.c` (1)

- [ ] `py/objzip.c:64` — `tuple->items[i] = next;`

### `py/persistentcode.c` (1)

- [ ] `py/persistentcode.c:248` — `tuple->items[i] = load_obj(reader);`

### `py/scheduler.c` (1)

- [ ] `py/scheduler.c:191` — `node->callback = callback;`

## extmod/ — only what your build enables (252 sites)

### `extmod/vfs_posix.c` (29)

- [ ] `extmod/vfs_posix.c:248` — `t->items[0] = mp_obj_new_str_from_cstr(fn);`
- [ ] `extmod/vfs_posix.c:250` — `t->items[0] = mp_obj_new_bytes((const byte *)fn, strlen(fn));`
- [ ] `extmod/vfs_posix.c:255` — `t->items[1] = MP_OBJ_NEW_SMALL_INT(DTTOIF(dirent->d_type));`
- [ ] `extmod/vfs_posix.c:258` — `t->items[1] = MP_OBJ_NEW_SMALL_INT(MP_S_IFDIR);`
- [ ] `extmod/vfs_posix.c:260` — `t->items[1] = MP_OBJ_NEW_SMALL_INT(MP_S_IFREG);`
- [ ] `extmod/vfs_posix.c:262` — `t->items[1] = MP_OBJ_NEW_SMALL_INT(dirent->d_type);`
- [ ] `extmod/vfs_posix.c:267` — `t->items[1] = MP_OBJ_NEW_SMALL_INT(0);`
- [ ] `extmod/vfs_posix.c:271` — `t->items[2] = MP_OBJ_NEW_SMALL_INT(dirent->d_ino);`
- [ ] `extmod/vfs_posix.c:273` — `t->items[2] = MP_OBJ_NEW_SMALL_INT(0);`
- [ ] `extmod/vfs_posix.c:368` — `t->items[0] = MP_OBJ_NEW_SMALL_INT(sb.st_mode);`
- [ ] `extmod/vfs_posix.c:369` — `t->items[1] = mp_obj_new_int_from_uint(sb.st_ino);`
- [ ] `extmod/vfs_posix.c:370` — `t->items[2] = mp_obj_new_int_from_uint(sb.st_dev);`
- [ ] `extmod/vfs_posix.c:371` — `t->items[3] = mp_obj_new_int_from_uint(sb.st_nlink);`
- [ ] `extmod/vfs_posix.c:372` — `t->items[4] = mp_obj_new_int_from_uint(sb.st_uid);`
- [ ] `extmod/vfs_posix.c:373` — `t->items[5] = mp_obj_new_int_from_uint(sb.st_gid);`
- [ ] `extmod/vfs_posix.c:374` — `t->items[6] = mp_obj_new_int_from_uint(sb.st_size);`
- [ ] `extmod/vfs_posix.c:375` — `t->items[7] = mp_obj_new_int_from_uint(sb.st_atime);`
- [ ] `extmod/vfs_posix.c:376` — `t->items[8] = mp_obj_new_int_from_uint(sb.st_mtime);`
- [ ] `extmod/vfs_posix.c:377` — `t->items[9] = mp_obj_new_int_from_uint(sb.st_ctime);`
- [ ] `extmod/vfs_posix.c:411` — `t->items[0] = MP_OBJ_NEW_SMALL_INT(sb.f_bsize);`
- [ ] `extmod/vfs_posix.c:412` — `t->items[1] = MP_OBJ_NEW_SMALL_INT(sb.f_frsize);`
- [ ] `extmod/vfs_posix.c:413` — `t->items[2] = MP_OBJ_NEW_SMALL_INT(sb.f_blocks);`
- [ ] `extmod/vfs_posix.c:414` — `t->items[3] = MP_OBJ_NEW_SMALL_INT(sb.f_bfree);`
- [ ] `extmod/vfs_posix.c:415` — `t->items[4] = MP_OBJ_NEW_SMALL_INT(sb.f_bavail);`
- [ ] `extmod/vfs_posix.c:416` — `t->items[5] = MP_OBJ_NEW_SMALL_INT(sb.f_files);`
- [ ] `extmod/vfs_posix.c:417` — `t->items[6] = MP_OBJ_NEW_SMALL_INT(sb.f_ffree);`
- [ ] `extmod/vfs_posix.c:418` — `t->items[7] = MP_OBJ_NEW_SMALL_INT(F_FAVAIL);`
- [ ] `extmod/vfs_posix.c:419` — `t->items[8] = MP_OBJ_NEW_SMALL_INT(F_FLAG);`
- [ ] `extmod/vfs_posix.c:420` — `t->items[9] = MP_OBJ_NEW_SMALL_INT(F_NAMEMAX);`

### `extmod/vfs_fat.c` (26)

- [ ] `extmod/vfs_fat.c:157` — `t->items[0] = mp_obj_new_str_from_cstr(fn);`
- [ ] `extmod/vfs_fat.c:159` — `t->items[0] = mp_obj_new_bytes((const byte *)fn, strlen(fn));`
- [ ] `extmod/vfs_fat.c:163` — `t->items[1] = MP_OBJ_NEW_SMALL_INT(MP_S_IFDIR);`
- [ ] `extmod/vfs_fat.c:166` — `t->items[1] = MP_OBJ_NEW_SMALL_INT(MP_S_IFREG);`
- [ ] `extmod/vfs_fat.c:168` — `t->items[2] = MP_OBJ_NEW_SMALL_INT(0); // no inode number`
- [ ] `extmod/vfs_fat.c:169` — `t->items[3] = mp_obj_new_int_from_uint(fno.fsize);`
- [ ] `extmod/vfs_fat.c:479` — `t->items[0] = MP_OBJ_NEW_SMALL_INT(mode); // st_mode`
- [ ] `extmod/vfs_fat.c:480` — `t->items[1] = MP_OBJ_NEW_SMALL_INT(0); // st_ino`
- [ ] `extmod/vfs_fat.c:481` — `t->items[2] = MP_OBJ_NEW_SMALL_INT(0); // st_dev`
- [ ] `extmod/vfs_fat.c:482` — `t->items[3] = MP_OBJ_NEW_SMALL_INT(0); // st_nlink`
- [ ] `extmod/vfs_fat.c:483` — `t->items[4] = MP_OBJ_NEW_SMALL_INT(0); // st_uid`
- [ ] `extmod/vfs_fat.c:484` — `t->items[5] = MP_OBJ_NEW_SMALL_INT(0); // st_gid`
- [ ] `extmod/vfs_fat.c:485` — `t->items[6] = mp_obj_new_int_from_uint(fno.fsize); // st_size`
- [ ] `extmod/vfs_fat.c:486` — `t->items[7] = timeutils_obj_from_timestamp(seconds); // st_atime`
- [ ] `extmod/vfs_fat.c:487` — `t->items[8] = timeutils_obj_from_timestamp(seconds); // st_mtime`
- [ ] `extmod/vfs_fat.c:488` — `t->items[9] = timeutils_obj_from_timestamp(seconds); // st_ctime`
- [ ] `extmod/vfs_fat.c:508` — `t->items[0] = MP_OBJ_NEW_SMALL_INT(fatfs->csize * SECSIZE(fatfs)); // f_bsize`
- [ ] `extmod/vfs_fat.c:509` — `t->items[1] = t->items[0]; // f_frsize`
- [ ] `extmod/vfs_fat.c:510` — `t->items[2] = MP_OBJ_NEW_SMALL_INT((fatfs->n_fatent - 2)); // f_blocks`
- [ ] `extmod/vfs_fat.c:511` — `t->items[3] = MP_OBJ_NEW_SMALL_INT(nclst); // f_bfree`
- [ ] `extmod/vfs_fat.c:512` — `t->items[4] = t->items[3]; // f_bavail`
- [ ] `extmod/vfs_fat.c:513` — `t->items[5] = MP_OBJ_NEW_SMALL_INT(0); // f_files`
- [ ] `extmod/vfs_fat.c:514` — `t->items[6] = MP_OBJ_NEW_SMALL_INT(0); // f_ffree`
- [ ] `extmod/vfs_fat.c:515` — `t->items[7] = MP_OBJ_NEW_SMALL_INT(0); // f_favail`
- [ ] `extmod/vfs_fat.c:516` — `t->items[8] = MP_OBJ_NEW_SMALL_INT(0); // f_flags`
- [ ] `extmod/vfs_fat.c:517` — `t->items[9] = MP_OBJ_NEW_SMALL_INT(FF_MAX_LFN); // f_namemax`

### `extmod/vfs_lfsx.c` (26)

- [ ] `extmod/vfs_lfsx.c:83` — `config->sync = MP_VFS_LFSx(dev_sync);`
- [ ] `extmod/vfs_lfsx.c:201` — `t->items[0] = mp_obj_new_str_from_cstr(info.name);`
- [ ] `extmod/vfs_lfsx.c:203` — `t->items[0] = mp_obj_new_bytes((const byte *)info.name, strlen(info.name));`
- [ ] `extmod/vfs_lfsx.c:205` — `t->items[1] = MP_OBJ_NEW_SMALL_INT(info.type == LFSx_MACRO(_TYPE_REG) ? MP_S_IFREG : MP_S_IFDIR);`
- [ ] `extmod/vfs_lfsx.c:206` — `t->items[2] = MP_OBJ_NEW_SMALL_INT(0); // no inode number`
- [ ] `extmod/vfs_lfsx.c:207` — `t->items[3] = MP_OBJ_NEW_SMALL_INT(info.size);`
- [ ] `extmod/vfs_lfsx.c:402` — `t->items[0] = MP_OBJ_NEW_SMALL_INT(info.type == LFSx_MACRO(_TYPE_REG) ? MP_S_IFREG : MP_S_IFDIR); // st_mode`
- [ ] `extmod/vfs_lfsx.c:403` — `t->items[1] = MP_OBJ_NEW_SMALL_INT(0); // st_ino`
- [ ] `extmod/vfs_lfsx.c:404` — `t->items[2] = MP_OBJ_NEW_SMALL_INT(0); // st_dev`
- [ ] `extmod/vfs_lfsx.c:405` — `t->items[3] = MP_OBJ_NEW_SMALL_INT(0); // st_nlink`
- [ ] `extmod/vfs_lfsx.c:406` — `t->items[4] = MP_OBJ_NEW_SMALL_INT(0); // st_uid`
- [ ] `extmod/vfs_lfsx.c:407` — `t->items[5] = MP_OBJ_NEW_SMALL_INT(0); // st_gid`
- [ ] `extmod/vfs_lfsx.c:408` — `t->items[6] = mp_obj_new_int_from_uint(info.size); // st_size`
- [ ] `extmod/vfs_lfsx.c:409` — `t->items[7] = timeutils_obj_from_timestamp(mtime); // st_atime`
- [ ] `extmod/vfs_lfsx.c:410` — `t->items[8] = timeutils_obj_from_timestamp(mtime); // st_mtime`
- [ ] `extmod/vfs_lfsx.c:411` — `t->items[9] = timeutils_obj_from_timestamp(mtime); // st_ctime`
- [ ] `extmod/vfs_lfsx.c:438` — `t->items[0] = MP_OBJ_NEW_SMALL_INT(self->lfs.cfg->block_size); // f_bsize`
- [ ] `extmod/vfs_lfsx.c:439` — `t->items[1] = t->items[0]; // f_frsize`
- [ ] `extmod/vfs_lfsx.c:440` — `t->items[2] = MP_OBJ_NEW_SMALL_INT(self->lfs.cfg->block_count); // f_blocks`
- [ ] `extmod/vfs_lfsx.c:441` — `t->items[3] = MP_OBJ_NEW_SMALL_INT(self->lfs.cfg->block_count - n_used_blocks); // f_bfree`
- [ ] `extmod/vfs_lfsx.c:442` — `t->items[4] = t->items[3]; // f_bavail`
- [ ] `extmod/vfs_lfsx.c:443` — `t->items[5] = MP_OBJ_NEW_SMALL_INT(0); // f_files`
- [ ] `extmod/vfs_lfsx.c:444` — `t->items[6] = MP_OBJ_NEW_SMALL_INT(0); // f_ffree`
- [ ] `extmod/vfs_lfsx.c:445` — `t->items[7] = MP_OBJ_NEW_SMALL_INT(0); // f_favail`
- [ ] `extmod/vfs_lfsx.c:446` — `t->items[8] = MP_OBJ_NEW_SMALL_INT(0); // f_flags`
- [ ] `extmod/vfs_lfsx.c:447` — `t->items[9] = MP_OBJ_NEW_SMALL_INT(LFSx_MACRO(_NAME_MAX)); // f_namemax`

### `extmod/vfs_rom.c` (25)

- [ ] `extmod/vfs_rom.c:365` — `t->items[0] = mp_obj_new_str((const char *)name_str, name_len);`
- [ ] `extmod/vfs_rom.c:367` — `t->items[0] = mp_obj_new_bytes(name_str, name_len);`
- [ ] `extmod/vfs_rom.c:370` — `t->items[1] = MP_OBJ_NEW_SMALL_INT(type);`
- [ ] `extmod/vfs_rom.c:371` — `t->items[2] = MP_OBJ_NEW_SMALL_INT(0);`
- [ ] `extmod/vfs_rom.c:372` — `t->items[3] = mp_obj_new_int(data_len);`
- [ ] `extmod/vfs_rom.c:407` — `t->items[0] = MP_OBJ_NEW_SMALL_INT(stat == MP_IMPORT_STAT_FILE ? MP_S_IFREG : MP_S_IFDIR); // st_mode`
- [ ] `extmod/vfs_rom.c:408` — `t->items[1] = MP_OBJ_NEW_SMALL_INT(0); // st_ino`
- [ ] `extmod/vfs_rom.c:409` — `t->items[2] = MP_OBJ_NEW_SMALL_INT(0); // st_dev`
- [ ] `extmod/vfs_rom.c:410` — `t->items[3] = MP_OBJ_NEW_SMALL_INT(0); // st_nlink`
- [ ] `extmod/vfs_rom.c:411` — `t->items[4] = MP_OBJ_NEW_SMALL_INT(0); // st_uid`
- [ ] `extmod/vfs_rom.c:412` — `t->items[5] = MP_OBJ_NEW_SMALL_INT(0); // st_gid`
- [ ] `extmod/vfs_rom.c:413` — `t->items[6] = MP_OBJ_NEW_SMALL_INT(file_size); // st_size`
- [ ] `extmod/vfs_rom.c:414` — `t->items[7] = MP_OBJ_NEW_SMALL_INT(0); // st_atime`
- [ ] `extmod/vfs_rom.c:415` — `t->items[8] = MP_OBJ_NEW_SMALL_INT(0); // st_mtime`
- [ ] `extmod/vfs_rom.c:416` — `t->items[9] = MP_OBJ_NEW_SMALL_INT(0); // st_ctime`
- [ ] `extmod/vfs_rom.c:426` — `t->items[0] = MP_OBJ_NEW_SMALL_INT(1); // f_bsize`
- [ ] `extmod/vfs_rom.c:427` — `t->items[1] = MP_OBJ_NEW_SMALL_INT(0); // f_frsize`
- [ ] `extmod/vfs_rom.c:428` — `t->items[2] = mp_obj_new_int_from_uint(filesystem_len); // f_blocks`
- [ ] `extmod/vfs_rom.c:429` — `t->items[3] = MP_OBJ_NEW_SMALL_INT(0); // f_bfree`
- [ ] `extmod/vfs_rom.c:430` — `t->items[4] = MP_OBJ_NEW_SMALL_INT(0); // f_bavail`
- [ ] `extmod/vfs_rom.c:431` — `t->items[5] = MP_OBJ_NEW_SMALL_INT(0); // f_files`
- [ ] `extmod/vfs_rom.c:432` — `t->items[6] = MP_OBJ_NEW_SMALL_INT(0); // f_ffree`
- [ ] `extmod/vfs_rom.c:433` — `t->items[7] = MP_OBJ_NEW_SMALL_INT(0); // f_favail`
- [ ] `extmod/vfs_rom.c:434` — `t->items[8] = MP_OBJ_NEW_SMALL_INT(0); // f_flags`
- [ ] `extmod/vfs_rom.c:435` — `t->items[9] = MP_OBJ_NEW_SMALL_INT(32767); // f_namemax`

### `extmod/modlwip.c` (23)

- [ ] `extmod/modlwip.c:376` — `&& socket->pcb.tcp->state == LISTEN;`
- [ ] `extmod/modlwip.c:497` — `socket->state = err;`
- [ ] `extmod/modlwip.c:506` — `socket->state = STATE_CONNECTED;`
- [ ] `extmod/modlwip.c:600` — `socket->state = STATE_PEER_CLOSED;`
- [ ] `extmod/modlwip.c:932` — `socket->type = MOD_NETWORK_SOCK_STREAM;`
- [ ] `extmod/modlwip.c:934` — `socket->state = STATE_NEW;`
- [ ] `extmod/modlwip.c:939` — `socket->type = mp_obj_get_int(args[1]);`
- [ ] `extmod/modlwip.c:983` — `socket->state = STATE_ACTIVE_UDP;`
- [ ] `extmod/modlwip.c:1081` — `socket->state = STATE_LISTENING;`
- [ ] `extmod/modlwip.c:1146` — `socket2->type = MOD_NETWORK_SOCK_STREAM;`
- [ ] `extmod/modlwip.c:1149` — `socket2->state = STATE_CONNECTED;`
- [ ] `extmod/modlwip.c:1163` — `client->items[0] = MP_OBJ_FROM_PTR(socket2);`
- [ ] `extmod/modlwip.c:1164` — `client->items[1] = lwip_format_inet_addr(&socket2->pcb.tcp->remote_ip, port);`
- [ ] `extmod/modlwip.c:1195` — `socket->state = STATE_CONNECTING;`
- [ ] `extmod/modlwip.c:1199` — `socket->state = STATE_NEW;`
- [ ] `extmod/modlwip.c:1253` — `socket->state = _ERR_BADF;`
- [ ] `extmod/modlwip.c:1460` — `socket->callback = args[3];`
- [ ] `extmod/modlwip.c:1674` — `socket->state = _ERR_BADF;`
- [ ] `extmod/modlwip.c:1863` — `tuple->items[0] = MP_OBJ_NEW_SMALL_INT(MOD_NETWORK_AF_INET);`
- [ ] `extmod/modlwip.c:1864` — `tuple->items[1] = MP_OBJ_NEW_SMALL_INT(MOD_NETWORK_SOCK_STREAM);`
- [ ] `extmod/modlwip.c:1865` — `tuple->items[2] = MP_OBJ_NEW_SMALL_INT(0);`
- [ ] `extmod/modlwip.c:1866` — `tuple->items[3] = MP_OBJ_NEW_QSTR(MP_QSTR_);`
- [ ] `extmod/modlwip.c:1867` — `tuple->items[4] = lwip_format_inet_addr(&ipcopy, port);`

### `extmod/modbluetooth.c` (22)

- [ ] `extmod/modbluetooth.c:119` — `self->type = uuid_bufinfo.len;`
- [ ] `extmod/modbluetooth.c:123` — `self->type = MP_BLUETOOTH_UUID_TYPE_128;`
- [ ] `extmod/modbluetooth.c:143` — `self->data[uuid_i / 2] = c << 4;`
- [ ] `extmod/modbluetooth.c:215` — `bufinfo->buf = self->data;`
- [ ] `extmod/modbluetooth.c:236` — `uuid->type = ringbuf_get(ringbuf);`
- [ ] `extmod/modbluetooth.c:239` — `uuid->data[i] = ringbuf_get(ringbuf);`
- [ ] `extmod/modbluetooth.c:442` — `o->irq_handler = handler_in;`
- [ ] `extmod/modbluetooth.c:626` — `service_handles->items[j] = MP_OBJ_NEW_SMALL_INT(handles[i][j]);`
- [ ] `extmod/modbluetooth.c:628` — `result->items[i] = MP_OBJ_FROM_PTR(service_handles);`
- [ ] `extmod/modbluetooth.c:1022` — `data_tuple->items[j++] = MP_OBJ_NEW_SMALL_INT(ringbuf_get16(ringbuf));`
- [ ] `extmod/modbluetooth.c:1025` — `data_tuple->items[j++] = MP_OBJ_NEW_SMALL_INT(ringbuf_get(ringbuf));`
- [ ] `extmod/modbluetooth.c:1032` — `data_tuple->items[j++] = MP_OBJ_FROM_PTR(bytes_addr);`
- [ ] `extmod/modbluetooth.c:1036` — `data_tuple->items[j++] = MP_OBJ_NEW_SMALL_INT((int8_t)ringbuf_get(ringbuf));`
- [ ] `extmod/modbluetooth.c:1041` — `data_tuple->items[j++] = MP_OBJ_FROM_PTR(uuid);`
- [ ] `extmod/modbluetooth.c:1052` — `data_tuple->items[j++] = MP_OBJ_FROM_PTR(bytes_data);`
- [ ] `extmod/modbluetooth.c:1158` — `data_tuple->items[data_tuple->len++] = MP_OBJ_NEW_SMALL_INT(numeric[i]);`
- [ ] `extmod/modbluetooth.c:1162` — `data_tuple->items[data_tuple->len++] = MP_OBJ_FROM_PTR(&mv_addr);`
- [ ] `extmod/modbluetooth.c:1165` — `data_tuple->items[data_tuple->len++] = MP_OBJ_NEW_SMALL_INT(numeric[i + n_unsigned]);`
- [ ] `extmod/modbluetooth.c:1169` — `data_tuple->items[data_tuple->len++] = MP_OBJ_FROM_PTR(uuid);`
- [ ] `extmod/modbluetooth.c:1203` — `data_tuple->items[data_tuple->len++] = MP_OBJ_FROM_PTR(&mv_data[i]);`
- [ ] `extmod/modbluetooth.c:1757` — `entry->data = data;`
- [ ] `extmod/modbluetooth.c:1778` — `entry->data = data;`

### `extmod/modsocket.c` (17)

- [ ] `extmod/modsocket.c:61` — `s->type = MOD_NETWORK_SOCK_STREAM;`
- [ ] `extmod/modsocket.c:68` — `s->type = mp_obj_get_int(args[1]);`
- [ ] `extmod/modsocket.c:76` — `s->state = MOD_NETWORK_SS_NEW;`
- [ ] `extmod/modsocket.c:87` — `self->nic = mod_network_find_nic(ip);`
- [ ] `extmod/modsocket.c:148` — `self->state = MOD_NETWORK_SS_LISTENING;`
- [ ] `extmod/modsocket.c:171` — `socket2->type = self->type;`
- [ ] `extmod/modsocket.c:177` — `socket2->state = MOD_NETWORK_SS_NEW;`
- [ ] `extmod/modsocket.c:191` — `socket2->nic = self->nic;`
- [ ] `extmod/modsocket.c:196` — `client->items[0] = MP_OBJ_FROM_PTR(socket2);`
- [ ] `extmod/modsocket.c:197` — `client->items[1] = netutils_format_inet_addr(ip, port, NETUTILS_BIG);`
- [ ] `extmod/modsocket.c:221` — `self->state = MOD_NETWORK_SS_CONNECTED;`
- [ ] `extmod/modsocket.c:504` — `self->state = MOD_NETWORK_SS_CLOSED;`
- [ ] `extmod/modsocket.c:607` — `tuple->items[0] = MP_OBJ_NEW_SMALL_INT(MOD_NETWORK_AF_INET);`
- [ ] `extmod/modsocket.c:608` — `tuple->items[1] = MP_OBJ_NEW_SMALL_INT(MOD_NETWORK_SOCK_STREAM);`
- [ ] `extmod/modsocket.c:609` — `tuple->items[2] = MP_OBJ_NEW_SMALL_INT(0);`
- [ ] `extmod/modsocket.c:610` — `tuple->items[3] = MP_OBJ_NEW_QSTR(MP_QSTR_);`
- [ ] `extmod/modsocket.c:611` — `tuple->items[4] = netutils_format_inet_addr(out_ip, port, NETUTILS_BIG);`

### `extmod/network_ppp_lwip.c` (11)

- [ ] `extmod/network_ppp_lwip.c:81` — `self->state = STATE_CONNECTED;`
- [ ] `extmod/network_ppp_lwip.c:87` — `self->state = STATE_ACTIVE;`
- [ ] `extmod/network_ppp_lwip.c:92` — `self->state = STATE_INACTIVE;`
- [ ] `extmod/network_ppp_lwip.c:97` — `self->state = STATE_ERROR;`
- [ ] `extmod/network_ppp_lwip.c:113` — `self->state = STATE_INACTIVE;`
- [ ] `extmod/network_ppp_lwip.c:114` — `self->stream = stream;`
- [ ] `extmod/network_ppp_lwip.c:133` — `self->state = STATE_INACTIVE;`
- [ ] `extmod/network_ppp_lwip.c:201` — `self->stream = kwargs->table[i].value;`
- [ ] `extmod/network_ppp_lwip.c:281` — `self->state = STATE_ACTIVE;`
- [ ] `extmod/network_ppp_lwip.c:313` — `self->state = STATE_CONNECTING;`
- [ ] `extmod/network_ppp_lwip.c:334` — `return mp_obj_new_bool(self->state == STATE_CONNECTED);`

### `extmod/machine_i2c_target.c` (9)

- [ ] `extmod/machine_i2c_target.c:123` — `data->state = STATE_IDLE;`
- [ ] `extmod/machine_i2c_target.c:148` — `data->state = STATE_IDLE;`
- [ ] `extmod/machine_i2c_target.c:155` — `data->state = STATE_ADDR_MATCH_READ;`
- [ ] `extmod/machine_i2c_target.c:158` — `data->state = STATE_ADDR_MATCH_WRITE;`
- [ ] `extmod/machine_i2c_target.c:166` — `data->state = STATE_READING;`
- [ ] `extmod/machine_i2c_target.c:180` — `data->state = STATE_READING;`
- [ ] `extmod/machine_i2c_target.c:194` — `data->state = STATE_WRITING;`
- [ ] `extmod/machine_i2c_target.c:207` — `data->state = STATE_MEM_ADDR_SELECT;`
- [ ] `extmod/machine_i2c_target.c:220` — `data->state = STATE_WRITING;`

### `extmod/modasyncio.c` (7)

- [ ] `extmod/modasyncio.c:120` — `task->ph_key = ticks();`
- [ ] `extmod/modasyncio.c:123` — `task->ph_key = args[2];`
- [ ] `extmod/modasyncio.c:236` — `self->data = mp_obj_dict_get(mp_asyncio_context, MP_OBJ_NEW_QSTR(MP_QSTR_CancelledError));`
- [ ] `extmod/modasyncio.c:264` — `self->data = dest[1];`
- [ ] `extmod/modasyncio.c:267` — `self->state = dest[1];`
- [ ] `extmod/modasyncio.c:278` — `self->state = TASK_STATE_DONE_WAS_WAITED_ON;`
- [ ] `extmod/modasyncio.c:281` — `self->state = task_queue_make_new(&task_queue_type, 0, 0, NULL);`

### `extmod/modbtree.c` (7)

- [ ] `extmod/modbtree.c:111` — `dbt->data = bufinfo.buf;`
- [ ] `extmod/modbtree.c:187` — `pair->items[0] = mp_obj_new_bytes(key.data, key.size);`
- [ ] `extmod/modbtree.c:188` — `pair->items[1] = mp_obj_new_bytes(val.data, val.size);`
- [ ] `extmod/modbtree.c:199` — `self->start_key = args[1];`
- [ ] `extmod/modbtree.c:201` — `self->end_key = args[2];`
- [ ] `extmod/modbtree.c:293` — `pair->items[0] = mp_obj_new_bytes(key.data, key.size);`
- [ ] `extmod/modbtree.c:294` — `pair->items[1] = mp_obj_new_bytes(val.data, val.size);`

### `extmod/modselect.c` (7)

- [ ] `extmod/modselect.c:255` — `poll_obj->ioctl = stream_p->ioctl;`
- [ ] `extmod/modselect.c:271` — `poll_obj->ioctl = stream_p->ioctl;`
- [ ] `extmod/modselect.c:276` — `elem->value = MP_OBJ_FROM_PTR(poll_obj);`
- [ ] `extmod/modselect.c:574` — `ret_list->items[n_ready++] = mp_obj_new_tuple(2, tuple);`
- [ ] `extmod/modselect.c:585` — `self->ret_tuple = mp_obj_new_tuple(2, NULL);`
- [ ] `extmod/modselect.c:613` — `t->items[0] = poll_obj->obj;`
- [ ] `extmod/modselect.c:614` — `t->items[1] = MP_OBJ_NEW_SMALL_INT(poll_obj_get_revents(poll_obj));`

### `extmod/vfs.c` (7)

- [ ] `extmod/vfs.c:276` — `vfs->next = *vfsp;`
- [ ] `extmod/vfs.c:423` — `t->items[1] = MP_OBJ_NEW_SMALL_INT(MP_S_IFDIR);`
- [ ] `extmod/vfs.c:424` — `t->items[2] = MP_OBJ_NEW_SMALL_INT(0); // no inode number`
- [ ] `extmod/vfs.c:511` — `t->items[0] = MP_OBJ_NEW_SMALL_INT(MP_S_IFDIR); // st_mode`
- [ ] `extmod/vfs.c:513` — `t->items[i] = MP_OBJ_NEW_SMALL_INT(0); // dev, nlink, uid, gid, size, atime, mtime, ctime`
- [ ] `extmod/vfs.c:538` — `t->items[i] = MP_OBJ_NEW_SMALL_INT(0);`
- [ ] `extmod/vfs.c:542` — `t->items[9] = MP_OBJ_NEW_SMALL_INT(MICROPY_ALLOC_PATH_MAX);`

### `extmod/modwebsocket.c` (6)

- [ ] `extmod/modwebsocket.c:134` — `self->state = FRAME_OPT;`
- [ ] `extmod/modwebsocket.c:137` — `self->state = CONTROL;`
- [ ] `extmod/modwebsocket.c:139` — `self->state = PAYLOAD;`
- [ ] `extmod/modwebsocket.c:158` — `self->state = CONTROL;`
- [ ] `extmod/modwebsocket.c:160` — `self->state = PAYLOAD;`
- [ ] `extmod/modwebsocket.c:189` — `self->state = FRAME_HEADER;`

### `extmod/modheapq.c` (5)

- [ ] `extmod/modheapq.c:47` — `heap->items[pos] = parent;`
- [ ] `extmod/modheapq.c:53` — `heap->items[pos] = item;`
- [ ] `extmod/modheapq.c:66` — `heap->items[pos] = heap->items[child_pos];`
- [ ] `extmod/modheapq.c:69` — `heap->items[pos] = item;`
- [ ] `extmod/modheapq.c:88` — `heap->items[0] = heap->items[heap->len];`

### `extmod/modtls_axtls.c` (4)

- [ ] `extmod/modtls_axtls.c:131` — `o_str->data = (const byte *)errstr;`
- [ ] `extmod/modtls_axtls.c:180` — `self->key = key_obj;`
- [ ] `extmod/modtls_axtls.c:181` — `self->cert = cert_obj;`
- [ ] `extmod/modtls_axtls.c:296` — `o->sock = sock;`

### `extmod/modopenamp_remoteproc_store.c` (3)

- [ ] `extmod/modopenamp_remoteproc_store.c:66` — `fstore->buf = metal_allocate_memory(RPROC_FILE_STORE_BUF_SIZE);`
- [ ] `extmod/modopenamp_remoteproc_store.c:78` — `fstore->file = mp_vfs_open(MP_ARRAY_SIZE(args), args, (mp_map_t *)&mp_const_empty_map);`
- [ ] `extmod/modopenamp_remoteproc_store.c:114` — `fstore->buf = metal_allocate_memory(size);`

### `extmod/modtls_mbedtls.c` (3)

- [ ] `extmod/modtls_mbedtls.c:195` — `o_str->data = o_str_buf;`
- [ ] `extmod/modtls_mbedtls.c:372` — `self->ecdsa_sign_callback = dest[1];`
- [ ] `extmod/modtls_mbedtls.c:376` — `self->handler = dest[1];`

### `extmod/network_wiznet5k.c` (3)

- [ ] `extmod/network_wiznet5k.c:448` — `socket->type = Sn_MR_TCP;`
- [ ] `extmod/network_wiznet5k.c:451` — `socket->type = Sn_MR_UDP;`
- [ ] `extmod/network_wiznet5k.c:523` — `socket2->type = socket->type;`

### `extmod/modopenamp.c` (2)

- [ ] `extmod/modopenamp.c:235` — `self->name = args[ARG_name].u_obj;`
- [ ] `extmod/modopenamp.c:236` — `self->callback = args[ARG_callback].u_obj;`

### `extmod/modre.c` (2)

- [ ] `extmod/modre.c:97` — `groups->items[i - 1] = match_group(self_in, MP_OBJ_NEW_SMALL_INT(i));`
- [ ] `extmod/modre.c:333` — `match->str = where;`

### `extmod/modwebrepl.c` (2)

- [ ] `extmod/modwebrepl.c:157` — `self->cur_file = mp_builtin_open(2, open_args, (mp_map_t *)&mp_const_empty_map);`
- [ ] `extmod/modwebrepl.c:208` — `self->state = STATE_NORMAL;`

### `extmod/network_cyw43.c` (2)

- [ ] `extmod/network_cyw43.c:168` — `t->items[3] = MP_OBJ_NEW_SMALL_INT(res->rssi);`
- [ ] `extmod/network_cyw43.c:170` — `t->items[5] = MP_OBJ_NEW_SMALL_INT(MP_OBJ_SMALL_INT_VALUE(t->items[5]) + 1);`

### `extmod/moductypes.c` (1)

- [ ] `extmod/moductypes.c:636` — `bufinfo->buf = self->addr;`

### `extmod/network_ninaw10.c` (1)

- [ ] `extmod/network_ninaw10.c:915` — `socket->callback = (void *)optval;`

### `extmod/vfs_reader.c` (1)

- [ ] `extmod/vfs_reader.c:117` — `reader->data = rf;`

### `extmod/vfs_rom_file.c` (1)

- [ ] `extmod/vfs_rom_file.c:86` — `bufinfo->buf = (void *)self->file_data;`

## Bulk element moves (25)

`mp_seq_*` / `memcpy`/`memmove` over pointer arrays. These move references
without any `->field =` syntax, so no field-based grep finds them. Covering
the `mp_seq_*` macros in `py/obj.h` handles most of these at the source.

- [ ] `py/objarray.c:154` — `memcpy(o->items, bufinfo.buf, len * sz);`
- [ ] `py/objarray.c:326` — `mp_seq_cat((byte *)res->items, lhs_bufinfo.buf, lhs_bufinfo.len, rhs_bufinfo.buf, rhs_len * sz, byte);`
- [ ] `py/objarray.c:455` — `mp_seq_copy((byte *)self->items + self->len * sz, arg_bufinfo.buf, len * sz, byte);`
- [ ] `py/objarray.c:537` — `mp_seq_replace_slice_grow_inplace(dest_items, o->len,`
- [ ] `py/objarray.c:540` — `mp_seq_replace_slice_no_grow(dest_items, o->len,`
- [ ] `py/objarray.c:571` — `memcpy(res->items, (uint8_t *)o->items + slice.start * sz, (slice.stop - slice.start) * sz);`
- [ ] `py/objarray.c:688` — `memcpy(o->items, items, n);`
- [ ] `py/objdict.c:272` — `memcpy(other->map.table, self->map.table, self->map.alloc * sizeof(mp_map_elem_t));`
- [ ] `py/objexcept.c:246` — `memcpy(o_tuple->items, args, n_args * sizeof(mp_obj_t));`
- [ ] `py/objfun.c:435` — `memcpy(o->extra_args, def_pos_args->items, n_def_args * sizeof(mp_obj_t));`
- [ ] `py/objfun.c:515` — `memcpy(o->extra_args, def_pos_args->items, def_pos_args->len * sizeof(mp_obj_t));`
- [ ] `py/objlist.c:120` — `mp_seq_cat(s->items, o->items, o->len, p->items, p->len, mp_obj_t);`
- [ ] `py/objlist.c:173` — `mp_seq_copy(res->items, self->items + slice.start, res->len, mp_obj_t);`
- [ ] `py/objlist.c:195` — `mp_seq_replace_slice_grow_inplace(self->items, self->len,`
- [ ] `py/objlist.c:198` — `mp_seq_replace_slice_no_grow(self->items, self->len,`
- [ ] `py/objlist.c:254` — `memcpy(self->items + self->len, arg->items, sizeof(mp_obj_t) * arg->len);`
- [ ] `py/objlist.c:271` — `memmove(self->items + index, self->items + index + 1, (self->len - index) * sizeof(mp_obj_t));`
- [ ] `py/objnamedtuple.c:117` — `memcpy(&tuple->items[0], args, sizeof(mp_obj_t) * n_args);`
- [ ] `py/objset.c:180` — `memcpy(other->set.table, self->set.table, self->set.alloc * sizeof(mp_obj_t));`
- [ ] `py/objstr.c:752` — `memmove(res->items, &res->items[idx], used * sizeof(mp_obj_t));`
- [ ] `py/objtuple.c:153` — `mp_seq_cat(s->items, o->items, o->len, p->items, p->len, mp_obj_t);`
- [ ] `py/objtuple.c:192` — `mp_seq_copy(res->items, self->items + slice.start, res->len, mp_obj_t);`
- [ ] `py/runtime.c:798` — `mp_seq_copy(args2 + args2_len, args, n_args, mp_obj_t);`
- [ ] `py/runtime.c:826` — `mp_seq_copy(args2 + args2_len, items, len, mp_obj_t);`
- [ ] `py/sequence.c:41` — `memcpy(dest, items, copy_sz);`
