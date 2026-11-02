## Stellar-Shabdiz:

This is the repository for project titled "Common-case latency-optimum Responsive Unauthenticated Byzantine Consensus"

## Setup

On an ubuntu system:
```bash
# --- Update system and install basic tools ---
sudo apt update
sudo apt -y upgrade
sudo apt install -y \
  git build-essential pkg-config autoconf automake libtool \
  bison flex libpq-dev libunwind-dev parallel sed perl \
  wget curl cmake

# --- Install GCC 14 / G++ 14 and set as default ---
sudo apt install -y gcc-14 g++-14
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100
sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 100
sudo update-alternatives --set gcc /usr/bin/gcc-14
sudo update-alternatives --set g++ /usr/bin/g++-14

# --- Install Clang 18 ---
sudo apt install -y clang-18 libclang-18-dev libclang-rt-18-dev \
  libunwind-18 libunwind-dev

# --- Install Rust 1.88 using rustup ---
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
source "$HOME/.cargo/env"
rustup install 1.88.0
rustup default 1.88.0

# --- Verify Rust/Cargo ---
rustc --version
cargo --version

# --- Install Stellar-Core dependencies and build ---
sudo apt install -y libsodium-dev libpq-dev libunwind-dev

git clone https://anonymous.4open.science/r/stellar-core-ED50.git
cd stellar-core

./autogen.sh
./configure
make -j"$(nproc)"
```

### Key Modified Files compared to Stellar-core by Stellar

Stellar-overlay.x, OverlayManagerImpl.cpp ,Peer.cpp, Config.cpp

and their corresponding .h files


## Running Experiments

Run the four-node Shabdiz experiment with RUNGCP.ipynb. Stores results in /home/user/work/experiments/shabdiz/latest_4node_run


## Post-Processing

Post process the results with PostProcess.ipynb. Uses results stored in /home/user/work/experiments/shabdiz/latest_4node_run to plot throughput and latency vs time plots

## TetraBFT normal path

Implements [Basic TetraBFT, Section 3.2](https://arxiv.org/html/2405.02615v2):
proposal followed by four voting phases, with n-f quorums. Each batch is an
independent instance; recovery and pipelined multi-shot TetraBFT are not included.

Add these top-level settings to every replica configuration:

```toml
TETRABFT_MODE=true
TETRABFT_LEADER="G...public key of the leader..."
```

Use the same leader key everywhere and `SEND_CUSTOM_MESSAGE=true` only on the
leader. Keep PBFT_MODE, ITHS_MODE and ENABLE_SCP_TRACKING disabled. Membership
is self plus QUORUM_SET validators; every replica must have the same membership
of at least four nodes. Rebuild all replicas with the default current schema.
Run the existing shab_client against the leader on port 12000 with aggregate
concurrency >=100 (the batch size). Check `[TETRA COMMITTED]` logs for matching
sequence numbers and digests. Reads retain the existing immediate-ACK behavior.
Write ACKs indicate leader-local execution, not a Byzantine client certificate.
Use fresh, fully connected deployments; there is no restart/state-transfer
support, and replicas more than 64 instances behind cannot catch up.

Voting-core tests (not full Stellar integration tests):

```sh
g++ -std=c++17 -Wall -Wextra -Werror -pedantic -Isrc tests/tetrabft/normal_path.cpp -o /tmp/tetrabft-test
/tmp/tetrabft-test
```
