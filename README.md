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
