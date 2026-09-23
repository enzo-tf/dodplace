"""Install the KiCad action: manifest, shim and the repo pointer.

Nothing here touches the user's board. The only writes are the plugin directory
inside KiCad's user data folder and, unless disabled, a one-line pointer file
that lets the shim survive the checkout being moved.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
from dataclasses import dataclass, field
from importlib.resources import files
from pathlib import Path

PLUGIN_ID = "org.dodplace.placer"
ACTION_ID = "analyze"
ACTION_NAME = "dodplace: analyze board"
REPORT_ACTION_ID = "report"
REPORT_ACTION_NAME = "dodplace: report only"
PLUGIN_DIRNAME = "dodplace"
SHIM_NAME = "main.py"
MANIFEST_NAME = "plugin.json"
#: KiCad's plugin manager runs `pip install -r requirements.txt` in the plugin
#: directory and only marks the plugin ready when that succeeds. The file must
#: exist even when it installs nothing (see requirements.txt.src).
REQUIREMENTS_NAME = "requirements.txt"

#: Toolbar icons, relative to the plugin directory. KiCad draws a plugin button
#: only from these bitmaps: with show-button and no icon the button is blank.
ICON_DIRNAME = "icons"
ICONS_LIGHT = (
    f"{ICON_DIRNAME}/dodplace-light-24.png",
    f"{ICON_DIRNAME}/dodplace-light-48.png",
)
ICONS_DARK = (
    f"{ICON_DIRNAME}/dodplace-dark-24.png",
    f"{ICON_DIRNAME}/dodplace-dark-48.png",
)
#: A distinct bitmap for the read-only report action, so the two toolbar
#: buttons are told apart at a glance.
REPORT_ICONS_LIGHT = (
    f"{ICON_DIRNAME}/dodplace-report-light-24.png",
    f"{ICON_DIRNAME}/dodplace-report-light-48.png",
)
REPORT_ICONS_DARK = (
    f"{ICON_DIRNAME}/dodplace-report-dark-24.png",
    f"{ICON_DIRNAME}/dodplace-report-dark-48.png",
)

#: Everything KiCad expects to find in the installed plugin directory.
ALL_ICONS = (*ICONS_LIGHT, *ICONS_DARK, *REPORT_ICONS_LIGHT, *REPORT_ICONS_DARK)
REQUIRED_FILES = (MANIFEST_NAME, SHIM_NAME, REQUIREMENTS_NAME, *ALL_ICONS)

#: Identifier rules copied from https://go.kicad.org/api/schemas/v1
PLUGIN_ID_PATTERN = re.compile(r"^[a-zA-Z][-_a-zA-Z0-9.]{0,98}[a-zA-Z0-9]$")
ACTION_ID_PATTERN = re.compile(r"^[a-zA-Z][-_a-zA-Z0-9.]{0,48}[a-zA-Z0-9]$")
ALLOWED_SCOPES = ("pcb", "schematic", "footprint", "symbol", "project_manager",
                  "footprint_wizard")


class InstallError(RuntimeError):
    """The installation cannot proceed."""


@dataclass
class InstallReport:
    plugins_dir: Path
    plugin_dir: Path
    config_file: Path | None = None
    wrote: list[Path] = field(default_factory=list)
    problems: list[str] = field(default_factory=list)

    def summary(self) -> str:
        lines = [f"plugin installed in {self.plugin_dir}"]
        for path in self.wrote:
            lines.append(f"  wrote {path}")
        if self.config_file is not None:
            lines.append(f"  repo pointer {self.config_file}")
        lines.append(
            "  KiCad runs `pip install -r requirements.txt` on startup and only "
            "then registers the action"
        )
        lines.append("  restart KiCad to pick the action up")
        return "\n".join(lines)


# ---------------------------------------------------------------------------
# Locations
# ---------------------------------------------------------------------------


def default_plugins_dir() -> Path:
    """KiCad's user plugin directory, following the documented layout.

    ``${KICAD_DOCUMENTS_HOME}/<version>/plugins`` on every platform; the
    highest installed version wins.
    """
    home = os.environ.get("KICAD_DOCUMENTS_HOME")
    base = Path(home) if home else Path.home() / "Documents" / "KiCad"
    if base.is_dir():
        versions = [
            entry for entry in base.iterdir() if entry.is_dir() and entry.name[:1].isdigit()
        ]
        versions.sort(key=lambda path: [int(part) for part in path.name.split(".") if part.isdigit()])
        if versions:
            return versions[-1] / "plugins"
    return base / "10.0" / "plugins"


def default_config_path() -> Path:
    xdg = os.environ.get("XDG_CONFIG_HOME")
    base = Path(xdg) if xdg else Path.home() / ".config"
    return base / "dodplace" / "repo"


def default_fallback_log() -> Path:
    xdg = os.environ.get("XDG_CACHE_HOME")
    base = Path(xdg) if xdg else Path.home() / ".cache"
    return base / "dodplace" / "shim.log"


def find_uv() -> Path | None:
    found = shutil.which("uv")
    if found:
        return Path(found)
    for candidate in (Path("/opt/homebrew/bin/uv"), Path("/usr/local/bin/uv"),
                      Path.home() / ".local/bin/uv", Path.home() / ".cargo/bin/uv"):
        if candidate.is_file():
            return candidate
    return None


def venv_python(repo: Path) -> Path:
    return repo / "python" / ".venv" / "bin" / "python"


# ---------------------------------------------------------------------------
# Manifest
# ---------------------------------------------------------------------------


def build_manifest() -> dict:
    return {
        "$schema": "https://go.kicad.org/api/schemas/v1",
        "identifier": PLUGIN_ID,
        "name": "dodplace",
        "description": (
            "Data-oriented automatic PCB placement. The analyze action reads the "
            "board, resolves optional part data and validates the scene without "
            "modifying anything."
        ),
        "runtime": {"type": "python", "min_version": "3.9"},
        "actions": [
            {
                "identifier": ACTION_ID,
                "name": ACTION_NAME,
                "description": (
                    "Extract, enrich and validate the board; writes a scene and a "
                    "report next to it and shows the summary in the message panel"
                ),
                "scopes": ["pcb"],
                "show-button": True,
                "entrypoint": SHIM_NAME,
                "icons-light": list(ICONS_LIGHT),
                "icons-dark": list(ICONS_DARK),
            },
            {
                "identifier": REPORT_ACTION_ID,
                "name": REPORT_ACTION_NAME,
                "description": (
                    "Same analysis without writing any artifact: just the report in "
                    "the message panel and in the plugin log"
                ),
                "scopes": ["pcb"],
                "show-button": True,
                "entrypoint": SHIM_NAME,
                # KiCad appends these to the command line, so one shim serves
                # both actions.
                "args": ["--report-only"],
                "icons-light": list(REPORT_ICONS_LIGHT),
                "icons-dark": list(REPORT_ICONS_DARK),
            },
        ],
    }


def validate_manifest(manifest: dict) -> list[str]:
    """Mirror of the official schema's constraints; returns problem strings."""
    problems: list[str] = []
    for key in ("identifier", "name", "description", "runtime", "actions"):
        if key not in manifest:
            problems.append(f"manifest is missing '{key}'")
    if problems:
        return problems

    if not PLUGIN_ID_PATTERN.match(str(manifest["identifier"])):
        problems.append(f"identifier {manifest['identifier']!r} violates the schema pattern")
    runtime = manifest["runtime"]
    if runtime.get("type") not in ("python", "exec"):
        problems.append("runtime.type must be 'python' or 'exec'")

    actions = manifest["actions"]
    if not isinstance(actions, list) or not actions:
        problems.append("at least one action is required")
        return problems
    for action in actions:
        for key in ("identifier", "name", "description", "entrypoint"):
            if not action.get(key):
                problems.append(f"action is missing '{key}'")
        if not ACTION_ID_PATTERN.match(str(action.get("identifier", ""))):
            problems.append(f"action identifier {action.get('identifier')!r} violates the schema")
        for scope in action.get("scopes", []):
            if scope not in ALLOWED_SCOPES:
                problems.append(f"unknown scope {scope!r}")
    return problems


# ---------------------------------------------------------------------------
# Shim
# ---------------------------------------------------------------------------


def render_shim(repo: Path, *, config: Path, fallback_log: Path | None = None) -> str:
    fallback_log = fallback_log if fallback_log is not None else default_fallback_log()
    template = files("dodplace.plugin").joinpath("main.py.src").read_text()
    uv = find_uv()
    replacements = {
        "@@REPO@@": str(repo.resolve()),
        "@@PYTHON@@": str(venv_python(repo).resolve()),
        "@@UV@@": str(uv) if uv else "",
        "@@CONFIG@@": str(config),
        "@@FALLBACK_LOG@@": str(fallback_log),
    }
    shim = template
    for token, value in replacements.items():
        shim = shim.replace(token, value)
    return shim


def render_requirements() -> str:
    """The (empty of packages) requirements file KiCad insists on finding."""
    return files("dodplace.plugin").joinpath("requirements.txt.src").read_text()


def render_icons() -> dict[str, bytes]:
    """The toolbar bitmaps, keyed by their path inside the plugin directory."""
    package = files("dodplace.plugin").joinpath(ICON_DIRNAME)
    return {name: package.joinpath(Path(name).name).read_bytes() for name in ALL_ICONS}


def verify_payload(
    manifest: dict, shim: str, requirements: str, icons: dict[str, bytes] | None = None
) -> list[str]:
    """Self-check of what we are about to write.

    Mirrors what KiCad needs, so a missing or malformed file is caught here
    instead of showing up as an action that never appears.
    """
    problems = validate_manifest(manifest)
    if "@@" in shim:
        problems.append("the shim still contains unsubstituted template tokens")
    if not shim.lstrip().startswith("#!"):
        problems.append("the shim needs a shebang line")
    if not requirements.strip():
        problems.append("the requirements file must not be empty")
    # The plugin environment must not need the network: everything installed
    # there has to be a comment.
    packages = [
        line.strip()
        for line in requirements.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    if packages:
        problems.append(
            "the requirements file must install nothing, found: " + ", ".join(packages[:3])
        )

    # A toolbar button needs a bitmap: KiCad renders nothing without one, and
    # the action silently never appears.
    for action in manifest.get("actions", []):
        if not action.get("show-button"):
            continue
        for key in ("icons-light", "icons-dark"):
            declared = action.get(key, [])
            if key == "icons-light" and not declared:
                problems.append(
                    f"action {action.get('identifier')!r} shows a button but declares no {key}"
                )
            for relative in declared:
                if icons is not None and relative not in icons:
                    problems.append(f"icon {relative!r} is declared but not shipped")
    if icons is not None:
        for relative, data in icons.items():
            if not data.startswith(b"\x89PNG"):
                problems.append(f"icon {relative!r} is not a PNG file")
    return problems


# ---------------------------------------------------------------------------
# Install / uninstall / status
# ---------------------------------------------------------------------------


def _check_repo(repo: Path) -> list[str]:
    problems: list[str] = []
    if not (repo / "python" / "pyproject.toml").is_file():
        problems.append(f"{repo} does not look like the dodplace checkout")
        return problems
    python = venv_python(repo)
    if not python.is_file():
        problems.append(f"{python} is missing; run `uv sync` in {repo / 'python'}")
        return problems
    try:
        probe = subprocess.run(
            [str(python), "-m", "dodplace", "--version"],
            capture_output=True, text=True, timeout=60,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        problems.append(f"cannot run the dodplace module: {exc}")
        return problems
    if probe.returncode != 0:
        problems.append(
            f"`python -m dodplace --version` failed: {probe.stderr.strip() or probe.stdout.strip()}"
        )
    return problems


def install_plugin(
    repo: Path | str,
    plugins_dir: Path | str | None = None,
    *,
    config_file: Path | str | None = None,
    write_config: bool = True,
    fallback_log: Path | str | None = None,
) -> InstallReport:
    """Write the manifest and the shim; refuse rather than half-install."""
    repo = Path(repo).resolve()
    plugins_dir = Path(plugins_dir) if plugins_dir else default_plugins_dir()
    config_path = Path(config_file) if config_file else default_config_path()

    report = InstallReport(plugins_dir=plugins_dir, plugin_dir=plugins_dir / PLUGIN_DIRNAME)
    report.problems = _check_repo(repo)

    # Build everything first and verify it in memory: a failure must not leave a
    # half-written plugin behind.
    manifest = build_manifest()
    shim = render_shim(
        repo,
        config=config_path,
        fallback_log=Path(fallback_log) if fallback_log else None,
    )
    requirements = render_requirements()
    icons = render_icons()
    report.problems += verify_payload(manifest, shim, requirements, icons)
    if report.problems:
        raise InstallError("; ".join(report.problems))

    report.plugin_dir.mkdir(parents=True, exist_ok=True)

    manifest_path = report.plugin_dir / MANIFEST_NAME
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    report.wrote.append(manifest_path)

    shim_path = report.plugin_dir / SHIM_NAME
    shim_path.write_text(shim)
    shim_path.chmod(0o755)
    report.wrote.append(shim_path)

    requirements_path = report.plugin_dir / REQUIREMENTS_NAME
    requirements_path.write_text(requirements)
    report.wrote.append(requirements_path)

    for relative, data in icons.items():
        icon_path = report.plugin_dir / relative
        icon_path.parent.mkdir(parents=True, exist_ok=True)
        icon_path.write_bytes(data)
        report.wrote.append(icon_path)

    if write_config:
        config_path.parent.mkdir(parents=True, exist_ok=True)
        config_path.write_text(
            "# dodplace checkout used by the KiCad plugin shim.\n"
            "# One absolute path per line; edit or delete freely.\n"
            f"{repo}\n"
        )
        report.config_file = config_path
    return report


def uninstall_plugin(plugins_dir: Path | str | None = None) -> Path | None:
    """Remove only our own plugin directory; returns it when something went."""
    plugins_dir = Path(plugins_dir) if plugins_dir else default_plugins_dir()
    plugin_dir = plugins_dir / PLUGIN_DIRNAME
    if not plugin_dir.exists():
        return None
    shutil.rmtree(plugin_dir)
    return plugin_dir


def status(repo: Path | str | None = None, plugins_dir: Path | str | None = None) -> dict:
    """Everything worth knowing when the action does not appear."""
    plugins_dir = Path(plugins_dir) if plugins_dir else default_plugins_dir()
    plugin_dir = plugins_dir / PLUGIN_DIRNAME
    info: dict = {
        "plugins_dir": str(plugins_dir),
        "plugin_dir": str(plugin_dir),
        "installed": (plugin_dir / MANIFEST_NAME).is_file(),
        "files": {name: (plugin_dir / name).is_file() for name in REQUIRED_FILES},
        "uv": str(find_uv()) if find_uv() else None,
    }

    if repo is not None:
        repo = Path(repo).resolve()
        info["repo"] = str(repo)
        info["venv_python"] = str(venv_python(repo))
        info["repo_problems"] = _check_repo(repo)
    elif (plugin_dir / SHIM_NAME).is_file():
        baked = re.search(r'^REPO = "(.*)"$', (plugin_dir / SHIM_NAME).read_text(), re.M)
        info["repo"] = baked.group(1) if baked else None
        if info["repo"]:
            info["repo_problems"] = _check_repo(Path(info["repo"]))
    config = default_config_path()
    info["config"] = str(config)
    info["config_repo"] = config.read_text().strip().splitlines()[-1] if config.is_file() else None
    return info
