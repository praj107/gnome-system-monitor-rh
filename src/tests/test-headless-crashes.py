#!/usr/bin/env python3
"""
Headless regression tests for two crashes that no unit test reaches, and for
one rendering race in the developer screenshot:

  * closing the main window (what the close button does) must end the
    application with status 0; a signal group bound to the window used to be
    finalised after the window's weak references were cleared, and the window
    then called a notify on freed memory;
  * confirming a kill when no toplevel is the active window (a compositor
    that reports no focus, as the headless one does) must show the refusal
    and keep the application running; the dialog code used to unref GTK's own
    list of toplevels and then open a dialog with no parent;
  * a screenshot taken right after the `tab` verb must show the header's view
    switcher on the page that is showing; it used to be captured before the
    switcher's new state reached its render node.

The application runs in a private headless Weston on its own runtime
directory, with in-memory settings, no D-Bus session and the developer
sandbox switch on, so it never touches the real session, never signals a
process other than the harmless one this test starts, and never asks for
privileges. It needs a developer build (-Ddev_ipc=true); the test is not
registered otherwise. Exit status 77 means skipped (no Weston).

usage: test-headless-crashes.py APP CTL SCHEMA_DIR
"""

import os
import shutil
import signal
import subprocess
import sys
import tempfile
import struct
import time
import zlib

SKIP = 77
APP_START_TIMEOUT_S = 30
APP_EXIT_TIMEOUT_S = 15
CLOSE_RUNS = 3
HEADER_BAND_PX = 48  # the header bar, with the view switcher in it


class Session:
    """A private Weston plus the environment an app instance runs in."""

    def __init__(self, root, schema_dir):
        self.root = root
        self.runtime = os.path.join(root, "run")
        os.makedirs(self.runtime, mode=0o700)
        for name in ("data", "state", "config", "cache"):
            os.makedirs(os.path.join(root, "xdg", name))

        self.schemas = os.path.join(root, "schemas")
        os.makedirs(self.schemas)
        for name in os.listdir(schema_dir):
            if name.endswith(".gschema.xml") or name.endswith(".enums.xml"):
                shutil.copy(os.path.join(schema_dir, name), self.schemas)
        subprocess.run(["glib-compile-schemas", self.schemas], check=True)

        self.socket = os.path.join(root, "ipc.sock")
        self.weston = subprocess.Popen(
            ["weston", "--backend=headless", "--renderer=pixman",
             "--socket=wl-test", "--width=1280", "--height=960",
             "--idle-time=0", "--log=" + os.path.join(root, "weston.log")],
            env={**os.environ, "XDG_RUNTIME_DIR": self.runtime},
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self._wait_for(os.path.join(self.runtime, "wl-test"), "weston")

    def env(self):
        env = {k: v for k, v in os.environ.items()
               if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
        env.update({
            "XDG_RUNTIME_DIR": self.runtime,
            "WAYLAND_DISPLAY": "wl-test",
            "GDK_BACKEND": "wayland",
            "GSK_RENDERER": "cairo",
            "DBUS_SESSION_BUS_ADDRESS": "disabled:",
            "GSETTINGS_BACKEND": "memory",
            "GSETTINGS_SCHEMA_DIR": self.schemas,
            "XDG_DATA_HOME": os.path.join(self.root, "xdg", "data"),
            "XDG_STATE_HOME": os.path.join(self.root, "xdg", "state"),
            "XDG_CONFIG_HOME": os.path.join(self.root, "xdg", "config"),
            "XDG_CACHE_HOME": os.path.join(self.root, "xdg", "cache"),
            "GSM_DEV_IPC": "1",
            "GSM_DEV_SANDBOX": "1",
            "GSM_DEV_IPC_SOCKET": self.socket,
        })
        return env

    def _wait_for(self, path, what):
        deadline = time.monotonic() + APP_START_TIMEOUT_S
        while not os.path.exists(path):
            if time.monotonic() > deadline:
                raise AssertionError("%s did not come up" % what)
            time.sleep(0.1)

    def start_app(self, app, log_name):
        if os.path.exists(self.socket):
            os.unlink(self.socket)
        log = open(os.path.join(self.root, log_name), "wb")
        proc = subprocess.Popen([app], env=self.env(), stdout=log,
                                stderr=subprocess.STDOUT)
        self._wait_for(self.socket, "the application's control socket")
        return proc, os.path.join(self.root, log_name)

    def stop(self):
        self.weston.terminate()
        try:
            self.weston.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.weston.kill()


def ctl(session, ctl_path, *args):
    result = subprocess.run([sys.executable, ctl_path, *args],
                            env=session.env(), capture_output=True,
                            text=True, timeout=30)
    return result.returncode, result.stdout.strip()


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def test_close_exits_cleanly(session, app, ctl_path):
    for run in range(CLOSE_RUNS):
        proc, log = session.start_app(app, "close-%d.log" % run)
        status, out = ctl(session, ctl_path, "close")
        check(status == 0 and out.startswith("OK"), "close refused: " + out)
        try:
            code = proc.wait(timeout=APP_EXIT_TIMEOUT_S)
        except subprocess.TimeoutExpired:
            proc.kill()
            raise AssertionError("run %d: the app did not exit after close" % run)
        check(code == 0, "run %d: exit status %d after close "
              "(negative is a signal)" % (run, code))


def test_refused_kill_keeps_the_app_running(session, app, ctl_path):
    tmp_target = os.path.join(session.root, "gsmtesttarget")
    shutil.copy(shutil.which("sleep"), tmp_target)
    target = subprocess.Popen([tmp_target, "600"])
    proc, log = session.start_app(app, "refusal.log")
    try:
        for args in (("action", "win.show-page::processes"),
                     ("select", "gsmtesttarget"),
                     ("action", "win.send-signal-kill")):
            status, out = ctl(session, ctl_path, *args)
            check(status == 0 and out.startswith("OK"),
                  "%s: %s" % (" ".join(args), out))
        time.sleep(1)
        status, out = ctl(session, ctl_path, "click", "Force Stop")
        check(status == 0 and out.startswith("OK"), "click: " + out)
        time.sleep(2)

        check(proc.poll() is None, "the app exited after the refusal "
              "(status %s)" % proc.poll())
        status, out = ctl(session, ctl_path, "text")
        check(status == 0 and out.startswith("OK"),
              "the app stopped answering after the refusal: " + out)
        check(target.poll() is None, "the sandbox let the target be signalled")
        with open(log, "r", errors="replace") as f:
            critical = [line for line in f if "CRITICAL" in line]
        check(not critical, "GLib criticals after the refusal: %r" % critical[:3])

        # Dismiss the refusal first: a closing window waits for its modal.
        status, out = ctl(session, ctl_path, "click", "Close")
        check(status == 0 and out.startswith("OK"),
              "the refusal was not shown to dismiss: " + out)
        time.sleep(1.5)  # let the dismissed dialog finish closing
        ctl(session, ctl_path, "close")
        check(proc.wait(timeout=APP_EXIT_TIMEOUT_S) == 0,
              "the app did not exit cleanly after the refusal")
    finally:
        if proc.poll() is None:
            proc.kill()
        target.send_signal(signal.SIGTERM)
        target.wait(timeout=10)


def read_png_rows(path, first_rows):
    """The first rows of an 8-bit RGB(A) PNG as bytes, with no imaging library."""
    with open(path, "rb") as f:
        data = f.read()
    check(data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG: " + path)
    pos, idat, header = 8, b"", None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    width, height, depth, color = header[:4]
    check(depth == 8 and color in (2, 6), "unexpected PNG format in " + path)
    bpp = 3 if color == 2 else 4
    stride = width * bpp
    raw = zlib.decompress(idat)
    rows, prev = [], bytearray(stride)
    for y in range(min(first_rows, height)):
        base = y * (stride + 1)
        ftype, line = raw[base], bytearray(raw[base + 1:base + 1 + stride])
        for i in range(stride):
            left = line[i - bpp] if i >= bpp else 0
            up = prev[i]
            upleft = prev[i - bpp] if i >= bpp else 0
            if ftype == 1:
                line[i] = (line[i] + left) & 255
            elif ftype == 2:
                line[i] = (line[i] + up) & 255
            elif ftype == 3:
                line[i] = (line[i] + (left + up) // 2) & 255
            elif ftype == 4:
                p = left + up - upleft
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - upleft)
                pred = left if pa <= pb and pa <= pc else (up if pb <= pc else upleft)
                line[i] = (line[i] + pred) & 255
        rows.append(bytes(line))
        prev = line
    return b"".join(rows)


def test_switcher_follows_the_tab_verb(session, app, ctl_path):
    proc, log = session.start_app(app, "switcher.log")
    try:
        status, out = ctl(session, ctl_path, "tab")
        check(status == 0 and "available=" in out, "tab listing: " + out)
        pages = out.split("available=")[1].split(",")
        check(len(pages) >= 3, "too few tabs to test: " + out)

        headers_seen = set()
        # Visit every page, then the first again, so that each switch is a
        # change from a different selected button.
        for page in pages + pages[:1]:
            status, out = ctl(session, ctl_path, "tab", page)
            check(status == 0 and out.startswith("OK"), "tab %s: %s" % (page, out))
            now = os.path.join(session.root, "now-%s.png" % page)
            later = os.path.join(session.root, "later-%s.png" % page)
            status, out = ctl(session, ctl_path, "shot", now)
            check(status == 0 and out.startswith("OK"), "shot: " + out)
            time.sleep(1.5)  # many frames: whatever is going to settle has
            status, out = ctl(session, ctl_path, "shot", later)
            check(status == 0 and out.startswith("OK"), "shot: " + out)

            header_now = read_png_rows(now, HEADER_BAND_PX)
            header_later = read_png_rows(later, HEADER_BAND_PX)
            check(header_now == header_later,
                  "after `tab %s` the header in the first screenshot "
                  "differs from the one taken 1.5 s later (see %s and %s): "
                  "the switcher did not follow the page" % (page, now, later))
            headers_seen.add(header_later)
        # The comparison above would pass vacuously if the header never
        # changed with the page.
        check(len(headers_seen) >= len(pages),
              "the switcher looks the same on different pages")

        ctl(session, ctl_path, "close")
        check(proc.wait(timeout=APP_EXIT_TIMEOUT_S) == 0,
              "the app did not exit cleanly")
    finally:
        if proc.poll() is None:
            proc.kill()


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    app, ctl_path, schema_dir = sys.argv[1:]

    if shutil.which("weston") is None:
        print("1..0 # SKIP weston is not installed")
        return SKIP
    if not os.path.isdir("/dev/shm"):
        print("1..0 # SKIP no RAM-backed scratch directory")
        return SKIP

    failures = 0
    tests = (test_close_exits_cleanly, test_refused_kill_keeps_the_app_running,
             test_switcher_follows_the_tab_verb)
    with tempfile.TemporaryDirectory(prefix="gsm-headless-", dir="/dev/shm") as root:
        session = Session(root, schema_dir)
        try:
            for test in tests:
                try:
                    test(session, app, ctl_path)
                    print("ok - %s" % test.__name__)
                except (AssertionError, subprocess.SubprocessError) as err:
                    failures += 1
                    print("not ok - %s: %s" % (test.__name__, err))
        finally:
            session.stop()
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
