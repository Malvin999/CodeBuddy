from __future__ import annotations

import os
import plistlib
import re
import subprocess
from pathlib import Path
from typing import Optional


def launchd_label() -> str:
    return "com.codebuddy.agent"


def launchd_plist_path() -> Path:
    return Path.home() / "Library" / "LaunchAgents" / f"{launchd_label()}.plist"


def render_launchd_plist(
    *,
    python_executable: str,
    state_path: Path,
    repo_root: Path,
    log_dir: Path,
) -> str:
    label = launchd_label()
    environment: dict[str, str] = {}
    local_src = repo_root / "src"
    if (local_src / "codex_buddy").exists():
        environment["PYTHONPATH"] = str(local_src)
    payload = {
        "Label": label,
        "ProgramArguments": [
            python_executable,
            "-m",
            "codex_buddy",
            "--state-path",
            str(state_path),
            "agent",
        ],
        "WorkingDirectory": str(repo_root),
        "RunAtLoad": True,
        "KeepAlive": True,
        "StandardOutPath": str(log_dir / f"{label}.stdout.log"),
        "StandardErrorPath": str(log_dir / f"{label}.stderr.log"),
    }
    if environment:
        payload["EnvironmentVariables"] = environment
    return plistlib.dumps(payload, fmt=plistlib.FMT_XML, sort_keys=False).decode("utf-8")


def install_launchd_service(
    plist_path: Path,
    plist_text: str,
    *,
    launchctl_bin: str = "launchctl",
) -> None:
    plist_path.parent.mkdir(parents=True, exist_ok=True)
    plist_path.write_text(plist_text, encoding="utf-8")

    subprocess.run(
        [launchctl_bin, "bootout", _launchd_domain(), str(plist_path)],
        check=False,
        capture_output=True,
        text=True,
    )
    subprocess.run(
        [launchctl_bin, "bootstrap", _launchd_domain(), str(plist_path)],
        check=True,
        capture_output=True,
        text=True,
    )


def uninstall_launchd_service(
    plist_path: Path,
    *,
    launchctl_bin: str = "launchctl",
) -> None:
    if not plist_path.exists():
        return

    subprocess.run(
        [launchctl_bin, "bootout", _launchd_domain(), str(plist_path)],
        check=False,
        capture_output=True,
        text=True,
    )
    plist_path.unlink()


def launchd_service_status(
    label: str,
    *,
    launchctl_bin: str = "launchctl",
) -> dict:
    completed = subprocess.run(
        [launchctl_bin, "list", label],
        capture_output=True,
        text=True,
    )
    raw_output = (completed.stdout or completed.stderr).strip()
    pid = _parse_launchctl_int(raw_output, "PID") if completed.returncode == 0 else None
    last_exit_status = _parse_launchctl_int(raw_output, "LastExitStatus") if completed.returncode == 0 else None
    parsed_label = _parse_launchctl_string(raw_output, "Label") if completed.returncode == 0 else None
    loaded = completed.returncode == 0
    if not raw_output:
        printed = subprocess.run(
            [launchctl_bin, "print", f"{_launchd_domain()}/{label}"],
            capture_output=True,
            text=True,
        )
        raw_output = (printed.stdout or printed.stderr).strip()
        if printed.returncode == 0:
            loaded = True
            pid = _parse_print_pid(raw_output)
            last_exit_status = _parse_print_last_exit_code(raw_output)
            parsed_label = label

    return {
        "label": parsed_label or label,
        "loaded": loaded,
        "running": pid is not None and pid > 0,
        "pid": pid,
        "last_exit_status": last_exit_status,
        "returncode": completed.returncode,
        "raw": raw_output,
    }


def _launchd_domain() -> str:
    return f"gui/{os.getuid()}"


def _parse_launchctl_int(output: str, key: str) -> Optional[int]:
    match = re.search(rf'"{re.escape(key)}"\s*=\s*(-?\d+);', output)
    if match is None:
        return None
    return int(match.group(1))


def _parse_launchctl_string(output: str, key: str) -> Optional[str]:
    match = re.search(rf'"{re.escape(key)}"\s*=\s*"([^"]+)";', output)
    if match is None:
        return None
    return match.group(1)


def _parse_print_pid(output: str) -> Optional[int]:
    match = re.search(r"\bpid\s*=\s*(\d+)", output)
    return int(match.group(1)) if match else None


def _parse_print_last_exit_code(output: str) -> Optional[int]:
    match = re.search(r"last exit code\s*=\s*(-?\d+)", output)
    return int(match.group(1)) if match else None
