"""The KiCad plugin: manifest, shim, installer and the analyze action.

Nothing here touches the real KiCad directories: every test works in a
temporary plugins directory, a temporary config file and a temporary log.
"""

from __future__ import annotations

import json
import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

import pytest

from dodplace.ir import read_scene
from dodplace.plugin import install as installer
from dodplace.plugin import runner
from dodplace.plugin.install import (
    ACTION_ID,
    ALL_ICONS,
    ICON_DIRNAME,
    ICONS_DARK,
    ICONS_LIGHT,
    REPORT_ACTION_ID,
    REPORT_ICONS_LIGHT,
    ACTION_NAME,
    MANIFEST_NAME,
    PLUGIN_DIRNAME,
    PLUGIN_ID,
    REQUIRED_FILES,
    REQUIREMENTS_NAME,
    SHIM_NAME,
    InstallError,
    build_manifest,
    render_requirements,
    render_shim,
    validate_manifest,
    verify_payload,
)
from dodplace.solver import find_solver

FIXTURES = Path(__file__).resolve().parent / "fixtures"


@pytest.fixture
def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


@pytest.fixture
def plugins_dir(tmp_path: Path) -> Path:
    return tmp_path / "kicad" / "plugins"


@pytest.fixture
def install_env(tmp_path: Path, plugins_dir: Path):
    """Installer arguments that keep every write inside tmp_path."""
    return {
        "plugins_dir": plugins_dir,
        "config_file": tmp_path / "config" / "repo",
        "fallback_log": tmp_path / "cache" / "shim.log",
    }


# ===========================================================================
# Manifest
# ===========================================================================


def test_manifest_satisfies_the_official_schema():
    manifest = build_manifest()
    assert validate_manifest(manifest) == []

    # The constraints from https://go.kicad.org/api/schemas/v1, checked directly
    # so the test does not simply mirror our own validator.
    import re

    assert re.match(r"^[a-zA-Z][-_a-zA-Z0-9.]{0,98}[a-zA-Z0-9]$", manifest["identifier"])
    assert manifest["identifier"] == PLUGIN_ID
    assert manifest["runtime"]["type"] in ("python", "exec")
    # Parity with the official round_tracks example.
    assert manifest["$schema"] == "https://go.kicad.org/api/schemas/v1"
    assert manifest["runtime"]["min_version"] == "3.9"
    assert manifest["actions"], "at least one action"
    action = manifest["actions"][0]
    assert re.match(r"^[a-zA-Z][-_a-zA-Z0-9.]{0,48}[a-zA-Z0-9]$", action["identifier"])
    assert action["identifier"] == ACTION_ID
    assert action["entrypoint"] == SHIM_NAME
    assert action["scopes"] == ["pcb"]
    assert action["show-button"] is True
    # A toolbar button is drawn only from these bitmaps.
    assert action["icons-light"] and action["icons-dark"]
    assert all(name.endswith(".png") for name in action["icons-light"])
    assert len(action["name"]) <= 200 and len(action["description"]) <= 500
    assert len(manifest["name"]) <= 200 and len(manifest["description"]) <= 500
    assert manifest["name"] == "dodplace"
    assert ACTION_NAME.startswith("dodplace")

    # Two actions: the full analysis and the read-only report.
    assert [entry["identifier"] for entry in manifest["actions"]] == [ACTION_ID, REPORT_ACTION_ID]
    report_action = manifest["actions"][1]
    assert report_action["args"] == ["--report-only"]
    assert report_action["entrypoint"] == SHIM_NAME, "one shim serves both"
    assert report_action["icons-light"] == list(REPORT_ICONS_LIGHT)
    assert report_action["icons-light"] != manifest["actions"][0]["icons-light"]


def test_manifest_validation_catches_broken_documents():
    broken = build_manifest()
    broken["identifier"] = "1bad"
    assert validate_manifest(broken)

    broken = build_manifest()
    broken["runtime"] = {"type": "magic"}
    assert validate_manifest(broken)

    broken = build_manifest()
    broken["actions"][0]["scopes"] = ["3d"]
    assert validate_manifest(broken)

    broken = build_manifest()
    del broken["actions"][0]["entrypoint"]
    assert validate_manifest(broken)

    broken = build_manifest()
    del broken["description"]
    assert validate_manifest(broken)


# ===========================================================================
# Installer
# ===========================================================================


def test_install_writes_manifest_shim_and_pointer(repo_root, install_env, plugins_dir):
    report = installer.install_plugin(repo_root, **install_env)

    plugin_dir = plugins_dir / PLUGIN_DIRNAME
    # Every file KiCad looks for, including the requirements file it runs pip on
    # and the icons it draws the toolbar button from.
    assert sorted(path.name for path in plugin_dir.iterdir()) == sorted(
        [MANIFEST_NAME, SHIM_NAME, REQUIREMENTS_NAME, ICON_DIRNAME]
    )
    for name in REQUIRED_FILES:
        assert (plugin_dir / name).is_file(), name
    assert report.wrote == [plugin_dir / name for name in
                            (MANIFEST_NAME, SHIM_NAME, REQUIREMENTS_NAME, *ALL_ICONS)]
    for name in ALL_ICONS:
        assert (plugin_dir / name).read_bytes().startswith(b"\x89PNG")

    manifest = json.loads((plugin_dir / MANIFEST_NAME).read_text())
    assert validate_manifest(manifest) == []

    shim = (plugin_dir / SHIM_NAME).read_text()
    assert str(repo_root.resolve()) in shim
    assert "@@" not in shim, "every template token must be substituted"
    assert str(install_env["fallback_log"]) in shim
    assert os.access(plugin_dir / SHIM_NAME, os.X_OK)

    pointer = install_env["config_file"]
    assert pointer.is_file()
    assert pointer.read_text().strip().splitlines()[-1] == str(repo_root.resolve())
    assert report.config_file == pointer
    assert "restart KiCad" in report.summary()


def test_install_is_idempotent(repo_root, install_env, plugins_dir):
    installer.install_plugin(repo_root, **install_env)
    first = (plugins_dir / PLUGIN_DIRNAME / SHIM_NAME).read_text()
    installer.install_plugin(repo_root, **install_env)
    assert (plugins_dir / PLUGIN_DIRNAME / SHIM_NAME).read_text() == first


def test_install_can_skip_the_pointer_file(repo_root, install_env):
    installer.install_plugin(repo_root, write_config=False, **install_env)
    assert not install_env["config_file"].exists()


def test_install_refuses_a_repo_without_a_venv(tmp_path, install_env, plugins_dir):
    fake = tmp_path / "fake"
    (fake / "python").mkdir(parents=True)
    (fake / "python" / "pyproject.toml").write_text("")
    with pytest.raises(InstallError) as excinfo:
        installer.install_plugin(fake, **install_env)
    assert "uv sync" in str(excinfo.value)
    assert not (plugins_dir / PLUGIN_DIRNAME).exists(), "a refused install must write nothing"


def test_uninstall_removes_only_our_directory(repo_root, install_env, plugins_dir):
    installer.install_plugin(repo_root, **install_env)
    neighbour = plugins_dir / "someone-elses-plugin"
    neighbour.mkdir()
    (neighbour / "plugin.json").write_text("{}")

    removed = installer.uninstall_plugin(plugins_dir=plugins_dir)
    assert removed == plugins_dir / PLUGIN_DIRNAME
    assert not (plugins_dir / PLUGIN_DIRNAME).exists()
    assert (neighbour / "plugin.json").is_file()
    assert installer.uninstall_plugin(plugins_dir=plugins_dir) is None


def test_status_reports_installation_state(repo_root, install_env, plugins_dir):
    before = installer.status(repo=repo_root, plugins_dir=plugins_dir)
    assert before["installed"] is False
    assert before["repo_problems"] == []

    installer.install_plugin(repo_root, **install_env)
    after = installer.status(plugins_dir=plugins_dir)
    assert after["installed"] is True
    assert after["repo"] == str(repo_root.resolve())
    assert after["repo_problems"] == []


def test_default_plugins_dir_follows_kicad_layout(monkeypatch, tmp_path):
    documents = tmp_path / "KiCad"
    (documents / "9.0" / "plugins").mkdir(parents=True)
    (documents / "10.0" / "plugins").mkdir(parents=True)
    monkeypatch.setenv("KICAD_DOCUMENTS_HOME", str(documents))
    assert installer.default_plugins_dir() == documents / "10.0" / "plugins"

    monkeypatch.setenv("KICAD_DOCUMENTS_HOME", str(tmp_path / "empty"))
    assert installer.default_plugins_dir().name == "plugins"


# ===========================================================================
# Shim
# ===========================================================================


def kicad_python() -> Path | None:
    base = Path("/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions")
    if not base.is_dir():
        return None
    versions = sorted(
        (entry for entry in base.iterdir() if entry.is_dir() and entry.name[:1].isdigit()),
        key=lambda path: [int(part) for part in path.name.split(".")],
        reverse=True,
    )
    for version in versions:
        candidate = version / "bin" / "python3"
        if candidate.is_file():
            return candidate
    return None


def test_shim_compiles_under_kicads_own_interpreter(tmp_path):
    """KiCad runs the shim with its bundled Python (3.9): it must parse there."""
    interpreter = kicad_python()
    if interpreter is None:
        pytest.skip("no KiCad Python on this machine")

    shim = render_shim(
        Path("/tmp/repo"),
        config=tmp_path / "config",
        fallback_log=tmp_path / "shim.log",
    )
    path = tmp_path / "main.py"
    path.write_text(shim)
    # compile() in a subprocess: checks the syntax without writing bytecode.
    probe = subprocess.run(
        [str(interpreter), "-c",
         "import sys; compile(open(sys.argv[1]).read(), sys.argv[1], 'exec')", str(path)],
        capture_output=True, text=True,
    )
    assert probe.returncode == 0, probe.stderr
    assert "f'" not in shim and 'f"' not in shim, "keep the shim free of modern syntax"


def test_shim_relays_output_and_exit_code(tmp_path):
    """A fake repo with a fake interpreter proves the relay works."""
    fake_repo = tmp_path / "fakerepo"
    venv_bin = fake_repo / "python" / ".venv" / "bin"
    venv_bin.mkdir(parents=True)
    (fake_repo / "python" / "pyproject.toml").write_text("")
    fake_python = venv_bin / "python"
    fake_python.write_text('#!/bin/sh\necho "FAKE $@"\necho "oops" >&2\nexit 7\n')
    fake_python.chmod(fake_python.stat().st_mode | stat.S_IEXEC)

    shim = tmp_path / "main.py"
    shim.write_text(
        render_shim(fake_repo, config=tmp_path / "config", fallback_log=tmp_path / "shim.log")
    )
    probe = subprocess.run([sys.executable, str(shim)], capture_output=True, text=True)
    assert probe.returncode == 7
    assert "FAKE -E -m dodplace plugin analyze --ipc" in probe.stdout
    assert "oops" in probe.stderr


def test_shim_reports_a_missing_repo(tmp_path):
    fallback = tmp_path / "shim.log"
    shim = tmp_path / "main.py"
    shim.write_text(
        render_shim(tmp_path / "gone", config=tmp_path / "config", fallback_log=fallback)
    )
    probe = subprocess.run([sys.executable, str(shim)], capture_output=True, text=True)
    assert probe.returncode == 1
    assert "re-run `dodplace plugin install`" in probe.stderr
    assert "no dodplace checkout found" in fallback.read_text()


def test_shim_uses_the_configured_repo_when_the_baked_one_is_gone(tmp_path):
    """The pointer file is the escape hatch when the checkout moves."""
    moved = tmp_path / "moved"
    venv_bin = moved / "python" / ".venv" / "bin"
    venv_bin.mkdir(parents=True)
    (moved / "python" / "pyproject.toml").write_text("")
    fake_python = venv_bin / "python"
    fake_python.write_text('#!/bin/sh\necho "MOVED OK"\n')
    fake_python.chmod(fake_python.stat().st_mode | stat.S_IEXEC)

    config = tmp_path / "config"
    config.write_text("# pointer\n" + str(moved) + "\n")
    shim = tmp_path / "main.py"
    shim.write_text(
        render_shim(tmp_path / "gone", config=config, fallback_log=tmp_path / "shim.log")
    )
    probe = subprocess.run([sys.executable, str(shim)], capture_output=True, text=True)
    assert probe.returncode == 0
    assert "MOVED OK" in probe.stdout


# ===========================================================================
# analyze
# ===========================================================================


def test_analyze_writes_artifacts_and_validates(tmp_path):
    board = tmp_path / "board.kicad_pcb"
    shutil.copy(FIXTURES / "scene_fixture.kicad_pcb", board)

    outcome = runner.analyze(board, found_via="explicit", log_path=tmp_path / "plugin.log")
    assert outcome.ok, outcome.summary

    artifacts = tmp_path / runner.ARTIFACT_DIRNAME
    for name in (runner.SCENE_NAME, runner.REPORT_NAME, runner.ENRICH_REPORT_NAME,
                 runner.EXTRACT_NAME):
        assert (artifacts / name).is_file(), name

    # The scene the plugin produced is a valid IR scene.
    scene = read_scene(str(artifacts / runner.SCENE_NAME))
    assert scene.counts["comps"] == 4
    assert scene.counts["nets"] == 6

    report = json.loads((artifacts / runner.REPORT_NAME).read_text())
    assert report["schema"] == "dodplace.report/1"
    assert report["found_via"] == "explicit"
    assert report["counts"]["pins"] == 14
    assert "NO_MASS" in report["degraded"]

    summary = "\n".join(outcome.summary)
    assert "4 components" in summary
    assert "engine validation" in summary
    assert "artifacts" in summary
    assert summary in outcome.log.read_text()


def test_analyze_uses_the_enrichment_store_next_to_the_board(tmp_path):
    board = tmp_path / "board.kicad_pcb"
    shutil.copy(FIXTURES / "scene_fixture.kicad_pcb", board)
    extra = tmp_path / "extra.toml"
    extra.write_text('[[component]]\nref = "U1"\nheight_mm = 9.0\n')

    # The cache the plugin uses lives next to the board: seed it there and the
    # run must pick it up without any --store argument.
    from dodplace.enrich import EnrichmentStore, PartFacts, Provenance

    artifacts = tmp_path / runner.ARTIFACT_DIRNAME
    store = EnrichmentStore(artifacts / "cache")
    record = PartFacts(identity="lcsc:C14663")
    record.set("mass_g", 0.004, Provenance("parts_file", "seeded.csv"))
    store.put("parts", "lcsc:C14663", record)

    outcome = runner.analyze(board, found_via="explicit")
    assert outcome.ok
    seeded = json.loads((artifacts / runner.ENRICH_REPORT_NAME).read_text())
    assert seeded["components"]["C1"]["mass_g"]["source"] == "parts_file"

    outcome = runner.analyze(board, found_via="explicit", extra_path=extra)
    report = json.loads(
        (tmp_path / runner.ARTIFACT_DIRNAME / runner.ENRICH_REPORT_NAME).read_text()
    )
    assert report["components"]["U1"]["height_mm"]["source"] == "extra_toml"
    assert report["components"]["U1"]["height_mm"]["value"] == pytest.approx(9.0)


def test_analyze_survives_a_missing_engine(tmp_path, monkeypatch):
    """No solver built is a note in the summary, not a failure."""
    monkeypatch.setattr(runner, "run_solver", lambda *a, **k: (_ for _ in ()).throw(
        FileNotFoundError("dodplace-solve was not found")
    ))
    board = tmp_path / "board.kicad_pcb"
    shutil.copy(FIXTURES / "scene_fixture.kicad_pcb", board)
    outcome = runner.analyze(board, found_via="explicit")
    assert outcome.ok
    assert any("engine validation skipped" in line for line in outcome.summary)


def test_find_board_prefers_an_explicit_path(tmp_path):
    board = tmp_path / "explicit.kicad_pcb"
    board.write_text("")
    found, how = runner.find_board(board)
    assert found == board and how == "explicit"

    with pytest.raises(runner.BoardNotFound):
        runner.find_board(tmp_path / "missing.kicad_pcb")


def test_find_board_falls_back_to_the_working_directory(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    with pytest.raises(runner.BoardNotFound) as excinfo:
        runner.find_board(None)
    assert "pass --board PATH" in str(excinfo.value)

    (tmp_path / "only.kicad_pcb").write_text("")
    found, how = runner.find_board(None)
    assert found.name == "only.kicad_pcb" and how == "cwd"

    (tmp_path / "second.kicad_pcb").write_text("")
    with pytest.raises(runner.BoardNotFound):
        runner.find_board(None)


def test_find_board_with_ipc_reports_the_reason(tmp_path, monkeypatch):
    """When IPC cannot help, the message must say why, not just "no board"."""

    def unavailable():
        raise runner.IpcUnavailable("KiCad is running but reports no open board")

    monkeypatch.chdir(tmp_path)
    monkeypatch.setattr(runner, "board_from_ipc", unavailable)
    with pytest.raises(runner.BoardNotFound) as excinfo:
        runner.find_board(None, use_ipc=True)
    assert "reports no open board" in str(excinfo.value)


def test_ipc_discovery_explains_itself_when_no_board_is_open():
    """No PCB editor open means KiCad answers "no handler" for GetOpenDocuments.

    That reason must reach the user: it is the difference between "kicad-python
    is missing", "KiCad is not running" and "open a board first".
    """
    try:
        path, how = runner.board_from_ipc()
    except runner.IpcUnavailable as exc:
        reason = str(exc)
        assert reason, "the reason must never be empty"
        # KiCad closed and "no board open" are different problems.
        assert "cannot connect to KiCad" in reason or "no open board" in reason
    else:
        assert how == "ipc"
        assert path.is_file()


def test_log_resolution_prefers_the_board_then_walks_up(tmp_path, monkeypatch):
    from dodplace.cli import _resolve_log_path

    board = tmp_path / "project" / "board.kicad_pcb"
    board.parent.mkdir()
    board.write_text("")
    log = board.parent / ".dodplace" / "plugin.log"
    log.parent.mkdir()
    log.write_text("hello\n")
    assert _resolve_log_path(str(board)) == log

    nested = board.parent / "deep" / "deeper"
    nested.mkdir(parents=True)
    monkeypatch.chdir(nested)
    assert _resolve_log_path(None) == log


@pytest.mark.skipif(find_solver() is None, reason="dodplace-solve is not built")
def test_analyze_output_is_accepted_by_the_engine(tmp_path):
    """The strongest check: the scene the plugin writes passes the C validator."""
    board = tmp_path / "board.kicad_pcb"
    shutil.copy(FIXTURES / "scene_fixture.kicad_pcb", board)
    outcome = runner.analyze(board, found_via="explicit")
    assert outcome.solver is not None
    assert outcome.solver.returncode == 0, outcome.solver.stderr
    assert "4 components" in outcome.solver.stdout


# ===========================================================================
# The requirements file KiCad insists on
# ===========================================================================


def test_requirements_file_exists_and_installs_nothing(repo_root, install_env, plugins_dir):
    """KiCad only marks a plugin ready when `pip install -r` returns 0.

    Without this file the dependency job fails and the action never appears, so
    it must exist - and it must stay empty of packages: the shim is stdlib-only
    and the real dependencies live in the project's uv environment.
    """
    installer.install_plugin(repo_root, **install_env)
    requirements = (plugins_dir / PLUGIN_DIRNAME / REQUIREMENTS_NAME).read_text()
    assert requirements.strip()
    packages = [
        line.strip()
        for line in requirements.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    assert packages == [], f"the plugin environment must not install anything: {packages}"


def test_payload_verification_catches_a_missing_requirements_file(repo_root):
    """The self-check that would have caught the bug the trace revealed."""
    manifest = build_manifest()
    shim = render_shim(repo_root, config=Path("/tmp/cfg"), fallback_log=Path("/tmp/log"))
    good = render_requirements()

    assert verify_payload(manifest, shim, good) == []
    assert verify_payload(manifest, shim, "") != []
    assert verify_payload(manifest, shim, "# only a comment\n") == []

    with_package = good + "kicad-python>=0.4\n"
    problems = verify_payload(manifest, shim, with_package)
    assert any("must install nothing" in problem for problem in problems)

    unsubstituted = shim.replace("@@REPO@@", "@@REPO@@")
    assert verify_payload(manifest, unsubstituted, good) == []

    # The bug the trace revealed: a button with no bitmap is invisible.
    from dodplace.plugin.install import render_icons

    icons = render_icons()
    assert verify_payload(manifest, shim, good, icons) == []

    no_icons = dict(manifest)
    no_icons["actions"] = [dict(manifest["actions"][0], **{"icons-light": []})]
    problems = verify_payload(no_icons, shim, good, icons)
    assert any("declares no icons-light" in problem for problem in problems)

    declared_but_missing = dict(manifest)
    problems = verify_payload(declared_but_missing, shim, good, {})
    assert any("declared but not shipped" in problem for problem in problems)


def test_install_refuses_an_unverifiable_payload(repo_root, install_env, plugins_dir, monkeypatch):
    """A payload that fails verification must not be written at all."""
    monkeypatch.setattr(
        installer,
        "verify_payload",
        lambda manifest, shim, requirements, icons=None: ["broken on purpose"],
    )
    with pytest.raises(InstallError) as excinfo:
        installer.install_plugin(repo_root, **install_env)
    assert "broken on purpose" in str(excinfo.value)
    assert not (plugins_dir / PLUGIN_DIRNAME).exists()


def test_status_lists_the_required_files(repo_root, install_env, plugins_dir):
    installer.install_plugin(repo_root, **install_env)
    info = installer.status(plugins_dir=plugins_dir)
    assert info["files"] == {name: True for name in REQUIRED_FILES}


def test_icons_are_valid_pngs_of_the_expected_sizes(repo_root):
    """KiCad loads them with wxBitmap::LoadFile; they must be real PNGs."""
    import struct

    from dodplace.plugin.install import render_icons

    icons = render_icons()
    assert len(icons) == 8
    for relative, data in icons.items():
        assert data.startswith(b"\x89PNG\r\n\x1a\n"), relative
        width, height = struct.unpack(">II", data[16:24])
        expected = 24 if "24" in relative else 48
        assert (width, height) == (expected, expected), relative


def test_installed_plugin_has_everything_kicad_reads(install_env, plugins_dir):
    """The end state KiCad's loader walks: manifest, entrypoint, icons, reqs."""
    installer.install_plugin(Path(__file__).resolve().parents[2], **install_env)
    plugin_dir = plugins_dir / PLUGIN_DIRNAME
    manifest = json.loads((plugin_dir / MANIFEST_NAME).read_text())
    action = manifest["actions"][0]

    entrypoint = plugin_dir / action["entrypoint"]
    assert entrypoint.is_file() and os.access(entrypoint, os.X_OK)
    for key in ("icons-light", "icons-dark"):
        for relative in action[key]:
            assert (plugin_dir / relative).is_file(), relative
    assert (plugin_dir / REQUIREMENTS_NAME).is_file()


# ===========================================================================
# The environment KiCad hands to an action
# ===========================================================================

#: What KiCad exports before running a plugin action: it describes *KiCad's*
#: bundled Python 3.9. Inherited by any other interpreter, it is fatal.
KICAD_POLLUTED_ENV = {
    "PYTHONHOME": "/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current",
    "PYTHONPATH": "/Applications/KiCad/KiCad.app/Contents/SharedSupport/scripting:"
                  "/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/"
                  "Versions/3.9/lib/python3.9/site-packages",
    "VIRTUAL_ENV": "/Users/someone/Library/Caches/KiCad/10.0/python-environments/org.dodplace.placer",
}


def test_shim_asks_the_child_to_ignore_python_environment_variables():
    """`-E` is what makes the child immune to KiCad's PYTHONHOME."""
    shim = render_shim(Path("/tmp/repo"), config=Path("/tmp/cfg"))
    assert '"-E"' in shim or "'-E'" in shim
    assert shim.index("-E") < shim.index('"-m"') if '"-m"' in shim else True


def test_shim_strips_inherited_python_variables_from_the_child(tmp_path):
    """The fake interpreter prints the variables it received: they must be gone.

    The shim is run by KiCad's own Python, exactly as KiCad does it: our 3.12
    cannot even start under KiCad's PYTHONHOME, which is the whole point.
    """
    interpreter = kicad_python()
    if interpreter is None:
        pytest.skip("no KiCad Python on this machine")
    fake_repo = tmp_path / "fakerepo"
    venv_bin = fake_repo / "python" / ".venv" / "bin"
    venv_bin.mkdir(parents=True)
    (fake_repo / "python" / "pyproject.toml").write_text("")
    fake_python = venv_bin / "python"
    fake_python.write_text(
        "#!/bin/sh\n"
        'echo "PYTHONHOME=[${PYTHONHOME}]"\n'
        'echo "PYTHONPATH=[${PYTHONPATH}]"\n'
        'echo "VIRTUAL_ENV=[${VIRTUAL_ENV}]"\n'
        'echo "REPO=[${DODPLACE_PLUGIN_REPO}]"\n'
    )
    fake_python.chmod(fake_python.stat().st_mode | stat.S_IEXEC)

    shim = tmp_path / "main.py"
    shim.write_text(render_shim(fake_repo, config=tmp_path / "cfg",
                                fallback_log=tmp_path / "shim.log"))
    probe = subprocess.run(
        [str(interpreter), str(shim)],
        capture_output=True, text=True,
        env={**os.environ, **KICAD_POLLUTED_ENV},
    )
    assert probe.returncode == 0, probe.stderr
    assert "PYTHONHOME=[]" in probe.stdout
    assert "PYTHONPATH=[]" in probe.stdout
    assert "VIRTUAL_ENV=[]" in probe.stdout
    assert f"REPO=[{fake_repo}]" in probe.stdout


@pytest.mark.skipif(not Path(__file__).resolve().parents[1].joinpath(".venv/bin/python").is_file(),
                    reason="the project venv is not synced")
def test_the_real_cli_starts_under_kicads_environment(tmp_path):
    """Regression: this used to die with 'No module named encodings'."""
    venv_python = Path(__file__).resolve().parents[1] / ".venv" / "bin" / "python"
    board = tmp_path / "board.kicad_pcb"
    shutil.copy(FIXTURES / "scene_fixture.kicad_pcb", board)

    probe = subprocess.run(
        [str(venv_python), "-E", "-m", "dodplace", "plugin", "analyze",
         "--board", str(board), "--no-solver"],
        capture_output=True, text=True,
        env={**os.environ, **KICAD_POLLUTED_ENV},
    )
    assert probe.returncode == 0, probe.stderr
    assert "4 components" in probe.stdout

    # Without -E the very same call must fail, which is the bug being guarded.
    broken = subprocess.run(
        [str(venv_python), "-m", "dodplace", "--version"],
        capture_output=True, text=True,
        env={**os.environ, **KICAD_POLLUTED_ENV},
    )
    assert broken.returncode != 0
    assert "encodings" in broken.stderr


# ===========================================================================
# Polish: the report-only action and the unresolved summary
# ===========================================================================


def test_report_only_writes_no_artifact(tmp_path):
    board = tmp_path / "board.kicad_pcb"
    shutil.copy(FIXTURES / "scene_fixture.kicad_pcb", board)

    outcome = runner.analyze(board, found_via="explicit", write_artifacts=False)
    assert outcome.ok
    artifacts = tmp_path / runner.ARTIFACT_DIRNAME
    assert sorted(path.name for path in artifacts.iterdir()) == [runner.LOG_NAME]
    assert any("report only" in line for line in outcome.summary)
    # The engine still validated the scene, built in a temporary directory.
    assert outcome.solver is None or outcome.solver.returncode == 0


def test_full_run_still_writes_everything(tmp_path):
    board = tmp_path / "board.kicad_pcb"
    shutil.copy(FIXTURES / "scene_fixture.kicad_pcb", board)
    runner.analyze(board, found_via="explicit", write_artifacts=True)
    artifacts = tmp_path / runner.ARTIFACT_DIRNAME
    assert (artifacts / runner.SCENE_NAME).is_file()
    assert (artifacts / runner.REPORT_NAME).is_file()


def test_summary_lists_the_unresolved_fields(tmp_path):
    """The panel should say what is missing, not only which bits are degraded."""
    board = tmp_path / "board.kicad_pcb"
    shutil.copy(FIXTURES / "scene_fixture.kicad_pcb", board)
    outcome = runner.analyze(board, found_via="explicit", write_artifacts=False)
    unresolved = [line for line in outcome.summary if "unresolved:" in line]
    assert unresolved, outcome.summary
    line = unresolved[0]
    # Height is resolved from the STEP models, mass and the rest are not.
    assert "mass_g 4/4" in line
    assert "orientation_deg 4/4" in line
    assert "height_mm" not in line


def test_shim_forwards_only_the_arguments_it_knows(tmp_path):
    """KiCad appends a manifest action's args; anything else must be dropped."""
    fake_repo = tmp_path / "fakerepo"
    venv_bin = fake_repo / "python" / ".venv" / "bin"
    venv_bin.mkdir(parents=True)
    (fake_repo / "python" / "pyproject.toml").write_text("")
    fake_python = venv_bin / "python"
    fake_python.write_text('#!/bin/sh\necho "ARGS $@"\n')
    fake_python.chmod(fake_python.stat().st_mode | stat.S_IEXEC)

    shim = tmp_path / "main.py"
    shim.write_text(render_shim(fake_repo, config=tmp_path / "cfg",
                                fallback_log=tmp_path / "shim.log"))
    probe = subprocess.run(
        [sys.executable, str(shim), "--report-only", "--kicad-internal-thing"],
        capture_output=True, text=True,
    )
    assert probe.returncode == 0, probe.stderr
    assert "--report-only" in probe.stdout
    assert "--kicad-internal-thing" not in probe.stdout
    assert "ignoring unexpected argument" in (tmp_path / "shim.log").read_text()
