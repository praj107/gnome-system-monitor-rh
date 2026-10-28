#!/usr/bin/env python3
"""
Headless regression tests for two crashes that no unit test reaches:

  * closing the main window (what the close button does) must end the
    application with status 0; a signal group bound to the window used to be
    finalised after the window's weak references were cleared, and the window
    then called a notify on freed memory;
  * confirming a kill when no toplevel is the active window (a compositor
    that reports no focus, as the headless one does) must show the refusal
    and keep the application running; the dialog code used to unref GTK's own
    list of toplevels and then open a dialog with no parent.

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
import time

SKIP = 77
APP_START_TIMEOUT_S = 30
APP_EXIT_TIMEOUT_S = 15
CLOSE_RUNS = 3


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
    tests = (test_close_exits_cleanly, test_refused_kill_keeps_the_app_running)
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
