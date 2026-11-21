#!/usr/bin/env python3
"""Drive the Lilka browser over serial.

Usage:
  tools/serial_test.py [port] "goto https://..." "back" "dump"
`dump` writes the last fetched HTML to /tmp/last_dump.html.
"""
import sys, time
import serial

def main():
    args = sys.argv[1:]
    port = args[0] if args and args[0].startswith("/dev") else "/dev/cu.usbmodem101"
    cmds = [a for a in args if not a.startswith("/dev")]

    s = serial.Serial(port, 115200, timeout=1)
    time.sleep(1.5)
    s.reset_input_buffer()

    for c in cmds:
        s.reset_input_buffer()
        print(f"\n>> {c}")
        s.write((c + "\n").encode())
        deadline = time.time() + (40 if c.startswith("dump") else 12)
        buf = b""
        while time.time() < deadline:
            if s.in_waiting:
                buf += s.read(s.in_waiting)
            if c.startswith("dump"):
                if b"DUMPEND" in buf:
                    break
                if time.time() > deadline - 5 and b"DUMPBEGIN" not in buf:
                    break
        if c.startswith("dump"):
            i = buf.find(b"DUMPBEGIN ")
            if i >= 0:
                j = buf.find(b"\n", i)
                length = int(buf[i + 10:j])
                start = j + 1
                data = buf[start:start + length]
                open("/tmp/last_dump.html", "wb").write(data)
                print(f"dumped {len(data)}/{length} bytes -> /tmp/last_dump.html")
                continue
            else:
                print("no DUMPBEGIN in output")
                continue
        text = buf.decode("utf-8", "replace")
        for line in text.splitlines():
            if line.strip():
                print(line)

if __name__ == "__main__":
    main()
