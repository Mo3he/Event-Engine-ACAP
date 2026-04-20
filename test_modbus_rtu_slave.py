#!/usr/bin/env python3
"""
Modbus RTU slave over TCP -- for RS-485 testing via Axis PortManager.

Connect to A8207 (.218) GenericTCPServer which bridges the RS-485 port to TCP.
D3110 (.159) ACAP sends RTU frames over its RS-485 port (via PortManager on port 4001).
The two devices are wired together (A+ to A+, B- to B-).

Slave ID: 1
Registers (FC03 holding):
  0 = 42  (toggles to 99 every 15s)
  1 = 1234
FC04 input reg 0 = 5678
FC01 coil 0 = 1
FC02 discrete 0 = 0

Usage:
  python3 test_modbus_rtu_slave.py [host] [port]
  python3 test_modbus_rtu_slave.py 10.129.174.218 4001
"""

import socket, struct, time, logging, sys, threading

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")

SLAVE_ID = 1
HOST = sys.argv[1] if len(sys.argv) > 1 else "10.129.174.218"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 4001

holding  = {0: 42, 1: 1234}
inputs   = {0: 5678}
coils    = {0: 1}
discrete = {0: 0}


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 0x0001:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


def append_crc(data: bytes) -> bytes:
    crc = crc16(data)
    return data + struct.pack("<H", crc)


def check_crc(data: bytes) -> bool:
    if len(data) < 3:
        return False
    payload, recv_crc = data[:-2], struct.unpack("<H", data[-2:])[0]
    return crc16(payload) == recv_crc


def build_response(req: bytes) -> bytes | None:
    if len(req) < 6:
        return None
    slave = req[0]
    if slave != SLAVE_ID:
        return None  # not for us
    if not check_crc(req):
        logging.warning("CRC mismatch")
        return None

    fc = req[1]

    if fc == 0x01:  # Read Coils
        addr, count = struct.unpack(">HH", req[2:6])
        bits = [coils.get(addr + i, 0) for i in range(count)]
        byte_count = (count + 7) // 8
        packed = 0
        for i, b in enumerate(bits):
            packed |= (b & 1) << i
        body = bytes([slave, fc, byte_count]) + packed.to_bytes(byte_count, "little")
        logging.info(f"FC01 coil[{addr}:{addr+count}] -> {bits}")
        return append_crc(body)

    elif fc == 0x02:  # Read Discrete Inputs
        addr, count = struct.unpack(">HH", req[2:6])
        bits = [discrete.get(addr + i, 0) for i in range(count)]
        byte_count = (count + 7) // 8
        packed = 0
        for i, b in enumerate(bits):
            packed |= (b & 1) << i
        body = bytes([slave, fc, byte_count]) + packed.to_bytes(byte_count, "little")
        logging.info(f"FC02 discrete[{addr}:{addr+count}] -> {bits}")
        return append_crc(body)

    elif fc == 0x03:  # Read Holding Registers
        addr, count = struct.unpack(">HH", req[2:6])
        regs = [holding.get(addr + i, 0) for i in range(count)]
        body = bytes([slave, fc, count * 2]) + b"".join(struct.pack(">H", r) for r in regs)
        logging.info(f"FC03 holding[{addr}:{addr+count}] -> {regs}")
        return append_crc(body)

    elif fc == 0x04:  # Read Input Registers
        addr, count = struct.unpack(">HH", req[2:6])
        regs = [inputs.get(addr + i, 0) for i in range(count)]
        body = bytes([slave, fc, count * 2]) + b"".join(struct.pack(">H", r) for r in regs)
        logging.info(f"FC04 input[{addr}:{addr+count}] -> {regs}")
        return append_crc(body)

    else:
        # Exception: illegal function
        body = bytes([slave, fc | 0x80, 0x01])
        logging.warning(f"Unsupported FC {fc:#04x}")
        return append_crc(body)


def toggle_registers():
    """Toggle holding reg 0 between 42 and 99 every 15 seconds."""
    while True:
        time.sleep(15)
        holding[0] = 99 if holding[0] == 42 else 42
        logging.info(f"Register 0 toggled to {holding[0]}")


def read_rtu_frame(sock: socket.socket) -> bytes | None:
    """
    Read one RTU frame. RTU has no length header so we use a simple
    approach: read with a short inter-frame gap timeout.
    We read up to 256 bytes with a 100ms timeout after first byte.
    Wait up to 30s for the first byte (persistent connection).
    """
    sock.settimeout(30.0)
    try:
        first = sock.recv(1)
        if not first:
            return None
    except socket.timeout:
        return None  # keep-alive: return None to loop again without reconnecting

    sock.settimeout(0.1)  # inter-character timeout
    buf = first
    while True:
        try:
            chunk = sock.recv(256)
            if not chunk:
                break
            buf += chunk
        except socket.timeout:
            break
    return buf if len(buf) >= 4 else None


def run():
    logging.info(f"Connecting to {HOST}:{PORT} (A8207 PortManager RS-485 bridge)...")
    while True:
        try:
            with socket.create_connection((HOST, PORT), timeout=10) as sock:
                logging.info(f"Connected. Listening as Modbus RTU slave ID={SLAVE_ID}")
                while True:
                    frame = read_rtu_frame(sock)
                    if frame is None:
                        continue  # timeout keepalive -- stay connected
                    logging.debug(f"RX: {frame.hex()}")
                    resp = build_response(frame)
                    if resp:
                        logging.debug(f"TX: {resp.hex()}")
                        sock.sendall(resp)
        except (ConnectionRefusedError, OSError) as e:
            logging.error(f"Connection failed: {e} -- retrying in 5s")
            time.sleep(5)


if __name__ == "__main__":
    threading.Thread(target=toggle_registers, daemon=True).start()
    run()
