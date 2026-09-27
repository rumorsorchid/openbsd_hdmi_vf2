#!/usr/bin/env python3
"""
vf2uart.py - talk to a StarFive VisionFive 2 over its debug UART.

Only the Python standard library is used (termios), so this runs on the
python3 that ships with macOS and on any Linux.

  boot    PORT SPL ITB   UART-boot: send SPL to the mask ROM (XMODEM-1K),
                         then u-boot.itb to the SPL (YMODEM), then console.
                         Nothing is written to the board.
  flash   PORT SPL ITB   At a running U-Boot prompt: load both files with
                         loady, write them to SPI flash (SPL at 0x0, ITB at
                         0x100000) and read them back to verify.
  console PORT           Plain serial console. Ctrl-] quits.

The mask ROM only listens for a few seconds after power-on, so start
"boot" first and switch the board on afterwards.
"""

import binascii
import os
import select
import sys
import termios
import time
import tty
import zlib

SOH, STX, EOT, ACK, NAK, CAN, CRC = 0x01, 0x02, 0x04, 0x06, 0x15, 0x18, 0x43
PROMPT = b"# "
SPL_OFFSET = 0x0
ITB_OFFSET = 0x100000
SPL_MAX = 0xF0000       # the U-Boot environment area starts at 0xF0000
LOAD_ADDR = "${kernel_addr_r}"
OUR_VERSION = b"openbsd-hdmi-vf2"


def log(msg):
    sys.stderr.write(msg + "\n")
    sys.stderr.flush()


class Port:
    def __init__(self, path, baud=115200):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(self.fd)
        speed = getattr(termios, "B%d" % baud)
        attrs[0] = 0                                    # iflag
        attrs[1] = 0                                    # oflag
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[3] = 0                                    # lflag
        attrs[4] = speed
        attrs[5] = speed
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        termios.tcflush(self.fd, termios.TCIOFLUSH)
        self.pending = b""

    def write(self, data):
        view = memoryview(data)
        while view:
            select.select([], [self.fd], [], 5)
            try:
                n = os.write(self.fd, view)
            except BlockingIOError:
                continue
            view = view[n:]

    def read(self, timeout):
        """Return whatever arrives within timeout seconds (maybe b"")."""
        if self.pending:
            data, self.pending = self.pending, b""
            return data
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            return b""
        try:
            return os.read(self.fd, 4096)
        except BlockingIOError:
            return b""

    def getc(self, timeout):
        """One byte or None."""
        deadline = time.monotonic() + timeout
        while True:
            if self.pending:
                c, self.pending = self.pending[0], self.pending[1:]
                return c
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            data = self.read(left)
            if data:
                self.pending = data

    def drain(self, quiet=0.2):
        """Swallow input until the line has been quiet for a while."""
        self.pending = b""
        while self.read(quiet):
            pass

    def discard_input(self):
        """Drop whatever has arrived and not been read yet."""
        self.pending = b""
        termios.tcflush(self.fd, termios.TCIFLUSH)

    def close(self):
        os.close(self.fd)


def crc16(data):
    return binascii.crc_hqx(data, 0)


def send_block(port, seq, payload, size, pad, tries=10, ack_timeout=10,
               first=False):
    head = STX if size == 1024 else SOH
    block = payload.ljust(size, pad)
    frame = bytes([head, seq & 0xFF, 0xFF - (seq & 0xFF)]) + block
    frame += crc16(block).to_bytes(2, "big")
    for _ in range(tries):
        port.write(frame)
        # A receiver still polling may have sent one more 'C' while the
        # first block was on the wire; that is not a request to resend it.
        crossed = time.monotonic() + 1.0 if first else 0
        while True:
            c = port.getc(ack_timeout)
            if c != CRC or time.monotonic() >= crossed:
                break
        if c == ACK:
            return True
        if c == CAN:
            raise IOError("receiver cancelled the transfer")
    if tries == 1:
        return False
    raise IOError("block %d was not acknowledged" % seq)


def wait_for_crc_request(port, timeout, what):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        c = port.getc(deadline - time.monotonic())
        if c == CRC:
            return
        if c is not None and c not in (NAK,):
            sys.stdout.buffer.write(bytes([c]))
            sys.stdout.flush()
    raise TimeoutError("no XMODEM/YMODEM request from the %s" % what)


def wait_for_poll(port, timeout, what, run=1, quiet=0.5):
    """Wait until the receiver is polling and saying nothing else: `run`
    'C' bytes in a row and `quiet` seconds without any other byte.  Text
    before the poll is shown, and a 'C' inside it is not taken for one.

    The JH7110 boot ROM needs run=5: it prints "(C)StarFive" before it
    starts polling and once more a moment later, and a sender that starts
    on an early C has that second greeting arrive where it expects an ACK.
    """
    deadline = time.monotonic() + timeout
    seen = 0
    last_other = time.monotonic()
    while time.monotonic() < deadline:
        c = port.getc(0.1)
        if c == CRC:
            seen += 1
        elif c is not None:
            # The C's held back were text after all
            text = b"C" * seen + (bytes([c]) if c != NAK else b"")
            seen = 0
            last_other = time.monotonic()
            sys.stdout.buffer.write(text)
            sys.stdout.flush()
        if seen >= run and time.monotonic() - last_other >= quiet:
            # Polls that queued up while we waited are not replies
            port.discard_input()
            return
    raise TimeoutError("no XMODEM/YMODEM request from the %s" % what)


# Offset of the version word in the JH7110's sfspl header
SFSPL_VERSION_OFFSET = 0x284
SFSPL_VERSION = b"\x01\x01\x01\x01"
ITB_MAX = 0xF00000      # the FIT slot runs from 0x100000 to the end of flash


def check_images(spl, itb):
    """The boot ROM checks a header it only finds in the .normal.out image;
    the plain u-boot-spl.bin transfers fine and then boots nothing."""
    if spl[SFSPL_VERSION_OFFSET:SFSPL_VERSION_OFFSET + 4] != SFSPL_VERSION:
        raise ValueError("the SPL is not an sfspl image; use "
                         "u-boot-spl.bin.normal.out, not u-boot-spl.bin")
    if len(spl) > SPL_MAX:
        raise ValueError("SPL is larger than its flash slot")
    if len(itb) > ITB_MAX:
        raise ValueError("u-boot.itb is larger than its flash slot")


def send_eot(port):
    for _ in range(10):
        port.write(bytes([EOT]))
        c = port.getc(5)
        if c == ACK:
            return
    raise IOError("end of transfer was not acknowledged")


def progress(done, total, what):
    sys.stderr.write("\r  %s: %d/%d KiB" % (what, done // 1024, total // 1024))
    sys.stderr.flush()


def xmodem1k_send(port, data, what):
    seq = 1
    for off in range(0, len(data), 1024):
        send_block(port, seq, data[off:off + 1024], 1024, b"\x1a",
                   first=seq == 1)
        seq += 1
        progress(min(off + 1024, len(data)), len(data), what)
    send_eot(port)
    sys.stderr.write("\n")


def ymodem_send(port, name, data, what):
    header = name.encode() + b"\0" + str(len(data)).encode() + b"\0"
    send_block(port, 0, header, 128, b"\0", first=True)
    wait_for_crc_request(port, 10, what)
    seq = 1
    for off in range(0, len(data), 1024):
        send_block(port, seq, data[off:off + 1024], 1024, b"\x1a",
                   first=seq == 1)
        seq += 1
        progress(min(off + 1024, len(data)), len(data), what)
    send_eot(port)
    sys.stderr.write("\n")
    # End of batch: an empty block 0 once the receiver asks again. Some
    # receivers (lrzsz rb) finish without acknowledging it.
    wait_for_crc_request(port, 10, what)
    send_block(port, 0, b"", 128, b"\0", tries=1, ack_timeout=2)


def console(port):
    log("--- console: Ctrl-] quits ---")
    stdin = sys.stdin.fileno()
    saved = termios.tcgetattr(stdin) if os.isatty(stdin) else None
    try:
        if saved:
            tty.setraw(stdin)
        if port.pending:
            os.write(sys.stdout.fileno(), port.pending)
            port.pending = b""
        while True:
            r, _, _ = select.select([stdin, port.fd], [], [])
            if port.fd in r:
                data = port.read(0)
                if data:
                    os.write(sys.stdout.fileno(), data)
            if stdin in r:
                data = os.read(stdin, 1024)
                if not data or b"\x1d" in data:
                    break
                port.write(data.replace(b"\n", b"\r") if not saved else data)
    finally:
        if saved:
            termios.tcsetattr(stdin, termios.TCSADRAIN, saved)
        log("\n--- console closed ---")


def read_file(path):
    with open(path, "rb") as f:
        return f.read()


def cmd_boot(path, spl, itb):
    spl_data, itb_data = read_file(spl), read_file(itb)
    check_images(spl_data, itb_data)
    port = Port(path)
    log("Waiting for the mask ROM. Power the board on now (boot switches")
    log("RGPIO_0 and RGPIO_1 both at H = UART boot)...")
    port.drain()
    wait_for_poll(port, 300, "mask ROM", run=5)
    log("\nROM is asking; sending the SPL (XMODEM-1K)")
    xmodem1k_send(port, spl_data, "SPL")
    log("Waiting for the SPL to bring up DRAM and ask for u-boot.itb...")
    wait_for_poll(port, 60, "SPL")
    log("\nSending u-boot.itb (YMODEM)")
    ymodem_send(port, os.path.basename(itb), itb_data, "u-boot.itb")
    console(port)
    port.close()


def run(port, cmd, timeout=30, expect=PROMPT):
    """Send a U-Boot command, return its output up to the next prompt."""
    port.drain()
    port.write(cmd.encode() + b"\r")
    out = b""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        data = port.read(0.5)
        if data:
            out += data
            if out.rstrip().endswith(expect.rstrip()):
                return out.decode(errors="replace")
    raise TimeoutError("U-Boot did not finish %r:\n%s"
                       % (cmd, out.decode(errors="replace")))


def uboot_crc32(output):
    # "crc32 for 44000000 ... 44025249 ==> 1a2b3c4d"
    for line in output.splitlines():
        if "==>" in line:
            return int(line.split("==>")[1].strip()[:8], 16)
    raise ValueError("no crc32 result in:\n" + output)


def transfer_to_ram(port, name, data):
    port.drain()
    port.write(("loady %s\r" % LOAD_ADDR).encode())
    wait_for_poll(port, 15, "U-Boot loady")
    ymodem_send(port, name, data, name)
    port.drain(1.0)


def load_and_write(port, name, data, offset):
    want = zlib.crc32(data) & 0xFFFFFFFF
    log("%s: %d bytes, crc32 %08x, to SPI offset %#x"
        % (name, len(data), want, offset))
    transfer_to_ram(port, name, data)

    got = uboot_crc32(run(port, "crc32 %s %#x" % (LOAD_ADDR, len(data))))
    if got != want:
        raise IOError("%s arrived damaged in RAM (crc32 %08x)" % (name, got))

    out = run(port, "sf update %s %#x %#x" % (LOAD_ADDR, offset, len(data)),
              timeout=300)
    log(out.strip())
    if "ERROR" in out or "rror" in out.split("\n", 1)[-1]:
        raise IOError("sf update failed")

    # Read it back from flash over the RAM copy and compare.
    run(port, "mw.b %s 0 %#x" % (LOAD_ADDR, len(data)))
    run(port, "sf read %s %#x %#x" % (LOAD_ADDR, offset, len(data)),
        timeout=120)
    got = uboot_crc32(run(port, "crc32 %s %#x" % (LOAD_ADDR, len(data))))
    if got != want:
        raise IOError("%s read back from flash with crc32 %08x, expected "
                      "%08x -- do not power off; flash again" % (name, got, want))
    log("%s verified in SPI flash" % name)


def cmd_flash(path, spl, itb, force=False):
    spl_data, itb_data = read_file(spl), read_file(itb)
    check_images(spl_data, itb_data)
    port = Port(path)
    port.drain()
    out = run(port, "version", timeout=10)
    log(out.strip())
    if OUR_VERSION not in out.encode() and not force:
        raise RuntimeError(
            "The running U-Boot is not this project's build. UART-boot it "
            "first (so you know it works on your board), then flash from "
            "its prompt. Use --force to flash anyway.")
    # Serial-only input while transferring: polling the USB keyboard between
    # characters can let the UART FIFO overflow during loady.
    run(port, "setenv stdin serial", timeout=10)
    out = run(port, "sf probe", timeout=20)
    log(out.strip())
    if "SF: Detected" not in out:
        raise IOError("SPI flash not detected")
    load_and_write(port, "u-boot-spl.bin.normal.out", spl_data, SPL_OFFSET)
    load_and_write(port, "u-boot.itb", itb_data, ITB_OFFSET)
    log("Done. Power off, set RGPIO_0 and RGPIO_1 back to L (flash boot) "
        "and power on.")
    port.close()


def main(argv):
    if len(argv) >= 2 and argv[1] == "console" and len(argv) == 3:
        port = Port(argv[2])
        console(port)
        return 0
    if len(argv) >= 5 and argv[1] == "boot":
        cmd_boot(argv[2], argv[3], argv[4])
        return 0
    if len(argv) >= 5 and argv[1] == "flash":
        cmd_flash(argv[2], argv[3], argv[4], force="--force" in argv[5:])
        return 0
    sys.stderr.write(__doc__)
    return 64


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except KeyboardInterrupt:
        sys.exit(130)
    except (IOError, OSError, ValueError, RuntimeError, TimeoutError) as e:
        log("\nerror: %s" % e)
        sys.exit(1)
