"""Laptop app: unlock the HeloVT lock (XIAO nRF52840) over BLE.

Every command is signed with the secret key in lock_key.txt (see xiao_led_ble.ino for the protocol).
It connects to the lock named in lock_key.txt.

Install:  pip install bleak
Setup:    python make_key.py   (once; then upload the firmware)
Run:      python laptop_app.py
"""

import asyncio
import hashlib
import hmac
import threading
import tkinter as tk
from pathlib import Path

from bleak import BleakClient, BleakScanner

# These must match the UUIDs in xiao_led_ble.ino
STATE_CHAR_UUID = "19b10101-e8f2-537e-4f6c-d104768a1214"
NONCE_CHAR_UUID = "19b10104-e8f2-537e-4f6c-d104768a1214"
COMMAND_CHAR_UUID = "19b10105-e8f2-537e-4f6c-d104768a1214"

CMD_LOCK, CMD_UNLOCK = 0, 1
RESULT_TEXT = {0: "no command yet", 1: "OK", 2: "rejected: wrong key",
               4: "rejected: bad command", 5: "solenoid cooling down, try again shortly"}
RES_OK = 1

KEY_FILE = Path(__file__).with_name("lock_key.txt")


def load_key():
    """Returns (key, lock name) from lock_key.txt."""
    try:
        lines = KEY_FILE.read_text().split()
    except FileNotFoundError:
        raise SystemExit(f"{KEY_FILE.name} not found. Run `python make_key.py` first.")
    if len(lines) < 2:
        raise SystemExit(f"{KEY_FILE.name} has no lock name. Run `python make_key.py` again "
                         "(it keeps the key), then re-upload the firmware.")
    return bytes.fromhex(lines[0]), lines[1]


class BleWorker:
    """Runs an asyncio loop in a background thread so the GUI stays responsive."""

    def __init__(self, key, name, on_state):
        self.key = key
        self.name = name
        self.on_state = on_state  # called with (unlocked, result) on every State notification
        self.loop = asyncio.new_event_loop()
        self.client = None
        self.state_event = None
        self.last_state = None
        threading.Thread(target=self.loop.run_forever, daemon=True).start()

    def run(self, coro, callback=None):
        future = asyncio.run_coroutine_threadsafe(coro, self.loop)
        if callback:
            future.add_done_callback(callback)

    def _handle_state(self, _char, data):
        self.last_state = (bool(data[0]), data[1])
        self.state_event.set()
        self.on_state(*self.last_state)

    async def connect(self):
        device = await BleakScanner.find_device_by_name(self.name, timeout=10)
        if device is None:
            raise RuntimeError(f"Could not find '{self.name}'")
        self.client = BleakClient(device)
        await self.client.connect()
        self.state_event = asyncio.Event()
        await self.client.start_notify(STATE_CHAR_UUID, self._handle_state)

    async def send(self, cmd):
        """Sign and send a command. Returns (unlocked, result) as reported by the board."""
        if not self.client or not self.client.is_connected:
            await self.connect()
        nonce = await self.client.read_gatt_char(NONCE_CHAR_UUID)
        mac = hmac.new(self.key, bytes(nonce) + bytes([cmd]), hashlib.sha256).digest()
        self.state_event.clear()
        await self.client.write_gatt_char(COMMAND_CHAR_UUID, bytes([cmd]) + mac, response=True)
        await asyncio.wait_for(self.state_event.wait(), timeout=3)
        return self.last_state

    async def disconnect(self):
        if self.client and self.client.is_connected:
            await self.client.disconnect()


class App:
    def __init__(self, root):
        self.root = root
        key, name = load_key()
        self.ble = BleWorker(key, name, self.on_state)
        self.unlocked = False

        root.title(f"HeloVT Lock - {name}")
        root.geometry("320x180")

        self.status = tk.Label(root, text="Disconnected", fg="gray", wraplength=300)
        self.status.pack(pady=10)

        self.button = tk.Button(root, text="Unlock", width=18, height=2, command=self.press)
        self.button.pack(pady=10)

        root.protocol("WM_DELETE_WINDOW", self.on_close)

    def show(self, unlocked, result=None):
        self.unlocked = unlocked
        self.button.config(text="Lock now" if unlocked else "Unlock")
        text = "UNLOCKED (relocks automatically)" if unlocked else "Locked"
        color = "green"
        if result is not None and result != RES_OK:
            text += f" - {RESULT_TEXT.get(result, f'error {result}')}"
            color = "red"
        self.status.config(text=f"Connected - {text}", fg=color)

    def on_state(self, unlocked, result):
        # Board-initiated changes, e.g. the auto-relock. Runs on the BLE thread.
        self.root.after(0, lambda: self.show(unlocked))

    def press(self):
        cmd = CMD_LOCK if self.unlocked else CMD_UNLOCK
        self.button.config(state=tk.DISABLED)
        self.status.config(text="Sending...", fg="orange")

        def done(future):
            def update():
                self.button.config(state=tk.NORMAL)
                err = future.exception()
                if err:
                    self.status.config(text=f"Error: {err!r}", fg="red")
                    return
                self.show(*future.result())
            self.root.after(0, update)

        self.ble.run(self.ble.send(cmd), done)

    def on_close(self):
        self.ble.run(self.ble.disconnect())
        self.root.after(300, self.root.destroy)


if __name__ == "__main__":
    root = tk.Tk()
    App(root)
    root.mainloop()
