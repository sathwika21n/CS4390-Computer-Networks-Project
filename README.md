# CS4390-Computer-Networks-Project

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

## What to complete next

Work through these steps in order to finish and demonstrate the full project:

1. **Finish and verify the tracker server.** Implement the matching request and
	response formats in [protocol.h](protocol.h) for `createtracker`,
	`updatetracker`, `LIST`, and `GET`. Confirm that it accepts one request per
	TCP connection and that the client can use it from another machine.
2. **Implement peer file serving.** Add a listener on the configured
	`peer_ip:peer_port` that accepts peer requests and sends the requested byte
	ranges from regular files in `shared_dir`. Handle invalid filenames,
	unavailable files, and invalid ranges without crashing.
3. **Implement actual peer-to-peer downloads.** Use tracker results to locate
	peers, request file data from them, and save/reassemble the received bytes.
	Keep this separate from the current `GET` menu action, which downloads a
	`.track` metadata file from the tracker only.
4. **Test the complete flow with two peers.** Create metadata, publish each
	peer's available byte range, list files, download a file from the other
	peer, and compare the completed file's size and MD5 with the source. Also
	test missing files, bad ranges, interrupted connections, and empty files.
5. **Prepare the demo and submission.** Set each machine's tracker address and
	reachable peer IP in its config, build and run the protocol tests, record
	the exact demo steps, and document any known limitations or unfinished
	requirements.