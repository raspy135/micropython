# GC Performance Proposal — esp32 port (pocketdeck firmware)

## Diagnosis

Current configuration (as of commit `6a432ec9e`):

- Split-heap auto-grow, initial 3.2 MB, new areas up to 3 MB (`MICROPY_GC_MAX_NEW_AREA_SIZE`),
  all via `malloc()` → octal 120 MHz PSRAM.
- `gc_init()` sets `gc_alloc_threshold = (size_t)-1` (py/gc.c:236) → the allocation-count
  GC trigger is disabled. Collection happens only on allocation failure or explicit
  `gc.collect()`.
- Marking is a full stop-the-world pointer chase over the whole live set.
  In PSRAM every cache-missing hop costs ~1-2 µs → multi-MB live sets produce
  100+ ms STW pauses, with the GIL held. All Python threads stall ("system freeze").
- `gc_deal_with_stack_overflow()` re-walks the ENTIRE allocation table every time the
  256-entry mark stack overflows. On multi-MB heaps it overflows routinely → a single
  collect may traverse the heap tables repeatedly.
- Freeze spiral: once the heap is full and a collect frees 0 bytes, *every subsequent
  allocation* runs another full mark.

Existing customizations:
- `GC_ALLOC_FLAG_IS_DATA` (ATB nibble bit 3): data blocks not scanned for pointers.
  Sound when set explicitly by the allocator for pure-byte buffers.
- Experimental auto-tag (bit 8 "candidate", `n_blocks > 3000` heuristic): UNSOUND —
  a block auto-tagged "data" may later be written with real pointers; the next
  collect would then free live objects. Do not ship this path; Step 3 replaces it
  with a sound barrier-based scheme.

## Step 1 — Adaptive threshold + no-progress backoff (half a day, low risk)

1. In `gc_collect_end()` / sweep, set:
   ```c
   // collect again only after allocating ~ as much again as since last collect
   MP_STATE_MEM(gc_alloc_threshold) = 2 * MP_STATE_MEM(gc_alloc_amount);
   MP_STATE_MEM(gc_alloc_amount) = 0;
   ```
   (instead of leaving threshold at -1 / resetting amount only at start).
2. Track `gc_last_freed` bytes per collect. If a collect freed 0 bytes and no
   block has been freed since, skip collect-on-alloc-failure (go straight to
   area growth / MemoryError). One full GC per pressure event, not one per alloc.
3. Raise `MICROPY_ALLOC_GC_STACK_SIZE` (256 → 2048) or back `gc_block_stack`
   with a reserved buffer, to eliminate whole-table re-walks.

Expected: removes the freeze spiral entirely; collects happen predictably while
there is headroom (sooner = smaller live set = faster mark).

## Step 2 — GC metadata + first area in internal RAM (1-2 days, low risk)

- ATB/FTB tables are touched by every alloc/free and by every mark step.
  For a 16 MB heap they are only ~500 KB. Allocate them with
  `heap_caps_malloc(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)` and store pointers
  in `mp_state_mem_area_t` (layout change in `gc_add`/`gc_init`: pool range in
  PSRAM, tables in internal RAM).
- Put the *first* heap area (boot-time framework/UI objects = hot set) fully in
  internal RAM: custom `MP_PLAT_ALLOC_HEAP` that uses internal RAM while
  `total_heap < N` (e.g. 384 KB), PSRAM beyond.

Expected: alloc/free fast path and mark traversal several times faster
(internal RAM random access ≈ 10-20x cheaper than PSRAM).

## Step 3 — Incremental tri-color mark-sweep (1-2 weeks; the dramatic fix)

Turn collection into a resumable state machine driven from `gc_alloc()`:

```
IDLE → SCAN_ROOTS → MARK (repeat: mark_chunk(N blocks)) → RESCAN_STACKS → SWEEP → IDLE
```

- `mark_chunk(N)`: run `gc_mark_subtree` work for a bounded budget (µs-based via
  `esp_timer_get_time()`, e.g. 200 µs) then return. Every allocation advances the
  machine → GC work smeared over time; max added latency per alloc is bounded and
  small; total throughput similar (same work, overlapped with execution).
- Tri-color invariant maintained with a **Dijkstra incremental-update write
  barrier** (no yellow objects; mark-on-store):

  ```c
  // called on every store of object `child` into tracked container `parent`
  #define GC_BARRIER(parent, child) do {                     \
      if (MP_STATE_THREAD(gc_lock_depth) & GC_ACTIVE_FLAG) { \
          gc_barrier_mark(parent, child);                    \
      }                                                      \
  } while (0)
  ```

  When idle it is a single flag test (same cost class as existing
  `gc_is_collecting()` checks).
- Store sites to instrument (complete list for MicroPython):
  - `py/vm.c` / emitter: `STORE_ATTR`, `STORE_SUBSCR`, `STORE_GLOBAL`
  - `py/objlist.c` (`mp_obj_list_store`, `extend`, `append` via store)
  - `py/objdict.c`, `py/objset.c` (entry inserts — or mark the whole table
    conservatively since it's not GC-managed memory; see upstream precedents)
  - `py/obj.c: mp_obj_set_zip` / `py/objarray.c` store (only if IS_DATA blocks
    may hold pointers — with the data-flag contract they don't; skip)
  - `py/objclosure.c` free-variable write, `py/objtype.c` instance attr store
- Roots: C stacks / pystack cannot take barriers → final `RESCAN_STACKS` phase
  rescans thread stacks (8 KB each — cheap). The heap — the expensive part —
  is never re-scanned.
- Sweep can stay as-is (single linear table walk) or later be chunked too.
- Bonus: makes large raw buffers sound without the unsound auto-tag: a data-flag
  block is treated as "everything inside is unknown-white"; the barrier at any
  site that could store a pointer into it keeps it correct.

## Step 4 (later) — Generational nursery using existing split-heap

- 64-128 KB nursery area in internal RAM; everything allocates there first.
- Minor collect: scan roots + nursery only (small, fast RAM) ≈ 1-3 ms; survivors
  promoted to PSRAM old-gen areas (already `mp_state_mem_area_t`-separate).
- Old→young stores recorded via one extra flag bit per old head block (nibble
  bit available), or a small remembered-set.
- Full major GC becomes rare; combined with Step 3 it never pauses visibly.

## Also worth doing regardless

- Keep feeding WDT inside `MICROPY_GC_HOOK_LOOP` (currently empty) as a safety net.
- Remove/rearm the auto-`HEAD_DATA_CANDIDATE` experiment (unsound as-is).
- Consider `gc.collect()` placement in `gpt_rt.py` etc: with Steps 1+3 the explicit
  collects become unnecessary and their big pauses disappear.

## Suggested order

1. **Step 1 today** — removes the freeze spiral; measurable immediately with
   `benchmark.py`.
2. **Step 2 this week** — pure constant-factor win, no algorithm risk.
3. **Step 3** — eliminates long pauses entirely; the "dramatic" answer.
4. Step 4 only if GC frequency still shows up in profiles after Step 3.
