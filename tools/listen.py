#!/usr/bin/env python3
"""Keep the serial port open, send an optional command, then stream output
continuously for a fixed window so slow crashes (assert/backtrace/reboot) are
captured with the surrounding context.

Usage:
  tools/listen.py [port] [SECONDS] ["goto https://..."]
"""
import sys, time
import serial

def main():
    a = sys.argv[1:]
    port = a[0] if a and a[0].startswith("/dev") else "/dev/cu.usbmodem101"
    secs = 150
    cmd = None
    for x in a[1:]:
        if x.startswith("/dev"):
            continue
        if x.isdigit():
            secs = int(x)
        else:
            cmd = x

    s = serial.Serial(port, 115200, timeout=0.5)
    time.sleep(1.0)
    s.reset_input_buffer()

    t0 = time.time()
    end = t0 + secs
    if cmd:
        print(f"\n>> {cmd}", flush=True)
        s.write((cmd + "\n").encode())
    print(f"--- listening {secs}s from {time.strftime('%H:%M:%S')} ---", flush=True)

    while time.time() < end:
        n = s.in_waiting
        if n:
            data = s.read(n)
            line = data.decode("utf-8", "replace")
            # timestamp each chunk for a timeline
            print(f"[{time.strftime('%H:%M:%S')}] {line}", end="", flush=True)

if __name__ == "__main__":
    main()
