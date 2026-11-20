# CS4390 Computer Networks Project

[Project outline](https://docs.google.com/document/d/1LWsZo2D4uYXuAwpFO6OcWllf97uuh9KJos3dZKTnB6c/edit?usp=sharing)

Currently, the clientThreadConfig.cfg is configured to have tracker run on csa-linux02.utdallas.edu at port 3490.
Please update this file to match which server you want tracker to run.
start_demo.sh contains a way to run everything in one server, as it falls back to starting its own tracker if there is no tracker at the configured location. Please uncomment the lines containing `TRACKER` that are commented to make it work in that format.

This project shares files between peers using a tracker, peer client, peer
server and downloader. Build on Linux or Ubuntu WSL with `make clean && make` after
installing the OpenSSL development headers.

The peer client supports `createtracker`, `updatetracker`, `LIST` and `GET`.
GET verifies the returned tracker metadata and automatically downloads the
file from peers. Peers advertise and serve durably completed segments during
downloads. The client resumes cached jobs at startup and refreshes metadata
for incomplete jobs at its configured interval. Tracker filenames, shared filenames and
descriptions use single protocol tokens.

Run `./tracker` with `sconfig` on the tracker host, `./peer_server` on serving
peers and `./peer_client clientThreadConfig.cfg` to access the menu. The
tracker defaults to TCP 3490 and the peer server normally serves
`./shared_files` on TCP 9001. Set the tracker and peer IP addresses in
`clientThreadConfig.cfg` and match its peer port and directories with
`serverThreadConfig.cfg` before running across machines.

See [DOWNLOADER.md](DOWNLOADER.md) for build and test commands, resume behavior
and the LAN demo.

## Team responsibilities

- Jazminee maintains the tracker service.
- Sathwika maintains the tracker-facing client, metadata/menu integration and peer updates.
- Kriti maintains the peer server and checksum utilities.
- Veda maintains TCP downloading, segmentation and file reconstruction.
