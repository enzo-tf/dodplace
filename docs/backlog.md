# dodplace — backlog

Work that is known, sized, and deliberately not done. Everything here is either
cosmetic or off the critical path; nothing here blocks a certification.

## v0.2.1 — split the long files

The project's rule is a hundred and fifty useful lines per C file. It holds for
most of the engine and for every file added in the last three cycles except
these, and a split changes no behaviour - it is pure structure, so it wants a
cycle where nothing else moves.

| File | Lines | What comes out |
|---|---:|---|
| `src/context.c` | 1279 | pass 1 allocation / pass 2 fill / pass 3 finalize / validation / sizing estimates |
| `src/main.c` | 613 | option parsing, the report printer, the bench printer |
| `src/solver_core.c` | 426 | the pipeline, the multi-start block, the debug dumps |
| `src/solver_state.c` | 401 | the allocation pass, the anchor derivation, the copper box |
| `include/place/context.h` | 369 | the struct, the ingress API, the validation report |
| `src/lns_repair.c` | 333 | the board test, the spiral search, ruin & recreate |
| `src/solver_internal.h` | 321 | the state struct, the stage helpers |
| `src/scene_load.c` | 318 | the header, the section table, the payloads |
| `src/solver_refine.c` | 279 | the move loop, the torque heuristic, the rotation sweep |
| `src/io_sections.c` | 279 | the expect table, the save table |
| `src/swap_window.c` | 251 | the geometry and the sweep |
| `src/swap_assign.c` | 217 | the cost matrix, the Hungarian solve, the apply |
| `src/solver_restarts.c` | 220 | the worker, the batch scheduler |
| `src/cost_metrics.c` | 235 | one file per cost term |

Order: the files added by the recent cycles first (`swap_window.c`,
`solver_restarts.c`, `swap_assign.c`), then the pre-existing giants, one commit
each so a bisect can still find a regression.

## Silk: the engine-side port

The nudge runs at the board-writing step (`python/src/dodplace/kicad/silk.py`)
and costs 15.88 s on r10 against 0.43 s without it, because it is Python looping
over 19 308 capsules and 4 143 pads. The engine version is specified already:
kinds 45 (capsules) and 46 (the component-to-silk CSR) in `ir/spec.md`,
`silk_check.c` for the same two distance tests, and the nudge at the end of
`solver_legal`. It is the way to bring that fifteen seconds under one, and it
moves the decision back where every other placement decision lives.

## Silk: the five warnings that remain

`silk_overlap` is 6 before the pass and 5 after: the guard refuses any nudge
that would break copper, courtyard or hole clearance, and the moves that would
clear the rest of them are outside the 0.20 mm window. A wider search - the
window permutation machinery already in the engine, applied to paired silk -
is the next thing to try, not a bigger step size.

## Open options nobody has calibrated

* `--assign-model wa` (the weighted-average cost matrix) is implemented and
  measured **worse** than the star model on r10 - with WA the identity is the
  optimal assignment for most buckets. It stays available; a board where it
  wins would be evidence worth having.
* `--chain N` (chained annealing) is implemented and measured worse than more
  walks in the first round. The machinery is for a board whose first round is
  expensive.
* The 480 LEDs are locked in the source board and stay locked. Any measurement
  that treats them as movable is measuring a different problem.
