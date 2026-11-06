# Stellar-Shabdiz

Research artifact for evaluating Shabdiz and related Byzantine consensus protocols in a common Stellar Core codebase. The custom protocols share the overlay transport, transaction batching, workload generator, and client acknowledgement interface.

## Protocols and scope

| Protocol | Implementation | Selection |
| --- | --- | --- |
| Shabdiz | Existing custom consensus implementation | Default; all protocol switches below disabled |
| IT-HS | Existing custom consensus implementation | Source switch `ITHS_MODE=true` |
| PBFT | Existing custom pre-prepare/prepare/commit implementation | Source switch `PBFT_MODE=true` |
| SCP | Native Stellar consensus with the existing transaction-generation and measurement harness | Source switch `ENABLE_SCP_TRACKING=true` |
| TetraBFT | Basic TetraBFT normal path: proposal and four voting phases | Source switch `TETRABFT_MODE=true` |
| Simple-IT | Consecutive-round normal path with Bracha RBC, or optimistic RBC | Source switch `SIMPLE_IT_MODE=true` |
| Forget-IT | First-view normal path: prepare, vote-1, vote-2 | Source switch `FORGET_IT_MODE=true` |

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

All protocols are selected in [`src/overlay/experiments/ExperimentConfig.h`](src/overlay/experiments/ExperimentConfig.h). Set the switch for the desired protocol to `true`, leave every other protocol switch `false`, and rebuild all replicas from the same settings.

| Run | Switch to enable in `ExperimentConfig.h` |
| --- | --- |
| Shabdiz | None; leave all protocol switches `false` |
| IT-HS | `ITHS_MODE=true` |
| PBFT | `PBFT_MODE=true` |
| SCP | `ENABLE_SCP_TRACKING=true` |
| TetraBFT | `TETRABFT_MODE=true` |
| Simple-IT | `SIMPLE_IT_MODE=true` |
| Forget-IT | `FORGET_IT_MODE=true` |

The build rejects configurations with more than one protocol enabled. Keep `NonPrune_MODE=false` for the default Shabdiz configuration; enable it only for an explicitly labelled no-pruning experiment.

For every custom protocol, use the same leader setup: `SEND_CUSTOM_MESSAGE=true` only on node1 (or your chosen proposer), and `false` on the other replicas. For SCP, this setting selects the load-generating node. No protocol-specific TOML blocks or leader public keys are required. TetraBFT, Simple-IT, and Forget-IT followers learn the authenticated proposer from the first valid proposal for instance 1 and retain that identity throughout the run. As with the existing benchmark setup, exactly one node must initiate proposals.

`SIMPLE_IT_OPTIMISTIC` is also a build-time setting in `ExperimentConfig.h`. With `true` (the default), optimistic delivery requires `ceil((n+2f-2)/2)` matching ECHOs from **nonleader** replicas, followed by `n-f` consensus COMMITs. The critical path has three message delays. Auxiliary VOTE, ACK, and READY messages are also sent and must be included in communication measurements. With `false`, Simple-IT uses Bracha-style ECHO/READY delivery followed by COMMIT, giving a four-delay normal path. Build every replica with the same variant and report it with each result.

Forget-IT starts each instance in view 1 with a SAFE guard. A replica sends vote-2 after `n-f` matching vote-1 messages and decides after `n-f` vote-2 messages, with the paper's additional `f+1` matching vote-1 acceptance condition. Its normal path has three message delays.

If upgrading from the earlier runtime-selection version, remove its protocol mode, variant, and leader-key options from `stellar-core.cfg`; these options are no longer parsed.

## Configure and run a private network

1. Put replica IP addresses, one per line, in `tsm_ips.txt`. Do not include client machines.
2. Update `STELLAR_CORE`, `BASE_DIR`, and the IP-file path in `gcp_setup_stellar_private.sh` for your installation. Run `bash gcp_setup_stellar_private.sh start` to generate node identities and configurations in the configured output directory.
3. Set `SEND_CUSTOM_MESSAGE=true` at the top of node1's generated configuration, before any TOML table headers, and leave it false on all other replicas. Use the same compiled protocol settings on every replica.
4. Run `bash gcp_setup_stellar_private.sh` to initialize the databases. Copy each node's configuration and required directories to its host, preserving or adjusting the configured paths.
5. Start each replica with `src/stellar-core run --conf /path/to/nodeN/stellar-core.cfg`. Wait for the leader's client-listener readiness log before starting the workload.

The setup script recreates node directories in its `start` step; use a separate output directory for each fresh experiment. Replicas must share the same network passphrase and membership, have sufficient overlay connection capacity, and be fully connected. Preserve the generated `KNOWN_PEERS` convention, which includes all replica endpoints including self: the experiment startup gate expects `KNOWN_PEERS.size()-1` authenticated peers.

For TetraBFT, Simple-IT, and Forget-IT, membership is the union of the local node and validators in its resolved `QUORUM_SET`. This must describe the same set of at least four replicas everywhere. Instances are kept within a bounded window (default 64, `NORMAL_PATH_WINDOW`) ahead of local execution; this is not a catch-up mechanism for disconnected or restarted replicas. Simple-IT also retains recent completed instances to finish auxiliary RBC messages.

### Custom-protocol workload

Run the client against the leader's port 12000:

```bash
./shab_client <leader-ip> 12000 150 100000000 100 360 0 2
```

Arguments are, in order: leader address, port, maximum in-flight requests **per thread**, total request limit, server batch-size hint, duration in seconds, send interval in microseconds, and number of client threads. The example uses two threads with 150 outstanding requests each for a 360-second run. The server batch size defaults to 100 (`SERVER_BATCH_SIZE` in `ExperimentConfig.h`); the client hint does not change it. Provide enough outstanding writes to fill a batch. An incomplete final batch is not flushed automatically.

Select the operation mix through `currentWorkload` in `src/overlay/experiments/ExperimentConfig.h` and rebuild the client. The default `WORKLOAD_W` generates updates only; the header also provides A, B, F, and explicit read-ratio workloads. Record the workload, key distribution, batch size, concurrency, and measurement interval with each run.

READ requests retain the same immediate-ACK behavior across all custom protocols: they bypass consensus and do not return a stored value. Report these separately from committed-write throughput and do not interpret them as linearizable read measurements. Write ACKs for the new paths indicate execution at the leader, not a Byzantine client quorum certificate.

### SCP workload

Set `ENABLE_SCP_TRACKING=true`, rebuild the replicas, and enable `SEND_CUSTOM_MESSAGE` on the load-generating node. This harness creates accounts and submits native Stellar transactions through SCP; it does not use the custom TCP client. Its workload parameters include `NUM_TEST_ACCOUNTS` and `SCP_OPS_PER_TX` in `ExperimentConfig.h`. Use the private network's native ledger bootstrap and check successful ledger advancement and transaction application before measuring. Report SCP operations and ledger-close measurements with their units; they are not the custom key-value client's ACK measurements.

## Experiment orchestration and results

`RunGCP.ipynb` provisions replica/client machines, builds the binaries, generates private-network configurations, runs the client, and collects outputs. Its default experiment uses four replicas. Set the project, service account, image family, zones, usernames, local/remote paths, node-count list, load points, and output location for your environment. Review its VM deletion settings and automatic Git commit/push step before executing it.

For every custom protocol, select its switch in `ExperimentConfig.h` and compile on every host before running. The notebook's existing `SEND_CUSTOM_MESSAGE=true` setting on node1 is sufficient; no additional protocol-specific configuration insertion is needed. Do not reuse a binary built for another mode. Use the SCP harness separately from the notebook's custom-client workload.

Retain the source revision, build configuration, node configurations, client output, replica logs, and deployment/load parameters for each experiment. The new paths log `[TETRA COMMITTED]`, `[SIMPLE-IT COMMITTED]`, or `[FORGET-IT COMMITTED]` with sequence number, batch digest, and transaction count. Compare sequence numbers and digests across replicas before accepting a run.

`PostProcess.ipynb` contains the existing throughput and latency analysis. Point it at the output directory for the selected run and align its parsing and measurement windows with that run's output format. Keep protocol variants and read/write measurements distinct. The notebooks require environment-specific configuration; they are not a one-command reproduction of every paper figure.

## Implementation references

- [Basic TetraBFT, Section 3.2](https://arxiv.org/abs/2405.02615v2).
- [Simple-IT, normal operation in Figure 2](https://arxiv.org/abs/2606.14404v2).
- [Optimistic reliable broadcast, Figure 1](https://arxiv.org/abs/2505.02761v4).
- [Forget-IT, Algorithm 1](https://eprint.iacr.org/2026/355).

## Code organization

`src/overlay/OverlayManagerImpl.cpp` handles overlay connections, peer management, and message transport. Experiment code lives in `src/overlay/experiments/`:

| File | Responsibility |
| --- | --- |
| `Shabdiz.cpp` | Shabdiz proposals, votes, and COLLECT path |
| `ITHS.cpp` | IT-HS proposals and voting phases |
| `PBFT.cpp` | PBFT slots, proposals, votes, and execution |
| `TetraBFT.cpp` | TetraBFT normal-path progression |
| `SimpleIT.cpp` | Simple-IT normal-path progression and parent check |
| `ForgetIT.cpp` | Forget-IT normal-path progression and vote acceptance |
| `SCPBenchmark.cpp` | Native SCP workload generation and tracking; SCP consensus remains in `src/scp/` |
| `NormalPathProtocol.cpp` | Shared transport, validation, and ordered execution for the three new normal paths |
| `Experiment.cpp` / `ExperimentState.h` | Protocol dispatch, shared legacy proposal construction, and benchmark bookkeeping |
| `ExperimentClient.cpp` | TCP request handling, immediate read ACKs, server batching, and write ACKs |
| `ExperimentConfig.h` | Build-time protocol selection and run/workload parameters |

`ExperimentConfig.h` centralizes all protocol switches, the Simple-IT variant, client port, server batch size, history/window limits, forced-COLLECT timing, SCP workload size, and YCSB workload/key distribution. Per-node settings, including `SEND_CUSTOM_MESSAGE`, remain in `stellar-core.cfg`, with parsing in `src/main/Config.cpp`. The shared normal-path voting state remains in `src/overlay/NormalPathConsensus.h`.

After pulling a revision that adds or moves source files, run `./autogen.sh`, then your usual `./configure` options and `make`. The build generates its source list from tracked files; `make` alone may otherwise retain the old list.
