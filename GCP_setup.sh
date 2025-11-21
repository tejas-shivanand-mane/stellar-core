git clone https://github.com/tejas-shivanand-mane/stellar-core.git
sudo apt update
sudo apt -y install git
git clone https://github.com/tejas-shivanand-mane/stellar-core.git
sudo apt -y install autoconf automake libtool
sudo apt -y install build-essential
sudo apt -y install autoconf automake libtool pkg-config build-essential libsodium-dev

sudo apt -y install libpq-dev
sudo apt -y install libunwind-dev

sudo apt -y install flex bison

sudo apt -y install cargo
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
source ~/.cargo/env
rustup toolchain install 1.88.0
rustup default 1.88.0

curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
export PATH="$HOME/.cargo/bin:$PATH"


cd stellar-core
./autogen.sh
./configure
make -j$(nproc)
