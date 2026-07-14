from __future__ import annotations

import socket
import subprocess
import threading
import time
import unittest
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
from pathlib import Path
import shutil


ROOT = Path(__file__).resolve().parents[1]
WEB_ROOT = ROOT / "bridge" / "src" / "vibe_stick" / "web"
CHROME = (
    shutil.which("google-chrome-stable")
    or shutil.which("google-chrome")
    or shutil.which("chromium")
    or shutil.which("chromium-browser")
)


class TelemetryWebTests(unittest.TestCase):
    def test_telemetry_assets_have_expected_structure(self) -> None:
        html = (WEB_ROOT / "telemetry" / "index.html").read_text(encoding="utf-8")
        js = (WEB_ROOT / "telemetry" / "telemetry.js").read_text(encoding="utf-8")

        self.assertIn('href="./telemetry.css"', html)
        self.assertIn('src="./telemetry.js"', html)
        self.assertIn('const FIXTURE_KEY = "fixture";', js)
        self.assertIn("REFRESH_MS = 2000", js)
        self.assertIn("/sessions/${encodeURIComponent(session.id)}/export.csv", js)
        self.assertIn("buildChart(sessions, mode)", js)

        node = shutil.which("node") or shutil.which("nodejs")
        self.assertIsNotNone(node)
        subprocess.run(
            [node, "--check", str(WEB_ROOT / "telemetry" / "telemetry.js")],
            check=True,
            capture_output=True,
            text=True,
        )

    def test_fixture_dashboard_renders_in_headless_browser(self) -> None:
        if not CHROME:
            self.skipTest("Chrome or Chromium is not installed")
        server = self._serve_directory(WEB_ROOT)
        try:
            url = f"http://127.0.0.1:{server.server_port}/telemetry/?fixture=industrial"
            dom = self._dump_dom(url)
        finally:
            server.shutdown()
            server.server_close()

        self.assertIn("VibeStick Telemetry", dom)
        self.assertIn("Fixture mode", dom)
        self.assertIn("VibeStick A", dom)
        self.assertIn("VibeStick B", dom)
        self.assertIn("stale", dom.lower())
        self.assertIn("Voltage mode", dom)
        self.assertIn("CSV", dom)
        self.assertIn("data:text/csv", dom)
        self.assertGreaterEqual(dom.count('class="telemetry-series"'), 2)
        self.assertIn("4.08 V", dom)
        self.assertIn("76 %", dom)

    def _dump_dom(self, url: str) -> str:
        command = [
            str(CHROME),
            "--headless=new",
            "--disable-gpu",
            "--no-sandbox",
            "--allow-file-access-from-files",
            "--run-all-compositor-stages-before-draw",
            "--virtual-time-budget=4000",
            "--dump-dom",
            url,
        ]
        result = subprocess.run(command, check=True, capture_output=True, text=True)
        return result.stdout

    def _serve_directory(self, directory: Path) -> ThreadingHTTPServer:
        class QuietHandler(SimpleHTTPRequestHandler):
            def log_message(self, format: str, *args: object) -> None:  # noqa: A003
                return

        handler = lambda *args, **kwargs: QuietHandler(*args, directory=str(directory), **kwargs)
        server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self._wait_for_port(server.server_address[1])
        return server

    def _wait_for_port(self, port: int) -> None:
        deadline = time.time() + 10
        while time.time() < deadline:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
                sock.settimeout(0.2)
                try:
                    sock.connect(("127.0.0.1", port))
                except OSError:
                    time.sleep(0.05)
                    continue
                return
        raise AssertionError(f"server on port {port} did not become ready")


if __name__ == "__main__":
    unittest.main()
