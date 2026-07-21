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

Run the four-node Shabdiz experiment:

python3 run_shabdiz_4nodes.py

Results are saved under:

~/work/experiments/shabdiz/

## Post-Processing

Plot throughput and latency for the latest run:

python3 plot_shabdiz_timeseries.py

Or specify a run directory:

python3 plot_shabdiz_timeseries.py <run-directory>

Generated plots and CSV files are saved in:

<run-directory>/plots/