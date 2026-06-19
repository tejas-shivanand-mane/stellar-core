#!/bin/bash
#SBATCH --job-name=shabdiz
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=32
#SBATCH --mem=64G
#SBATCH --time=02:00:00
#SBATCH --partition=short
#SBATCH --output=/rhome/tmane002/work/shabdiz-logs/shabdiz-%j.log

module load slurm/24.11.1
module load gcc/12.2.0

# --- Activate conda properly in batch jobs ---
source /etc/profile.d/conda.sh 2>/dev/null || source $HOME/.bashrc
conda activate /rhome/tmane002/stellar

export RUSTUP_HOME=$HOME/local/rustup
export CARGO_HOME=$HOME/local/cargo
source $HOME/local/cargo/env

STELLAR_CORE=/rhome/tmane002/work/stellar-core/src/stellar-core
STELLAR_DIR=/rhome/tmane002/work/stellar-core
BASE_DIR=/rhome/tmane002/work/stellar-private

# --- Number of server nodes (default 4, override via --export=NUM_SERVERS=N) ---
NUM_SERVERS=${NUM_SERVERS:-4}
TOTAL_NODES=$(( NUM_SERVERS + 1 ))  # +1 for dedicated client node

# ---------------------------------------------------------------
# PHASE 1: Compile (single node, runs first)
# ---------------------------------------------------------------
if [ "${PHASE}" != "run" ]; then
    echo "=== PHASE 1: Compiling (NUM_SERVERS=$NUM_SERVERS) ==="

    mkdir -p /rhome/tmane002/work/shabdiz-logs

    cd $STELLAR_DIR

    # --- Remove only the stale object file causing linker error ---

    make -j16
    if [ $? -ne 0 ]; then
        echo "ERROR: stellar-core compile failed, aborting."
        exit 1
    fi
    echo "stellar-core compile done."

    g++ -O2 -std=c++17 -pthread -Isrc src/overlay/shab_client.cpp -o shab_client
    if [ $? -ne 0 ]; then
        echo "ERROR: shab_client compile failed, aborting."
        exit 1
    fi
    echo "shab_client compile done."

    echo "=== Submitting experiment phase with $TOTAL_NODES nodes ($NUM_SERVERS servers + 1 client) ==="
    sbatch --nodes=$TOTAL_NODES --ntasks=$TOTAL_NODES --cpus-per-task=8 --mem=16G \
           --export=PHASE=run,NUM_SERVERS=$NUM_SERVERS \
           $STELLAR_DIR/run_shabdiz.sh
    exit 0
fi

# ---------------------------------------------------------------
# PHASE 2: Experiment (NUM_SERVERS+1 nodes, runs after compile)
# ---------------------------------------------------------------
echo "=== PHASE 2: Running experiment (NUM_SERVERS=$NUM_SERVERS) ==="

# --- Get all allocated hostnames ---
HOSTNAMES=($(scontrol show hostnames $SLURM_NODELIST))

# --- First NUM_SERVERS nodes are servers, last node is client ---
SERVER_HOSTS=("${HOSTNAMES[@]:0:$NUM_SERVERS}")
CLIENT_HOST="${HOSTNAMES[$NUM_SERVERS]}"

echo "Server nodes: ${SERVER_HOSTS[@]}"
echo "Client node:  $CLIENT_HOST"

# --- Generate tsm_ips.txt from server nodes only (IPv4 only, with fallback) ---
> $STELLAR_DIR/tsm_ips.txt
for h in "${SERVER_HOSTS[@]}"; do
    # Try getent first, then nslookup as fallback
    IP=$(getent hosts $h | awk '{print $1}' | grep -v '^fe80' | head -1)
    if [ -z "$IP" ]; then
        IP=$(nslookup $h 2>/dev/null | awk '/^Address:/ && !/:#/ {print $2}' | grep -v '^fe80' | head -1)
    fi
    if [ -z "$IP" ]; then
        echo "ERROR: Could not resolve IP for host $h"
        exit 1
    fi
    echo "$IP" >> $STELLAR_DIR/tsm_ips.txt
done

echo "Server IPs:"
cat $STELLAR_DIR/tsm_ips.txt

# --- Verify we got exactly NUM_SERVERS IPs ---
ACTUAL=$(wc -l < $STELLAR_DIR/tsm_ips.txt)
if [ "$ACTUAL" -ne "$NUM_SERVERS" ]; then
    echo "ERROR: Expected $NUM_SERVERS IPs but got $ACTUAL. Check getent output."
    exit 1
fi

# --- Wipe and recreate stellar-private for a clean run ---
rm -rf $BASE_DIR && mkdir -p $BASE_DIR

# --- Setup configs and init DBs ---
cp $STELLAR_DIR/gcp_setup_stellar_private.sh $BASE_DIR/
cd $BASE_DIR
chmod +x gcp_setup_stellar_private.sh
./gcp_setup_stellar_private.sh start
./gcp_setup_stellar_private.sh

# --- Patch node1 and node2 configs ---
sed -i '1s/^/SEND_CUSTOM_MESSAGE=true\n/' $BASE_DIR/node1/stellar-core.cfg
sed -i '1s/^/MEMORY_PROF=true\n/' $BASE_DIR/node2/stellar-core.cfg

# --- Launch stellar-core on each server node via srun ---
for i in $(seq 0 $(( NUM_SERVERS - 1 ))); do
    NODE="node$(( i + 1 ))"
    HOST="${SERVER_HOSTS[$i]}"
    CFG="$BASE_DIR/$NODE/stellar-core.cfg"
    echo "Starting $NODE on $HOST..."
    srun --nodes=1 --ntasks=1 --nodelist=$HOST \
        $STELLAR_CORE run --conf $CFG &
done

# --- Wait for nodes to initialize ---
sleep 15

# --- Run shab_client on dedicated client node targeting node1 ---
NODE1_IP=$(sed -n '1p' $STELLAR_DIR/tsm_ips.txt)
echo "Running shab_client on $CLIENT_HOST targeting node1 at $NODE1_IP..."
srun --nodes=1 --ntasks=1 --nodelist=$CLIENT_HOST \
    $STELLAR_DIR/shab_client $NODE1_IP 12000 400 4800000 100 0

# --- Cleanup ---
echo "Experiment done, shutting down stellar-core nodes..."
kill $(jobs -p) 2>/dev/null
wait