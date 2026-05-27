from __future__ import annotations

import json
import os
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Optional

_CODEX_USAGE_URL = "https://chatgpt.com/backend-api/wham/usage"


@dataclass(frozen=True)
class CodexUsageWindow:
    label: str
    used_percent: float
    reset_at: Optional[float]


@dataclass(frozen=True)
class CodexUsageSnapshot:
    primary: Optional[CodexUsageWindow]
    secondary: Optional[CodexUsageWindow]
    fetched_at: float

    def as_ble_payload(self, *, now: Optional[float] = None) -> Optional[dict[str, object]]:
        if self.primary is None or self.secondary is None:
            return None
        now = time.time() if now is None else now
        return {
            "live": True,
            "short_pct": _round_percent(100 - self.primary.used_percent),
            "short_window": self.primary.label,
            "short_reset": _format_reset(self.primary.reset_at, now=now),
            "long_pct": _round_percent(100 - self.secondary.used_percent),
            "long_window": self.secondary.label,
            "long_reset": _format_reset(self.secondary.reset_at, now=now),
        }


class CodexUsageClient:
    def __init__(
        self,
        *,
        auth_path: Optional[Path] = None,
        timeout: float = 10.0,
        opener: Optional[Any] = None,
    ) -> None:
        self.auth_path = auth_path or _default_auth_path()
        self.timeout = timeout
        self.opener = opener or urllib.request.urlopen

    def fetch(self) -> Optional[CodexUsageSnapshot]:
        credentials = _load_credentials(self.auth_path)
        if credentials is None:
            return None
        request = urllib.request.Request(
            _CODEX_USAGE_URL,
            headers={
                "Authorization": "Bearer {}".format(credentials["access_token"]),
                "Accept": "application/json",
                "User-Agent": "code-buddy",
            },
        )
        account_id = credentials.get("account_id")
        if account_id:
            request.add_header("ChatGPT-Account-Id", account_id)

        try:
            with self.opener(request, timeout=self.timeout) as response:
                body = response.read(1024 * 1024)
        except (OSError, urllib.error.URLError, urllib.error.HTTPError, TimeoutError):
            return None

        try:
            payload = json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return None

        rate_limit = payload.get("rate_limit")
        if not isinstance(rate_limit, dict):
            return None
        primary = _parse_window(rate_limit.get("primary_window"), fallback_label="5h")
        secondary = _parse_window(
            rate_limit.get("secondary_window"),
            fallback_label=_secondary_label(rate_limit.get("secondary_window"), rate_limit.get("primary_window")),
        )
        if primary is None and secondary is None:
            return None
        return CodexUsageSnapshot(primary=primary, secondary=secondary, fetched_at=time.time())


def _default_auth_path() -> Path:
    root = os.environ.get("CODEX_HOME")
    if root:
        return Path(root).expanduser() / "auth.json"
    return Path.home() / ".codex" / "auth.json"


def _load_credentials(path: Path) -> Optional[dict[str, str]]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    tokens = data.get("tokens")
    if not isinstance(tokens, dict):
        return None
    access_token = tokens.get("access_token")
    if not isinstance(access_token, str) or not access_token:
        return None
    account_id = tokens.get("account_id")
    return {
        "access_token": access_token,
        "account_id": account_id if isinstance(account_id, str) else "",
    }


def _parse_window(value: object, *, fallback_label: str) -> Optional[CodexUsageWindow]:
    if not isinstance(value, dict):
        return None
    used = value.get("used_percent")
    if not isinstance(used, (int, float)):
        return None
    seconds = value.get("limit_window_seconds")
    label = _primary_label(seconds) if fallback_label == "5h" else fallback_label
    reset_at = value.get("reset_at")
    return CodexUsageWindow(
        label=label,
        used_percent=_clamp_percent(float(used)),
        reset_at=float(reset_at) if isinstance(reset_at, (int, float)) and reset_at > 0 else None,
    )


def _primary_label(seconds: object) -> str:
    if not isinstance(seconds, int) or seconds <= 0:
        return "5h"
    hours = max(1, seconds // 3600)
    return "{}h".format(hours)


def _secondary_label(secondary: object, primary: object) -> str:
    if not isinstance(secondary, dict):
        return "7d"
    seconds = secondary.get("limit_window_seconds")
    if isinstance(seconds, int):
        hours = seconds // 3600
        if hours >= 24 * 7:
            return "7d"
        if hours == 24:
            return "1d"
        if hours > 0:
            return "{}h".format(hours)
    secondary_reset = secondary.get("reset_at")
    primary_reset = primary.get("reset_at") if isinstance(primary, dict) else None
    if (
        isinstance(secondary_reset, (int, float))
        and isinstance(primary_reset, (int, float))
        and secondary_reset - primary_reset >= 3 * 24 * 60 * 60
    ):
        return "7d"
    return "7d"


def _format_reset(reset_at: Optional[float], *, now: float) -> str:
    if reset_at is None:
        return "-"
    seconds = max(0, int(reset_at - now))
    days = seconds // 86400
    hours = (seconds % 86400) // 3600
    minutes = (seconds % 3600) // 60
    if days > 0:
        return "{}d {}h".format(days, hours)
    if hours > 0:
        return "{}h {}m".format(hours, minutes)
    return "{}m".format(minutes)


def _round_percent(value: float) -> int:
    return int(round(_clamp_percent(value)))


def _clamp_percent(value: float) -> float:
    if value < 0:
        return 0
    if value > 100:
        return 100
    return value
