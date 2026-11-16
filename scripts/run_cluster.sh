#!/bin/bash
# Starts nodes n1, n2 and n3 in the background. Logs go to logs/<id>.log.
cd "$(dirname "$0")/.."

if [ ! -x build/dcache_node ]; then
    echo "build/dcache_node not found - build first (cmake --build build -j)"
    exit 1
fi
if [ -f .pids ]; then
    echo "cluster seems to be running already (.pids exists) - run scripts/stop_cluster.sh first"
    exit 1
fi

mkdir -p logs
for id in n1 n2 n3; do
    ./build/dcache_node --id "$id" --config cluster.conf > "logs/$id.log" 2>&1 &
    echo $! >> .pids
done
sleep 0.5
echo "cluster started (pids: $(tr '\n' ' ' < .pids))"
