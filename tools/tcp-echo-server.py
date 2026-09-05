#!/usr/bin/env python3
"""The host side of D5's wire test.

QEMU's user-mode networking forwards guest-initiated TCP to the host
without privileges and without a tap device, so the guest can reach a
server here at 10.0.2.2 and the whole thing runs in CI as an ordinary
user. That is the entire reason the wire test connects OUT rather than
having the host connect IN, which would need hostfwd and a listener
inside NeoOS whose readiness the host cannot observe.

Echoes bytes back until the peer closes. One connection, then exit --
the test makes exactly one.
"""
import socket, sys

port = int(sys.argv[1]) if len(sys.argv) > 1 else 7900

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", port))
s.listen(1)
# Bounded, so a guest that never connects does not leave this running
# forever in CI.
s.settimeout(300)
print(f"tcp-echo-server: listening on {port}", flush=True)

try:
    conn, addr = s.accept()
except socket.timeout:
    print("tcp-echo-server: nobody connected", flush=True)
    sys.exit(1)

print(f"tcp-echo-server: connection from {addr}", flush=True)
conn.settimeout(120)
total = 0
try:
    while True:
        data = conn.recv(65536)
        if not data:
            break
        conn.sendall(data)
        total += len(data)
except (socket.timeout, ConnectionError):
    pass
print(f"tcp-echo-server: echoed {total} bytes", flush=True)
conn.close()
