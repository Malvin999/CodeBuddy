import asyncio
from datetime import datetime, timezone
from pathlib import Path

from codex_buddy.agent import BuddyAgent, ManagedSessionRuntime
from codex_buddy.catalog import SessionPrompt, SessionRecord
from codex_buddy.events import ApprovalRequest, TurnState
from codex_buddy.proxy import ApprovalRequestResolved
from codex_buddy.state_store import BridgeStateStore, PersistedState


class _FakeBridge:
    def __init__(self, *, workdir, on_event, on_close, codex_path="codex") -> None:
        self.workdir = workdir
        self.on_event = on_event
        self.on_close = on_close
        self.codex_path = codex_path
        self.started = False
        self.stopped = False
        self.approvals = []
        self.proxy_url = "ws://127.0.0.1:4567"

    async def start(self) -> None:
        self.started = True

    async def stop(self) -> None:
        self.stopped = True

    async def respond_to_device_approval(self, request_id: str, decision: str) -> None:
        self.approvals.append((request_id, decision))


def _record(session_id: str, tokens_total: int, tokens_session: int = 0) -> SessionRecord:
    return SessionRecord(
        session_id=session_id,
        source="vscode",
        originator="Codex Desktop",
        cwd="/tmp/project",
        state="running",
        last_activity_at=100.0,
        latest_message="Working",
        entries=["Working"],
        tokens_total=tokens_total,
        tokens_session=tokens_session,
        control_capability="readonly",
    )


def test_agent_launch_registers_managed_session_and_routes_device_approval(tmp_path):
    created = []

    def factory(*, workdir, on_event, on_close, codex_path, codex_launch_path):
        bridge = _FakeBridge(
            workdir=workdir,
            on_event=on_event,
            on_close=on_close,
            codex_path=codex_path,
        )
        created.append(bridge)
        return bridge

    async def exercise():
        agent = BuddyAgent(
            tmp_path / "state.json",
            watcher=None,
            managed_session_factory=factory,
            clock=lambda: 120.0,
        )
        response = await agent.launch(Path("/tmp/demo"))
        bridge = created[0]
        await bridge.on_event(TurnState(thread_id="thr-1", turn_id="turn-1", active=True))
        await bridge.on_event(
            ApprovalRequest(
                thread_id="thr-1",
                turn_id="turn-1",
                request_id="req-1",
                command="rm -rf /tmp/demo",
                cwd="/tmp/demo",
                reason="Needs approval",
            )
        )
        await agent._handle_device_permission("req-1", "deny")
        return agent.status_payload(), response, bridge

    status, response, bridge = asyncio.run(exercise())

    assert response == {"ok": True, "proxy_url": "ws://127.0.0.1:4567"}
    assert bridge.started is True
    assert bridge.approvals == [("req-1", "deny")]
    assert status["snapshot"]["waiting"] == 1
    assert status["snapshot"]["prompt"]["id"] == "req-1"
    assert status["sessions"][0]["control_capability"] == "managed"
    assert status["sessions"][0]["session_id"] == "thr-1"


def test_agent_token_ledger_seeds_existing_sessions_then_accumulates_deltas(tmp_path):
    state_path = tmp_path / "state.json"
    BridgeStateStore(state_path).save(
        PersistedState(
            tokens_today=10,
            tokens_date="2026-05-28",
            tokens_total=100,
            token_ledger_initialized=False,
        )
    )
    now = datetime(2026, 5, 28, 12, 0, tzinfo=timezone.utc).timestamp()
    agent = BuddyAgent(state_path, watcher=None, clock=lambda: now)

    agent._observe_session_tokens([_record("session-1", 50)])

    assert agent._tokens_today == 10
    assert agent._tokens_total == 100
    assert agent._token_seen_totals == {"session-1": 50}

    agent._observe_session_tokens([_record("session-1", 70), _record("session-2", 5)])

    assert agent._tokens_today == 35
    assert agent._tokens_total == 125
    assert agent._token_seen_totals == {"session-1": 70, "session-2": 5}


def test_agent_token_ledger_resets_today_at_local_midnight(tmp_path):
    state_path = tmp_path / "state.json"
    BridgeStateStore(state_path).save(
        PersistedState(
            tokens_today=25,
            tokens_date="2026-05-28",
            tokens_total=100,
            token_seen_totals={"session-1": 50},
            token_ledger_initialized=True,
        )
    )
    current_time = [datetime(2026, 5, 28, 12, 0, tzinfo=timezone.utc).timestamp()]
    agent = BuddyAgent(state_path, watcher=None, clock=lambda: current_time[0])

    current_time[0] = datetime(2026, 5, 29, 12, 0, tzinfo=timezone.utc).timestamp()
    agent._observe_session_tokens([_record("session-1", 60)])

    assert agent._tokens_today == 10
    assert agent._tokens_total == 110
    assert agent._tokens_date == "2026-05-29"


def test_agent_launch_passes_saved_real_codex_path_to_managed_bridge(tmp_path):
    state_path = tmp_path / "state.json"
    BridgeStateStore(state_path).save(
        PersistedState(
            paired_device_id="device-1",
            paired_device_name="Codex-4DAD",
            real_codex_path="/usr/local/bin/codex",
            codex_launch_path="/usr/local/bin:/usr/bin:/bin",
        )
    )
    created = []

    def factory(*, workdir, on_event, on_close, codex_path, codex_launch_path):
        bridge = _FakeBridge(
            workdir=workdir,
            on_event=on_event,
            on_close=on_close,
            codex_path=codex_path,
        )
        bridge.codex_launch_path = codex_launch_path
        created.append(bridge)
        return bridge

    async def exercise():
        agent = BuddyAgent(
            state_path,
            watcher=None,
            managed_session_factory=factory,
            clock=lambda: 120.0,
        )
        await agent.launch(Path("/tmp/demo"))

    asyncio.run(exercise())

    assert created[0].codex_path == "/usr/local/bin/codex"
    assert created[0].codex_launch_path == "/usr/local/bin:/usr/bin:/bin"


def test_managed_runtime_ignores_unrelated_approval_resolution():
    runtime = ManagedSessionRuntime(control_id="managed-1", workdir=Path("/tmp/demo"))

    runtime.apply(TurnState(thread_id="thr-1", turn_id="turn-1", active=True), now=100.0)
    runtime.apply(
        ApprovalRequest(
            thread_id="thr-1",
            turn_id="turn-1",
            request_id="req-1",
            command="rm -f /tmp/demo",
            cwd="/tmp/demo",
            reason="Needs approval",
        ),
        now=101.0,
    )

    runtime.apply(ApprovalRequestResolved(request_id="req-2"), now=102.0)

    assert runtime.state == "waiting"
    assert runtime.pending_prompt == SessionPrompt(
        request_id="req-1",
        tool="Bash",
        hint="rm -f /tmp/demo",
    )


class _FlakyBle:
    created: list["_FlakyBle"] = []

    def __init__(self, device_id: str, *, device_name: str, on_permission) -> None:
        self.device_id = device_id
        self.device_name = device_name
        self.on_permission = on_permission
        self.disconnect_calls = 0
        self.snapshot_calls = 0
        self._should_fail = not self.created
        self.created.append(self)

    async def connect(self) -> None:
        if self._should_fail:
            raise RuntimeError("temporary connect failure")

    async def disconnect(self) -> None:
        self.disconnect_calls += 1

    async def send_snapshot(self, snapshot) -> None:
        self.snapshot_calls += 1


def test_agent_ble_loop_recreates_transport_after_connect_failure(tmp_path):
    _FlakyBle.created = []
    state_path = tmp_path / "state.json"
    BridgeStateStore(state_path).save(
        PersistedState(
            paired_device_id="device-1",
            paired_device_name="Codex-4DAD",
        )
    )

    async def exercise():
        agent = BuddyAgent(
            state_path,
            watcher=None,
            reconnect_interval=0.01,
            ble_factory=_FlakyBle,
        )
        task = asyncio.create_task(agent._ble_loop())
        await asyncio.sleep(0.08)
        agent._stopped.set()
        await task
        return agent

    agent = asyncio.run(exercise())

    assert len(_FlakyBle.created) >= 2
    assert _FlakyBle.created[0].disconnect_calls == 1
    assert agent._ble_connected is True
    assert agent._ble is _FlakyBle.created[-1]
