#!/usr/bin/env python3
import sys, time, serial
def find_port():
    """$STRUMENTO_PORT, else the first USB serial device that looks like a Core2."""
    import glob, os
    if os.environ.get("STRUMENTO_PORT"): return os.environ["STRUMENTO_PORT"]
    for pat in ("/dev/cu.usbserial*","/dev/cu.wchusbserial*","/dev/cu.SLAB_USBtoUART*",
                "/dev/ttyUSB*","/dev/ttyACM*"):
        hits=sorted(glob.glob(pat))
        if hits: return hits[0]
    raise SystemExit("No USB serial port found - plug in the Core2 or set STRUMENTO_PORT")
p = serial.Serial(find_port(), 115200, timeout=0.2)
# pulse reset (RTS=EN on Core2)
p.setDTR(False); p.setRTS(True); time.sleep(0.1); p.setRTS(False); time.sleep(0.1)
deadline = time.time() + float(sys.argv[1] if len(sys.argv)>1 else 30)
while time.time() < deadline:
    d = p.read(4096)
    if d:
        sys.stdout.write(d.decode("utf-8","replace")); sys.stdout.flush()
