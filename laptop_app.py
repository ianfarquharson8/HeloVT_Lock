"""Laptop app: click a button to toggle the XIAO nRF52840's LED over BLE.

Install:  pip install bleak
Run:      python laptop_app.py
"""

import asyncio
import threading
import tkinter as tk

from bleak import BleakClient, BleakScanner

# These must match the UUIDs in xiao_led_ble.ino
DEVICE_NAME = "XIAO-LED"
LED_CHAR_UUID = "19b10001-e8f2-537e-4f6c-d104768a1214"


class BleWorker:
    """Runs an asyncio loop in a background thread so the GUI stays responsive."""

    def __init__(self):
        self.loop = asyncio.new_event_loop()
        self.client = None
        threading.Thread(target=self.loop.run_forever, daemon=True).start()

    def run(self, coro, callback=None):
        future = asyncio.run_coroutine_threadsafe(coro, self.loop)
        if callback:
            future.add_done_callback(callback)

    async def connect(self):
        device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=10)
        if device is None:
            raise RuntimeError(f"Could not find '{DEVICE_NAME}'")
        self.client = BleakClient(device)
        await self.client.connect()

    async def set_led(self, on):
        if not self.client or not self.client.is_connected:
            await self.connect()
        await self.client.write_gatt_char(LED_CHAR_UUID, bytes([1 if on else 0]), response=True)

    async def disconnect(self):
        if self.client and self.client.is_connected:
            await self.client.disconnect()


class App:
    def __init__(self, root):
        self.root = root
        self.ble = BleWorker()
        self.led_on = False

        root.title("XIAO LED Control")
        root.geometry("300x180")

        self.status = tk.Label(root, text="Disconnected", fg="gray")
        self.status.pack(pady=10)

        self.button = tk.Button(root, text="Turn LED ON", width=18, height=2,
                                command=self.toggle)
        self.button.pack(pady=10)

        root.protocol("WM_DELETE_WINDOW", self.on_close)

    def set_status(self, text, color="black"):
        self.root.after(0, lambda: self.status.config(text=text, fg=color))

    def toggle(self):
        target = not self.led_on
        self.button.config(state=tk.DISABLED)
        self.set_status("Sending...", "orange")

        def done(future):
            def update():
                self.button.config(state=tk.NORMAL)
                err = future.exception()
                if err:
                    self.status.config(text=f"Error: {err}", fg="red")
                    return
                self.led_on = target
                self.button.config(text="Turn LED OFF" if target else "Turn LED ON")
                self.status.config(text=f"Connected - LED {'ON' if target else 'OFF'}",
                                   fg="green")
            self.root.after(0, update)

        self.ble.run(self.ble.set_led(target), done)

    def on_close(self):
        self.ble.run(self.ble.disconnect())
        self.root.after(300, self.root.destroy)


if __name__ == "__main__":
    root = tk.Tk()
    App(root)
    root.mainloop()
