#!/bin/bash
#SBATCH --job-name=shabdiz
#SBATCH --cpus-per-task=2
#SBATCH --mem=8G
#SBATCH --time=02:00:00
#SBATCH --partition=short
#SBATCH --constraint=milan
#SBATCH --output=/rhome/tmane002/work/shabdiz-logs/shabdiz-%j.log

module load slurm/24.11.1

STELLAR_CORE=/rhome/tmane002/work/stellar-core/src/stellar-core
STELLAR_DIR=/rhome/tmane002/work/stellar-core
BASE_DIR=/rhome/tmane002/work/stellar-private

NUM_SERVERS=${NUM_SERVERS:-4}

echo "=== Running experiment (NUM_SERVERS=$NUM_SERVERS) ==="

HOSTNAMES=($(scontrol show hostnames $SLURM_NODELIST))

SERVER_HOSTS=("${HOSTNAMES[@]:0:$NUM_SERVERS}")
CLIENT_HOST="${HOSTNAMES[$NUM_SERVERS]}"

echo "Server nodes: ${SERVER_HOSTS[@]}"
echo "Client node:  $CLIENT_HOST"

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

rm -rf $BASE_DIR && mkdir -p $BASE_DIR

cp $STELLAR_DIR/gcp_setup_stellar_private.sh $BASE_DIR/
cd $BASE_DIR
chmod +x gcp_setup_stellar_private.sh
./gcp_setup_stellar_private.sh start
./gcp_setup_stellar_private.sh

sed -i '1s/^/SEND_CUSTOM_MESSAGE=true\n/' $BASE_DIR/node1/stellar-core.cfg
sed -i '1s/^/MEMORY_PROF=true\n/' $BASE_DIR/node2/stellar-core.cfg

for i in $(seq 0 $(( NUM_SERVERS - 1 ))); do
    NODE="node$(( i + 1 ))"
    HOST="${SERVER_HOSTS[$i]}"
    CFG="$BASE_DIR/$NODE/stellar-core.cfg"
    echo "Starting $NODE on $HOST..."
    srun --nodes=1 --ntasks=1 --nodelist=$HOST \
        $STELLAR_CORE run --conf $CFG &
done

sleep 15

NODE1_IP=$(sed -n '1p' $STELLAR_DIR/tsm_ips.txt)
echo "Running shab_client on $CLIENT_HOST targeting node1 at $NODE1_IP..."
srun --nodes=1 --ntasks=1 --nodelist=$CLIENT_HOST \
    $STELLAR_DIR/shab_client $NODE1_IP 12000 400 4800000 100 0

echo "Experiment done, shutting down stellar-core nodes..."
kill $(jobs -p) 2>/dev/null
wait