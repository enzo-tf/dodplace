"""The `dodplace: analyze board` action.

Read-only by construction: it reads a board, resolves optional data, writes the
scene and the reports into ``<board>/.dodplace/`` and asks the C engine to
validate the scene. It never modifies the board file, and never touches the
KiCad document, so it is safe to run at any time.

The board is found in this order: an explicit path, then the board open in
KiCad over the IPC API, then the single ``*.kicad_pcb`` in the working
directory. That makes the same code usable from the toolbar and from a terminal.
"""

from __future__ import annotations

import json
import tempfile
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

from ..enrich import EnrichmentStore, resolve
from ..enrich.providers import datasheet_pdf, extra_toml, parts_file
from ..ir import write_scene
from ..ir.spec import degraded_names
from ..kicad import (
    ExtractError,
    HeadlessError,
    HeadlessUnavailable,
    build_scene,
    extract_board,
)
from ..solver import SolverRun, run_solver
from .install import default_fallback_log

ARTIFACT_DIRNAME = ".dodplace"
SCENE_NAME = "scene.bin"
REPORT_NAME = "report.json"
ENRICH_REPORT_NAME = "enrich_report.json"
LOG_NAME = "plugin.log"
EXTRACT_NAME = "extract.json"

EXTRA_CANDIDATES = ("dodplace.toml", "extra.toml")


class BoardNotFound(RuntimeError):
    """No board could be located."""


class IpcUnavailable(RuntimeError):
    """The board could not be located over the IPC API, and why.

    The reason matters: ``GetOpenDocuments`` is a PCB-editor-side command, so
    KiCad answers "no handler available for request" when the PCB editor is not
    open, which is very different from "kicad-python is missing".
    """


class AnalyzeError(RuntimeError):
    """The pipeline could not complete."""


@dataclass
class AnalyzeOutcome:
    board: Path
    found_via: str
    artifacts: Path
    log: Path
    summary: list[str] = field(default_factory=list)
    solver: SolverRun | None = None
    ok: bool = True

    def to_json(self) -> dict:
        payload = {
            "schema": "dodplace.plugin/1",
            "board": str(self.board),
            "found_via": self.found_via,
            "artifacts": str(self.artifacts),
            "log": str(self.log),
            "ok": self.ok,
            "summary": self.summary,
        }
        if self.solver is not None:
            payload["solver"] = {
                "command": self.solver.command,
                "returncode": self.solver.returncode,
                "stdout": self.solver.stdout,
                "stderr": self.solver.stderr,
            }
        return payload


# ---------------------------------------------------------------------------
# Board discovery
# ---------------------------------------------------------------------------


def board_from_ipc() -> tuple[Path, str]:
    """The board currently open in KiCad, or :class:`IpcUnavailable` with why."""
    try:
        from kipy import KiCad
        from kipy.errors import ConnectionError as IpcConnectionError
    except ImportError as exc:
        raise IpcUnavailable(
            "kicad-python is not installed; run `uv sync --extra ipc`"
        ) from exc

    kicad = KiCad()  # connecting is lazy: the first request is what may fail

    try:
        board = kicad.get_board()
    except IpcConnectionError as exc:
        raise IpcUnavailable(f"cannot connect to KiCad (is it running?): {exc}") from exc
    except Exception as exc:  # noqa: BLE001
        raise IpcUnavailable(
            "KiCad is running but reports no open board - open the PCB editor "
            f"with a board loaded ({exc})"
        ) from exc

    try:
        name = str(board.name)
        project = board.get_project()
        project_path = Path(project.path)
    except Exception as exc:  # noqa: BLE001
        raise IpcUnavailable(f"KiCad reported a board but not its path: {exc}") from exc

    for candidate in (project_path / name, project_path / f"{name}.kicad_pcb"):
        if candidate.is_file():
            return candidate, "ipc"
    raise IpcUnavailable(
        f"KiCad reported board {name!r} in {str(project_path)!r} but no such file exists"
    )


def find_board(explicit: str | Path | None = None, *, use_ipc: bool = False) -> tuple[Path, str]:
    if explicit:
        path = Path(explicit)
        if not path.is_file():
            raise BoardNotFound(f"board not found: {path}")
        return path, "explicit"

    ipc_reason = None
    if use_ipc:
        try:
            return board_from_ipc()
        except IpcUnavailable as exc:
            ipc_reason = str(exc)

    candidates = sorted(Path.cwd().glob("*.kicad_pcb"))
    if len(candidates) == 1:
        return candidates[0], "cwd"
    if use_ipc:
        raise BoardNotFound(
            f"no board from the IPC API: {ipc_reason}; and no single .kicad_pcb "
            "in the working directory"
        )
    raise BoardNotFound(
        "no board given: pass --board PATH, or run this from the directory holding "
        "the .kicad_pcb, or start it from KiCad"
    )


def save_open_board() -> str | None:
    """Ask KiCad to save the open board; None on success, else the reason."""
    try:
        from kipy import KiCad

        KiCad().get_board().save()
        return None
    except Exception as exc:  # noqa: BLE001
        return str(exc)


# ---------------------------------------------------------------------------
# The action
# ---------------------------------------------------------------------------


def _log_header(log_path: Path, board: Path, found_via: str) -> None:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("a") as handle:
        handle.write("\n" + "=" * 72 + "\n")
        handle.write(f"dodplace analyze  {datetime.now().isoformat(timespec='seconds')}\n")
        handle.write(f"board: {board}  (found via {found_via})\n")


def _log_lines(log_path: Path, lines: list[str]) -> None:
    with log_path.open("a") as handle:
        for line in lines:
            handle.write(line + "\n")


def analyze(
    board_path: str | Path,
    *,
    found_via: str = "explicit",
    store_dir: str | Path | None = None,
    extra_path: str | Path | None = None,
    datasheets_dir: str | Path | None = None,
    log_path: str | Path | None = None,
    validate_with_solver: bool = True,
    write_artifacts: bool = True,
    import_parts_file: bool = True,
) -> AnalyzeOutcome:
    """Run the whole pipeline on a board.

    With ``write_artifacts=False`` nothing but the log is left behind: the scene
    is built in a temporary directory so the engine can still validate it. That
    is what the "report only" action uses.
    """
    board = Path(board_path).resolve()
    artifacts = board.parent / ARTIFACT_DIRNAME
    artifacts.mkdir(parents=True, exist_ok=True)
    log = Path(log_path) if log_path else artifacts / LOG_NAME
    _log_header(log, board, found_via)

    outcome = AnalyzeOutcome(board=board, found_via=found_via, artifacts=artifacts, log=log)
    summary: list[str] = []
    pending_prelude: list[str] = []

    try:
        doc = extract_board(str(board))
    except (HeadlessUnavailable, HeadlessError, ExtractError) as exc:
        summary.append(f"extraction failed: {exc}")
        _log_lines(log, summary)
        outcome.summary = summary
        outcome.ok = False
        return outcome

    if write_artifacts:
        (artifacts / EXTRACT_NAME).write_text(json.dumps(doc, indent=1, sort_keys=True))

    store = EnrichmentStore(store_dir) if store_dir else EnrichmentStore(artifacts / "cache")

    # A parts file dropped next to the board is imported automatically: it is
    # project data, meant to live beside the board, and importing is idempotent.
    prelude: list[str] = []
    if import_parts_file:
        found = parts_file.discover(board)
        if found is not None:
            try:
                imported = parts_file.import_file(found, store)
                prelude.append(
                    f"  parts file: {found.name} ({imported.records} record(s))"
                )
            except parts_file.PartsFileError as exc:
                prelude.append(f"  parts file {found.name} ignored: {exc}")

    if extra_path is None:
        extra_path = next(
            (board.parent / name for name in EXTRA_CANDIDATES if (board.parent / name).is_file()),
            None,
        )
    rules = datasheet_pdf.load_rules() if datasheets_dir else None

    overlay = None
    enrichment_report = None
    try:
        overlay, enrichment_report = resolve(
            doc,
            extra_path=extra_path,
            store=store,
            rules=rules,
            datasheets_dir=datasheets_dir,
            board_path=board,
        )
    except (extra_toml.ExtraTomlError, datasheet_pdf.DatasheetError) as exc:
        summary.append(f"enrichment failed: {exc}")
        _log_lines(log, summary)
        outcome.summary = summary
        outcome.ok = False
        return outcome

    try:
        result = build_scene(doc, enrichment=overlay, enrichment_report=enrichment_report)
    except ExtractError as exc:
        summary.append(f"scene build failed: {exc}")
        _log_lines(log, summary)
        outcome.summary = summary
        outcome.ok = False
        return outcome

    scene_path = artifacts / SCENE_NAME
    scratch: tempfile.TemporaryDirectory | None = None
    if not write_artifacts:
        scratch = tempfile.TemporaryDirectory(prefix="dodplace-report-")
        scene_path = Path(scratch.name) / SCENE_NAME
    write_scene(str(scene_path), result.scene)

    report = result.report
    coverage = report.coverage
    counts = result.builder.counts()
    summary.append(f"board: {board}")
    summary.append(f"  found via: {found_via}")
    summary.extend(prelude)
    summary.append(
        f"  {counts['comps']} components, {counts['pins']} pins, "
        f"{counts['nets']} nets ({counts['net_entries']} connections)"
    )
    summary.append(
        f"  enrichment: {enrichment_report.parts_matched} part(s), "
        f"{enrichment_report.datasheets_read} datasheet(s), "
        f"{enrichment_report.datasheets_cached} cached, "
        f"{enrichment_report.step_models_read} STEP model(s)"
    )
    summary.append(
        f"  coverage: height {coverage['height_mm']}/{coverage['components']}, "
        f"mass {coverage['mass_g']}/{coverage['components']}, "
        f"orientation {coverage['orientation_deg']}/{coverage['components']}, "
        f"pin roles {coverage['pin_role']}/{coverage['pins']}, "
        f"pin current {coverage['pin_current']}/{coverage['pins']}"
    )
    constraints = ", ".join(
        f"{value} {name}" for name, value in report.constraints.items() if value
    )
    summary.append(f"  constraints: {constraints or 'none'}")
    summary.append(
        f"  degraded (0x{report.degraded:08X}): "
        f"{' '.join(degraded_names(report.degraded)) or 'none'}"
    )
    for warning in report.warnings[:5]:
        summary.append(f"  warning: {warning}")
    if len(report.warnings) > 5:
        summary.append(f"  ... and {len(report.warnings) - 5} more warning(s)")

    if validate_with_solver:
        try:
            outcome.solver = run_solver(scene_path)
        except (FileNotFoundError, TimeoutError) as exc:
            summary.append(f"  engine validation skipped: {exc}")
        else:
            summary.append(
                f"  engine validation: {'ok' if outcome.solver.ok else 'FAILED'} "
                f"(exit {outcome.solver.returncode})"
            )
            if not outcome.solver.ok:
                outcome.ok = False
                for line in outcome.solver.stderr.splitlines()[-3:]:
                    summary.append(f"    {line}")

    unresolved = [
        (name, count) for name, count in enrichment_report.unresolved.items() if count
    ]
    if unresolved:
        unresolved.sort(key=lambda item: (-item[1], item[0]))
        width = lambda name: coverage["pins"] if name.startswith("pin_") else coverage["components"]
        summary.append(
            "  unresolved: "
            + ", ".join(f"{name} {count}/{width(name)}" for name, count in unresolved[:5])
        )
        if len(unresolved) > 5:
            summary.append(f"    (+{len(unresolved) - 5} more field(s) in the enrichment report)")

    report_payload = {
        "schema": "dodplace.report/1",
        "board": str(board),
        "found_via": found_via,
        "counts": counts,
        "coverage": coverage,
        "constraints": report.constraints,
        "degraded_mask": report.degraded,
        "degraded": degraded_names(report.degraded),
        "netclasses": report.netclass_names,
        "derived_courtyards": report.derived_courtyards,
        "diff_pairs": report.diff_pairs,
        "warnings": report.warnings,
        "artifacts": {
            "scene": str(scene_path),
            "extract": str(artifacts / EXTRACT_NAME),
            "enrich_report": str(artifacts / ENRICH_REPORT_NAME),
            "log": str(log),
        },
    }
    if write_artifacts:
        (artifacts / REPORT_NAME).write_text(json.dumps(report_payload, indent=1, sort_keys=True))
        (artifacts / ENRICH_REPORT_NAME).write_text(
            json.dumps(enrichment_report.to_json(), indent=1, sort_keys=True)
        )
        summary.append(f"  artifacts: {artifacts}")
    else:
        summary.append(f"  report only: no artifact written (log: {log})")

    if scratch is not None:
        scratch.cleanup()

    _log_lines(log, summary)
    outcome.summary = summary
    return outcome


def report_missing_board(log_path: Path | None = None) -> None:
    """Record a shim-level failure where a human will look for it."""
    path = Path(log_path) if log_path else default_fallback_log()
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("a") as handle:
            handle.write(
                f"{datetime.now().isoformat(timespec='seconds')} "
                "dodplace plugin: no board found (no --board, no IPC board, "
                "no .kicad_pcb in the working directory)\n"
            )
    except OSError:
        pass
