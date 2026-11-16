#!/bin/bash
# Stops the nodes started by run_cluster.sh (SIGTERM -> clean shutdown).
cd "$(dirname "$0")/.."

if [ ! -f .pids ]; then
    echo "no .pids file - cluster not running?"
    exit 0
fi
while read -r pid; do
    kill "$pid" 2>/dev/null
done < .pids
rm -f .pids
echo "cluster stopped"
