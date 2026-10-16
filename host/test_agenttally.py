"""Tests for the host bridge: python3 -m unittest host/test_agenttally.py"""

import json
import os
import socket
import sys
import tempfile
import threading
import time
import unittest
import urllib.request
from http.server import ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(__file__))
import agenttally as at  # noqa: E402


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class BridgeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        at.HOME = tempfile.mkdtemp()
        at.STATE_FILE = os.path.join(at.HOME, "state.json")
        cls.port = free_port()
        cls.httpd = ThreadingHTTPServer(("127.0.0.1", cls.port), at.Api)
        threading.Thread(target=cls.httpd.serve_forever, daemon=True).start()
        cls.sim_port = free_port()
        cls.stop = threading.Event()
        threading.Thread(target=at.sim_server, args=(cls.sim_port, cls.stop), daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.stop.set()
        cls.httpd.shutdown()

    def setUp(self):
        for s in at.STORE.snapshot():
            at.STORE.remove(s["id"])

    def hook(self, **payload):
        payload.setdefault("session_id", "s1")
        payload.setdefault("cwd", "/Users/me/code/çiçek-app")
        req = urllib.request.Request(f"http://127.0.0.1:{self.port}/hook/claude", data=json.dumps(payload).encode(),
                                     headers={"Content-Type": "application/json"})
        self.assertEqual(json.loads(urllib.request.urlopen(req).read()), {"ok": True})

    def session(self, sid="claude:s1"):
        return at.STORE.get(sid)

    def test_claude_lifecycle(self):
        self.hook(hook_event_name="SessionStart", source="startup")
        s = self.session()
        self.assertEqual((s["state"], s["name"]), ("idle", "çiçek-app"))

        self.hook(hook_event_name="UserPromptSubmit", prompt="fix the test")
        self.assertEqual(self.session()["state"], "working")

        self.hook(hook_event_name="PreToolUse", tool_name="Bash",
                  tool_input={"command": "npm test", "description": "Run tests"})
        self.assertEqual(self.session()["detail"], "$ Run tests")

        self.hook(hook_event_name="PermissionRequest", tool_name="Bash", tool_input={"command": "rm -rf build"})
        s = self.session()
        self.assertEqual(s["state"], "waiting")
        self.assertIn("rm -rf build", s["detail"])

        self.hook(hook_event_name="PostToolUse", tool_name="Bash", tool_input={})
        self.assertEqual(self.session()["state"], "working")

        self.hook(hook_event_name="Stop", last_assistant_message="**Done.** All `48` tests pass.")
        s = self.session()
        self.assertEqual((s["state"], s["ack"]), ("done", False))
        self.assertEqual(s["reply"], "Done. All 48 tests pass.")
        self.assertEqual(s["detail"], "Done.")

        at.STORE.ack("claude:s1")
        self.assertEqual((self.session()["state"], self.session()["ack"]), ("done", True))

        self.hook(hook_event_name="SessionEnd", reason="prompt_input_exit")
        self.assertIsNone(self.session())

    def test_question_and_notifications(self):
        self.hook(hook_event_name="UserPromptSubmit", prompt="go")
        self.hook(hook_event_name="PreToolUse", tool_name="AskUserQuestion",
                  tool_input={"questions": [{"question": "Which board?"}]})
        self.assertEqual(self.session()["state"], "waiting")
        self.assertEqual(self.session()["detail"], "Question: Which board?")
        self.hook(hook_event_name="PostToolUse", tool_name="AskUserQuestion")
        self.hook(hook_event_name="Notification", notification_type="idle_prompt", message="waiting")
        self.assertEqual(self.session()["state"], "done")
        # an idle_prompt must not re-flag an already acknowledged session
        at.STORE.ack("claude:s1")
        self.hook(hook_event_name="Notification", notification_type="idle_prompt", message="waiting")
        self.assertTrue(self.session()["ack"])

    def test_subagent_tools_do_not_raise_questions(self):
        self.hook(hook_event_name="UserPromptSubmit", prompt="go")
        self.hook(hook_event_name="SubagentStart", agent_id="a1", agent_type="Explore")
        self.assertEqual(self.session()["sub"], 1)
        self.hook(hook_event_name="PreToolUse", tool_name="Grep", tool_input={"pattern": "foo"}, agent_id="a1")
        self.assertEqual(self.session()["detail"], "[sub] Grep foo")
        self.hook(hook_event_name="SubagentStop", agent_id="a1", agent_type="Explore")
        self.assertEqual(self.session()["sub"], 0)

    def test_generic_event_and_eviction(self):
        req = urllib.request.Request(f"http://127.0.0.1:{self.port}/api/event",
                                     data=json.dumps([{"source": "codex", "id": f"t{i}", "state": "working"}
                                                      for i in range(at.MAX_SESSIONS + 3)]).encode(),
                                     headers={"Content-Type": "application/json"})
        urllib.request.urlopen(req).read()
        self.assertEqual(len(at.STORE.snapshot()), at.MAX_SESSIONS)
        bad = urllib.request.Request(f"http://127.0.0.1:{self.port}/api/event",
                                     data=b'{"state":"bogus"}', headers={"Content-Type": "application/json"})
        with self.assertRaises(urllib.error.HTTPError):
            urllib.request.urlopen(bad)

    def test_wire_format_is_ascii_and_bounded(self):
        at.STORE.upsert("claude:x", state="done", name="çiçek — ışık", detail="İş “bitti” " + "x" * 300,
                        cwd=os.path.expanduser("~/code"), prompt="ş" * 500, reply="ok")
        w = at.wire_session(at.STORE.get("claude:x"))
        line = json.dumps(w, ensure_ascii=False)
        self.assertEqual(w["name"], "çiçek — ışık")
        self.assertEqual(at.ascii_fold("ok 🚀 done 日本", 40), "ok done")
        self.assertLessEqual(len(w["det"]), 96)
        self.assertEqual(w["cwd"], "~/code")
        self.assertLess(len(line.encode()), 2048)
        self.assertIn("act", w)

    def test_simulator_link_gets_snapshot_and_updates(self):
        at.STORE.upsert("claude:pre", state="working", name="before")
        c = socket.create_connection(("127.0.0.1", self.sim_port))
        c.settimeout(3)
        c.sendall(b'boot noise\n{"t":"hello","v":1,"fw":"test","board":"sim","w":240,"h":320}\n')
        f = c.makefile("r")
        msgs = [json.loads(f.readline()) for _ in range(3)]
        self.assertEqual([m["t"] for m in msgs], ["hi", "clr", "s"])
        self.assertEqual(msgs[2]["name"], "before")
        at.STORE.upsert("claude:pre", state="done")
        self.assertEqual(json.loads(f.readline())["st"], "done")
        c.sendall(b'{"t":"ack","id":"claude:pre"}\n')
        self.assertEqual(json.loads(f.readline())["ack"], 1)
        c.close()

    def test_interrupt_detection(self):
        with tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False) as t:
            t.write(json.dumps({"type": "assistant", "message": {"content": []}}) + "\n")
            t.write(json.dumps({"type": "user", "message": {"content": [
                {"type": "text", "text": "[Request interrupted by user]"}]}}) + "\n")
        self.assertTrue(at.transcript_interrupted(t.name, time.time() - 60))
        self.assertFalse(at.transcript_interrupted(t.name, time.time() + 60))

    def test_hook_install_roundtrip(self):
        cfg = os.path.join(tempfile.mkdtemp(), "settings.json")
        existing = {"model": "opus", "hooks": {"Stop": [{"hooks": [{"type": "command", "command": "say hi"}]}]}}
        with open(cfg, "w") as f:
            json.dump(existing, f)
        os.environ["CLAUDE_CONFIG_DIR"] = os.path.dirname(cfg)
        try:
            at.install_hooks(47321)
            at.install_hooks(47321)  # idempotent
            s = at.load_settings()
            self.assertEqual(len(s["hooks"]["Stop"]), 2)
            self.assertEqual(len(s["hooks"]["PreToolUse"]), 1)
            at.write_settings(at.strip_our_hooks(at.load_settings()))
            self.assertEqual(at.load_settings(), existing)
        finally:
            del os.environ["CLAUDE_CONFIG_DIR"]


if __name__ == "__main__":
    unittest.main()
