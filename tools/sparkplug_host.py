#!/usr/bin/env python3
"""
Sparkplug B validation host for Event Engine.

Acts as a minimal Sparkplug primary host so the edge node running on an Axis
device can be verified without Ignition, EBI or any other SCADA system. It
decodes the protobuf payloads itself, so the only dependency is paho-mqtt.

Beyond printing traffic it checks the parts of the spec that are easy to get
wrong:
  - NBIRTH arrives before any NDATA
  - seq starts at 0 on NBIRTH and increments by 1 (mod 256) on every message
  - NDEATH carries the same bdSeq as the NBIRTH it terminates
  - metrics published as aliases resolve against the birth certificate

Usage:
    pip install paho-mqtt
    ./sparkplug_host.py --broker 192.168.1.100 --group Building1

    # Ask the edge node to re-publish its birth certificate
    ./sparkplug_host.py --broker 192.168.1.100 --group Building1 --rebirth

    # Write a metric back to the node (fires a Sparkplug Command trigger)
    ./sparkplug_host.py --broker 192.168.1.100 --group Building1 \
        --write "Speaker/Play=true"

    # Announce ourselves as a primary host so the node rebirths on reconnect
    ./sparkplug_host.py --broker 192.168.1.100 --group Building1 --host-id ScadaHost1
"""

import argparse
import struct
import sys
import time

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("paho-mqtt is required:  pip install paho-mqtt")

# ---------------------------------------------------------------- protobuf ---
WT_VARINT, WT_64BIT, WT_LEN, WT_32BIT = 0, 1, 2, 5

DATATYPES = {
    1: "Int8", 2: "Int16", 3: "Int32", 4: "Int64",
    5: "UInt8", 6: "UInt16", 7: "UInt32", 8: "UInt64",
    9: "Float", 10: "Double", 11: "Boolean", 12: "String",
    13: "DateTime", 14: "Text", 15: "UUID",
}
DATATYPE_IDS = {v.lower(): k for k, v in DATATYPES.items()}

SIGNED = {1: 8, 2: 16, 3: 32, 4: 64}


def read_varint(buf, pos):
    value = shift = 0
    while pos < len(buf):
        byte = buf[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, pos
        shift += 7
    raise ValueError("truncated varint")


def write_varint(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)


def iter_fields(buf):
    pos = 0
    while pos < len(buf):
        key, pos = read_varint(buf, pos)
        field, wire = key >> 3, key & 0x07
        if wire == WT_VARINT:
            value, pos = read_varint(buf, pos)
        elif wire == WT_64BIT:
            value, pos = buf[pos:pos + 8], pos + 8
        elif wire == WT_32BIT:
            value, pos = buf[pos:pos + 4], pos + 4
        elif wire == WT_LEN:
            length, pos = read_varint(buf, pos)
            value, pos = buf[pos:pos + length], pos + length
        else:
            raise ValueError(f"unsupported wire type {wire}")
        yield field, wire, value


def decode_metric(buf):
    m = {"name": None, "alias": None, "datatype": None, "value": None, "is_null": False}
    raw_int = None
    for field, _wire, value in iter_fields(buf):
        if field == 1:
            m["name"] = value.decode("utf-8", "replace")
        elif field == 2:
            m["alias"] = value
        elif field == 3:
            m["timestamp"] = value
        elif field == 4:
            m["datatype"] = value
        elif field == 5:
            m["is_historical"] = bool(value)
        elif field == 7:
            m["is_null"] = bool(value)
        elif field in (10, 11):
            raw_int = value
        elif field == 12:
            m["value"] = struct.unpack("<f", value)[0]
        elif field == 13:
            m["value"] = struct.unpack("<d", value)[0]
        elif field == 14:
            m["value"] = bool(value)
        elif field == 15:
            m["value"] = value.decode("utf-8", "replace")

    if raw_int is not None and m["value"] is None:
        bits = SIGNED.get(m["datatype"])
        if bits and raw_int >= (1 << (bits - 1)):
            raw_int -= 1 << bits
        m["value"] = raw_int
    if m["is_null"]:
        m["value"] = None
    return m


def decode_payload(buf):
    payload = {"timestamp": None, "seq": None, "metrics": []}
    for field, _wire, value in iter_fields(buf):
        if field == 1:
            payload["timestamp"] = value
        elif field == 2:
            payload["metrics"].append(decode_metric(value))
        elif field == 3:
            payload["seq"] = value
    return payload


def encode_metric(name, datatype, value):
    out = bytearray()
    out += write_varint(1 << 3 | WT_LEN) + write_varint(len(name)) + name.encode()
    out += write_varint(4 << 3 | WT_VARINT) + write_varint(datatype)
    if datatype == 11:
        out += write_varint(14 << 3 | WT_VARINT) + write_varint(1 if value else 0)
    elif datatype in (12, 14, 15):
        data = str(value).encode()
        out += write_varint(15 << 3 | WT_LEN) + write_varint(len(data)) + data
    elif datatype == 10:
        out += write_varint(13 << 3 | WT_64BIT) + struct.pack("<d", float(value))
    elif datatype == 9:
        out += write_varint(12 << 3 | WT_32BIT) + struct.pack("<f", float(value))
    elif datatype in (4, 8, 13):
        out += write_varint(11 << 3 | WT_VARINT) + write_varint(int(value) & 0xFFFFFFFFFFFFFFFF)
    else:
        out += write_varint(10 << 3 | WT_VARINT) + write_varint(int(value) & 0xFFFFFFFF)
    return bytes(out)


def encode_payload(metrics, seq=None):
    out = bytearray()
    out += write_varint(1 << 3 | WT_VARINT) + write_varint(int(time.time() * 1000))
    for blob in metrics:
        out += write_varint(2 << 3 | WT_LEN) + write_varint(len(blob)) + blob
    if seq is not None:
        out += write_varint(3 << 3 | WT_VARINT) + write_varint(seq)
    return bytes(out)


# ------------------------------------------------------------------- state ---
class NodeState:
    def __init__(self, node_id):
        self.node_id = node_id
        self.aliases = {}
        self.bdseq = None
        self.expected_seq = None
        self.birth_seen = False
        self.problems = []

    def flag(self, msg):
        self.problems.append(msg)
        print(f"  \033[31mSPEC VIOLATION\033[0m  {msg}")

    def check_seq(self, msgtype, seq):
        if seq is None:
            self.flag(f"{msgtype} is missing a seq number")
            return
        if msgtype == "NBIRTH":
            if seq != 0:
                self.flag(f"NBIRTH seq is {seq}, must be 0")
            self.expected_seq = 1
            return
        if self.expected_seq is None:
            return
        if seq != self.expected_seq:
            self.flag(f"{msgtype} seq is {seq}, expected {self.expected_seq}")
        self.expected_seq = (seq + 1) % 256

    def resolve(self, metric):
        if metric["name"]:
            if metric["alias"] is not None:
                self.aliases[metric["alias"]] = metric["name"]
            return metric["name"]
        if metric["alias"] is not None:
            name = self.aliases.get(metric["alias"])
            if name is None:
                self.flag(f"alias {metric['alias']} was never declared in a BIRTH")
                return f"<alias {metric['alias']}>"
            return name
        return "<unnamed>"


nodes = {}


def show(msgtype, node_id, payload):
    node = nodes.setdefault(node_id, NodeState(node_id))
    seq = payload["seq"]
    colour = {"NBIRTH": 32, "DBIRTH": 32, "NDEATH": 31, "DDEATH": 31,
              "NCMD": 35, "DCMD": 35}.get(msgtype, 36)
    print(f"\n\033[{colour}m{msgtype}\033[0m  {node_id}  seq={seq}")

    if msgtype == "NBIRTH":
        node.aliases.clear()
        node.birth_seen = True
    elif msgtype in ("NDATA", "DDATA") and not node.birth_seen:
        node.flag(f"{msgtype} received before any NBIRTH")

    if msgtype == "NDEATH":
        if seq is not None:
            node.flag("NDEATH must not carry a seq number")
    elif msgtype in ("NCMD", "DCMD"):
        pass  # host-to-node commands are outside the edge node's seq stream
    else:
        node.check_seq(msgtype, seq)

    for metric in payload["metrics"]:
        name = node.resolve(metric)
        dtype = DATATYPES.get(metric["datatype"], "?")
        value = metric["value"]
        extra = " [historical]" if metric.get("is_historical") else ""
        alias = f" @{metric['alias']}" if metric["alias"] is not None else ""
        print(f"    {name:<34}{alias:<6} {dtype:<9} = {value}{extra}")

        if name == "bdSeq":
            if msgtype == "NBIRTH":
                node.bdseq = value
            elif msgtype == "NDEATH":
                if node.bdseq is not None and value != node.bdseq:
                    node.flag(f"NDEATH bdSeq {value} does not match NBIRTH bdSeq {node.bdseq}")
                else:
                    print(f"    \033[32mbdSeq matches the NBIRTH it terminates\033[0m")


# -------------------------------------------------------------------- main ---
def main():
    ap = argparse.ArgumentParser(description="Sparkplug B validation host")
    ap.add_argument("--broker", required=True)
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--username")
    ap.add_argument("--password")
    ap.add_argument("--group", required=True, help="Sparkplug group id to watch")
    ap.add_argument("--node", help="Edge node id (default: every node in the group)")
    ap.add_argument("--host-id", help="Announce this primary host id via STATE")
    ap.add_argument("--spec", choices=["2.2", "3.0"], default="3.0")
    ap.add_argument("--rebirth", action="store_true", help="Send Node Control/Rebirth then keep watching")
    ap.add_argument("--write", action="append", default=[],
                    metavar="NAME=VALUE", help="Write a metric via NCMD (repeatable)")
    ap.add_argument("--write-type", default="String",
                    help="Datatype for --write values (default String)")
    args = ap.parse_args()

    node_filter = args.node or "+"
    watch = f"spBv1.0/{args.group}/#"
    state_topic = (f"spBv1.0/STATE/{args.host_id}" if args.spec == "3.0"
                   else f"STATE/{args.host_id}") if args.host_id else None

    def on_connect(client, _userdata, _flags, rc, *_):
        if rc != 0:
            sys.exit(f"connection refused (rc={rc})")
        print(f"connected to {args.broker}:{args.port}, watching {watch}")
        client.subscribe(watch, qos=0)
        if state_topic:
            online = '{"online":true,"timestamp":%d}' % int(time.time() * 1000) \
                     if args.spec == "3.0" else "ONLINE"
            client.publish(state_topic, online, qos=1, retain=True)
            print(f"announced primary host on {state_topic}")

        if args.rebirth or args.write:
            topic = f"spBv1.0/{args.group}/NCMD/{args.node}" if args.node else None
            if not topic:
                print("\n--node is required to send NCMD; skipping commands")
                return
            metrics = []
            if args.rebirth:
                metrics.append(encode_metric("Node Control/Rebirth", 11, True))
            for spec in args.write:
                if "=" not in spec:
                    print(f"ignoring malformed --write '{spec}'")
                    continue
                name, _, value = spec.partition("=")
                dtype = DATATYPE_IDS.get(args.write_type.lower(), 12)
                if dtype == 11:
                    value = value.strip().lower() in ("true", "1", "on", "yes")
                metrics.append(encode_metric(name.strip(), dtype, value))
            if metrics:
                client.publish(topic, encode_payload(metrics), qos=0)
                print(f"sent NCMD with {len(metrics)} metric(s) to {topic}")

    def on_message(_client, _userdata, msg):
        parts = msg.topic.split("/")
        if len(parts) < 4 or parts[0] != "spBv1.0":
            return
        msgtype = parts[2]
        if msgtype == "STATE":
            return
        node_id = parts[3]
        if node_filter != "+" and node_id != node_filter:
            return
        try:
            payload = decode_payload(msg.payload)
        except Exception as exc:
            print(f"\n\033[31mUNDECODABLE\033[0m {msg.topic}: {exc}")
            return
        show(msgtype, node_id, payload)

    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    except AttributeError:
        client = mqtt.Client()
    if args.username:
        client.username_pw_set(args.username, args.password)
    if state_topic:
        offline = '{"online":false,"timestamp":%d}' % int(time.time() * 1000) \
                  if args.spec == "3.0" else "OFFLINE"
        client.will_set(state_topic, offline, qos=1, retain=True)

    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(args.broker, args.port, 60)

    try:
        client.loop_forever()
    except KeyboardInterrupt:
        print("\n\n=== summary ===")
        for node in nodes.values():
            status = f"\033[31m{len(node.problems)} problem(s)\033[0m" if node.problems \
                     else "\033[32mno spec violations\033[0m"
            print(f"{node.node_id}: {status}, {len(node.aliases)} metrics declared")
            for problem in node.problems:
                print(f"  - {problem}")


if __name__ == "__main__":
    main()
