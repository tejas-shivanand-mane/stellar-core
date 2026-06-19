#!/bin/bash
#SBATCH --job-name=shabdiz
#SBATCH --cpus-per-task=2
#SBATCH --mem-per-cpu=4G
#SBATCH --time=02:00:00
#SBATCH --partition=short
#SBATCH --constraint=milan
#SBATCH --output=/rhome/tmane002/work/shabdiz-logs/shabdiz-%j.log

source /etc/profile.d/modules.sh 2>/dev/null || source /usr/share/Modules/init/bash 2>/dev/null
module load slurm/24.11.1
module load gcc/12.2.0

SRUN=/opt/linux/rocky/8.x/x86_64/pkgs/slurm/24.11.1/bin/srun
GCC_LIBS=/opt/linux/rocky/8.x/x86_64/pkgs/gcc/12.2.0/lib/gcc/x86_64-pc-linux-gnu/12.2.0:/opt/linux/rocky/8.x/x86_64/pkgs/slurm/24.11.1/lib:/opt/linux/rocky/8.x/x86_64/pkgs/gcc/12.2.0/lib64

STELLAR_CORE=/rhome/tmane002/work/stellar-core/src/stellar-core
STELLAR_DIR=/rhome/tmane002/work/stellar-core
BASE_DIR=/rhome/tmane002/work/stellar-private

NUM_SERVERS=${NUM_SERVERS:-4}

# --- Wipe old logs, keep current job's log ---
find /rhome/tmane002/work/shabdiz-logs -name "shabdiz-*.log" ! -name "shabdiz-${SLURM_JOB_ID}.log" -delete

echo "=== Running experiment (NUM_SERVERS=$NUM_SERVERS) ==="

# --- Get all allocated hostnames ---
HOSTNAMES=($(scontrol show hostnames $SLURM_NODELIST))
echo "Allocated nodes: ${HOSTNAMES[@]}"

# --- Last node is client, rest are servers ---
SERVER_HOSTS=("${HOSTNAMES[@]:0:$NUM_SERVERS}")
CLIENT_HOST="${HOSTNAMES[$NUM_SERVERS]}"

echo "Server nodes: ${SERVER_HOSTS[@]}"
echo "Client node:  $CLIENT_HOST"

# --- Generate tsm_ips.txt (one IP per server, duplicates OK) ---
> $STELLAR_DIR/tsm_ips.txt
for h in "${SERVER_HOSTS[@]}"; do
    IP=$(getent ahosts $h 2>/dev/null | awk '/STREAM/ {print $1}' | grep -E '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$' | head -1)
    if [ -z "$IP" ]; then
        IP=$(getent hosts $h 2>/dev/null | awk '{print $1}' | grep -E '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$' | head -1)
    fi
    if [ -z "$IP" ]; then
        echo "ERROR: Could not resolve IPv4 for host $h"
        exit 1
    fi
    echo "$IP" >> $STELLAR_DIR/tsm_ips.txt
done

echo "Server IPs:"
cat $STELLAR_DIR/tsm_ips.txt

ACTUAL=$(wc -l < $STELLAR_DIR/tsm_ips.txt)
if [ "$ACTUAL" -ne "$NUM_SERVERS" ]; then
    echo "ERROR: Expected $NUM_SERVERS IPs but got $ACTUAL."
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

# --- Launch stellar-core instances ---
# Each srun picks up the next available task slot on allocated nodes
for i in $(seq 0 $(( NUM_SERVERS - 1 ))); do
    NODE="node$(( i + 1 ))"
    CFG="$BASE_DIR/$NODE/stellar-core.cfg"
    echo "Starting $NODE..."
    $SRUN --nodes=1 --ntasks=1 --cpus-per-task=2 \
        --export=ALL,LD_LIBRARY_PATH=$GCC_LIBS \
        $STELLAR_CORE run --conf $CFG &
done

# --- Dynamic wait based on number of servers ---
WAIT_TIME=$(( 15 + NUM_SERVERS * 2 ))
echo "Waiting ${WAIT_TIME}s for nodes to initialize..."
sleep $WAIT_TIME

# --- Run shab_client on dedicated client node ---
NODE1_IP=$(sed -n '1p' $STELLAR_DIR/tsm_ips.txt)
echo "Running shab_client on $CLIENT_HOST targeting node1 at $NODE1_IP..."
$SRUN --nodes=1 --ntasks=1 --cpus-per-task=2 \
    --nodelist=$CLIENT_HOST \
    --export=ALL,LD_LIBRARY_PATH=$GCC_LIBS \
    $STELLAR_DIR/shab_client $NODE1_IP 12000 400 4800000 100 0

# --- Cleanup ---
echo "Experiment done, shutting down stellar-core nodes..."
kill $(jobs -p) 2>/dev/null
wait