#!/bin/bash

echo "=== Cleaning old builds and track files ==="
make clean
rm -f *.track

echo "=== Compiling project with Makefile ==="
make

if [ $? -ne 0 ]; then
    echo "Compilation failed! Fix errors before running demo."
    exit 1
fi

echo "=== Ensuring shared directory exists ==="
mkdir -p shared_files

echo "=== Starting Tracker Server in background ==="
./tracker &
TRACKER_PID=$!

echo "Tracker started with PID $TRACKER_PID"
echo "You can now launch './peer_client' or './peer_server' in separate terminals."

# Keep script running so user can press Ctrl+C to stop tracker
trap "kill $TRACKER_PID; echo 'Tracker stopped.'; exit" INT
wait
