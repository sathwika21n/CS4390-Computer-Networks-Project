#!/usr/bin/env bash
set -u

TORRENTS_DIR="torrents"
DEMO_ROOT="demo_runtime"
BASE_CLIENT_CFG="${CLIENT_CFG:-clientThreadConfig.cfg}"

TRACKER_HOST="127.0.0.1"
TRACKER_PORT="3490"
PEER_IP="127.0.0.1"
UPDATE_INTERVAL="15"

SMALL_FILE="small.bin"
LARGE_FILE="large.bin"

PIDS=()

trim_whitespace() {
    local s="$1"
    s="${s#"${s%%[![:space:]]*}"}"
    s="${s%"${s##*[![:space:]]}"}"
    printf '%s' "$s"
}

load_client_thread_config() {
    local cfg_path="$1"
    if [ ! -f "$cfg_path" ]; then
        echo "Error: config file not found: $cfg_path" >&2
        exit 1
    fi

    while IFS= read -r line || [ -n "$line" ]; do
        line="${line%%#*}"
        line="$(trim_whitespace "$line")"
        [ -z "$line" ] && continue

        case "$line" in
            *=*)
                local key="${line%%=*}"
                local val="${line#*=}"
                key="$(trim_whitespace "$key")"
                val="$(trim_whitespace "$val")"
                case "$key" in
                    tracker_host)
                        [ -n "$val" ] && TRACKER_HOST="$val"
                        ;;
                    tracker_port)
                        [ -n "$val" ] && TRACKER_PORT="$val"
                        ;;
                    peer_ip)
                        [ -n "$val" ] && PEER_IP="$val"
                        ;;
                    update_interval)
                        [ -n "$val" ] && UPDATE_INTERVAL="$val"
                        ;;
                esac
                ;;
        esac
    done < "$cfg_path"
}

load_client_thread_config "$BASE_CLIENT_CFG"

cleanup() {
    echo ""
    echo "=== Cleaning up running demo processes ==="
    for pid in "${PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
        fi
    done
    wait 2>/dev/null || true
    echo "Demo cleanup complete."
}

trap cleanup INT TERM EXIT

echo "=== Cleaning old builds and stale .track files ==="
make clean
rm -rf "$DEMO_ROOT"
rm -f ./*.track "$TORRENTS_DIR"/*.track

echo "=== Compiling project ==="
make
if [ $? -ne 0 ]; then
    echo "Compilation failed! Fix errors before running demo."
    exit 1
fi

mkdir -p "$TORRENTS_DIR" "$DEMO_ROOT/peer1/shared" "$DEMO_ROOT/peer2/shared"

# Tune LARGE_BYTES on your machine so Peers 3-8 take at least 1m 20s (80s) to download it.
SMALL_BYTES=65536
LARGE_BYTES=10485760
head -c "$SMALL_BYTES" </dev/urandom > "$DEMO_ROOT/peer1/shared/$SMALL_FILE"
head -c "$LARGE_BYTES" </dev/urandom > "$DEMO_ROOT/peer2/shared/$LARGE_FILE"

write_peer_configs() {
    local peer_name="$1"
    local peer_port="$2"
    local peer_dir="$DEMO_ROOT/$peer_name"
    local share_dir="$peer_dir/shared"
    local dl_dir="$peer_dir/downloads"

    mkdir -p "$share_dir" "$dl_dir"

    cat > "$peer_dir/serverThreadConfig.cfg" <<EOF
PORT=${peer_port}
SHARED_DIR=${share_dir}
DOWNLOAD_DIR=${dl_dir}
EOF

    cat > "$peer_dir/clientThreadConfig.cfg" <<EOF
tracker_host=${TRACKER_HOST}
tracker_port=${TRACKER_PORT}
peer_ip=${PEER_IP}
peer_port=${peer_port}
shared_dir=${share_dir}
download_dir=${dl_dir}
update_interval=${UPDATE_INTERVAL}
EOF
}

launch_peer() {
    local label="$1"
    local peer_name="$2"
    local peer_port="$3"
    local cmd_script="$4"

    local peer_dir="$DEMO_ROOT/$peer_name"
    local fifo="$peer_dir/cmd.fifo"

    write_peer_configs "$peer_name" "$peer_port"
    rm -f "$fifo"
    mkfifo "$fifo"

    ./peer_server "$peer_dir/serverThreadConfig.cfg" > "$peer_dir/server.log" 2>&1 &
    local srv_pid=$!
    PIDS+=("$srv_pid")

    {
        printf "%b" "$cmd_script"
        sleep 3600
    } > "$fifo" &
    local feeder_pid=$!
    PIDS+=("$feeder_pid")

    ./peer_client "$peer_dir/clientThreadConfig.cfg" < "$fifo" > "$peer_dir/client.log" 2>&1 &
    local cli_pid=$!
    PIDS+=("$cli_pid")

    eval "${peer_name}_SRV_PID=$srv_pid"
    eval "${peer_name}_CLI_PID=$cli_pid"
    eval "${peer_name}_FEEDER_PID=$feeder_pid"
    eval "${peer_name}_LABEL=$label"
}

stop_peer() {
    local peer_name="$1"
    local srv_var="${peer_name}_SRV_PID"
    local cli_var="${peer_name}_CLI_PID"
    local feeder_var="${peer_name}_FEEDER_PID"
    local label_var="${peer_name}_LABEL"

    local srv_pid="${!srv_var:-}"
    local cli_pid="${!cli_var:-}"
    local feeder_pid="${!feeder_var:-}"
    local label="${!label_var:-$peer_name}"

    [ -n "$feeder_pid" ] && kill "$feeder_pid" 2>/dev/null || true
    [ -n "$cli_pid" ] && kill "$cli_pid" 2>/dev/null || true
    [ -n "$srv_pid" ] && kill "$srv_pid" 2>/dev/null || true

    [ -n "$feeder_pid" ] && wait "$feeder_pid" 2>/dev/null || true
    [ -n "$cli_pid" ] && wait "$cli_pid" 2>/dev/null || true
    [ -n "$srv_pid" ] && wait "$srv_pid" 2>/dev/null || true

    echo "${label} terminated"
}

print_peer_snapshot() {
    local start_idx="$1"
    local end_idx="$2"

    for i in $(seq "$start_idx" "$end_idx"); do
        local log_file="$DEMO_ROOT/peer${i}/client.log"
        local dl_dir="$DEMO_ROOT/peer${i}/downloads"
        local small_sz large_sz last_msg

        small_sz=$(stat -c%s "$dl_dir/$SMALL_FILE" 2>/dev/null || echo 0)
        large_sz=$(stat -c%s "$dl_dir/$LARGE_FILE" 2>/dev/null || echo 0)
        last_msg=$(grep -v -E '^(Peer client \(|1\) createtracker|2\) updatetracker|3\) LIST|4\) GET tracker file|0\) Quit|Choice:)' "$log_file" 2>/dev/null | tail -n 1 || true)

        if [ -n "$last_msg" ]; then
            echo "Peer${i}: small=${small_sz}/${SMALL_BYTES}B, large=${large_sz}/${LARGE_BYTES}B | ${last_msg}"
        else
            echo "Peer${i}: small=${small_sz}/${SMALL_BYTES}B, large=${large_sz}/${LARGE_BYTES}B"
        fi
    done
}

echo "=== t = 0s: Starting Tracker Server, Peer1 (small file), and Peer2 (large file) ==="
# ./tracker > "$DEMO_ROOT/tracker.log" 2>&1 &
# TRACKER_PID=$!
# PIDS+=("$TRACKER_PID")
sleep 1

launch_peer "Peer1" "peer1" 9101 "1\n${SMALL_FILE}\nSmall_Demo_File\n"
echo "Peer1: createtracker ${SMALL_FILE} initiated"

launch_peer "Peer2" "peer2" 9102 "1\n${LARGE_FILE}\nLarge_Demo_File\n"
echo "Peer2: createtracker ${LARGE_FILE} initiated"

echo "=== Waiting 30s before starting Peers 3 to 8 ==="
sleep 30

echo "=== t = 30s: Starting Peers 3 to 8 (LIST -> GET both files) ==="
for i in $(seq 3 8); do
    cmds="3\n4\n${SMALL_FILE}.track\n4\n${LARGE_FILE}.track\n"
    launch_peer "Peer${i}" "peer${i}" "$((9100 + i))" "$cmds"
    echo "Peer${i}: List & Get ${SMALL_FILE}.track, ${LARGE_FILE}.track"
done

echo "=== Waiting 60s (total 1m 30s) before starting Peers 9 to 13 and stopping Peers 1 & 2 ==="
sleep 50

echo "=== ~t = 80s: Download progress check (Peers 3 to 8) ==="
print_peer_snapshot 3 8

sleep 10

echo "=== t = 1m 30s: Starting Peers 9 to 13 ==="
for i in $(seq 9 13); do
    cmds="3\n4\n${SMALL_FILE}.track\n4\n${LARGE_FILE}.track\n"
    launch_peer "Peer${i}" "peer${i}" "$((9100 + i))" "$cmds"
    echo "Peer${i}: List & Get ${SMALL_FILE}.track, ${LARGE_FILE}.track"
done

echo "=== t = 1m 30s: Stopping Peer1 and Peer2 ==="
stop_peer "peer1"
stop_peer "peer2"

sleep 2
echo "=== ~t = 1m 32s: Post-1m30s status check (Peers 3 to 13) ==="
print_peer_snapshot 3 13

echo "=== Demo timeline active. Logs are in $DEMO_ROOT/. Press Ctrl+C to stop remaining processes. ==="
wait
# wait "$TRACKER_PID"