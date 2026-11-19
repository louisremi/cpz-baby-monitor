#!/usr/bin/env python3
"""On-device end-to-end driver (run as root on a CardputerZero).

  e2e_driver.py start   stop APPLaunch, create a virtual keyboard, launch the
                        app as the desktop user, then serve commands from
                        /tmp/cpzbm-e2e.fifo (daemonized)
  e2e_driver.py cmd "key 4" | "type 123456" | "shot /tmp/s.png" | "stop"
  e2e_driver.py cmd "key volup"   (also voldown, mute)

The app reads keys from the uinput device via CAP_LORA_KEYBOARD_DEVICE, so the
real keyboard and launcher are untouched; "stop" restores APPLaunch.
"""
import os
import subprocess
import sys
import time

FIFO = "/tmp/cpzbm-e2e.fifo"
LOG = "/tmp/cpzbm-e2e.log"
APP = "/usr/share/cpz-baby-monitor/bin/cpz-baby-monitor"
USER = "louisremi"


def user_env():
    uid = int(subprocess.check_output(["id", "-u", USER]).strip())
    return uid, f"XDG_RUNTIME_DIR=/run/user/{uid}"


def launcher(action):
    uid, env = user_env()
    subprocess.call(["su", USER, "-c", f"{env} systemctl --user {action} APPLaunch.service"])


def shot(path):
    from PIL import Image
    with open("/sys/class/graphics/fb0/virtual_size") as f:
        w, h = (int(v) for v in f.read().strip().split(","))
    with open("/sys/class/graphics/fb0/bits_per_pixel") as f:
        bpp = int(f.read())
    with open("/dev/fb0", "rb") as f:
        raw = f.read(w * h * bpp // 8)
    if bpp == 16:
        img = Image.frombytes("RGB", (w, h), raw, "raw", "BGR;16")
    else:
        img = Image.frombytes("RGBA", (w, h), raw, "raw", "BGRA").convert("RGB")
    img.save(path)


def serve():
    import libevdev
    from libevdev import InputEvent, evbit

    dev = libevdev.Device()
    dev.name = "cpzbm-e2e-keyboard"
    keys = {}
    for c in "0123456789abcdefghijklmnopqrstuvwxyz":
        code = evbit("EV_KEY", "KEY_" + c.upper())
        keys[c] = code
        dev.enable(code)
    named = {
        "esc": "KEY_ESC",
        "enter": "KEY_ENTER",
        "bs": "KEY_BACKSPACE",
        "up": "KEY_UP",
        "down": "KEY_DOWN",
        # fn+S / fn+D / fn+A on the real keyboard: the compositor runs
        # wfpanelctl, the app plays the confirmation tone.
        "volup": "KEY_VOLUMEUP",
        "voldown": "KEY_VOLUMEDOWN",
        "mute": "KEY_MUTE",
    }
    for k, v in named.items():
        keys[k] = evbit("EV_KEY", v)
        dev.enable(keys[k])
    ui = dev.create_uinput_device()
    os.chmod(ui.devnode, 0o666)
    time.sleep(0.5)

    launcher("stop")
    uid, env = user_env()
    log = open(LOG, "w")
    # Launch in a transient user scope, like APPLaunch does (a user service):
    # polkit decisions such as NetworkManager's Wi-Fi switch depend on the
    # process being part of the user's logind session.
    app = subprocess.Popen(
        ["su", USER, "-c",
         f"{env} systemd-run --user --scope --quiet --collect "
         f"--setenv=CAP_LORA_KEYBOARD_DEVICE={ui.devnode} -- {APP}"],
        stdout=log, stderr=subprocess.STDOUT)

    def press(code):
        ui.send_events([InputEvent(code, 1), InputEvent(evbit("EV_SYN", "SYN_REPORT"), 0)])
        time.sleep(0.05)
        ui.send_events([InputEvent(code, 0), InputEvent(evbit("EV_SYN", "SYN_REPORT"), 0)])
        time.sleep(0.15)

    if os.path.exists(FIFO):
        os.unlink(FIFO)
    os.mkfifo(FIFO)
    try:
        while True:
            with open(FIFO) as f:
                for line in f:
                    parts = line.strip().split(" ", 1)
                    if not parts or not parts[0]:
                        continue
                    op, arg = parts[0], parts[1] if len(parts) > 1 else ""
                    if op == "key":
                        press(keys[arg])
                    elif op == "type":
                        for ch in arg:
                            press(keys[ch])
                    elif op == "shot":
                        shot(arg)
                    elif op == "stop":
                        raise SystemExit
    finally:
        # pkill needs -f: the process name is truncated to 15 characters.
        subprocess.call(["pkill", "-INT", "-f", f"^{APP}$"])
        app.terminate()
        try:
            app.wait(5)
        except subprocess.TimeoutExpired:
            app.kill()
        launcher("start")
        os.unlink(FIFO)


def main():
    if sys.argv[1] == "start":
        if os.fork() == 0:
            os.setsid()
            sys.stdout = sys.stderr = open("/tmp/cpzbm-e2e-driver.log", "w")
            serve()
            os._exit(0)
        for _ in range(50):
            if os.path.exists(FIFO):
                print("driver ready")
                return
            time.sleep(0.2)
        print("driver failed to start; see /tmp/cpzbm-e2e-driver.log")
        sys.exit(1)
    elif sys.argv[1] == "cmd":
        with open(FIFO, "w") as f:
            f.write(sys.argv[2] + "\n")
        time.sleep(0.3)


if __name__ == "__main__":
    main()
