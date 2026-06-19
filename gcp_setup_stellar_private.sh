#!/bin/bash
set -e

# STELLAR_CORE=/home/tejas/stellar-core/src/stellar-core
# BASE_DIR=/home/tejas/stellar-private

STELLAR_CORE=/rhome/tmane002/work/stellar-core/src/stellar-core
BASE_DIR=/rhome/tmane002/work/stellar-private


# Read IPs into an array (defines the number of nodes)
# mapfile -t NODE_IPS < /home/tejas/stellar-core/tsm_ips.txt
mapfile -t NODE_IPS < /rhome/tmane002/work/stellar-core/tsm_ips.txt


# Get the total number of nodes from the IP list size
NUM_NODES=${#NODE_IPS[@]}
echo "Detected $NUM_NODES nodes based on tsm_ips.txt."

# --------------------------------------------------------------------
# Helper function to generate node name (e.g., node1, node2)
# --------------------------------------------------------------------
get_node_name() {
    echo "node$(( $1 + 1 ))"
}

if [ "$1" == "start" ]; then
    
    # Declare associative arrays to hold ports
    declare -A HTTP_PORT
    declare -A PEER_PORT

    # Remove old nodes and create new directories, and assign ports dynamically
    for (( i=0; i<NUM_NODES; i++ )); do
        NODE=$(get_node_name $i)
        
        # 🟢 Port step size changed from 10000 to 10.
        PEER_PORT[$NODE]=$(( 11625 + (i * 10) ))
        HTTP_PORT[$NODE]=$(( 11626 + (i * 10) ))

        echo "Cleaning and creating directory for $NODE. Ports: Peer ${PEER_PORT[$NODE]}, HTTP ${HTTP_PORT[$NODE]}..."
        rm -rf "$BASE_DIR/$NODE"
        mkdir -p "$BASE_DIR/$NODE"
    done

    # Arrays to hold seeds and public keys
    declare -A NODE_SEED
    declare -A NODE_PUBLIC

    # Generate seeds and extract public keys, create local history and db directories
    for (( i=0; i<NUM_NODES; i++ )); do
        NODE=$(get_node_name $i)
        NODE_DIR="$BASE_DIR/$NODE"
        SEED_FILE="$NODE_DIR/node.seed"

        echo "Generating seed for $NODE..."
        if ! $STELLAR_CORE gen-seed > "$SEED_FILE"; then
            echo "ERROR: Failed to generate seed for $NODE." >&2
            exit 1
        fi

        NODE_SEED[$NODE]=$(grep "Secret seed:" "$SEED_FILE" | awk '{print $3}')
        NODE_PUBLIC[$NODE]=$(grep "Public:" "$SEED_FILE" | awk '{print $2}')

        mkdir -p "$NODE_DIR/history" "$NODE_DIR/db"
    done

    # Step 1: Create config files for all nodes
    for (( i=0; i<NUM_NODES; i++ )); do
        NODE=$(get_node_name $i)
        NODE_DIR="$BASE_DIR/$NODE"
        CFG="$NODE_DIR/stellar-core.cfg"

        echo "Creating config file for $NODE..."
        
        # Build known peers list
        # 🟢 FIX: Use an array to store entries and IFS to join them cleanly with a comma and newline.
        PEER_ENTRIES=() 
        for (( j=0; j<NUM_NODES; j++ )); do
            PEER_NODE=$(get_node_name $j)
            PEER_ENTRIES+=("\"${NODE_IPS[$j]}:${PEER_PORT[$PEER_NODE]}\"")
        done
        
        # Join all entries with a comma and a newline
        # The result will be a list of quoted strings separated by ",\n"
        KNOWN_PEERS_CFG=$(IFS=$',\n'; echo "${PEER_ENTRIES[*]}")
        
        # Build validators list (QUORUM) excluding the current node
        VALIDATORS_CFG=""
        for (( V_i=0; V_i<NUM_NODES; V_i++ )); do
            V=$(get_node_name $V_i)
            if [ "$V" != "$NODE" ]; then
                VALIDATORS_CFG+="[[VALIDATORS]]
NAME=\"$V\"
HOME_DOMAIN=\"private\"
PUBLIC_KEY=\"${NODE_PUBLIC[$V]}\"

HISTORY=\"local $BASE_DIR/$V/history/{0}\"

"
            fi
        done

cat > "$CFG" <<EOF
NODE_SEED="${NODE_SEED[$NODE]}"
NODE_IS_VALIDATOR=true
NODE_HOME_DOMAIN="private"
RUN_STANDALONE=false
HTTP_PORT=${HTTP_PORT[$NODE]}
PEER_PORT=${PEER_PORT[$NODE]}

NETWORK_PASSPHRASE="Private Stellar Network"

DATABASE="sqlite3://$NODE_DIR/db/stellar.db"
BUCKET_DIR_PATH="$NODE_DIR/buckets"
LOG_FILE_PATH="$NODE_DIR/stellar-core.log"

# Known peers
KNOWN_PEERS=[
${KNOWN_PEERS_CFG}
]

# Home domain (HIGH quality)
[[HOME_DOMAINS]]
HOME_DOMAIN="private"
QUALITY="HIGH"

# Validators (exclude self)
$VALIDATORS_CFG
EOF
    done

else

    # Step 2: Initialize databases for all nodes
    for (( i=0; i<NUM_NODES; i++ )); do
        NODE=$(get_node_name $i)
        CFG="$BASE_DIR/$NODE/stellar-core.cfg"

        rm -f "$BASE_DIR/$NODE/stellar-core.log"
        echo "Initializing database for $NODE..."
        if ! $STELLAR_CORE new-db --conf "$CFG"; then
            echo "⚠ Warning: new-db failed for $NODE, continuing..."
        fi
    done

    echo "✅ $NUM_NODES-node private Stellar network setup complete!"
    echo "Start the nodes with (substituting node number):"
    
    # Generate startup commands for reference
    for (( i=0; i<NUM_NODES; i++ )); do
        NODE=$(get_node_name $i)
        echo "$STELLAR_CORE run --conf $BASE_DIR/$NODE/stellar-core.cfg &"
    done

fi