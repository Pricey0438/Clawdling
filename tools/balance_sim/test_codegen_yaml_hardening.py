"""Hardening tests for the codegen's YAML parser."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SCRIPT = os.path.join(ROOT, "firmware", "scripts", "gen_balance_h.py")


def _run(yaml_text):
    with tempfile.NamedTemporaryFile(mode="w", suffix=".yaml", delete=False) as ftmp:
        ftmp.write(yaml_text)
        yaml_path = ftmp.name
    out_path = yaml_path + ".h"
    try:
        proc = subprocess.run(
            [sys.executable, SCRIPT, "--yaml", yaml_path, "--out", out_path],
            capture_output=True, text=True,
        )
        return proc
    finally:
        try: os.unlink(yaml_path)
        except OSError: pass
        try: os.unlink(out_path)
        except OSError: pass


def test_tab_indent_rejected():
    proc = _run("xp:\n\tper_session_ctx_pct: 5\n")
    assert proc.returncode != 0, "tab indent should fail"
    assert "tab" in proc.stderr.lower(), f"error msg should mention tabs; got: {proc.stderr}"


def test_duplicate_key_rejected():
    proc = _run("care:\n  decay_interval_sec: 480\n  decay_interval_sec: 600\n")
    assert proc.returncode != 0, "duplicate key should fail"
    assert "duplicate" in proc.stderr.lower(), f"error msg should mention duplicate; got: {proc.stderr}"


def test_duplicate_top_level_key_rejected():
    proc = _run("foo: 1\nfoo: 2\n")
    assert proc.returncode != 0, "duplicate top-level key should fail"
    assert "duplicate" in proc.stderr.lower(), f"error msg should mention duplicate; got: {proc.stderr}"


def main():
    test_tab_indent_rejected()
    test_duplicate_key_rejected()
    test_duplicate_top_level_key_rejected()
    print("ok")


if __name__ == "__main__":
    main()
