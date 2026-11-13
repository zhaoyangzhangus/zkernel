#!/usr/bin/env python3

from pathlib import Path
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("BUILD", str(ROOT / "build")))
if not BUILD.is_absolute():
    BUILD = ROOT / BUILD
SERIAL = BUILD / "qemu-serial.log"
MONITOR = Path(
    os.environ.get(
        "QEMU_MONITOR_SOCKET",
        str(Path(tempfile.gettempdir()) / "system-xhci-hub-wakeup-probe.sock"),
    )
)
MOUSE_EVENT_DELAY = float(os.environ.get("XHCI_TEST_MOUSE_DELAY", "0.006"))
BOOT_READY_TIMEOUT = float(
    os.environ.get("XHCI_TEST_BOOT_READY_TIMEOUT", "60.0")
)
QEMU_SECONDS = os.environ.get("XHCI_TEST_QEMU_SECONDS", "90")
POLL_INTERVAL = float(os.environ.get("XHCI_TEST_POLL_INTERVAL", "0.25"))
OUT = BUILD / "xhci-hub-wakeup-probe.stdout"
ERR = BUILD / "xhci-hub-wakeup-probe.stderr"


def serial_text():
    try:
        return SERIAL.read_text(errors="replace")
    except OSError:
        return ""


def wait_for(predicate, timeout, label):
    if POLL_INTERVAL <= 0.0:
        raise RuntimeError("XHCI_TEST_POLL_INTERVAL must be positive")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            print("PASS:", label)
            return True
        time.sleep(POLL_INTERVAL)
    print("TIMEOUT:", label)
    return False


class HMP:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(0.25)
        self.sock.connect(str(path))
        self._read(3.0)

    def _read(self, timeout):
        end = time.monotonic() + timeout
        data = bytearray()
        while time.monotonic() < end:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                break
            data.extend(chunk)
            if b"(qemu)" in data:
                break
        return data.decode(errors="replace")

    def cmd(self, command, timeout=3.0):
        self.sock.sendall((command + "\n").encode())
        text = self._read(timeout).replace("\r", "")
        print()
        print("HMP>", command)
        for line in text.splitlines():
            if line.strip() != "(qemu)":
                print("   ", line)
        return text

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass


def print_new_diag(baseline_len):
    text = serial_text()[baseline_len:]
    wanted = []
    for line in text.splitlines():
        if (
            "OS_DIAG_" in line
            or "OS_USB_RUNTIME_HUB_CHANGE" in line
            or "OS_XHCI_FAIL" in line
        ):
            wanted.append(line)

    print()
    print("=== NEW GUEST DIAGNOSTICS ===")
    if not wanted:
        print("(none)")
    else:
        for line in wanted:
            print(line)


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    for path in (MONITOR, OUT, ERR):
        try:
            path.unlink()
        except FileNotFoundError:
            pass

    out = OUT.open("wb")
    err = ERR.open("wb")
    runner = None
    hmp = None

    try:
        runner = subprocess.Popen(
            [
                str(ROOT / "run-qemu.sh"),
                "--headless",
                "--usb-nested",
                "--monitor-socket", str(MONITOR),
                "--seconds", QEMU_SECONDS,
                "--no-build",
            ],
            cwd=ROOT,
            stdout=out,
            stderr=err,
        )

        if not wait_for(lambda: MONITOR.exists(), 12.0, "HMP socket online"):
            return 2

        if not wait_for(
            lambda: "OS_XHCI_HUB_RUNTIME_OK" in serial_text(),
            BOOT_READY_TIMEOUT,
            "Hub runtime ready",
        ):
            return 3

        # Hub 运行时就绪发生在显示初始化后的用户运行时自测之前。先等待这个独立启动门禁，
        # 再移除设备，以免恢复探测将较慢的自测误报为阶段失败。
        if not wait_for(
            lambda: "OS_USER_RUNTIME_SUBTESTS_OK" in serial_text(),
            BOOT_READY_TIMEOUT,
            "boot runtime self-tests ready",
        ):
            return 4
        if "OS_STAGE_FAIL" in serial_text():
            return 5

        # 只有启动关键的用户运行时自测完成后才连接 HMP。已连接的监视器会增加 TCG 主循环
        # 负载，否则可能扰乱该自测的调度时间窗。
        end = time.monotonic() + 5.0
        while hmp is None:
            try:
                hmp = HMP(MONITOR)
            except OSError:
                if time.monotonic() >= end:
                    raise
                time.sleep(0.05)

        # 给刚刚布防的中断端点留出足够时间，使其进入 QEMU 的常规 NAK/重试状态；即使此
        # 测试拓扑使用允许的最慢间隔，也必须如此。
        time.sleep(1.0)

        print()
        print("========================================")
        print("BEFORE DEVICE_DEL")
        print("========================================")
        hmp.cmd("info usb")

        baseline_text = serial_text()
        baseline_len = len(baseline_text)
        baseline_change = baseline_text.count("OS_USB_RUNTIME_HUB_CHANGE")

        hmp.cmd("device_del vm-nested-mouse")

        for delay in (0.2, 1.0, 5.0):
            time.sleep(delay if delay == 0.2 else delay - (0.2 if delay == 1.0 else 1.0))
            print()
            print("========================================")
            print(f"{delay:.1f}s AFTER DEVICE_DEL")
            print("========================================")
            hmp.cmd("info usb")
            print_new_diag(baseline_len)

        print()
        print("Waiting up to 40s for a runtime Hub change ...")
        got_change = wait_for(
            lambda: serial_text().count("OS_USB_RUNTIME_HUB_CHANGE")
                    > baseline_change,
            40.0,
            "Runtime Hub change after nested detach",
        )

        stage_failed = "OS_STAGE_FAIL" in serial_text()

        print_new_diag(baseline_len)

        print()
        print("=== RESULT ===")
        print("runtime_hub_change:", "YES" if got_change else "NO")
        print("stage_failure:", "YES" if stage_failed else "NO")

        if ERR.exists():
            qemu_err = ERR.read_text(errors="replace").strip()
            if qemu_err:
                print()
                print("=== QEMU STDERR TAIL ===")
                print(qemu_err[-6000:])

        return 0 if got_change and not stage_failed else 1

    finally:
        if hmp is not None:
            try:
                hmp.cmd("quit", 0.5)
            except Exception:
                pass
            hmp.close()

        if runner is not None:
            try:
                runner.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                runner.terminate()
                try:
                    runner.wait(timeout=2.0)
                except subprocess.TimeoutExpired:
                    runner.kill()
                    runner.wait()

        out.close()
        err.close()

        try:
            MONITOR.unlink()
        except FileNotFoundError:
            pass


if __name__ == "__main__":
    sys.exit(main())
