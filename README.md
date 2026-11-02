# Stellar-Shabdiz

Research artifact for evaluating Shabdiz and related Byzantine consensus protocols in a common Stellar Core codebase. The custom protocols share the overlay transport, transaction batching, workload generator, and client acknowledgement interface.

## Protocols and scope

| Protocol | Implementation | Selection |
| --- | --- | --- |
| Shabdiz | Existing custom consensus implementation | Default; all protocol switches below disabled |
| IT-HS | Existing custom consensus implementation | Source switch `ITHS_MODE=true` |
| PBFT | Existing custom pre-prepare/prepare/commit implementation | Source switch `PBFT_MODE=true` |
| SCP | Native Stellar consensus with the existing transaction-generation and measurement harness | Source switch `ENABLE_SCP_TRACKING=true` |
| TetraBFT | Basic TetraBFT normal path: proposal and four voting phases | Configuration `TETRABFT_MODE=true` |
| Simple-IT | Consecutive-round normal path with Bracha RBC, or optimistic RBC | Configuration `SIMPLE_IT_MODE=true` |
| Forget-IT | First-view normal path: prepare, vote-1, vote-2 | Configuration `FORGET_IT_MODE=true` |

TetraBFT and Forget-IT process each batch as a fresh consensus instance. Simple-IT uses the previous round as its parent and waits for local parent execution before committing the child. These three implementations use a fixed leader and serialize proposals; they do not implement leader replacement, timeouts, restart recovery, state transfer, or the papers' pipelined variants. Simple-IT does not implement disabled rounds or speculative execution. This repository's PBFT implementation is the integrated custom baseline; external PBFT implementations used in separate experiments are not included here.

The new paths use `f = floor((n-1)/3)` and `n-f` consensus quorums. For `n=3f+1`, these are the papers' `2f+1` quorums; other network sizes use this conservative benchmark adaptation. Proposals carry full batches and votes carry SHA-256 digests over authenticated Stellar peer channels. Consequently, these are shared-transport benchmark implementations, not implementations of an information-theoretically secure transport or a long-message dispersal protocol.

## Build

Use a Linux development environment with GCC 14 or Clang 18, Rust 1.88.0 (as pinned in `rust-toolchain.toml`), and the Stellar Core build dependencies. On Ubuntu with these compiler packages available:

```bash
sudo apt update
sudo apt install -y git build-essential pkg-config autoconf automake libtool \
  bison flex libpq-dev libunwind-dev libsodium-dev parallel perl \
  wget curl cmake gcc-14 g++-14 clang-18 libclang-18-dev libclang-rt-18-dev

curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
source "$HOME/.cargo/env"
rustup toolchain install 1.88.0

git clone --recursive https://github.com/tejas-shivanand-mane/stellar-core.git
cd stellar-core
./autogen.sh
CC=gcc-14 CXX=g++-14 ./configure
make -j"$(nproc)"

g++ -O2 -std=c++17 -pthread -Isrc shab_client.cpp -o shab_client
```

Build every replica from the same revision, using the default **current** protocol schema. The custom message types are defined in `src/protocol-curr/xdr/Stellar-overlay.x`; the upstream next-protocol schema does not contain these extensions. Rebuild all replicas after changing source switches or the wire schema.

## Select a protocol

The existing source switches are in `src/overlay/OverlayManagerImpl.cpp`:

| Run | `ITHS_MODE` | `PBFT_MODE` | `ENABLE_SCP_TRACKING` |
| --- | --- | --- | --- |
| Shabdiz | `false` | `false` | `false` |
| IT-HS | `true` | `false` | `false` |
| PBFT | `false` | `true` | `false` |
| SCP | `false` | `false` | `true` |
| TetraBFT, Simple-IT, or Forget-IT | `false` | `false` | `false` |

Keep `NonPrune_MODE=false` for the default Shabdiz configuration; enable it only for an explicitly labelled no-pruning experiment. For Shabdiz, IT-HS, PBFT, and SCP, leave `TETRABFT_MODE`, `SIMPLE_IT_MODE`, and `FORGET_IT_MODE` disabled (their defaults).

For the newer protocols, add **one** of the following blocks at the top of every replica's configuration, before any TOML table headers. Replace the placeholder with the selected leader's full `G...` public key, identical on every replica.

**TetraBFT**

```toml
TETRABFT_MODE=true
TETRABFT_LEADER="<leader public key>"
```

**Simple-IT**

```toml
SIMPLE_IT_MODE=true
SIMPLE_IT_LEADER="<leader public key>"
SIMPLE_IT_OPTIMISTIC=true
```

With `SIMPLE_IT_OPTIMISTIC=true` (the default), optimistic delivery requires `ceil((n+2f-2)/2)` matching ECHOs from **nonleader** replicas, followed by `n-f` consensus COMMITs. The resulting critical path has three message delays. Auxiliary VOTE, ACK, and READY messages are also sent; they must be included in communication measurements. With `SIMPLE_IT_OPTIMISTIC=false`, the implementation uses Bracha-style ECHO/READY delivery followed by COMMIT, giving a four-delay normal path. Use identical settings on every replica and report the variant with each result.

**Forget-IT**

```toml
FORGET_IT_MODE=true
FORGET_IT_LEADER="<leader public key>"
```

Every instance starts in view 1 with a SAFE guard. A replica sends vote-2 after `n-f` matching vote-1 messages and decides after `n-f` vote-2 messages, with the paper's additional `f+1` matching vote-1 acceptance condition. The normal path has three message delays.

For every custom-protocol run, set `SEND_CUSTOM_MESSAGE=true` only on the leader and leave it false on all other replicas. For SCP, this setting selects the load-generating node. The newer protocol modes reject conflicting mode settings and an inconsistent local leader setting at startup.

## Configure and run a private network

1. Put replica IP addresses, one per line, in `tsm_ips.txt`. Do not include client machines.
2. Update `STELLAR_CORE`, `BASE_DIR`, and the IP-file path in `gcp_setup_stellar_private.sh` for your installation. Run `bash gcp_setup_stellar_private.sh start` to generate node identities and configurations in the configured output directory.
3. Add the protocol settings above to the generated configurations. Use node1 as the leader when using the supplied experiment scripts. Its public key is recorded in its generated `node.seed` file.
4. Run `bash gcp_setup_stellar_private.sh` to initialize the databases. Copy each node's configuration and required directories to its host, preserving or adjusting the configured paths.
5. Start each replica with `src/stellar-core run --conf /path/to/nodeN/stellar-core.cfg`. Wait for the leader's client-listener readiness log before starting the workload.

The setup script recreates node directories in its `start` step; use a separate output directory for each fresh experiment. Replicas must share the same network passphrase and membership, have sufficient overlay connection capacity, and be fully connected. Preserve the generated `KNOWN_PEERS` convention, which includes all replica endpoints including self: the experiment startup gate expects `KNOWN_PEERS.size()-1` authenticated peers.

For TetraBFT, Simple-IT, and Forget-IT, membership is the union of the local node and validators in its resolved `QUORUM_SET`. This must describe the same set of at least four replicas everywhere. Instances are kept within a bounded window of 64 ahead of local execution; this is not a catch-up mechanism for disconnected or restarted replicas. Simple-IT also retains recent completed instances to finish auxiliary RBC messages.

### Custom-protocol workload

Run the client against the leader's port 12000:

```bash
./shab_client <leader-ip> 12000 150 100000000 100 360 0 2
```

Arguments are, in order: leader address, port, maximum in-flight requests **per thread**, total request limit, server batch-size hint, duration in seconds, send interval in microseconds, and number of client threads. The example uses two threads with 150 outstanding requests each for a 360-second run. The server batch size is fixed at 100 in `OverlayManagerImpl.cpp`; the client hint does not change it. Provide enough outstanding writes to fill a batch. An incomplete final batch is not flushed automatically.

Select the operation mix through `currentWorkload` in `src/overlay/CustomYCSBWorkload.h` and rebuild the client. The default `WORKLOAD_W` generates updates only; the header also provides A, B, F, and explicit read-ratio workloads. Record the workload, key distribution, batch size, concurrency, and measurement interval with each run.

READ requests retain the same immediate-ACK behavior across all custom protocols: they bypass consensus and do not return a stored value. Report these separately from committed-write throughput and do not interpret them as linearizable read measurements. Write ACKs for the new paths indicate execution at the leader, not a Byzantine client quorum certificate.

### SCP workload

Set `ENABLE_SCP_TRACKING=true`, rebuild the replicas, and enable `SEND_CUSTOM_MESSAGE` on the load-generating node. This harness creates accounts and submits native Stellar transactions through SCP; it does not use the custom TCP client. Its workload parameters include `NUM_TEST_ACCOUNTS` and `SCP_OPS_PER_TX` in `OverlayManagerImpl.cpp`. Use the private network's native ledger bootstrap and check successful ledger advancement and transaction application before measuring. Report SCP operations and ledger-close measurements with their units; they are not the custom key-value client's ACK measurements.

## Experiment orchestration and results

`RunGCP.ipynb` provisions replica/client machines, builds the binaries, generates private-network configurations, runs the client, and collects outputs. Its default experiment uses four replicas. Set the project, service account, image family, zones, usernames, local/remote paths, node-count list, load points, and output location for your environment. Review its VM deletion settings and automatic Git commit/push step before executing it.

For TetraBFT, Simple-IT, and Forget-IT, insert the chosen configuration block into **every** generated replica configuration in the notebook's configuration-generation step, before those files are copied to the machines. The notebook currently inserts only `SEND_CUSTOM_MESSAGE=true` on node1. For source-selected protocols, compile the selected switches on every host before running; do not reuse a binary built for another mode. Use the SCP harness separately from the notebook's custom-client workload.

Retain the source revision, build configuration, node configurations, client output, replica logs, and deployment/load parameters for each experiment. The new paths log `[TETRA COMMITTED]`, `[SIMPLE-IT COMMITTED]`, or `[FORGET-IT COMMITTED]` with sequence number, batch digest, and transaction count. Compare sequence numbers and digests across replicas before accepting a run.

`PostProcess.ipynb` contains the existing throughput and latency analysis. Point it at the output directory for the selected run and align its parsing and measurement windows with that run's output format. Keep protocol variants and read/write measurements distinct. The notebooks require environment-specific configuration; they are not a one-command reproduction of every paper figure.

## Implementation references

- [Basic TetraBFT, Section 3.2](https://arxiv.org/abs/2405.02615v2).
- [Simple-IT, normal operation in Figure 2](https://arxiv.org/abs/2606.14404v2).
- [Optimistic reliable broadcast, Figure 1](https://arxiv.org/abs/2505.02761v4).
- [Forget-IT, Algorithm 1](https://eprint.iacr.org/2026/355).

The main integration is in `src/overlay/OverlayManagerImpl.cpp`; the new normal-path voting state is in `src/overlay/NormalPathConsensus.h`. Runtime options are defined in `src/main/Config.h` and parsed in `src/main/Config.cpp`.
