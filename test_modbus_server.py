#!/usr/bin/env python3
"""
Minimal Modbus TCP server for testing Event Engine modbus_read trigger.
No third-party libraries required -- pure Python sockets.

Slave 1 registers:
  FC03 (Holding) reg 0 = 42  (toggles to 99 every 15s)
  FC04 (Input)   reg 0 = 1234
  FC01 (Coil)    bit 0 = 1
  FC02 (Discrete)bit 0 = 0

Listen port: 5020  (non-privileged, avoids need for sudo)
"""

import socket, struct, threading, time, logging

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")

HOST = "0.0.0.0"
PORT = 5020

# Register storage (indexed by address, 0-based)
holding   = {0: 42}    # FC03
inputs    = {0: 1234}  # FC04
coils     = {0: 1}     # FC01
discrete  = {0: 0}     # FC02


def handle_request(data):
    """Parse a Modbus TCP ADU and return the response bytes, or None on error."""
    if len(data) < 8:
        return None
    tid, pid, length, unit = struct.unpack(">HHHB", data[:7])
    fc = data[7]

    if fc == 0x01:  # Read Coils
        addr, count = struct.unpack(">HH", data[8:12])
        bits = [coils.get(addr + i, 0) for i in range(count)]
        byte_count = (count + 7) // 8
        packed = 0
        for i, b in enumerate(bits):
            packed |= (b & 1) << i
        body = bytes([fc, byte_count]) + packed.to_bytes(byte_count, "little")
        logging.info(f"FC01 coil[{addr}] -> {bits}")

    elif fc == 0x02:  # Read Discrete Inputs
        addr, count = struct.unpack(">HH", data[8:12])
        bits = [discrete.get(addr + i, 0) for i in range(count)]
        byte_count = (count + 7) // 8
        packed = 0
        for i, b in enumerate(bits):
            packed |= (b & 1) << i
        body = bytes([fc, byte_count]) + packed.to_bytes(byte_count, "little")
        logging.info(f"FC02 discrete[{addr}] -> {bits}")

    elif fc == 0x03:  # Read Holding Registers
        addr, count = struct.unpack(">HH", data[8:12])
        regs = [holding.get(addr + i, 0) for i in range(count)]
        body = bytes([fc, count * 2]) + b"".join(struct.pack(">H", r) for r in regs)
        logging.info(f"FC03 holding[{addr}] -> {regs}")

    elif fc == 0x04:  # Read Input Registers
        addr, count = struct.unpack(">HH", data[8:12])
        regs = [inputs.get(addr + i, 0) for i in range(count)]
        body = bytes([fc, count * 2]) + b"".join(struct.pack(">H", r) for r in regs)
        logging.info(f"FC04 input[{addr}] -> {regs}")

    elif fc == 0x05:  # Write Single Coil
        addr, value = struct.unpack(">HH", data[8:12])
        coils[addr] = 1 if value == 0xFF00 else 0
        body = bytes([fc]) + data[8:12]
        logging.info(f"FC05 write coil[{addr}] = {coils[addr]}")

    elif fc == 0x06:  # Write Single Register
        addr, value = struct.unpack(">HH", data[8:12])
        holding[addr] = value
        body = bytes([fc]) + data[8:12]
        logging.info(f"FC06 write holding[{addr}] = {value}")

    else:
        # Exception response
        body = bytes([fc | 0x80, 0x01])
        logging.warning(f"Unsupported FC {fc:#04x}")

    mbap = struct.pack(">HHHB", tid, 0, 1 + len(body), unit)
    return mbap + body


def client_thread(conn, addr):
    logging.info(f"Connection from {addr}")
    conn.settimeout(30)
    try:
        while True:
            # Read MBAP header (7 bytes)
            hdr = b""
            while len(hdr) < 7:
                chunk = conn.recv(7 - len(hdr))
                if not chunk:
                    return
                hdr += chunk
            length = struct.unpack(">H", hdr[4:6])[0]
            # Read PDU (length - 1 remaining bytes after unit id)
            pdu = b""
            remaining = length - 1
            while len(pdu) < remaining:
                chunk = conn.recv(remaining - len(pdu))
                if not chunk:
                    return
                pdu += chunk
            response = handle_request(hdr + pdu)
            if response:
                conn.sendall(response)
    except (OSError, TimeoutError):
        pass
    finally:
        conn.close()
        logging.info(f"Disconnected {addr}")


def toggle_holding():
    """Toggle holding reg 0 between 42 and 99 every 15 seconds."""
    while True:
        time.sleep(15)
        holding[0] = 99 if holding[0] == 42 else 42
        logging.info(f"*** Holding reg 0 toggled to {holding[0]} ***")


if __name__ == "__main__":
    logging.info(f"Modbus TCP server starting on {HOST}:{PORT}")
    logging.info("Slave 1 | FC03 reg 0=42 (toggles 42<->99 every 15s) | FC04 reg 0=1234 | FC01 bit 0=1 | FC02 bit 0=0")
    threading.Thread(target=toggle_holding, daemon=True).start()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, PORT))
    srv.listen(10)
    logging.info(f"Listening on port {PORT}. Connect from camera: host=10.129.174.108 port={PORT} slave=1")
    while True:
        conn, addr = srv.accept()
        threading.Thread(target=client_thread, args=(conn, addr), daemon=True).start()

