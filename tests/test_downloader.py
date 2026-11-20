"""Real local TCP tests of the first single-threaded downloader increment."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from mock_peer import MockPeer

CLI = Path(__file__).resolve().parent / "downloader_cli"

class Downloads(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.out = Path(self.tmp.name) / "result"
        self.peers = []

    def tearDown(self):
        for peer in self.peers:
            peer.stop()
        self.tmp.cleanup()

    def peer(self, data, **kwargs):
        peer = MockPeer(data, **kwargs).start()
        self.peers.append(peer)
        return peer

    def run_dl(self, peer, size, chunk=1024, name="sample.bin", ok=True):
        result = subprocess.run([str(CLI), "127.0.0.1", str(peer.server_address[1]),
                                 name, str(self.out), str(size), str(chunk)],
                                capture_output=True, timeout=10)
        self.assertEqual(result.returncode == 0, ok, result.stderr.decode())
        return result

    def test_text_binary_empty_and_short_final_chunk(self):
        for data in (b"hello networks\n", bytes(range(256))*31+b"tail", b""):
            with self.subTest(size=len(data)):
                peer = self.peer(data)
                self.run_dl(peer, len(data))
                self.assertEqual(self.out.read_bytes(), data)
                expected = [("sample.bin", offset, min(1024, len(data)-offset))
                            for offset in range(0, len(data), 1024)]
                self.assertEqual(peer.requests, expected)
                self.out.unlink()

    def test_sequential_segment_requests(self):
        data = bytes(range(256))*25
        peer = self.peer(data, delay=.01)
        self.run_dl(peer, len(data))
        self.assertEqual(self.out.read_bytes(), data)
        self.assertEqual([r[1] for r in peer.requests], list(range(0, len(data), 1024)))

    def test_chunk_larger_than_transfer_buffer(self):
        data = bytes(range(256))*600+b"end"
        peer = self.peer(data)
        self.run_dl(peer, len(data), chunk=100000)
        self.assertEqual(self.out.read_bytes(), data)
        self.assertEqual([r[2] for r in peer.requests], [100000, len(data)-100000])

    def test_malformed_header_and_early_eof(self):
        for mode in ("malformed", "partial"):
            with self.subTest(mode=mode):
                peer = self.peer(b"example payload", mode=mode)
                self.run_dl(peer, len(peer.data), ok=False)
                self.assertTrue(self.out.exists())
                self.out.unlink()

    def test_refuses_existing_output(self):
        peer = self.peer(b"new")
        self.out.write_bytes(b"keep")
        self.run_dl(peer, 3, ok=False)
        self.assertEqual(self.out.read_bytes(), b"keep")
        self.assertEqual(peer.requests, [])

    def test_invalid_options(self):
        peer = self.peer(b"x")
        for kwargs in ({"chunk": 0}, {"name": "bad\nname"}, {"name": "x"*1024}):
            self.run_dl(peer, 1, ok=False, **kwargs)
            self.assertFalse(self.out.exists())

    def test_connection_refused(self):
        peer = self.peer(b"x")
        peer.stop()
        self.peers.remove(peer)
        self.run_dl(peer, 1, ok=False)

if __name__ == "__main__":
    unittest.main(verbosity=2)
