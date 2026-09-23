"""dodplace command line: KiCad board in, scene.bin out, solver call."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

from . import __version__
from .enrich import EnrichmentStore, resolve
from .enrich.providers import datasheet_pdf, extra_toml, parts_file, step_model
from .ir import degraded_names, read_scene, write_scene
from .kicad import (
    BACKENDS,
    ExtractError,
    HeadlessError,
    HeadlessUnavailable,
    build_scene,
    extract_board,
    find_kicad_python,
    kicad_python_version,
)
from .ir.spec import SIDE_BOTTOM, SIDE_BOTH, SIDE_TOP
from .solver import find_solver

SIDE_CHOICES = {"both": SIDE_BOTH, "top": SIDE_TOP, "bottom": SIDE_BOTTOM}

#: Default rules file next to a board, tried when --extra is not given.
EXTRA_CANDIDATES = ("dodplace.toml", "extra.toml")


def default_store_root(board_path: str | Path | None) -> Path:
    """Project-local cache: ``<board dir>/.dodplace/cache``."""
    if board_path:
        return Path(board_path).resolve().parent / ".dodplace" / "cache"
    return Path.cwd() / ".dodplace" / "cache"


def discover_extra(board_path: str | Path | None) -> Path | None:
    if not board_path:
        return None
    directory = Path(board_path).resolve().parent
    for name in EXTRA_CANDIDATES:
        candidate = directory / name
        if candidate.is_file():
            return candidate
    return None


def _store(args: argparse.Namespace, board_path=None) -> EnrichmentStore:
    root = Path(args.store) if getattr(args, "store", None) else default_store_root(board_path)
    return EnrichmentStore(root)


def _fail(message: str, code: int = 1) -> int:
    print(f"error: {message}", file=sys.stderr)
    return code


def cmd_extract(args: argparse.Namespace) -> int:
    try:
        doc = extract_board(args.board, backend=args.backend)
    except (HeadlessUnavailable, HeadlessError, ExtractError) as exc:
        return _fail(str(exc))
    Path(args.output).write_text(json.dumps(doc, indent=1, sort_keys=True))
    print(
        f"extracted {len(doc['components'])} components, {len(doc['nets'])} nets "
        f"({doc['backend']} {doc['backend_version']}) -> {args.output}"
    )
    return 0


def cmd_ingest(args: argparse.Namespace) -> int:
    if args.extract:
        doc = json.loads(Path(args.extract).read_text())
    else:
        try:
            doc = extract_board(args.board, backend=args.backend)
        except (HeadlessUnavailable, HeadlessError, ExtractError) as exc:
            return _fail(str(exc))

    board_path = args.board or doc.get("board", {}).get("path")
    store = None if args.no_store else _store(args, board_path)
    extra_path = Path(args.extra) if args.extra else discover_extra(board_path)

    if store is not None and not args.no_parts_file:
        found = parts_file.discover(board_path)
        if found is not None:
            try:
                imported = parts_file.import_file(found, store)
                print(f"  parts file: {found.name} ({imported.records} record(s))")
            except parts_file.PartsFileError as exc:
                print(f"  parts file {found.name} ignored: {exc}")

    overlay = None
    enrich_report = None
    if store is not None or extra_path is not None or args.datasheets:
        rules = datasheet_pdf.load_rules(args.rules) if (args.rules or args.datasheets) else None
        try:
            overlay, enrich_report = resolve(
                doc,
                extra_path=extra_path,
                store=store,
                rules=rules,
                datasheets_dir=args.datasheets,
                board_path=board_path,
                step_models=not args.no_step,
                heuristics=not args.no_heuristics,
            )
        except (extra_toml.ExtraTomlError, datasheet_pdf.DatasheetError) as exc:
            return _fail(str(exc))

    try:
        result = build_scene(
            doc,
            margin_mm=args.margin,
            allowed_sides=SIDE_CHOICES[args.sides],
            enrichment=overlay,
            enrichment_report=enrich_report,
        )
    except ExtractError as exc:
        return _fail(str(exc))

    write_scene(args.output, result.scene)
    report = result.report

    print(
        f"scene: {report.n_components} components, {report.n_pins} pins, "
        f"{report.n_nets} nets ({report.n_net_entries} connections) -> {args.output}"
    )
    if report.derived_courtyards:
        print(
            f"  courtyards derived from pads: {len(report.derived_courtyards)} "
            f"component(s)"
        )
    if report.inferred_pin_roles:
        print(f"  pin roles inferred from net names: {report.inferred_pin_roles}")
    if report.diff_pairs:
        print(f"  differential pairs matched: {len(report.diff_pairs)}")
    if report.authoritative_pin_roles:
        print(f"  pin roles from enrichment: {report.authoritative_pin_roles}")
    if enrich_report is not None:
        if store is not None:
            print(f"  enrichment store: {store.root}")
        if extra_path is not None:
            print(f"  user rules: {extra_path}")
        coverage = report.coverage
        print(
            f"  enrichment: {enrich_report.parts_matched} part match(es), "
            f"{enrich_report.datasheets_read} datasheet(s), "
            f"{enrich_report.step_models_read} STEP model(s)"
        )
        print(
            f"  coverage: height {coverage['height_mm']}/{coverage['components']}, "
            f"mass {coverage['mass_g']}/{coverage['components']}, "
            f"orientation {coverage['orientation_deg']}/{coverage['components']}, "
            f"pin current {coverage['pin_current']}/{coverage['pins']}"
        )
    if report.constraints:
        summary = ", ".join(f"{value} {key}" for key, value in report.constraints.items() if value)
        print(f"  constraints from enrichment: {summary or 'none'}")
    names = report.degraded_names()
    print(
        f"  degraded (mask 0x{report.degraded:08X}, {len(names)} categories): "
        f"{' '.join(names) if names else 'none'}"
    )
    for warning in report.warnings[:5]:
        print(f"  warning: {warning}")

    if args.report:
        payload = {
            "schema": "dodplace.report/1",
            "board": doc["board"].get("path"),
            "backend": f"{doc['backend']} {doc['backend_version']}",
            "counts": result.builder.counts(),
            "degraded_mask": report.degraded,
            "degraded": names,
            "derived_courtyards": report.derived_courtyards,
            "inferred_pin_roles": report.inferred_pin_roles,
            "diff_pairs": report.diff_pairs,
            "netclasses": report.netclass_names,
            "coverage": report.coverage,
            "constraints": report.constraints,
            "warnings": report.warnings,
        }
        Path(args.report).write_text(json.dumps(payload, indent=1, sort_keys=True))
        print(f"  report -> {args.report}")

    if args.enrich_report:
        payload = (enrich_report.to_json() if enrich_report is not None
                   else {"schema": "dodplace.enrichment/1", "components": {},
                         "unresolved": {}, "sources": {}, "warnings": []})
        Path(args.enrich_report).write_text(json.dumps(payload, indent=1, sort_keys=True))
        print(f"  enrichment report -> {args.enrich_report}")
    return 0


def cmd_show(args: argparse.Namespace) -> int:
    scene = read_scene(args.scene)
    counts = scene.counts
    print(f"scene: {args.scene} (IR {scene.version[0]}.{scene.version[1]})")
    print(
        f"  {counts['comps']} components, {counts['pins']} pins, "
        f"{counts['nets']} nets ({counts['net_entries']} connections)"
    )
    print(
        f"  {counts['polygons']} polygons ({counts['vertices']} vertices), "
        f"{scene.num_net_classes} net classes"
    )
    print(
        f"  constraints: {counts['decoupling']} decoupling, {counts['thermal']} thermal, "
        f"{counts['symmetry']} symmetry, {counts['diffpairs']} diffpair"
    )
    board = scene.board
    print(
        f"  board: {board.copper_layers} copper layers, "
        f"clearance {board.global_clearance} mm, margin {board.courtyard_fallback_margin} mm"
    )
    names = degraded_names(board.degraded_mask)
    print(
        f"  degraded (mask 0x{board.degraded_mask:08X}, {len(names)} categories): "
        f"{' '.join(names) if names else 'none'}"
    )
    return 0


def cmd_solve(args: argparse.Namespace) -> int:
    solver = find_solver(args.solver)
    if not solver:
        return _fail(
            "dodplace-solve not found; build it with "
            "`cmake --build --preset gcc-debug` or pass --solver PATH"
        )
    command = [str(solver), args.scene]
    if args.output:
        command += ["--identity", "-o", args.output]
    if args.quiet:
        command += ["--quiet"]
    return subprocess.call(command)


def cmd_apply(args: argparse.Namespace) -> int:
    """Write a legalised copy of a board from a solver placement."""
    from .kicad.apply import (
        ApplyError,
        apply_placement_headless,
        build_updates,
        run_drc,
        write_report,
    )
    from .kicad.headless import HeadlessError, HeadlessUnavailable

    board = Path(args.board)
    if not board.is_file():
        return _fail(f"board file not found: {board}")

    # The extract carries the original origins the movements are relative to.
    extract_path = Path(args.extract) if args.extract else (
        board.resolve().parent / ".dodplace" / "extract.json"
    )
    if extract_path.is_file():
        print(f"extract: {extract_path}")
        doc = json.loads(extract_path.read_text())
    else:
        print("extract: running the KiCad extractor (no --extract, no cached dump)")
        try:
            doc = extract_board(str(board), backend="auto")
        except (HeadlessUnavailable, HeadlessError, ExtractError) as exc:
            return _fail(str(exc))

    try:
        updates = build_updates(args.scene, args.placement, doc, moved_only=not args.all)
    except ApplyError as exc:
        return _fail(str(exc))

    print(f"updates: {len(updates)} footprint(s) to move")
    if not updates:
        print("  nothing moved: the placement matches the board")

    try:
        applied = apply_placement_headless(str(board), updates, args.output)
    except (ApplyError, HeadlessError, HeadlessUnavailable) as exc:
        return _fail(str(exc))

    report_path = args.report or str(Path(args.output).with_suffix("")) + ".report.json"
    write_report(report_path, updates, args.scene, args.placement, str(board), applied)
    print(f"wrote {args.output} ({applied} footprint(s) moved)")
    print(f"report {report_path}")

    if args.check_drc:
        try:
            bad, text = run_drc(args.output)
        except (ApplyError, HeadlessError) as exc:
            return _fail(str(exc))
        head = [line for line in text.splitlines() if line.strip()][:1]
        print(f"drc: {'violations found' if bad else 'clean'} ({head[0] if head else ''})")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="dodplace",
        description="KiCad front end for the dodplace automatic PCB placement engine",
    )
    parser.add_argument("--version", action="version", version=f"dodplace {__version__}")
    sub = parser.add_subparsers(dest="command", required=True)

    extract = sub.add_parser("extract", help="dump the board as extract JSON")
    extract.add_argument("board")
    extract.add_argument("-o", "--output", required=True)
    extract.add_argument("--backend", choices=BACKENDS, default="auto")
    extract.set_defaults(func=cmd_extract)

    ingest = sub.add_parser("ingest", help="board -> scene.bin (the engine's IR)")
    ingest.add_argument("board", nargs="?", help="board file (omit with --extract)")
    ingest.add_argument("-o", "--output", required=True)
    ingest.add_argument("--backend", choices=BACKENDS, default="auto")
    ingest.add_argument("--extract", help="reuse an existing extract JSON")
    ingest.add_argument(
        "--margin", type=float, default=0.25,
        help="courtyard fallback margin in mm (default 0.25)",
    )
    ingest.add_argument("--sides", choices=tuple(SIDE_CHOICES), default="both")
    ingest.add_argument("--report", help="write a JSON report next to the scene")
    ingest.add_argument("--extra", help="extra.toml with user rules "
                                        "(default: dodplace.toml or extra.toml next to the board)")
    ingest.add_argument("--store", help="enrichment cache directory "
                                        "(default: <board dir>/.dodplace/cache)")
    ingest.add_argument("--no-store", action="store_true", help="ignore the cache entirely")
    ingest.add_argument("--datasheets", help="directory holding datasheet PDFs")
    ingest.add_argument("--rules", help="datasheet rule file or directory")
    ingest.add_argument("--no-step", action="store_true", help="skip 3D model heights")
    ingest.add_argument("--no-heuristics", action="store_true",
                        help="skip the netlist decoupling heuristic")
    ingest.add_argument("--no-parts-file", action="store_true",
                        help="do not auto-import a parts file found next to the board")
    ingest.add_argument("--enrich-report", help="write the enrichment provenance report here")
    ingest.set_defaults(func=cmd_ingest)

    show = sub.add_parser("show", help="summarise a scene.bin")
    show.add_argument("scene")
    show.set_defaults(func=cmd_show)

    solve = sub.add_parser("solve", help="run the C engine on a scene.bin")
    solve.add_argument("scene")
    solve.add_argument("-o", "--output", help="write a placement.bin")
    solve.add_argument("--solver", help="path to the dodplace-solve binary")
    solve.add_argument("--quiet", action="store_true")

    apply_cmd = sub.add_parser("apply",
                               help="write a legalised board copy from a placement.bin")
    apply_cmd.add_argument("board", help="the .kicad_pcb the scene was built from")
    apply_cmd.add_argument("--scene", required=True, help="scene.bin (same component order)")
    apply_cmd.add_argument("--placement", required=True, help="placement.bin from the solver")
    apply_cmd.add_argument("-o", "--output", required=True, help="destination .kicad_pcb")
    apply_cmd.add_argument("--extract", help="reuse an extract JSON (default: .dodplace/extract.json)")
    apply_cmd.add_argument("--report", help="audit JSON (default: <output>.report.json)")
    apply_cmd.add_argument("--all", action="store_true",
                           help="write every component, not only the ones that moved")
    apply_cmd.add_argument("--check-drc", action="store_true",
                           help="run kicad-cli DRC on the result (slower)")
    apply_cmd.set_defaults(func=cmd_apply)
    solve.set_defaults(func=cmd_solve)

    enrich = sub.add_parser("enrich", help="manage the enrichment store")
    enrich_sub = enrich.add_subparsers(dest="enrich_command", required=True)

    parts = enrich_sub.add_parser("parts", help="import a CSV/TOML parts file")
    parts.add_argument("file")
    parts.add_argument("--store", help="store directory (default: ./.dodplace/cache)")
    parts.set_defaults(func=cmd_enrich_parts)

    datasheet = enrich_sub.add_parser("datasheet", help="extract facts from a datasheet PDF")
    datasheet.add_argument("pdf")
    datasheet.add_argument("--ref", default="?", help="component reference, for rule matching")
    datasheet.add_argument("--fpid", default="", help="footprint id, for rule matching")
    datasheet.add_argument("--mpn", default="", help="manufacturer part number, for rule matching")
    datasheet.add_argument("--rules", help="rule file or directory")
    datasheet.add_argument("--store", help="cache the extracted facts here")
    datasheet.set_defaults(func=cmd_enrich_datasheet)

    extra = enrich_sub.add_parser("extra", help="write a commented extra.toml starter")
    extra.add_argument("board", nargs="?", help="board file (omit with --extract)")
    extra.add_argument("-o", "--output", help="destination (default: next to the board)")
    extra.add_argument("--extract", help="reuse an existing extract JSON")
    extra.add_argument("--backend", choices=BACKENDS, default="auto")
    extra.add_argument("--force", action="store_true", help="replace an existing file")
    extra.set_defaults(func=cmd_enrich_extra)

    skeleton = enrich_sub.add_parser("skeleton",
                                     help="write a parts CSV pre-filled from the board")
    skeleton.add_argument("board", nargs="?", help="board file (omit with --extract)")
    skeleton.add_argument("-o", "--output", help="destination (default: next to the board)")
    skeleton.add_argument("--extract", help="reuse an existing extract JSON")
    skeleton.add_argument("--backend", choices=BACKENDS, default="auto")
    skeleton.add_argument("--force", action="store_true", help="replace an existing file")
    skeleton.set_defaults(func=cmd_enrich_skeleton)

    step = enrich_sub.add_parser("step", help="report the bounding box of a STEP model")
    step.add_argument("file")
    step.set_defaults(func=cmd_enrich_step)

    show = enrich_sub.add_parser("show", help="summarise the enrichment store")
    show.add_argument("--store", help="store directory (default: ./.dodplace/cache)")
    show.set_defaults(func=cmd_enrich_show)

    plugin = sub.add_parser("plugin", help="KiCad plugin: analyze action and installer")
    plugin_sub = plugin.add_subparsers(dest="plugin_command", required=True)

    analyze = plugin_sub.add_parser("analyze", help="read a board and report (read-only)")
    analyze.add_argument("--board", help="board file (default: the board open in KiCad)")
    analyze.add_argument("--ipc", action="store_true",
                         help="find the board through the KiCad IPC API")
    analyze.add_argument("--save", action="store_true",
                         help="ask KiCad to save the open board first")
    analyze.add_argument("--store", help="enrichment cache directory")
    analyze.add_argument("--extra", help="extra.toml with user rules")
    analyze.add_argument("--datasheets", help="directory holding datasheet PDFs")
    analyze.add_argument("--log", help="log file (default: <board>/.dodplace/plugin.log)")
    analyze.add_argument("--no-solver", action="store_true",
                         help="skip the C engine validation step")
    analyze.add_argument("--report-only", action="store_true",
                         help="write no artifact at all, only report and log")
    analyze.add_argument("--json", action="store_true", help="also print a JSON outcome")
    analyze.add_argument("--quiet", action="store_true")
    analyze.set_defaults(func=cmd_plugin_analyze)

    install = plugin_sub.add_parser("install", help="install the action into KiCad")
    install.add_argument("--repo", help="checkout to point the shim at")
    install.add_argument("--plugins-dir", help="KiCad plugins directory")
    install.add_argument("--config", help="repo pointer file (default ~/.config/dodplace/repo)")
    install.add_argument("--no-config", action="store_true", help="do not write the pointer file")
    install.add_argument("--fallback-log", help="where the shim logs its own failures")
    install.set_defaults(func=cmd_plugin_install)

    uninstall = plugin_sub.add_parser("uninstall", help="remove the installed action")
    uninstall.add_argument("--plugins-dir", help="KiCad plugins directory")
    uninstall.set_defaults(func=cmd_plugin_uninstall)

    status = plugin_sub.add_parser("status", help="installation and IPC state")
    status.add_argument("--repo", help="checkout to check")
    status.add_argument("--plugins-dir", help="KiCad plugins directory")
    status.set_defaults(func=cmd_plugin_status)

    log = plugin_sub.add_parser("log", help="show (or follow) the plugin log")
    log.add_argument("--board", help="board whose log to read")
    log.add_argument("-f", "--follow", action="store_true", help="keep printing new lines")
    log.set_defaults(func=cmd_plugin_log)

    info = sub.add_parser("info", help="show the toolchain the CLI would use")
    info.set_defaults(func=cmd_info)
    return parser


def cmd_enrich_parts(args: argparse.Namespace) -> int:
    store = _store(args)
    try:
        report = parts_file.import_file(args.file, store)
    except parts_file.PartsFileError as exc:
        return _fail(str(exc))
    print(f"imported {report.summary()} into {store.root}")
    for note in report.skipped[:5]:
        print(f"  note: {note}")
    if len(report.skipped) > 5:
        print(f"  ... and {len(report.skipped) - 5} more")
    return 0


def cmd_enrich_datasheet(args: argparse.Namespace) -> int:
    rules = datasheet_pdf.load_rules(args.rules) if args.rules else datasheet_pdf.load_rules()
    if not rules:
        return _fail("no datasheet rules found (pass --rules FILE)")
    if not datasheet_pdf.available():
        return _fail("pdfplumber is not installed; run `uv sync --extra pdf`")

    component = {"ref": args.ref, "fpid": args.fpid, "_mpn": args.mpn}
    try:
        facts = datasheet_pdf.extract_from_pdf(args.pdf, rules, component)
    except datasheet_pdf.DatasheetError as exc:
        return _fail(str(exc))

    if not facts.facts:
        print("no field matched; the rules did not apply to this document")
        return 1
    for name, fact in sorted(facts.facts.items()):
        print(f"  {name:22} {fact.value!r:>16}  [{fact.provenance.confidence:.2f}] "
              f"{fact.provenance.source_ref}")
    if args.store:
        store = _store(args)
        store.merge("parts", f"fp:{args.fpid or args.ref}", facts)
        print(f"cached into {store.root}")
    return 0


def _extract_for(args: argparse.Namespace) -> dict:
    """The extract to work from: a file, or the board it was asked about."""
    if getattr(args, "extract", None):
        return json.loads(Path(args.extract).read_text())
    try:
        return extract_board(args.board, backend=args.backend)
    except (HeadlessUnavailable, HeadlessError, ExtractError) as exc:
        raise ExtractError(str(exc)) from exc


def cmd_enrich_extra(args: argparse.Namespace) -> int:
    from .enrich import template

    try:
        doc = _extract_for(args)
    except ExtractError as exc:
        return _fail(str(exc))

    board = args.board or doc.get("board", {}).get("path")
    if not board:
        return _fail("no board path: pass a board file or use --extract with a board path")
    output = Path(args.output) if args.output else Path(board).resolve().parent / "extra.toml"
    try:
        written = template.write_template(doc, output, force=args.force)
    except FileExistsError as exc:
        return _fail(str(exc))

    hints = template.hints_from_extract(doc)
    print(f"wrote {written}")
    mechanical = ", ".join(
        f"{hints.count(p)} {p}*" for p in template.MECHANICAL_PREFIXES if hints.count(p)
    )
    thermal = ", ".join(
        f"{hints.count(p)} {p}*" for p in template.THERMAL_PREFIXES if hints.count(p)
    )
    if mechanical:
        print(f"  mechanical parts seeded: {mechanical}")
    if thermal:
        print(f"  thermal candidates seeded: {thermal}")
    print("  everything is commented or already valid: edit, then re-run the action")
    return 0


def cmd_enrich_skeleton(args: argparse.Namespace) -> int:
    from .enrich import template

    try:
        doc = _extract_for(args)
    except ExtractError as exc:
        return _fail(str(exc))

    board = args.board or doc.get("board", {}).get("path")
    output = Path(args.output) if args.output else (
        Path(board).resolve().parent / "dodplace-parts.csv" if board
        else Path.cwd() / "dodplace-parts.csv"
    )
    if output.exists() and not args.force:
        return _fail(f"{output} already exists; edit it, or pass --force to replace it")

    rows = template.skeleton_rows(doc)
    if not rows:
        return _fail("no supplier part number found on this board: nothing to pre-fill")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(template.render_skeleton(rows))
    print(f"wrote {output} ({len(rows)} row(s) to fill in)")
    print("  fill Height (mm), Mass (g), Power (W) and Datasheet, then re-run the action")
    return 0


def cmd_enrich_step(args: argparse.Namespace) -> int:
    try:
        bbox = step_model.read_bbox(args.file)
    except step_model.StepModelError as exc:
        return _fail(str(exc))
    size = bbox.size
    print(f"{args.file}")
    print(f"  bbox min {bbox.min} max {bbox.max}")
    print(f"  size {size[0]:.3f} x {size[1]:.3f} x {size[2]:.3f} mm (height = Z)")
    return 0


def cmd_enrich_show(args: argparse.Namespace) -> int:
    store = _store(args)
    total = 0
    for kind in ("parts", "datasheets", "steps"):
        count = store.count(kind)
        total += count
        print(f"  {kind:11} {count}")
    print(f"store: {store.root} ({total} record(s))")
    for identity in list(store.keys("parts"))[:10]:
        record = store.get("parts", identity)
        fields = ", ".join(sorted(record.facts)) if record else ""
        print(f"  {identity:24} {fields}")
    return 0


def _resolve_log_path(board: str | None) -> Path:
    """The log next to a board, else the nearest one, else the shim log."""
    from .plugin.install import default_fallback_log

    if board:
        return Path(board).resolve().parent / ".dodplace" / "plugin.log"
    here = Path.cwd().resolve()
    for directory in (here, *here.parents):
        candidate = directory / ".dodplace" / "plugin.log"
        if candidate.is_file():
            return candidate
    return default_fallback_log()


def cmd_plugin_analyze(args: argparse.Namespace) -> int:
    from .plugin.runner import BoardNotFound, analyze, find_board, save_open_board

    try:
        board, how = find_board(args.board, use_ipc=args.ipc)
    except BoardNotFound as exc:
        return _fail(str(exc), code=3)

    if args.save:
        reason = save_open_board()
        print("saved the open board through KiCad" if reason is None
              else f"warning: could not save the open board: {reason}")
    elif how == "ipc":
        print("note: reading the board file from disk; save in KiCad first "
              "if you have unsaved changes (or pass --save)")

    try:
        outcome = analyze(
            board,
            found_via=how,
            store_dir=args.store,
            extra_path=args.extra,
            datasheets_dir=args.datasheets,
            log_path=args.log,
            validate_with_solver=not args.no_solver,
            write_artifacts=not args.report_only,
        )
    except Exception as exc:  # noqa: BLE001 - the plugin must never traceback at the user
        return _fail(f"analysis failed: {exc}")

    if not args.quiet:
        for line in outcome.summary:
            print(line)
    if args.json:
        print(json.dumps(outcome.to_json(), indent=1, sort_keys=True))
    return 0 if outcome.ok else 1


def cmd_plugin_install(args: argparse.Namespace) -> int:
    from .plugin import install as installer

    repo = Path(args.repo) if args.repo else Path(__file__).resolve().parents[3]
    try:
        report = installer.install_plugin(
            repo,
            plugins_dir=args.plugins_dir,
            config_file=args.config,
            write_config=not args.no_config,
            fallback_log=args.fallback_log,
        )
    except installer.InstallError as exc:
        return _fail(f"cannot install: {exc}")
    print(report.summary())
    return 0


def cmd_plugin_uninstall(args: argparse.Namespace) -> int:
    from .plugin import install as installer

    removed = installer.uninstall_plugin(plugins_dir=args.plugins_dir)
    if removed is None:
        print(f"nothing installed in {args.plugins_dir or installer.default_plugins_dir()}")
        return 0
    print(f"removed {removed}")
    print("restart KiCad to drop the action")
    return 0


def cmd_plugin_status(args: argparse.Namespace) -> int:
    from .plugin import install as installer

    repo = Path(args.repo) if args.repo else None
    info = installer.status(repo=repo, plugins_dir=args.plugins_dir)
    print(f"plugins dir : {info['plugins_dir']}")
    print(f"installed   : {'yes' if info['installed'] else 'no'}")
    if info.get("files"):
        missing = [name for name, present in info["files"].items() if not present]
        detail = "all present" if not missing else "missing " + ", ".join(missing)
        print(f"plugin files: {detail}")
    if info.get("repo"):
        print(f"repo        : {info['repo']}")
    if info.get("config_repo"):
        print(f"config repo : {info['config_repo']}")
    print(f"uv          : {info['uv'] or 'not found'}")
    for problem in info.get("repo_problems", []):
        print(f"problem     : {problem}")

    try:
        from kipy import KiCad

        kicad = KiCad()
        print(f"KiCad IPC   : {kicad.get_version()}")
    except ImportError:
        print("KiCad IPC   : kicad-python not installed (uv sync --extra ipc)")
    except Exception as exc:  # noqa: BLE001
        print(f"KiCad IPC   : not reachable ({exc})")

    from .plugin.runner import IpcUnavailable, board_from_ipc

    try:
        board, _how = board_from_ipc()
        print(f"open board  : {board}")
    except IpcUnavailable as exc:
        print(f"open board  : none ({exc})")
    return 0


def cmd_plugin_log(args: argparse.Namespace) -> int:
    path = _resolve_log_path(args.board)
    if not path.is_file():
        return _fail(f"no log at {path}")
    print(f"--- {path} ---")
    with path.open() as handle:
        sys.stdout.write(handle.read())
    if not args.follow:
        return 0
    try:
        with path.open() as handle:
            handle.seek(0, os.SEEK_END)
            while True:
                line = handle.readline()
                if line:
                    sys.stdout.write(line)
                    sys.stdout.flush()
                else:
                    time.sleep(0.3)
    except KeyboardInterrupt:
        return 0
    return 0


def cmd_info(_args: argparse.Namespace) -> int:
    print(f"dodplace {__version__}")
    try:
        python = find_kicad_python()
        print(f"  KiCad python: {python} ({kicad_python_version()})")
    except (HeadlessUnavailable,) as exc:
        print(f"  KiCad python: unavailable ({exc})")
    solver = find_solver(None)
    print(f"  solver: {solver or 'not built'}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except KeyboardInterrupt:  # pragma: no cover
        return 130


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
