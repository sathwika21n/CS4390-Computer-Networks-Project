# CS4390 Computer Networks Project

[Project outline](https://docs.google.com/document/d/1LWsZo2D4uYXuAwpFO6OcWllf97uuh9KJos3dZKTnB6c/edit?usp=sharing)

This project shares files between peers using a tracker, peer client, peer
server and downloader. Build on Linux or Ubuntu WSL with `make clean && make` after
installing the OpenSSL development headers.

The peer client supports `createtracker`, `updatetracker`, `LIST` and `GET`.
GET verifies the returned tracker metadata and automatically downloads the
file from peers. A background thread periodically advertises regular files
in the configured shared folder. Tracker filenames, shared filenames and
descriptions use single protocol tokens.

Run `./tracker` with `sconfig` on the tracker host, `./peer_server` on serving
peers and `./peer_client clientThreadConfig.cfg` to access the menu. The
tracker defaults to TCP 3490 and the peer server serves `./shared_files` on
TCP 9001. Set the tracker and peer IP addresses in `clientThreadConfig.cfg`
before running across machines.

See [DOWNLOADER.md](DOWNLOADER.md) for build and test commands, resume behavior
and the LAN demo.

## Team responsibilities

- Jazminee maintains the tracker service.
- Sathwika maintains the tracker-facing client, metadata/menu integration and peer updates.
- Kriti maintains the peer server and checksum utilities.
- Veda maintains TCP downloading, segmentation and file reconstruction.
