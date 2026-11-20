# Downloader

The peer client starts a download after checking the tracker file against the
MD5 in the GET response. The downloader then verifies the payload against the
size and MD5 inside that tracker file. GET again to resume an interrupted
download with a fresh list of peers. The menu waits for the download to finish
or report an error.

## Protocol and scheduling

The peer server accepts `GET_CHUNK filename offset length\n` and replies with
`CHUNK length\n` followed by exactly that many bytes. Offsets are zero-based.
Each request asks for at most 1024 bytes and uses a separate TCP connection.
In tracker metadata, an update row is `IPv4:port:first:last:timestamp` with
inclusive endpoints. Initial rows can advertise the file size as the last
byte. The downloader clips all ranges at EOF.

The caller and up to three additional pthreads claim distinct slots from a
shared queue. Each slot covers up to 1024 bytes. A worker chooses the newest
idle peer that can serve the entire request. If all eligible peers are busy,
it tries the newest of them. After a failure, each remaining eligible peer
can be tried once for that request. If necessary, a worker divides a slot at
advertised range boundaries. A slot is recorded complete after all its bytes
have been received and written. Each TCP attempt has a 2.5-second deadline
covering connect, request and response. Tracker GET itself uses the existing
tracker client behavior.

Resume state lives under `download_dir/.downloader/<filename>/`. The downloader
syncs each slot's data before persisting its completion mark. Resume is keyed
to the expected size and MD5. If the completed payload has the wrong MD5,
completion marks are cleared so the next GET can refetch all slots. Verified
files are published with a same-filesystem hard link that refuses to replace an
existing destination. A per-file lock prevents concurrent writers. Use a Linux
filesystem that supports hard links and directory locks. Verified metadata is
saved under `download_dir/.trackers/` so a tracker GET can refresh metadata
without replacing a payload that happens to have the same filename.

`dl_download_track()` is the production entry point. The test CLI also retains
its original direct single-peer mode through `dl_download()`.

## Build on Linux or Ubuntu WSL

Install GCC, make and the OpenSSL development headers. Run from the checkout
root.

```sh
make clean && make
```

The Makefile outputs `tracker`, `peer_client` and `peer_server`, which are
tracked in the repository. Keep rebuilt binaries out of source commits.

## LAN demo

Build with `make clean && make` in each machine's checkout. Use the tracker
machine's reachable IPv4 address for `tracker_host`. Each peer needs its own
reachable IPv4 address for `peer_ip`. Allow inbound TCP 3490 on the tracker and
TCP 9001 on each serving peer. On each peer, keep `shared_dir=./shared_files`,
`peer_port=9001` and `download_dir=./downloads` in `clientThreadConfig.cfg`.
The peer server currently uses TCP 9001 and `./shared_files` from its source.
Changing `serverThreadConfig.cfg` alone does not change those settings.

1. On the tracker machine, start `./tracker` from the repository root. The
   supplied `sconfig` listens on port 3490 and stores metadata in `torrents/`.
2. On the source peer, create a fresh test file and start its server.

   ```sh
   mkdir -p shared_files
   dd if=/dev/urandom of=shared_files/demo-4390.bin bs=1024 count=8
   md5sum shared_files/demo-4390.bin
   ./peer_server
   ```

3. In a second terminal on the source peer, register the file and leave the
   client running to refresh the advertisement.

   ```sh
   ./peer_client clientThreadConfig.cfg
   ```

   Choose **1**, enter `demo-4390.bin`, then enter `demo` as the description.
   Look for `createtracker: succ`. On a repeat demo, use a fresh filename or
   reuse the exact same source bytes. Option **2** refreshes an existing entry.
4. On the receiving peer, run the production client and choose **3** to list
   the file. Choose **4** and enter `demo-4390.bin.track`. The client should log
   peer selection, byte offsets, completion and the verified MD5.

   ```sh
   ./peer_client clientThreadConfig.cfg
   ```

5. On the receiving peer, run `md5sum downloads/demo-4390.bin` and compare
   it with the source hash. To demonstrate multiple sources, copy the exact
   source bytes into a second serving peer's `shared_files`, start its
   `./peer_server`, and choose **2** for `demo-4390.bin` in its client.
   GET from a receiver where the destination does not yet exist. A larger
   source file makes concurrent use easier to observe.
