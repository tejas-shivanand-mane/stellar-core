#!/bin/bash
set -e

STELLAR_CORE=~/work/stellar-core/src/stellar-core
BASE_DIR=~/work/stellar-private



# # Node ports
# declare -A HTTP_PORT=( [node1]=11626 [node2]=21626 [node3]=31626 [node4]=41626 )
# declare -A PEER_PORT=( [node1]=11625 [node2]=21625 [node3]=31625 [node4]=41625 )

# # Remove old nodes
# rm -rf $BASE_DIR/node1 $BASE_DIR/node2 $BASE_DIR/node3 $BASE_DIR/node4
# mkdir -p $BASE_DIR/node1 $BASE_DIR/node2 $BASE_DIR/node3 $BASE_DIR/node4

# # Arrays to hold seeds and public keys
# declare -A NODE_SEED
# declare -A NODE_PUBLIC

# # Generate seeds and extract public keys, create local history and db directories
# for NODE in node1 node2 node3 node4; do
#     NODE_DIR="$BASE_DIR/$NODE"
#     SEED_FILE="$NODE_DIR/node.seed"

#     echo "Generating seed for $NODE..."
#     $STELLAR_CORE gen-seed > "$SEED_FILE"

#     NODE_SEED[$NODE]=$(grep "Secret seed:" "$SEED_FILE" | awk '{print $3}')
#     NODE_PUBLIC[$NODE]=$(grep "Public:" "$SEED_FILE" | awk '{print $2}')

#     mkdir -p "$NODE_DIR/history" "$NODE_DIR/db"
# done

# # Step 1: Create config files for all nodes
# for NODE in node1 node2 node3 node4; do
#     NODE_DIR="$BASE_DIR/$NODE"
#     CFG="$NODE_DIR/stellar-core.cfg"

#     echo "Creating config file for $NODE..."

#     # Build validators list excluding the current node
#     VALIDATORS_CFG=""
#     for V in node1 node2 node3 node4; do
#         if [ "$V" != "$NODE" ]; then
#             VALIDATORS_CFG+="[[VALIDATORS]]
# NAME=\"$V\"
# HOME_DOMAIN=\"private\"
# PUBLIC_KEY=\"${NODE_PUBLIC[$V]}\"


# HISTORY=\"local $BASE_DIR/$V/history/{0}\"

# "
#         fi
#     done

#     cat > $CFG <<EOF
# NODE_SEED="${NODE_SEED[$NODE]}"
# NODE_IS_VALIDATOR=true
# NODE_HOME_DOMAIN="private"
# RUN_STANDALONE=false
# HTTP_PORT=${HTTP_PORT[$NODE]}
# PEER_PORT=${PEER_PORT[$NODE]}

# NETWORK_PASSPHRASE="Private Stellar Network"

# DATABASE="sqlite3://$NODE_DIR/db/stellar.db"
# BUCKET_DIR_PATH="$NODE_DIR/buckets"
# LOG_FILE_PATH="$NODE_DIR/stellar-core.log"

# # Known peers
# KNOWN_PEERS=[
# "127.0.0.1:${PEER_PORT[node1]}",
# "127.0.0.1:${PEER_PORT[node2]}",
# "127.0.0.1:${PEER_PORT[node3]}",
# "127.0.0.1:${PEER_PORT[node4]}"
# ]

# # Home domain (HIGH quality)
# [[HOME_DOMAINS]]
# HOME_DOMAIN="private"
# QUALITY="HIGH"

# # Validators (exclude self)
# $VALIDATORS_CFG
# EOF
# done

# Step 2: Initialize databases for all nodes
for NODE in node1 node2 node3 node4; do
    CFG="$BASE_DIR/$NODE/stellar-core.cfg"

    rm -f $BASE_DIR/$NODE/stellar-core.log
    echo "Initializing database for $NODE..."
    if ! $STELLAR_CORE new-db --conf "$CFG"; then
        echo "⚠ Warning: new-db failed for $NODE, continuing..."
    fi
done

echo "✅ 4-node private Stellar network setup complete!"
echo "Start the nodes with:"
echo "$STELLAR_CORE run --conf $BASE_DIR/node1/stellar-core.cfg &"
echo "$STELLAR_CORE run --conf $BASE_DIR/node2/stellar-core.cfg &"
echo "$STELLAR_CORE run --conf $BASE_DIR/node3/stellar-core.cfg &"
echo "$STELLAR_CORE run --conf $BASE_DIR/node4/stellar-core.cfg &"
