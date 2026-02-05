## Stellar-O3RUBC:

'''
# --- Update and install basic tools ---
sudo apt update
sudo apt -y upgrade
sudo apt install -y git build-essential pkg-config autoconf automake libtool bison flex libpq-dev libunwind-dev parallel sed perl wget curl cmake
# --- Install GCC 14 and G++ 14 and make default ---
sudo apt install -y gcc-14 g++-14
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100
sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 100
sudo update-alternatives --set gcc /usr/bin/gcc-14
sudo update-alternatives --set g++ /usr/bin/g++-14
# --- Install Clang 18 ---
sudo apt install -y clang-18 libclang-18-dev libclang-rt-18-dev libunwind-18 libunwind-dev
# --- Install Rust 1.88 using rustup ---
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
source $HOME/.cargo/env
rustup install 1.88.0
rustup default 1.88.0
# --- Verify Rust/Cargo ---
rustc --version
cargo --version
# --- Install Stellar-Core dependencies ---
sudo apt install -y libsodium-dev libpq-dev libunwind-dev
git clone https://anonymous.4open.science/r/stellar-core-ED50.git
cd stellar-core/
./autogen.sh ; ./configure; make -j16
'''

### Key Modified Files

OverlayManagerImpl.cpp

Stellar-overlay.x

Peer.cpp

Config.cpp

and their corresponding .h files

### Setup

The script base.sh contains helper commands to create the required node directories and configuration files.

These commands are initially commented out:

Uncomment them during the first setup.

After the setup is complete, re-comment them to avoid re-creating the directories on subsequent runs.
