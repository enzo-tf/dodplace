# dodplace — Python front end

Turns a KiCad board into the engine's IR (`scene.bin`, see `../ir/spec.md`).
The C engine never sees a KiCad file, a PDF or an HTTP response.

```
board.kicad_pcb ──► extract (JSON) ──► enrich (phase 3) ──► scene.bin ──► dodplace-solve
```

## See also

[`../docs/data-map.md`](../docs/data-map.md) - every artifact the project
writes, which file each field lands in, and the diagrams.

## Running

```sh
uv sync --extra dev --extra pdf --extra ipc   # 88 MB; skip `step` unless you want OCCT
uv run dodplace info                          # which KiCad python and solver are visible

uv run dodplace ingest BOARD.kicad_pcb -o scene.bin --report report.json
uv run dodplace show scene.bin
uv run dodplace solve scene.bin -o placement.bin
uv run dodplace enrich skeleton BOARD.kicad_pcb    # parts CSV to fill in
uv run dodplace enrich extra BOARD.kicad_pcb       # commented rules starter

uv run pytest                                 # 160 tests, most of them without KiCad
```

`dodplace info` reports the interpreter and solver the CLI would use.

The solver itself lives in C (`../src/solver*.c`) and is documented stage by
stage in `../docs/data-map.md` §4b: semantic clustering, force-directed global
placement, simulated annealing, then MTV legalisation. Its own tests are
`../tests/test_solver.c` (run by `ctest`).

> `uv sync --extra X` **replaces** the extra set, so always list the extras you
> want together. `--all-extras` also installs OCCT + VTK (~850 MB), which the
> STEP provider does **not** need.

## KiCad plugin

Two actions in the PCB editor, both **read-only** — they never modify the board
file:

| Action | What it does |
|---|---|
| `dodplace: analyze board` | full pipeline, writes the scene and the reports into `<board>/.dodplace/`, validates with the C engine |
| `dodplace: report only` | the same analysis without writing anything but the log |

Both run the same shim; the second passes `--report-only`, which the shim
forwards from the manifest's `args`.

### Test it from a terminal first

The action and the command line run exactly the same code, so you can iterate
without restarting KiCad:

```sh
uv run dodplace plugin analyze --board path/to/board.kicad_pcb
uv run dodplace plugin analyze --ipc            # the board open in KiCad
uv run dodplace plugin analyze --ipc --save     # save it through KiCad first
uv run dodplace plugin analyze --ipc --report-only   # write nothing but the log
```

Output looks like this:

```
board: /…/board.kicad_pcb
  found via: explicit
  4 components, 14 pins, 6 nets (12 connections)
  enrichment: 0 part(s), 0 datasheet(s), 0 cached, 4 STEP model(s)
  coverage: height 4/4, mass 0/4, orientation 0/4, pin roles 0/14, pin current 0/14
  constraints: 2 decoupling
  degraded (0x0002007E): NO_MASS NO_ROTATION_DATA NO_PIN_ELEC NO_PIN_CURRENT …
  engine validation: ok (exit 0)
  unresolved: mass_g 4/4, orientation_deg 4/4, power_w 4/4, r_theta_ja 4/4
  artifacts: /…/.dodplace
```

The `unresolved` line is the actionable part: it names the optional fields no
source could supply, with how many components or pins they affect, so you know
exactly what to add to `extra.toml`, a parts file or a datasheets directory.

Exit codes: `0` fine, `1` pipeline or validation failure, `2` usage, `3` no
board found (KiCad closed and no `--board`).

### Install it into KiCad

```sh
uv run dodplace plugin install     # writes ~/Documents/KiCad/<version>/plugins/dodplace/
uv run dodplace plugin status      # install state, repo check, IPC reachability
uv run dodplace plugin uninstall   # removes only that directory
```

Restart KiCad afterwards; the action appears in the PCB editor toolbar (and in
the plugin list of the preferences dialog). The first launch makes KiCad create
a virtualenv for the plugin, which takes a moment.

`install` writes four things: `plugin.json`, a generated `main.py` shim, a
**`requirements.txt`**, and a one-line pointer file at `~/.config/dodplace/repo`. The shim is stdlib-only and
Python 3.9 compatible because KiCad runs it with **its own** interpreter; it
relays the call to this checkout's interpreter, using absolute paths baked in at
install time (a process started by KiCad from the Finder has no shell `PATH`).
The pointer file is the escape hatch if you move the checkout: edit it or
re-run `install`.

### Three traps found the hard way

The first two produce the same symptom — a plugin that loads cleanly in the
trace and no button anywhere — and neither prints an error. The third makes the
action fail the moment it runs.

**1. A missing `requirements.txt`.** KiCad's plugin manager runs
`pip install -r requirements.txt` in the plugin directory at startup and marks
the plugin **ready only when that succeeds**
(`common/api/api_plugin_manager.cpp`). Without the file the job fails and the
action is never registered; because the virtual environment already exists,
every later start repeats the same failing job.

dodplace ships a comment-only `requirements.txt`: the shim is stdlib-only and
the real dependencies live in the project's uv environment, so nothing needs to
be installed and no network access is required.

**2. A button with no bitmap.** `show-button: true` only creates a toolbar
entry; the bitmap comes exclusively from `icons-light` / `icons-dark`
(`common/api/api_plugin.cpp`), and with neither declared the button is blank.
The icons are generated by `tools/make_plugin_icons.py` (pure standard library)
and shipped as package data.

The installer verifies both before writing anything: it refuses a plugin whose
requirements would install packages, a button with no icon, and an icon declared
in the manifest but not shipped.

**3. KiCad's environment poisons a nested interpreter.** Before running an
action, KiCad exports `PYTHONHOME` and `PYTHONPATH` pointing at *its* bundled
Python 3.9. Our shim starts the project's Python 3.12, which inherits them,
looks for its standard library inside KiCad's 3.9 tree and dies with
`Fatal Python error: init_fs_encoding … No module named 'encodings'`.

The shim therefore runs the child with `-E` (ignore `PYTHON*` variables) *and*
strips `PYTHONHOME`, `PYTHONPATH`, `PYTHONSTARTUP`, `PYTHONEXECUTABLE`,
`PYTHONNOUSERSITE` and `VIRTUAL_ENV` from its environment. Guarded by a test
that reproduces KiCad's exact variables.

### The requirements.txt trap

KiCad's plugin manager runs `pip install -r requirements.txt` in the plugin
directory at startup and marks the plugin **ready only when that succeeds**
(`common/api/api_plugin_manager.cpp`). A plugin without that file is never
registered: no error dialog, no action, just silence — and because the virtual
environment already exists, every later start repeats the same failing job.

### Seeing the output

KiCad shows the action's stdout/stderr in its message panel (10.0.1+). For a
terminal, the same summary is appended to a log next to the board:

```sh
uv run dodplace plugin log --board path/to/board.kicad_pcb
uv run dodplace plugin log -f                 # follows the nearest log
```

### The action does not appear

1. `uv run dodplace plugin status` — it reports whether the plugin is installed,
   whether the three files KiCad needs are all present, whether the baked repo
   path still resolves, whether the venv answers, and whether the IPC socket is
   reachable. Shim-level failures also land in `~/.cache/dodplace/shim.log`.
2. Ask KiCad why. Quit it completely first — launching the binary while an
   instance runs only activates the existing window, so the trace never reaches
   the real process:

   ```sh
   pgrep -fl kicad || echo "fully quit"
   KICAD_ENABLE_WXTRACE=1 WXTRACE=KICAD_API /Applications/KiCad/KiCad.app/Contents/MacOS/kicad
   ```

   The lines that matter are `Manager: installing dependencies for …` and
   `Manager: marking <identifier> as ready`. The first without the second means
   the dependency job failed — almost always a missing or unusable
   `requirements.txt`.
3. **The action runs but exits 1** with `No module named 'encodings'` in the
   trace: the nested-interpreter environment problem above. Re-run
   `dodplace plugin install`; the shim is regenerated with the fix.
4. **"no board from the IPC API"** in a terminal run: `KiCad.get_board()` uses
   the `GetOpenDocuments` command, which only has a handler while the **PCB
   editor** is open with a board loaded. `plugin status` says which case you are
   in — *not reachable*, *no open board*, or a board path. The action itself runs
   from the PCB editor, so it never hits this.
5. `iCCP: known incorrect sRGB profile` and similar lines are libpng noise from
   KiCad's own icons. They mean nothing about the plugin.
6. macOS may ask to let KiCad control **System Events** right after an action
   runs. That is KiCad bringing its own window back to the front
   (`osascript -e 'tell application "System Events" … set the frontmost …'`),
   not the plugin; denying it is harmless.

### Why the action does not place anything yet

Applying a placement needs two things that are not finished: the bottom-side
orientation convention has to be verified with a real round trip through the IPC
API (see the note below), and the write-back must happen as a single undoable
commit. Until then the action deliberately cannot write to the board.

## Extraction backend

Extraction uses **KiCad's own `pcbnew` module, headless**, via the bundled
interpreter (3.9 on macOS, first `python3` with `pcbnew` elsewhere; override
with `DODPLACE_KICAD_PYTHON`). The extractor (`kicad/pcbnew_extract.py`) is
stdlib-only, so it never needs this project's dependencies, and only JSON
crosses the interpreter boundary.

**Why not the IPC API for reading?** Measured against KiCad 10.0.5:

| Need | IPC (kipy) | pcbnew |
|---|---|---|
| Chained board outline | only `BoardSegment`s on `Edge.Cuts`, arcs included | `GetBoardPolygonOutlines()` |
| Courtyard polygon | only `BoardSegment`s in the definition | `GetCourtyard()` |
| Pad size | buried in `padstack` per layer | `pad.GetSize()` |
| Layer by name | no server-side handler in 10.0.5 | not needed |
| Requires a running GUI | yes (KiCad 9/10) | no |

Reimplementing segment chaining over IPC is exactly the format drift the IPC
API exists to avoid, and it would gain nothing. IPC is used where only it can
help: in phase 4 the plugin saves the open board and **applies** the resulting
placement live, as one undoable commit.

## Extract document schema

The contract between board readers and `to_scene`:

```jsonc
{
  "schema": "dodplace.extract/1",
  "backend": "pcbnew", "backend_version": "10.0.5",
  "board":  { "path": "...", "copper_layers": 4, "outline": [[[x, y], ...]] },
  "keepouts": [ { "name": "...", "layer_mask": 3, "xy": [[[x, y], ...]] } ],
  "netclasses": [ { "name": "Default", "clearance_mm": 0.2, "track_width_mm": 0.25 } ],
  "nets": [ { "name": "GND", "netclass": "Default" } ],
  "components": [ {
      "ref": "U1", "value": "MCU", "fpid": "Pkg:QFN",
      "x": 10.0, "y": 10.0, "rot_deg": 90.0, "side": "top", "locked": false,
      "courtyard": [ [[x, y], ...] ],          // footprint-local mm, may be empty
      "pads": [ { "number": "1", "net": "GND", "x": -1.0, "y": -1.0,
                  "size_x": 0.5, "size_y": 0.5, "attrib": "smd", "drill": 0.0 } ]
  } ],
  "warnings": []
}
```

`x`/`y` for pads are **footprint-local and unrotated**, which is exactly what
`PIN_OFFSET_X`/`PIN_OFFSET_Y` want. The extractor verifies its own rotation
frame on every pad (`board = origin + R(theta) * local`) and reports a warning
rather than emitting wrong data.

## Responsibilities

The producer resolves what it can and marks what it could not, so the engine's
fallbacks are never implemented twice. See `../ir/spec.md` §6 and §8 for the
split, and `to_scene.py` for the exact list this front end raises.

## Enrichment

Optional data is resolved by a precedence chain, strongest first:

```
extra.toml  >  parts file  >  board  >  netlist heuristic  >  datasheet PDF  >  STEP model
```

Sources are *additive*: each fills what the previous ones left empty, and the
winner of every field is recorded. Whatever stays unresolved is left absent on
purpose, so the C engine applies its documented fallback and raises the matching
`DEGRADED_*` bit - no fallback is implemented twice.

| Source | Command | What it provides |
|---|---|---|
| `extra.toml` | `enrich extra BOARD` writes a starter; auto-detected next to the board | locked parts, fixed orientations, symmetry, thermal envelopes, decoupling targets, keepouts, ceiling, airflow, per-net clearance |
| parts file | `enrich skeleton BOARD` writes one to fill in; `enrich parts FILE` imports it; auto-imported next to the board | LCSC/MPN, height, mass, power, datasheet link, per-pad current/role |
| netlist heuristic | on by default, `--no-heuristics` to skip | pairs every capacitor with the ICs sharing its power net |
| datasheet PDF | `ingest --datasheets DIR --rules RULES` | height, power, Rθ, currents, extracted by rules with a confidence and a page number |
| STEP model | on by default, `--no-step` to skip | part height from the board's own 3D model references, no OCCT needed |
| name convention | on by default | height from `-H2.8` in a model or footprint name, **low confidence**, only when nothing measurable exists |

### Two starters instead of a blank page

```sh
uv run dodplace enrich extra    BOARD.kicad_pcb   # -> extra.toml next to the board
uv run dodplace enrich skeleton BOARD.kicad_pcb   # -> dodplace-parts.csv, one row per supplier code
```

`enrich extra` reads the board and seeds the rules that are obvious from it:
mechanical parts to lock (`H*`, `FID*`, `J*`), dissipating candidates (`U`, `Q`,
`L`, `CE`) with their part values in a comment, and a commented example of
everything else. Nothing is imposed: the file is valid and safe as generated.

`enrich skeleton` writes one CSV row per supplier part number found on the
board, with the identity columns filled and `Height (mm)`, `Mass (g)`,
`Power (W)` and `Datasheet` empty. One real board needed **14 rows for 536
components**. Both files are auto-discovered on the next run: the parts file is
imported into the store, the rules file is applied.

`[[component]]` selectors accept globs (`ref = "FID*"`, `fpid = "Tuile_LED:*"`),
and later blocks win field by field, so a family rule can be refined by a
specific one.

`extra.example.toml` at the repo root documents every user-rule knob, and
`src/dodplace/enrich/rules/*.toml` holds the shipped datasheet rules.

### Cache and report

The cache is a directory of JSON records, one file per key, sharded two levels
deep and written atomically. No SQL and nothing opaque - you can read, grep and
delete any record, and a corrupt one is treated as a miss:

```
<board>/.dodplace/cache/parts/<ab>/<sha256>.json       # lcsc:C25804, mpn:..., fp:...
<board>/.dodplace/cache/datasheets/<ab>/<sha256>.json   # keyed by content hash
```

The store never holds a PDF or a STEP file, only the facts extracted from them
(with source, confidence and timestamp). Point it elsewhere with
`--store DIR`, share it between projects, or skip it with `--no-store`.
`dodplace enrich show --store DIR` lists what is cached.

Two reports come out of a run:

* `--report FILE` - counts, coverage, constraints and the degraded categories.
* `--enrich-report FILE` - per component and per field: the value used, the
  source that won, its confidence, or `{"unresolved": true}`. This is the file
  to read when a placement looks odd, and the place to look for what to add to
  `extra.toml`.

### What enrichment cannot fix

Some bits are *irreducible* by design and always reported: `NO_NET_WEIGHTS`
(nothing carries net priority), `NO_MAX_LENGTH` and `NO_SEGREGATION` (no data
model yet in the engine), and `COURTYARD_BBOX` for parts without an exact
courtyard on the board.

## Tests

| File | Covers |
|---|---|
| `test_writer.py` | IR encode/decode, **byte-identical** round trip of the C golden fixture, builder validation |
| `test_naming.py` | net-name electrical inference, differential pair matching |
| `test_to_scene.py` | extract → scene: counts, flags, matrix, degraded bits (no KiCad) |
| `test_extract.py` | the frozen extract of the fixture board; live pcbnew comparison when available |
| `test_enrich.py` | store, every provider, precedence, coverage bits, PDF end to end |
| `test_pipeline.py` | extract → scene.bin → the C solver → placement.bin → numpy |
| `test_plugin.py` | manifest schema, installer in a temp dir, shim relay and 3.9 syntax, the analyze action |

`tests/fixtures/scene_fixture.kicad_pcb` is generated by
`tests/fixtures/make_fixture_board.py` (run under KiCad's interpreter) and
`scene_fixture.extract.json` is its frozen extract, so most tests run without
KiCad installed. The PDF test builds a valid PDF in memory; only the
`pcbnew`-backed tests and the official-model test skip when KiCad is absent.

## Known issue to settle in phase 4

The local frame is `board = origin + R(theta) * local`, verified pad by pad
against KiCad for both sides (`test_extract.py` relies on that check having
produced no warnings). Bottom-side parts come back from KiCad with a mirrored
orientation (`C1` in the fixture is at 180 degrees after a flip), so **when the
plugin writes orientations back through the IPC API, the bottom-side
orientation convention must be verified with a real round trip** before the
placement is applied. Until that is done, the plugin should treat the two sides
separately and report any mismatch rather than saving.

