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
        str(Path(tempfile.gettempdir()) / "system-xhci-hub-hotplug.sock"),
    )
)

RUNNER_STDOUT = BUILD / "b10b3-runner.stdout"
RUNNER_STDERR = BUILD / "b10b3-runner.stderr"
# QEMU 的 HMP mouse_move 路径合并更新的速度快于来宾 USB 轮询间隔。默认延迟刻意设得较长，
# 使压力阶段测量 HID 重新挂起，而不是宿主输入批处理。CI 仍可为特定控制器探测覆盖此值。
MOUSE_EVENT_DELAY = float(os.environ.get("XHCI_TEST_MOUSE_DELAY", "0.05"))
MOUSE_READY_TIMEOUT = float(
    os.environ.get("XHCI_TEST_MOUSE_READY_TIMEOUT", "5.0")
)
HUB_SETTLE_SECONDS = float(
    os.environ.get("XHCI_TEST_HUB_SETTLE", "1.0")
)
BOOT_READY_TIMEOUT = float(
    os.environ.get("XHCI_TEST_BOOT_READY_TIMEOUT", "60.0")
)
QEMU_SECONDS = os.environ.get("XHCI_TEST_QEMU_SECONDS", "120")
POLL_INTERVAL = float(os.environ.get("XHCI_TEST_POLL_INTERVAL", "0.25"))


def serial_text():
    if not SERIAL.exists():
        return ""

    try:
        return SERIAL.read_text(errors="replace")
    except OSError:
        # 无头模式下 QEMU 会直接写入此文件。文件扩展期间，Windows/WSL 挂载文件读取可能短暂返回
        # EAGAIN/ENODATA；下一轮轮询会重试。
        return ""


def fail(message):
    raise RuntimeError(message)


def wait_until(predicate, label, timeout=15.0):
    if POLL_INTERVAL <= 0.0:
        fail("XHCI_TEST_POLL_INTERVAL must be positive")
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        if predicate():
            print("PASS:", label)
            return

        time.sleep(POLL_INTERVAL)

    fail("timeout: " + label)


def hub_change_count():
    return serial_text().count(
        "OS_USB_RUNTIME_HUB_CHANGE"
    )


def latest_device_count():
    matches = re.findall(
        r"OS_USB_DEVICE_COUNT=(\d+)",
        serial_text()
    )

    if not matches:
        return 0

    return int(matches[-1])


class HMP:
    def __init__(self, path):
        self.sock = socket.socket(
            socket.AF_UNIX,
            socket.SOCK_STREAM
        )

        self.sock.settimeout(0.25)
        self.sock.connect(str(path))

        self._read_until_prompt(3.0)

    def _read_until_prompt(self, timeout):
        deadline = time.monotonic() + timeout
        data = bytearray()

        while time.monotonic() < deadline:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue

            if not chunk:
                break

            data.extend(chunk)

            if b"(qemu)" in data:
                break

        return data.decode(
            errors="replace"
        )

    def command(self, command, timeout=3.0):
        self.sock.sendall(
            (command + "\n").encode()
        )

        response = self._read_until_prompt(
            timeout
        )

        compact = response.replace(
            "\r",
            ""
        ).strip()

        if compact:
            print(
                "HMP>",
                command
            )

            for line in compact.splitlines():
                if line.strip() != "(qemu)":
                    print(
                        "   ",
                        line
                    )

        lowered = response.lower()

        bad = (
            "error:" in lowered or
            "unknown command" in lowered or
            "not found" in lowered or
            "duplicate id" in lowered
        )

        if bad:
            fail(
                "HMP command failed: " +
                command
            )

        return response

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass


def choose_usb_mouse(hmp):
    text = hmp.command(
        "info mice"
    )

    candidates = []

    for line in text.splitlines():
        m = re.search(
            r"Mouse\s+#(\d+)",
            line
        )

        if m:
            candidates.append(
                (
                    int(m.group(1)),
                    line
                )
            )

    if not candidates:
        fail(
            "QEMU monitor reports no mouse"
        )

    #
    # 优先选择明确标记为 QEMU USB 的鼠标。
    #
    for index, line in candidates:
        lowered = line.lower()

        if (
            "usb" in lowered and
            "mouse" in lowered
        ):
            hmp.command(
                f"mouse_set {index}"
            )

            return index

    #
    # 若当前 QEMU 版本使用了不同名称，热插入的 USB 鼠标通常是列表中的最新设备。
    #
    index = candidates[-1][0]

    hmp.command(
        f"mouse_set {index}"
    )

    return index


def send_mouse_events(hmp, count):
    direction = 1

    if MOUSE_EVENT_DELAY < 0.0:
        fail("XHCI_TEST_MOUSE_DELAY must not be negative")
    if HUB_SETTLE_SECONDS < 0.0:
        fail("XHCI_TEST_HUB_SETTLE must not be negative")

    for i in range(count):
        dx = 3 * direction
        dy = 1 if (i & 1) else -1
        hmp.command(
            f"mouse_move {dx} {dy}",
            timeout=1.0
        )

        direction = -direction

        #
        # 避免将宿主机的所有移动合并成一份来宾报告。
        #
        time.sleep(MOUSE_EVENT_DELAY)

    # 按键状态变化比单纯移动更适合作为端到端探测：即使无头 QEMU 前端合并了相对移动，
    # 也必须产生 boot-mouse 中断报告。
    hmp.command("mouse_button 1", timeout=1.0)
    time.sleep(MOUSE_EVENT_DELAY)
    hmp.command("mouse_button 0", timeout=1.0)


def print_serial_tail():
    s = serial_text()

    lines = [
        line
        for line in s.splitlines()
        if (
            "OS_XHCI" in line or
            "OS_USB" in line
        )
    ]

    print()
    print("=== SERIAL TAIL ===")

    for line in lines[-120:]:
        print(line)


def main():
    BUILD.mkdir(
        parents=True,
        exist_ok=True
    )

    for path in (
        SERIAL,
        MONITOR,
        RUNNER_STDOUT,
        RUNNER_STDERR,
    ):
        try:
            path.unlink()
        except FileNotFoundError:
            pass

    out = RUNNER_STDOUT.open("wb")
    err = RUNNER_STDERR.open("wb")

    runner = None
    hmp = None

    try:
        runner = subprocess.Popen(
            [
                str(ROOT / "run-qemu.sh"),
                "--headless",
                "--usb-nested",
                "--monitor-socket",
                str(MONITOR),
                "--seconds",
                QEMU_SECONDS,
                "--no-build",
            ],
            cwd=ROOT,
            stdout=out,
            stderr=err,
        )

        # ----------------------------------------------------
        # Monitor online
        # ----------------------------------------------------

        wait_until(
            lambda:
                MONITOR.exists(),
            "QEMU HMP socket online",
            12.0
        )

        # ----------------------------------------------------
        # Initial nested topology
        # ----------------------------------------------------

        wait_until(
            lambda:
                "OS_XHCI_MSIX_OK"
                in serial_text(),
            "MSI-X runtime ready",
            BOOT_READY_TIMEOUT
        )

        wait_until(
            lambda:
                latest_device_count() >= 3,
            "Hub -> Hub -> Mouse published",
            BOOT_READY_TIMEOUT
        )

        #
        # xHCI 可能在内核完成确定性启动自测前就已准备好 MSI-X。运行时边界显式启动持久延迟工作线程前，
        # 不要注入实时鼠标输入。
        #
        wait_until(
            lambda:
                "OS_DEFERRED_WORKER_OK"
                in serial_text(),
            "persistent deferred worker ready",
            BOOT_READY_TIMEOUT
        )

        # 延迟工作线程先于 graphics 工作线程发布。注入 HID 输入前等待完整的启动后调度器边界，
        # 否则首份报告可能与窗口工作线程启动竞争。
        wait_until(
            lambda:
                "OS_WINDOW_WORKER_OK"
                in serial_text(),
            "window worker ready",
            BOOT_READY_TIMEOUT
        )

        # 窗口工作线程在显示后的 user-runtime 自测前启动。自测仍在检查 VM、调度器和进程拆除时不要注入 HID 流量，
        # 否则热插拔测试可能制造无关的 USER_RUNTIME 超时。
        wait_until(
            lambda:
                "OS_USER_RUNTIME_SUBTESTS_OK"
                in serial_text(),
            "boot runtime self-tests ready",
            BOOT_READY_TIMEOUT
        )

        if "OS_STAGE_FAIL" in serial_text():
            fail(
                "boot runtime self-test failed before hotplug"
            )

        # 启动期间保持 monitor 服务在线，但等启动关键运行时自测完成后再连接客户端。活动 HMP 连接会明显改变
        # QEMU 主循环负载，在 TCG 上扰动时间窗口刻意收紧的自测。
        deadline = time.monotonic() + 5.0
        while True:
            try:
                hmp = HMP(MONITOR)
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.05)

        #
        # 先生成少量初始事件，刻意远低于 256，以便后续通过 256 里程碑证明重新插入后的 HID 工作正常。
        #
        choose_usb_mouse(hmp)

        send_mouse_events(
            hmp,
            8
        )

        wait_until(
            lambda:
                "OS_USB_MOUSE_EVENT_OK"
                in serial_text(),
            "initial nested mouse works",
            MOUSE_READY_TIMEOUT
        )

        if "OS_XHCI_HID_EVENTS_256" in serial_text():
            fail(
                "256 HID milestone occurred before hotplug test"
            )

        # 更改下游端口前，让 QEMU Hub 中断端点恢复到常规 NAK/重试状态，
        # 避免拓扑测试受最后一份鼠标报告的完成时序影响。
        time.sleep(HUB_SETTLE_SECONDS)

        baseline_changes = (
            hub_change_count()
        )

        print()
        print(
            "Initial Hub-change count:",
            baseline_changes
        )

        # ----------------------------------------------------
        # Disconnect mouse
        # ----------------------------------------------------

        print()
        print(
            "=== HOT UNPLUG nested mouse ==="
        )

        hmp.command(
            "device_del vm-nested-mouse"
        )

        wait_until(
            lambda:
                hub_change_count() >
                baseline_changes,
            "guest removed child subtree",
            10.0
        )

        if "OS_XHCI_FAIL=" in serial_text():
            fail(
                "xHCI failure during subtree removal"
            )

        removed_changes = (
            hub_change_count()
        )

        #
        # 复用设备 ID 前等待 QEMU 的删除请求完成。
        #
        time.sleep(0.5)

        # ----------------------------------------------------
        # Reconnect same physical Hub port
        # ----------------------------------------------------

        print()
        print(
            "=== HOT REPLUG nested mouse ==="
        )

        hmp.command(
            "device_add usb-mouse,"
            "id=vm-nested-mouse,"
            "bus=vm-xhci.0,"
            "port=1.1.1"
        )

        wait_until(
            lambda:
                hub_change_count() >
                removed_changes,
            "guest enumerated and published replacement child",
            12.0
        )

        # QEMU 的 device_add 是异步操作。Hub-change 标记表示 Slot 已发布，但宿主输入设备可能仍在加入
        # QEMU 活动鼠标列表。
        time.sleep(HUB_SETTLE_SECONDS)

        if "OS_XHCI_FAIL=" in serial_text():
            fail(
                "xHCI failure during child re-enumeration"
            )

        # ----------------------------------------------------
        # Prove new Slot.context owns HID runtime
        # ----------------------------------------------------

        print()
        print(
            "=== POST-REPLUG HID STRESS ==="
        )

        choose_usb_mouse(hmp)

        #
        # 发送足够多报告，即使宿主合并了少量移动，也能超过驱动的 256 次完成里程碑。
        #
        send_mouse_events(
            hmp,
            640
        )

        wait_until(
            lambda:
                "OS_XHCI_HID_EVENTS_256"
                in serial_text(),
            "replugged mouse reached >=256 HID completions",
            12.0
        )

        if (
            "OS_XHCI_FAIL="
            in serial_text() or
            "OS_XHCI_MSIX_FAIL" in serial_text() or
            "OS_STAGE_FAIL" in serial_text()
        ):
            fail(
                "runtime xHCI or boot-stage failure after replug"
            )

        final_changes = (
            hub_change_count()
        )

        if final_changes < baseline_changes + 2:
            fail(
                "expected separate disconnect and reconnect topology changes"
            )

        print()
        print(
            "========================================="
        )
        print(
            "V3.10.6 B10B-3 HUB HOTPLUG: PASS"
        )
        print(
            "========================================="
        )
        print()
        print(
            "Verified:"
        )
        print(
            "  Hub #2 port disconnect event"
        )
        print(
            "  child Slot subtree teardown"
        )
        print(
            "  DMA/resource release survived"
        )
        print(
            "  same physical port reconnect"
        )
        print(
            "  new Slot enumeration"
        )
        print(
            "  working context -> Slot.context publish"
        )
        print(
            "  replugged HID >=256 completions"
        )

        print_serial_tail()

        #
        # Clean QEMU exit.
        #
        hmp.command(
            "quit",
            timeout=2.0
        )

        runner.wait(
            timeout=5.0
        )

        return 0

    except Exception as exc:
        print()
        print(
            "========================================="
        )
        print(
            "V3.10.6 B10B-3 HUB HOTPLUG: FAIL"
        )
        print(
            "========================================="
        )
        print()
        print(
            str(exc)
        )

        print_serial_tail()

        if RUNNER_STDERR.exists():
            text = RUNNER_STDERR.read_text(
                errors="replace"
            ).strip()

            if text:
                print()
                print(
                    "=== QEMU STDERR ==="
                )
                print(text[-4000:])

        return 1

    finally:
        if hmp is not None:
            try:
                hmp.command(
                    "quit",
                    timeout=0.5
                )
            except Exception:
                pass

            hmp.close()

        if runner is not None:
            try:
                runner.wait(
                    timeout=2.0
                )
            except subprocess.TimeoutExpired:
                runner.terminate()

                try:
                    runner.wait(
                        timeout=2.0
                    )
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
