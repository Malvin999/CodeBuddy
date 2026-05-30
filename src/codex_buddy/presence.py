from __future__ import annotations

import re
import subprocess
import time
from dataclasses import dataclass
from datetime import datetime, time as datetime_time
from typing import Callable, Optional


_HID_IDLE_RE = re.compile(r'"HIDIdleTime"\s*=\s*(\d+)')


def macos_hid_idle_seconds() -> Optional[int]:
    try:
        result = subprocess.run(
            ["ioreg", "-c", "IOHIDSystem"],
            capture_output=True,
            check=False,
            text=True,
            timeout=1.0,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if result.returncode != 0:
        return None
    match = _HID_IDLE_RE.search(result.stdout)
    if match is None:
        return None
    return int(int(match.group(1)) / 1_000_000_000)


@dataclass(frozen=True)
class PresenceConfig:
    idle_threshold_seconds: int = 10 * 60
    work_start: datetime_time = datetime_time(9, 30)
    work_end: datetime_time = datetime_time(18, 30)
    workdays: tuple[int, ...] = (0, 1, 2, 3, 4)
    poll_interval_seconds: float = 10.0


class PresenceMonitor:
    def __init__(
        self,
        *,
        clock: Callable[[], float] = time.time,
        idle_reader: Callable[[], Optional[int]] = macos_hid_idle_seconds,
        config: PresenceConfig = PresenceConfig(),
    ) -> None:
        self.clock = clock
        self.idle_reader = idle_reader
        self.config = config
        self._last_poll_at = 0.0
        self._cached: Optional[dict[str, object]] = None

    def snapshot(self) -> dict[str, object]:
        now = self.clock()
        if (
            self._cached is not None
            and now - self._last_poll_at < self.config.poll_interval_seconds
        ):
            return dict(self._cached)

        idle_sec = self.idle_reader()
        self._last_poll_at = now
        work = self._in_work_period(now)
        if idle_sec is None:
            payload: dict[str, object] = {
                "state": "unknown",
                "idle_sec": 0,
                "work": work,
            }
        else:
            idle_sec = max(0, int(idle_sec))
            if idle_sec >= self.config.idle_threshold_seconds:
                state = "idle" if work else "away"
            else:
                state = "working" if work else "off"
            payload = {
                "state": state,
                "idle_sec": idle_sec,
                "work": work,
            }
        self._cached = payload
        return dict(payload)

    def _in_work_period(self, timestamp: float) -> bool:
        current = datetime.fromtimestamp(timestamp).astimezone()
        if current.weekday() not in self.config.workdays:
            return False
        current_time = current.time()
        return self.config.work_start <= current_time < self.config.work_end
