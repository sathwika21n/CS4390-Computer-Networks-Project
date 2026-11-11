"""Test-only provisional chunk server; not the team's peer_server.c."""
import argparse
import socketserver
import threading
import time
from pathlib import Path


class MockPeer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, data, mode="normal", port=0, delay=0, bind="127.0.0.1"):
        self.data, self.mode, self.delay = data, mode, delay
        self.requests = []
        self.active = self.peak = 0
        self.lock = threading.Lock()
        super().__init__((bind, port), Handler)

    def start(self):
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()
        return self

    def stop(self):
        self.shutdown()
        self.server_close()
        self.thread.join()


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        self.connection.settimeout(5)
        s = self.server
        with s.lock:
            s.active += 1
            s.peak = max(s.peak, s.active)
        try:
            line = self.rfile.readline(1025)
            if len(line) > 1024 or not line.endswith(b"\n"):
                return
            command, name, offset, length = line.decode("ascii").split()
            offset, length = int(offset), int(length)
            if command != "GET_CHUNK" or offset < 0 or length < 0 or offset + length > len(s.data):
                return
            with s.lock:
                s.requests.append((name, offset, length))
            time.sleep(s.delay)
            if s.mode == "stall":
                time.sleep(1)
                return
            if s.mode == "malformed":
                self.wfile.write(b"CHUNK nonsense\n")
                return
            self.wfile.write(f"CHUNK {length}\n".encode())
            data = s.data[offset:offset + length]
            if s.mode == "partial":
                data = data[:len(data)//2]
            # Fragment responses to exercise TCP framing independently of chunks.
            for pos in range(0, len(data), 137):
                self.wfile.write(data[pos:pos+137])
        except (OSError, ValueError, UnicodeError):
            pass
        finally:
            with s.lock:
                s.active -= 1


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("file", type=Path)
    p.add_argument("--port", type=int, default=0)
    p.add_argument("--bind", default="127.0.0.1")
    args = p.parse_args()
    # Explicit bind is useful for a controlled two-machine demonstration.
    server = MockPeer(args.file.read_bytes(), port=args.port, bind=args.bind)
    print(f"Mock peer listening on {server.server_address}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
