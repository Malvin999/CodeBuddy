from datetime import datetime

from codex_buddy.presence import PresenceMonitor


def _local_ts(year: int, month: int, day: int, hour: int, minute: int = 0) -> float:
    return datetime(year, month, day, hour, minute).astimezone().timestamp()


def test_presence_uses_user_idle_rules_during_work_period():
    now = _local_ts(2026, 5, 29, 10, 0)
    monitor = PresenceMonitor(clock=lambda: now, idle_reader=lambda: 10 * 60)

    assert monitor.snapshot()["state"] == "idle"


def test_presence_uses_user_idle_rules_outside_work_period():
    now = _local_ts(2026, 5, 30, 10, 0)
    monitor = PresenceMonitor(clock=lambda: now, idle_reader=lambda: 10 * 60)

    assert monitor.snapshot()["state"] == "away"


def test_presence_reports_working_or_off_below_idle_threshold():
    work_now = _local_ts(2026, 5, 29, 10, 0)
    off_now = _local_ts(2026, 5, 30, 10, 0)

    assert PresenceMonitor(clock=lambda: work_now, idle_reader=lambda: 599).snapshot()["state"] == "working"
    assert PresenceMonitor(clock=lambda: off_now, idle_reader=lambda: 599).snapshot()["state"] == "off"
