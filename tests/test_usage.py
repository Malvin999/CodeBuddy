import json

from codex_buddy.usage import CodexUsageClient


class _Response:
    def __init__(self, payload: dict) -> None:
        self.payload = payload

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        return False

    def read(self, limit: int = -1) -> bytes:
        return json.dumps(self.payload).encode("utf-8")


def test_codex_usage_client_reads_auth_and_formats_ble_payload(tmp_path):
    auth_path = tmp_path / "auth.json"
    auth_path.write_text(
        json.dumps(
            {
                "tokens": {
                    "access_token": "access-token",
                    "account_id": "acct-123",
                }
            }
        ),
        encoding="utf-8",
    )
    seen = {}

    def opener(request, *, timeout):
        seen["authorization"] = request.get_header("Authorization")
        seen["account"] = request.get_header("Chatgpt-account-id")
        seen["timeout"] = timeout
        return _Response(
            {
                "rate_limit": {
                    "primary_window": {
                        "limit_window_seconds": 18000,
                        "used_percent": 86.8,
                        "reset_at": 1000 + 58 * 60,
                    },
                    "secondary_window": {
                        "limit_window_seconds": 7 * 24 * 3600,
                        "used_percent": 52,
                        "reset_at": 1000 + 3 * 86400 + 21 * 3600,
                    },
                }
            }
        )

    snapshot = CodexUsageClient(auth_path=auth_path, timeout=3, opener=opener).fetch()

    assert seen == {
        "authorization": "Bearer access-token",
        "account": "acct-123",
        "timeout": 3,
    }
    assert snapshot is not None
    assert snapshot.as_ble_payload(now=1000) == {
        "live": True,
        "short_pct": 13,
        "short_window": "5h",
        "short_reset": "58m",
        "long_pct": 48,
        "long_window": "7d",
        "long_reset": "3d 21h",
    }


def test_codex_usage_client_returns_none_without_auth(tmp_path):
    assert CodexUsageClient(auth_path=tmp_path / "missing.json").fetch() is None


def test_codex_usage_client_does_not_synthesize_missing_window(tmp_path):
    auth_path = tmp_path / "auth.json"
    auth_path.write_text(
        json.dumps(
            {
                "tokens": {
                    "access_token": "access-token",
                    "account_id": "acct-123",
                }
            }
        ),
        encoding="utf-8",
    )

    def opener(request, *, timeout):
        return _Response(
            {
                "rate_limit": {
                    "primary_window": {
                        "limit_window_seconds": 18000,
                        "used_percent": 0,
                        "reset_at": 1000,
                    }
                }
            }
        )

    snapshot = CodexUsageClient(auth_path=auth_path, opener=opener).fetch()

    assert snapshot is not None
    assert snapshot.as_ble_payload(now=1000) is None
