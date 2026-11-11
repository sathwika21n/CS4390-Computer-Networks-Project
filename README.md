# CS4390-Computer-Networks-Project
Check out Project Outline: https://docs.google.com/document/d/1LWsZo2D4uYXuAwpFO6OcWllf97uuh9KJos3dZKTnB6c/edit?usp=sharing
## Peer client (midterm scope)

The peer client implements the peer-to-tracker `createtracker`, `updatetracker`,
`LIST`, and `GET` exchanges. Its shared protocol module parses both requests and
tracker responses. `GET` saves the returned `.track` file in the configured
download directory and verifies its MD5 before keeping it. A background pthread
periodically publishes the full byte range of regular files in the shared folder.

### Build

On macOS:

```sh
cc -std=c11 -Wall -Wextra -Wpedantic -pthread peer_client.c protocol.c -o peer_client
```

On Linux with OpenSSL development headers installed:

```sh
cc -std=c11 -Wall -Wextra -Wpedantic -pthread peer_client.c protocol.c -lcrypto -o peer_client
```

Run the protocol/parser and MD5 checks with:

```sh
cc -std=c11 -Wall -Wextra -Wpedantic test_protocol.c protocol.c -o test_protocol && ./test_protocol
```

### Configure and run

Edit [clientThreadConfig.cfg](clientThreadConfig.cfg) on each machine:

- `tracker_host` and `tracker_port` must match the tracker server's `sconfig`.
- `peer_ip` must be this peer's reachable LAN address, not `127.0.0.1`, for a
	two-machine demo.
- `peer_port` is the peer listener port; this project is using `3490`.
- `shared_dir` contains files that can be registered and advertised.
- `download_dir` receives verified tracker files.

Run `./peer_client` (or `./peer_client path/to/config`) and use the numbered
menu. `createtracker` accepts a one-token description because the specified
wire format has no quoting/escaping for spaces. Tracker filenames and shared
filenames likewise must not contain whitespace.

The client sends one request per TCP connection. The tracker implementation
must use the matching formats in [protocol.h](protocol.h); peer file serving
and actual peer-to-peer file downloads are implemented by the other assigned
components, not this tracker client.

-------------------------------------------
# CS4390 Computer Networks Project

A team peer-to-peer file-sharing project organized around a tracker, peer client,
peer server, and downloader. This branch contains Veda's first downloader
increment; it does not establish completion of the other components.

## Team responsibilities

- Jazminee: tracker service.
- Sathwika: tracker-facing client, metadata/menu integration, and peer updates.
- Kriti: peer server and checksum utilities.
- Veda: TCP downloader, segmentation, chunk requests, and file reconstruction.

These responsibilities follow the existing downloader integration notes.

## current progress

The downloader connects to one TCP peer, requests file ranges sequentially,
receives binary chunks, and writes them at their correct offsets. It is blocking
and single-threaded. Multiple-peer selection, concurrency, resume, timeouts,
checksum verification, and tracker/server integration are future work.

## Test the downloader

In Linux or Ubuntu/WSL, from the repository root:

```sh
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror downloader.c tests/downloader_cli.c -o tests/downloader_cli
python3 tests/test_downloader.py
```

PowerShell users can prefix each command with `wsl -d Ubuntu --`. Requires GCC,
POSIX development headers, and Python 3. Tests use a local mock peer, not the
team's actual peer server. The revised source compiled on 2026-11-09, and all seven integration tests passed in Ubuntu/WSL in 3.641 seconds.

See [DOWNLOADER.md](DOWNLOADER.md) for architecture, API/function explanations,
manual transfer instructions, limitations, and remaining integration work.
