from typing import List, Optional

from codex_buddy.catalog import SessionCatalog, SessionPrompt, SessionRecord


def _session(
    session_id: str,
    *,
    state: str,
    control_capability: str,
    last_activity_at: float,
    latest_message: str,
    entries: Optional[List[str]] = None,
    cwd: str = "/tmp/project",
    source: str = "cli",
    originator: str = "codex-tui",
    tokens_total: int = 0,
    tokens_session: int = 0,
    pending_prompt: Optional[SessionPrompt] = None,
    title: str = "",
) -> SessionRecord:
    return SessionRecord(
        session_id=session_id,
        source=source,
        originator=originator,
        cwd=cwd,
        state=state,
        last_activity_at=last_activity_at,
        latest_message=latest_message,
        entries=entries or ([latest_message] if latest_message else []),
        tokens_total=tokens_total,
        tokens_session=tokens_session,
        control_capability=control_capability,
        pending_prompt=pending_prompt,
        title=title,
    )


def test_single_managed_prompt_is_exported_while_readonly_running_session_still_counts():
    catalog = SessionCatalog(active_window_seconds=300, completed_window_seconds=120)
    catalog.upsert(
        _session(
            "managed-1",
            state="waiting",
            control_capability="managed",
            last_activity_at=100.0,
            latest_message="approve: rm -rf /tmp/foo",
            tokens_total=50,
            tokens_session=12,
            pending_prompt=SessionPrompt(request_id="req-1", tool="Bash", hint="rm -rf /tmp/foo"),
        )
    )
    catalog.upsert(
        _session(
            "readonly-1",
            state="running",
            control_capability="readonly",
            last_activity_at=95.0,
            latest_message="reading file...",
            tokens_total=25,
            tokens_session=8,
        )
    )

    snapshot = catalog.snapshot(now=120.0)

    assert snapshot.total == 2
    assert snapshot.running == 1
    assert snapshot.waiting == 1
    assert snapshot.msg == "approve: rm -rf /tmp/foo"
    assert snapshot.entries == ["approve: rm -rf /tmp/foo"]
    assert snapshot.tokens == 75
    assert snapshot.tokens_today == 20
    assert snapshot.tokens_active == 20
    assert snapshot.prompt == {
        "id": "req-1",
        "tool": "Bash",
        "hint": "rm -rf /tmp/foo",
    }


def test_multiple_managed_prompts_hide_prompt_and_force_host_disambiguation():
    catalog = SessionCatalog(active_window_seconds=300, completed_window_seconds=120)
    catalog.upsert(
        _session(
            "managed-1",
            state="waiting",
            control_capability="managed",
            last_activity_at=100.0,
            latest_message="approve: rm -rf /tmp/foo",
            pending_prompt=SessionPrompt(request_id="req-1", tool="Bash", hint="rm -rf /tmp/foo"),
        )
    )
    catalog.upsert(
        _session(
            "managed-2",
            state="waiting",
            control_capability="managed",
            last_activity_at=101.0,
            latest_message="approve: git push",
            pending_prompt=SessionPrompt(request_id="req-2", tool="Bash", hint="git push"),
        )
    )

    snapshot = catalog.snapshot(now=120.0)

    assert snapshot.total == 2
    assert snapshot.running == 0
    assert snapshot.waiting == 2
    assert snapshot.msg == "2 approvals waiting; open on host"
    assert snapshot.prompt is None


def test_readonly_waiting_session_never_exports_interactive_prompt():
    catalog = SessionCatalog(active_window_seconds=300, completed_window_seconds=120)
    catalog.upsert(
        _session(
            "readonly-1",
            state="waiting",
            control_capability="readonly",
            last_activity_at=100.0,
            latest_message="approve: apply patch",
            entries=["approve: apply patch", "waiting for approval"],
            pending_prompt=SessionPrompt(request_id="req-1", tool="Bash", hint="apply patch"),
        )
    )

    snapshot = catalog.snapshot(now=120.0)

    assert snapshot.total == 1
    assert snapshot.running == 0
    assert snapshot.waiting == 1
    assert snapshot.msg == "approval pending on host"
    assert snapshot.entries == ["approve: apply patch", "waiting for approval"]
    assert snapshot.prompt is None


def test_readonly_replace_does_not_clobber_managed_prompt_for_same_session_id():
    catalog = SessionCatalog(active_window_seconds=300, completed_window_seconds=120)
    catalog.upsert(
        _session(
            "shared-1",
            state="waiting",
            control_capability="managed",
            last_activity_at=100.0,
            latest_message="approve: rm /tmp/demo",
            entries=["approve: rm /tmp/demo"],
            pending_prompt=SessionPrompt(request_id="req-1", tool="Bash", hint="rm /tmp/demo"),
        )
    )

    catalog.replace_readonly(
        [
            _session(
                "shared-1",
                state="running",
                control_capability="readonly",
                last_activity_at=101.0,
                latest_message="Deleting `/tmp/demo`",
                entries=["Deleting `/tmp/demo`"],
                source="vscode",
                originator="Codex Desktop",
            )
        ]
    )

    snapshot = catalog.snapshot(now=120.0)
    sessions = catalog.sessions(now=120.0)

    assert snapshot.waiting == 1
    assert snapshot.prompt == {
        "id": "req-1",
        "tool": "Bash",
        "hint": "rm /tmp/demo",
    }
    assert sessions[0].control_capability == "managed"
    assert sessions[0].pending_prompt == SessionPrompt(request_id="req-1", tool="Bash", hint="rm /tmp/demo")


def test_recent_completed_session_becomes_primary_when_no_active_sessions_remain():
    catalog = SessionCatalog(active_window_seconds=300, completed_window_seconds=120)
    catalog.upsert(
        _session(
            "completed-1",
            state="completed",
            control_capability="readonly",
            last_activity_at=100.0,
            latest_message="Completed the refactor",
            entries=["Completed the refactor"],
        )
    )

    snapshot = catalog.snapshot(now=150.0)

    assert snapshot.total == 1
    assert snapshot.running == 0
    assert snapshot.waiting == 0
    assert snapshot.msg == "Completed the refactor"
    assert snapshot.entries == ["Completed the refactor"]
    assert snapshot.sessions == [{"id": "completed-1", "name": "project", "state": "done"}]


def test_catalog_exports_compact_sessions_with_titles_and_states():
    catalog = SessionCatalog(active_window_seconds=300, completed_window_seconds=120)
    catalog.upsert(
        _session(
            "running-1",
            state="running",
            control_capability="readonly",
            last_activity_at=100.0,
            latest_message="Working",
            title="评估 StickS3 接入 Codex App",
        )
    )
    catalog.upsert(
        _session(
            "waiting-1",
            state="waiting",
            control_capability="readonly",
            last_activity_at=101.0,
            latest_message="Approval",
            title="Update release notes",
        )
    )
    catalog.upsert(
        _session(
            "recent-1",
            state="recent",
            control_capability="readonly",
            last_activity_at=99.0,
            latest_message="Done",
            title="Recent work",
        )
    )

    snapshot = catalog.snapshot(now=110.0)

    assert snapshot.sessions == [
        {"id": "waiting-1", "name": "Update release notes", "state": "waiting"},
        {"id": "running-1", "name": "评估 StickS3 接入 Codex App", "state": "running"},
        {"id": "recent-1", "name": "Recent work", "state": "done"},
    ]


def test_stale_completed_sessions_drop_out_of_the_visible_snapshot():
    catalog = SessionCatalog(active_window_seconds=300, completed_window_seconds=120)
    catalog.upsert(
        _session(
            "completed-1",
            state="completed",
            control_capability="readonly",
            last_activity_at=100.0,
            latest_message="Completed the refactor",
        )
    )

    snapshot = catalog.snapshot(now=400.0)

    assert snapshot.total == 0
    assert snapshot.running == 0
    assert snapshot.waiting == 0
    assert snapshot.msg == "No Codex connected"
    assert snapshot.entries == []
    assert snapshot.prompt is None


def test_catalog_keeps_full_prompt_hint_for_device_side_scrolling():
    catalog = SessionCatalog(active_window_seconds=300, completed_window_seconds=120)
    hint = "你" * 30
    catalog.upsert(
        _session(
            "managed-long",
            state="waiting",
            control_capability="managed",
            last_activity_at=100.0,
            latest_message="approve: " + hint,
            entries=["approve: " + hint],
            pending_prompt=SessionPrompt(request_id="req-long", tool="Bash", hint=hint),
        )
    )

    snapshot = catalog.snapshot(now=120.0)

    assert snapshot.msg == "approve: " + ("你" * 16) + "..."
    assert snapshot.prompt == {
        "id": "req-long",
        "tool": "Bash",
        "hint": hint,
    }


def test_catalog_snapshot_clips_managed_prompt_by_display_width():
    catalog = SessionCatalog(active_window_seconds=300, completed_window_seconds=120)
    hint = "你" * 25
    catalog.upsert(
        _session(
            "managed-cjk",
            state="waiting",
            control_capability="managed",
            last_activity_at=100.0,
            latest_message="approve: " + hint,
            entries=["approve: " + hint],
            pending_prompt=SessionPrompt(request_id="req-cjk", tool="Bash", hint=hint),
        )
    )

    snapshot = catalog.snapshot(now=120.0)

    assert snapshot.msg == "approve: " + ("你" * 16) + "..."
    assert snapshot.prompt == {
        "id": "req-cjk",
        "tool": "Bash",
        "hint": hint,
    }
