"""KiCad plugin integration: the analyze action, the shim and the installer."""

from .install import (
    REQUIRED_FILES,
    InstallError,
    InstallReport,
    build_manifest,
    default_plugins_dir,
    install_plugin,
    render_requirements,
    status,
    uninstall_plugin,
    validate_manifest,
    verify_payload,
)
from .runner import AnalyzeOutcome, BoardNotFound, analyze, find_board

__all__ = [
    "REQUIRED_FILES",
    "AnalyzeOutcome",
    "BoardNotFound",
    "InstallError",
    "InstallReport",
    "analyze",
    "build_manifest",
    "default_plugins_dir",
    "find_board",
    "install_plugin",
    "render_requirements",
    "status",
    "uninstall_plugin",
    "validate_manifest",
    "verify_payload",
]
