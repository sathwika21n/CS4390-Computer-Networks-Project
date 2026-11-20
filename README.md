# CS4390-Computer-Networks-Project

Check out [the project outline](https://docs.google.com/document/d/1LWsZo2D4uYXuAwpFO6OcWllf97uuh9ZKTnB6c/edit?usp=sharing).

A team peer-to-peer file-sharing project organized around a tracker, peer client,
peer server, and downloader.

## Team responsibilities

- Jazminee: tracker service.
- Sathwika: tracker-facing client, metadata/menu integration, and peer updates.
- Kriti: peer server and checksum utilities.
- Veda: TCP downloader, segmentation, chunk requests, and file reconstruction.

## Peer client (midterm scope)

The peer client implements the peer-to-tracker `createtracker`, `updatetracker`,
`LIST`, and `GET` exchanges. Its shared protocol module parses both requests and
tracker responses. `GET` saves the returned `.track` file in the configured
download directory and verifies its MD5 before keeping it. A background pthread
periodically publishes the full byte range of regular files in the shared folder.

### Build

```sh
make
```

On macOS, the Makefile builds the tracker, peer client, and peer server. Run the
protocol/parser and MD5 checks with:

```sh
cc -std=c11 -Wall -Wextra -Wpedantic test_protocol.c protocol.c -o test_protocol && ./test_protocol
```

### Configure and run

Edit `clientThreadConfig.cfg` on each machine:

- `tracker_host` and `tracker_port` must match the tracker server's `sconfig`.
- `peer_ip` must be this peer's reachable LAN address, not `127.0.0.1`, for a	two-machine demo.
- `peer_port` is the peer listener port; this project uses `3490` for the peer
	client configuration.
- `shared_dir` contains files that can be registered and advertised.
- `download_dir` receives verified tracker files.

Run `./peer_client` (or `./peer_client path/to/config`) and use the numbered
menu. `createtracker` accepts a one-token description because the specified
wire format has no quoting/escaping for spaces. Tracker filenames and shared
filenames likewise must not contain whitespace.

The peer client sends one request per TCP connection. The tracker implementation
handles matching `createtracker`, `updatetracker`, `LIST`, and `GET` formats.

## Downloader progress

The downloader connects to one TCP peer, requests file ranges sequentially,
receives binary chunks, and writes them at their correct offsets. It is blocking
and single-threaded. Multiple-peer selection, concurrency, resume, and tracker-
integrated peer selection remain future work.

## Test the downloader

In Linux or Ubuntu/WSL, from the repository root:

```sh
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror downloader.c tests/downloader_cli.c -o tests/downloader_cli
python3 tests/test_downloader.py
```

PowerShell users can prefix each command with `wsl -d Ubuntu --`. Requires GCC,
POSIX development headers, and Python 3. Tests use a local mock peer, not the
team's actual peer server.

See [DOWNLOADER.md](DOWNLOADER.md) for architecture, API/function explanations,
manual transfer instructions, limitations, and remaining integration work.

## Remaining integration work

1. Align the peer server's shared directory and port with the peer client and
	tracker configuration.
2. Integrate tracker peer listings with the downloader for peer selection.
3. Add actual peer-to-peer download menu actions and MD5 verification of the
	completed source file.
4. Test the complete two-peer flow, including missing files, invalid ranges,
	interrupted connections, and empty files.
