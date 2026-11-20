#!/usr/bin/env bash
set -u

# Local multi-peer demo script. Run from the project checkout root.
cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1

TORRENTS_DIR="torrents"
DEMO_ROOT="demo_runtime"
BASE_CLIENT_CFG="${CLIENT_CFG:-clientThreadConfig.cfg}"

TRACKER_HOST="127.0.0.1"
TRACKER_PORT="3490"
PEER_IP="127.0.0.1"
UPDATE_INTERVAL="15"

SMALL_FILE="small.bin"
LARGE_FILE="large.bin"
SMALL_BYTES=65536
LARGE_BYTES=10485760 # Adjust after timing a rehearsal (10 MiB by default).
MIN_LARGE_SECONDS=80

PIDS=()
TRACKER_PID=""
TIMER_PID=""

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
                    tracker_host) [ -n "$val" ] && TRACKER_HOST="$val" ;;
                    tracker_port) [ -n "$val" ] && TRACKER_PORT="$val" ;;
                    peer_ip) [ -n "$val" ] && PEER_IP="$val" ;;
                    update_interval) [ -n "$val" ] && UPDATE_INTERVAL="$val" ;;
                esac
                ;;
        esac
    done < "$cfg_path"
}

load_client_thread_config "$BASE_CLIENT_CFG"

cleanup() {
    trap - INT TERM EXIT
    echo
    echo "=== Cleaning up running demo processes ==="
    for pid in "${PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
        fi
    done
    wait 2>/dev/null || true
    echo "Demo cleanup complete."
}
trap 'exit 130' INT
trap 'exit 143' TERM
trap cleanup EXIT

echo "=== Cleaning old builds and stale .track files ==="
make clean || exit 1
rm -rf -- "$DEMO_ROOT"
mkdir -p -- "$TORRENTS_DIR"
rm -f -- ./*.track "$TORRENTS_DIR"/*.track

echo "=== Compiling project ==="
make || { echo "Compilation failed!" >&2; exit 1; }

mkdir -p "$DEMO_ROOT/peer1/shared" "$DEMO_ROOT/peer2/shared"
head -c "$SMALL_BYTES" </dev/urandom > "$DEMO_ROOT/peer1/shared/$SMALL_FILE"
head -c "$LARGE_BYTES" </dev/urandom > "$DEMO_ROOT/peer2/shared/$LARGE_FILE"

write_peer_configs() {
    local peer_name="$1"
    local peer_port="$2"
    local peer_dir="$DEMO_ROOT/$peer_name"
    local share_dir="$peer_dir/shared"
    local dl_dir="$peer_dir/downloads"

    mkdir -p "$share_dir" "$dl_dir"

    cat > "$peer_dir/serverThreadConfig.cfg" <<CFG
PORT=${peer_port}
SHARED_DIR=${share_dir}
DOWNLOAD_DIR=${dl_dir}
CFG

    cat > "$peer_dir/clientThreadConfig.cfg" <<CFG
tracker_host=${TRACKER_HOST}
tracker_port=${TRACKER_PORT}
peer_ip=${PEER_IP}
peer_port=${peer_port}
shared_dir=${share_dir}
download_dir=${dl_dir}
update_interval=${UPDATE_INTERVAL}
CFG
}

launch_peer() {
    local label="$1"
    local peer_name="$2"
    local peer_port="$3"
    local cmd_script="$4"

    local peer_dir="$DEMO_ROOT/$peer_name"
    local fifo="$peer_dir/cmd.fifo"

    write_peer_configs "$peer_name" "$peer_port"
    rm -f -- "$fifo"
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

    printf -v "${peer_name}_SRV_PID" '%s' "$srv_pid"
    printf -v "${peer_name}_CLI_PID" '%s' "$cli_pid"
    printf -v "${peer_name}_FEEDER_PID" '%s' "$feeder_pid"
    printf -v "${peer_name}_LABEL" '%s' "$label"
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

# Time one real large-file transfer using Peer 3's downloader log.
# The downloader prints 'Requesting large.bin' when it begins processing
# the transfer and 'Complete ...large.bin (MD5 verified: ...)' on success.
# The polling interval introduces up to ~0.2s of measurement uncertainty.
monitor_large_download() {
    local log="$DEMO_ROOT/peer3/client.log"
    local results="$DEMO_ROOT/large_download_time.txt"
    local start_ms=""
    local end_ms=""

    printf 'Peer3 large.bin timing: waiting for download start\n' | tee "$results"
    while :; do
        if grep -Fq "[Downloader] Requesting ${LARGE_FILE} (" "$log" 2>/dev/null; then
            start_ms=$(date +%s%3N)
            printf 'Peer3 large.bin START: %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" | tee -a "$results"
            break
        fi
        if ! kill -0 "$peer3_CLI_PID" 2>/dev/null; then
            echo "Peer3 ended before the large.bin download started." | tee -a "$results"
            return 1
        fi
        sleep 0.1
    done

    while :; do
        if grep -Eq "\[Downloader\] Complete .*${LARGE_FILE} \(MD5 verified:" "$log" 2>/dev/null; then
            end_ms=$(date +%s%3N)
            break
        fi
        if ! kill -0 "$peer3_CLI_PID" 2>/dev/null; then
            echo "Peer3 exited before verified large.bin completion." | tee -a "$results"
            return 1
        fi
        sleep 0.1
    done

    local elapsed_ms=$((end_ms - start_ms))
    local seconds=$((elapsed_ms / 1000))
    local fraction=$((elapsed_ms % 1000))
    printf 'Peer3 large.bin FINISH: %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" | tee -a "$results"
    printf 'Peer3 large.bin measured duration: %d.%03d seconds\n' "$seconds" "$fraction" | tee -a "$results"
    if ((elapsed_ms >= MIN_LARGE_SECONDS * 1000)); then
        echo "PASS: large.bin took at least ${MIN_LARGE_SECONDS} seconds." | tee -a "$results"
    else
        echo "FAIL: large.bin took less than ${MIN_LARGE_SECONDS} seconds; use a larger file for the demo." | tee -a "$results"
    fi
}

echo "=== t = 0s: Starting tracker and seed peers ==="
./tracker > "$DEMO_ROOT/tracker.log" 2>&1 &
TRACKER_PID=$!
PIDS+=("$TRACKER_PID")
sleep 1
if ! kill -0 "$TRACKER_PID" 2>/dev/null; then
    echo "ERROR: Tracker failed to start (check $DEMO_ROOT/tracker.log)." >&2
    exit 1
fi

launch_peer "Peer1" "peer1" 9101 "1\n${SMALL_FILE}\nSmall_Demo_File\n"
echo "Peer1: createtracker ${SMALL_FILE} initiated"
launch_peer "Peer2" "peer2" 9102 "1\n${LARGE_FILE}\nLarge_Demo_File\n"
echo "Peer2: createtracker ${LARGE_FILE} initiated"

echo "=== Waiting 30s before starting Peers 3 to 8 ==="
sleep 30

echo "=== t ≈ 30s: Starting Peers 3 to 8 (LIST -> GET both files) ==="
for i in $(seq 3 8); do
    cmds="3\n4\n${SMALL_FILE}.track\n4\n${LARGE_FILE}.track\n"
    launch_peer "Peer${i}" "peer${i}" "$((9100 + i))" "$cmds"
    echo "Peer${i}: LIST & GET ${SMALL_FILE}.track, ${LARGE_FILE}.track"
    if ((i == 3)); then
        monitor_large_download &
        TIMER_PID=$!
        PIDS+=("$TIMER_PID")
    fi
done

echo "=== Waiting 60s before starting Peers 9 to 13 ==="
sleep 60

echo "=== t ≈ 90s: Starting Peers 9 to 13 ==="
for i in $(seq 9 13); do
    cmds="3\n4\n${SMALL_FILE}.track\n4\n${LARGE_FILE}.track\n"
    launch_peer "Peer${i}" "peer${i}" "$((9100 + i))" "$cmds"
    echo "Peer${i}: LIST & GET ${SMALL_FILE}.track, ${LARGE_FILE}.track"
done

echo "=== Stopping Peer1 and Peer2 ==="
stop_peer "peer1"
stop_peer "peer2"

echo "=== Demo running; logs in ${DEMO_ROOT}/. Press Ctrl+C to stop. ==="
echo "Large-file timing result: ${DEMO_ROOT}/large_download_time.txt"
wait "$TRACKER_PID"
