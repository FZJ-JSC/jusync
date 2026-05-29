#!/usr/bin/env python3
"""Fetch file lists from ALL ranks, then read a specific clip file."""
import zmq
import struct
import json
import time

MAGIC              = 0x55534446
REQ_GET_PROPERTY   = 400
REQ_GET_FILE       = 101
REQ_LIST_FILES     = 100
RESP_PROPERTY      = 401
RESP_FILE_CHUNK    = 201
RESP_FILE_COMPLETE = 202
RESP_NO_FILE       = 203
RESP_ERROR         = 204

CHUNK_HDR_SIZE = 292
PROPERTY_SIZE  = 276

HOST = "localhost"
PORT = 5556
FILTER = "clips/"

ctx = zmq.Context()
sock = ctx.socket(zmq.DEALER)
sock.connect(f"tcp://{HOST}:{PORT}")

poller = zmq.Poller()
poller.register(sock, zmq.POLLIN)

def send_req(sock, msg_type, req_id, rank, filename=b""):
    req = bytearray(276)
    struct.pack_into("<IIIi", req, 0, MAGIC, msg_type, req_id, rank)
    if filename:
        req[16:16+len(filename)] = filename
    sock.send(b"", zmq.SNDMORE)
    sock.send(req)
    return req_id

def recv_all(sock, poller, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        evts = dict(poller.poll(min(5000, (deadline-time.time())*1000)))
        if sock not in evts:
            break
        sock.setsockopt(zmq.RCVTIMEO, 3000)
        try:
            delim = sock.recv()
            data  = sock.recv()
            yield delim, data
        except zmq.Again:
            break

def fetch_file(filename, target_rank):
    """Fetch a single file, return raw bytes."""
    print(f"\n[3] Fetching '{filename}' from rank {target_rank}...")
    req_id = send_req(sock, REQ_GET_FILE, 100, target_rank, filename.encode())

    all_data = bytearray()
    total_size = 0
    done = False

    deadline = time.time() + 30
    while not done and time.time() < deadline:
        evts = dict(poller.poll(min(5000, (deadline-time.time())*1000)))
        if sock not in evts:
            print(f"  [!] Timeout waiting for data")
            break
        sock.setsockopt(zmq.RCVTIMEO, 3000)
        try:
            sock.recv()  # delimiter
            data = sock.recv()
        except zmq.Again:
            break

        if len(data) < 8:
            continue
        msg_type = struct.unpack_from("<I", data, 4)[0]

        if msg_type == RESP_FILE_CHUNK and len(data) >= CHUNK_HDR_SIZE:
            fsize = struct.unpack_from("<Q", data, 272)[0]
            offset = struct.unpack_from("<Q", data, 280)[0]
            csize  = struct.unpack_from("<I", data, 288)[0]
            raw    = data[CHUNK_HDR_SIZE:CHUNK_HDR_SIZE+csize]

            if total_size == 0:
                total_size = fsize

            all_data.extend(raw)
            print(f"  chunk: offset={offset:,} size={len(raw):,} total={len(all_data):,}/{total_size:,}")

        elif msg_type == RESP_FILE_COMPLETE:
            cs = struct.unpack_from("<Q", data, 272)[0]
            print(f"  complete: {cs:,} bytes")
            done = True

        elif msg_type == RESP_NO_FILE:
            print(f"  [!] File not found")
            done = True

        elif msg_type == RESP_ERROR:
            print(f"  [!] Error response")
            done = True

    return bytes(all_data)

# === Step 1: Get total worker count ===
print("[1] Querying total worker count...")
send_req(sock, REQ_GET_PROPERTY, 99, -1, b"totalWorkerCount")

for _, data in recv_all(sock, poller, 5):
    msg_type = struct.unpack_from("<I", data, 4)[0]
    if msg_type == RESP_PROPERTY and len(data) >= PROPERTY_SIZE:
        int_val = struct.unpack_from("<i", data, 16)[0]
        print(f"  total workers = {int_val}")
        num_ranks = int_val
        break
else:
    print("  No worker count, assuming 1")
    num_ranks = 1

# === Step 2: List clips/ files ===
print(f"\n[2] Listing clips/ on all {num_ranks} rank(s)...\n")
send_req(sock, REQ_LIST_FILES, 42, -1)

all_files = {}
seen_ranks = set()
completes  = set()

for _, data in recv_all(sock, poller, 15):
    if len(data) < 8:
        continue
    msg_type = struct.unpack_from("<I", data, 4)[0]

    if msg_type == RESP_FILE_CHUNK and len(data) >= CHUNK_HDR_SIZE:
        rank  = struct.unpack_from("<i", data, 12)[0]
        csize = struct.unpack_from("<I", data, 288)[0]
        if rank not in seen_ranks:
            seen_ranks.add(rank)
            all_files[rank] = []
            print(f"  [rank {rank}]")
        jpayload = data[CHUNK_HDR_SIZE:CHUNK_HDR_SIZE+csize]
        if jpayload:
            for f in json.loads(jpayload).get("files", []):
                n = f.get("name", "?")
                s = f.get("size", 0)
                if FILTER and not n.startswith(FILTER):
                    continue
                print(f"    {n:<40} {s:>10,d} bytes")
                all_files[rank].append((n, s))

    elif msg_type == RESP_FILE_COMPLETE:
        completes.add(struct.unpack_from("<i", data, 12)[0])
        if len(completes) >= num_ranks:
            break

    elif msg_type == RESP_NO_FILE:
        seen_ranks.add(struct.unpack_from("<i", data, 12)[0])

# === Step 3: Fetch a clip file ===
file_path = "clips/vtk_actor__spheres_0_Geom__r13_0.000000.usda"
raw = fetch_file(file_path, 13)

if raw:
    print(f"\n{'='*50}")
    print(f"File: {file_path}")
    print(f"Size: {len(raw):,} bytes")
    print(f"\n--- First 500 bytes ---")
    print(raw[:500].decode("utf-8", errors="replace"))
    if len(raw) > 500:
        print(f"\n... ({len(raw)-500} more bytes)")

    # Save to disk
    out = f"D:/JSC_Github/jusync/tools/{file_path.lstrip('/').replace('/', '_')}"
    with open(out, "wb") as f:
        f.write(raw)
    print(f"Saved to: {out}")
else:
    print("\n[!] Failed to fetch file")

print(f"\n{'='*50}")
print(f"Ranks polled:   {len(seen_ranks)}")
print(f"Total clipped:  {sum(len(v) for v in all_files.values())} files")

sock.close()
ctx.term()
