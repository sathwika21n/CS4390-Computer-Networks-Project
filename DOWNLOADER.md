# Downloader

This Linux/POSIX C module implements four features: connect to a TCP peer,
request/receive chunks, divide a file into segments, and reconstruct the output
using byte offsets. It is blocking and single-threaded, with one peer and one
connection per chunk. It has no tracker dependency.

## Architecture and protocol

Caller -> dl_download -> sequential segment loop -> fetch_chunk -> TCP peer
                                               -> pwrite -> output file

The caller supplies host, peer port, remote filename, destination, known file
size, and chunk size through dl_options in downloader.h. The peer must have the
whole file. Metadata discovery is future integration work.

Provisional protocol (coordinate with Kriti before integration):

```text
GET_CHUNK filename offset length\n
CHUNK length\n
[exactly length binary bytes]
```

Names are printable ASCII without spaces. Requests including newline are limited
to 1024 bytes. Headers must match the requested length. This protocol is test
scaffolding, not a claim that the team's peer server already supports it.

## Function walkthrough using Beej's concepts

Read downloader.h, then dl_download(), then fetch_chunk() and its helpers.
Reference: [Beej's Guide](https://beej.us/guide/bgnet/html/index.html), sections
5.1, 5.2, 5.4, 5.7, 5.9 and 7.4.

1. dl_download validates options and creates an exclusive output with open().
   Existing output is refused. A zero-byte file needs no network requests.
2. Segment the file in a loop: length = min(chunk_size, file_size - offset).
   For 10 bytes and chunk size 4, requests are (0,4), (4,4), (8,2).
   These are logical byte ranges; no separate segment files are created.
3. connect_peer calls getaddrinfo to translate host/port into socket addresses.
   socket() creates an endpoint and returns a file descriptor, or -1 on error.
   SOCK_STREAM requests TCP. connect() establishes the outgoing connection.
   Each failed address is closed before trying another resolved address.
4. send_all sends the request. send() reports how many bytes it accepted, which
   can be fewer than requested; the loop advances by that count. MSG_NOSIGNAL
   prevents a disconnected peer from terminating this Linux process via SIGPIPE.
5. receive_exact loops around recv(). A positive result counts received bytes;
   zero means orderly peer shutdown, which is failure before the expected bytes
   arrive; -1 means error. Interrupted operations are retried. TCP carries bytes,
   so a send call does not correspond to a recv call or a file segment.
6. fetch_chunk reads the header one byte at a time up to newline, checks it,
   then receives the binary body in blocks of at most 64 KiB. Binary payloads
   have explicit lengths; they are not NUL-terminated strings.
7. write_at loops around pwrite(), which writes at an explicit file offset.
   Offset 4 means the fifth byte, because offsets start at zero. For a block
   inside a segment, destination = segment offset + bytes already received.
   This places bytes directly in final order; no separate merge pass is needed.
8. close() releases each socket, including error paths. Closing the output is
   checked too. fail() copies a diagnostic into the caller's bounded buffer.
   dl_download returns 0 only after every segment and output close succeeds.

## Build and test

From the repository root in Linux or Ubuntu/WSL, with GCC and Python 3:

```sh
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror downloader.c tests/downloader_cli.c -o tests/downloader_cli
python3 tests/test_downloader.py
```

From PowerShell prefix each command with `wsl -d Ubuntu --`.
No pthread build flag is needed. This is POSIX source, not native Windows Winsock.

On 2026-11-09 the revised source compiled successfully in Ubuntu/WSL with the
flags above and no warnings. All seven revised integration tests subsequently passed in Ubuntu/WSL in 3.641 seconds.

Seven test methods cover text/binary/empty files, exact segment boundaries and
short final segments, ordered requests, segments larger than the transfer buffer,
fragmented TCP responses, malformed headers, early EOF, existing destinations,
invalid options, and refused connections. Successful downloads are compared byte
for byte. The Python mock runs in background threads to serve the test client;
the C downloader itself has no threads. These are local TCP tests, not a physical
two-machine demonstration.

Manual test, using two terminals (choose a fresh destination):

```sh
python3 tests/mock_peer.py sample.bin --port 51001
./tests/downloader_cli 127.0.0.1 51001 sample.bin received.bin SIZE 4096
cmp sample.bin received.bin
```

Replace SIZE with the exact byte count (`wc -c < sample.bin`). For two machines,
bind the mock with `--bind 0.0.0.0` and use its reachable address on the client.
The CLI arguments are HOST PORT NAME OUTPUT SIZE CHUNK.

## Limits and remaining work

Failure stops immediately and can leave an incomplete destination. Inspect/remove
that output before retrying; this version does not resume. Blocking network calls
can wait indefinitely for a silent peer. File size is supplied by the caller;
there is no hash validation, authentication, or crash-durable publication.
The API limits file size to signed 64-bit filesystem offsets.

Later increments: multiple peer/range metadata, newest-peer selection, fallback
and timeouts, concurrency, persistent resume, hashes and safe final publication.
Integrate tracker metadata/menu/update calls with Sathwika; agree server framing
and checksum handling with Kriti; coordinate tracker semantics with Jazminee.
Team feature completion is not established by this downloader branch.

## Preserved full version and staging

Before edits, the original source, header, docs, ignore file, and entire tests
folder were copied to backups/downloader-full-2026-11-09/. SHA256.json records
SHA-256 hashes of those original files. This ignored local backup includes the
advanced implementation and original tests. Keep it locally for later increments;
it is not included in this first commit. Restore selected files by copying them
back after reviewing; do not overwrite the current increment accidentally.

Stage exactly these files after reviewing and running tests:

```sh
git add -- .gitignore README.md DOWNLOADER.md downloader.c downloader.h tests/downloader_cli.c tests/mock_peer.py tests/test_downloader.py
```

Suggested commit message: `Add single-threaded TCP chunk downloader`
Stage only the listed source and documentation files; exclude the local backup and compiled binary.
