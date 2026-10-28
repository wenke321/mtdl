#!/usr/bin/env python3
"""Tiny HTTP/1.1 server with byte-range + keep-alive support, used by selftest.sh.

Usage: testsrv.py <file> [port] [bytes-per-second-per-connection]
The rate limit simulates mirrors that shape each connection independently.
"""
import os
import re
import socket
import sys
import threading
import time

PATH = sys.argv[1]
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8808
RATE = int(sys.argv[3]) if len(sys.argv) > 3 else 0
SIZE = os.path.getsize(PATH)
NAME = os.path.basename(PATH)


def serve(conn):
    """Serve requests on one connection, honouring keep-alive like a real mirror."""
    try:
        conn.settimeout(30)
        while True:
            buf = b""
            while b"\r\n\r\n" not in buf:
                chunk = conn.recv(65536)
                if not chunk:
                    return
                buf += chunk
                if len(buf) > 1 << 20:
                    return
            head = buf.decode("latin1")
            lines = head.split("\r\n")
            if len(lines) < 2:
                return
            method, target = lines[0].split()[:2]
            # Serve only our own file name; anything else is a 404. This is what
            # lets the self test check clean failure on a missing object.
            if target.split("?", 1)[0].rsplit("/", 1)[-1] != NAME:
                conn.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                             b"Connection: close\r\n\r\n")
                return
            rng = None
            for line in lines[1:]:
                if line.lower().startswith("range:"):
                    m = re.search(r"bytes=(\d*)-(\d*)", line)
                    if m:
                        start = int(m.group(1)) if m.group(1) else 0
                        end = int(m.group(2)) if m.group(2) else SIZE - 1
                        rng = (start, min(end, SIZE - 1))
            if method == "HEAD":
                conn.sendall(
                    f"HTTP/1.1 200 OK\r\nContent-Length: {SIZE}\r\n"
                    f"Accept-Ranges: bytes\r\nConnection: keep-alive\r\n\r\n".encode())
                continue

            if rng:
                start, end = rng
                length = end - start + 1
                hdr = (f"HTTP/1.1 206 Partial Content\r\nContent-Range: bytes {start}-{end}/{SIZE}\r\n"
                       f"Content-Length: {length}\r\nAccept-Ranges: bytes\r\n"
                       f"Connection: keep-alive\r\n\r\n")
            else:
                start, length = 0, SIZE
                hdr = (f"HTTP/1.1 200 OK\r\nContent-Length: {SIZE}\r\nAccept-Ranges: bytes\r\n"
                       f"Connection: keep-alive\r\n\r\n")
            conn.sendall(hdr.encode())

            with open(PATH, "rb") as f:
                f.seek(start)
                sent = 0
                chunk = 64 * 1024
                t0 = time.time()
                while sent < length:
                    n = min(chunk, length - sent)
                    data = f.read(n)
                    if not data:
                        break
                    conn.sendall(data)
                    sent += len(data)
                    if RATE:
                        ahead = sent / RATE - (time.time() - t0)
                        if ahead > 0:
                            time.sleep(ahead)
    except (BrokenPipeError, ConnectionResetError, socket.timeout):
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # 0.0.0.0 so the fan-out test can spread connections over 127.0.0.1..3.
    srv.bind(("0.0.0.0", PORT))
    srv.listen(512)
    print(f"serving {PATH} ({SIZE} bytes) on 0.0.0.0:{PORT} rate={RATE or 'unlimited'}",
          flush=True)
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=serve, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    main()
