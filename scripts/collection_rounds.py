"""Measurements shared by the local and GCP Shabdiz collection-round experiments."""
import json
import math
import re
from pathlib import Path


def check_round_settings(first, points):
    if type(first) is not int or not 1 <= first < 2**32:
        raise ValueError("Choose a positive 32-bit starting view")
    if (not points or any(type(n) is not int or not 0 <= n < 2**32 for n in points)
            or len(set(points)) != len(points)):
        raise ValueError("Choose distinct nonnegative 32-bit collection round counts")


def observed_rounds(logs, config, client_started_at):
    first, count = config["collection_start_view"], config["collection_rounds"]
    logs = list(logs)
    if len(logs) != config["num_nodes"]:
        raise ValueError(f"Expected {config['num_nodes']} replica logs, found {len(logs)}")
    phases = {"collect": {}, "proposal": {}}
    event = re.compile(r"\[SHABDIZ COLLECTION ROUND\] view=(\d+) phase=(collect|proposal) unix_us=(\d+)")
    last_commits = []
    for path in logs:
        last = 0
        with path.open(errors="replace") as stream:
            for line in stream:
                if "[FAST COMMITTED]" in line:
                    match = re.search(r"\bview=(\d+)", line)
                    if match:
                        last = max(last, int(match[1]))
                if "[SHABDIZ COLLECTION ROUND]" in line:
                    match = event.search(line)
                    if not match:
                        raise ValueError(f"Malformed collection event in {path}")
                    view, phase, stamp = int(match[1]), match[2], int(match[3])
                    if not first <= view < first + count or view in phases[phase]:
                        raise ValueError(f"Unexpected or duplicate {phase} in view {view}")
                    phases[phase][view] = stamp / 1_000_000
        last_commits.append(last)
    for phase, events in phases.items():
        if len(events) != count:
            raise ValueError(f"Observed {phase} in {len(events)}/{count} configured views "
                             f"(first view {first}, minimum final commit {min(last_commits)})")
    if min(last_commits) < first + count:
        raise ValueError(f"At least one replica did not commit past the window (minimum {min(last_commits)})")
    summary = {"observed_rounds": count, "min_final_commit_view": min(last_commits)}
    if count:
        summary.update(first_collect_at=phases["collect"][first],
                       last_slow_proposal_at=phases["proposal"][first + count - 1])
        if not (client_started_at <= summary["first_collect_at"] and
                summary["last_slow_proposal_at"] <= client_started_at + config["client_duration_sec"]):
            raise ValueError("Collection was outside the client workload; adjust start view or duration")
    return summary


def client_series(paths, config):
    import pandas as pd
    paths = list(paths)
    if len(paths) != config["active_clients"]:
        raise ValueError("Missing or extra client logs")
    clients = []
    for path in paths:
        ticks = {}
        with path.open(errors="replace") as stream:
            for line in stream:
                if "[CLIENT_SEC]" not in line or not line.endswith("\n"):
                    continue
                fields = dict(re.findall(r"(\w+)=(\S+)", line))
                sec, n = int(fields["sec"]), int(fields["requests"])
                latency = float(fields["avg_latency_ms"])
                if sec < 1 or sec in ticks or n < 0 or (n and not (math.isfinite(latency) and latency >= 0)):
                    raise ValueError(f"Invalid CLIENT_SEC in {path} at second {sec}")
                ticks[sec] = n, n * latency if n else 0
        if not ticks:
            raise ValueError(f"No client measurements in {path}")
        clients.append(ticks)
    end = min(config["client_duration_sec"], max(max(c) for c in clients))
    rows = []
    for sec in range(1, end + 1):
        n = latency = math.nan
        if all(sec in c for c in clients):
            n = sum(c[sec][0] for c in clients)
            latency = sum(c[sec][1] for c in clients) / n if n else math.nan
        rows.append((sec, n, latency))
    return pd.DataFrame(rows, columns=["sec", "throughput", "latency"])


def plot_collection_rounds(session, save=True, show=True):
    import matplotlib.pyplot as plt
    from matplotlib.ticker import FuncFormatter
    import pandas as pd

    session = Path(session).expanduser()
    trials = sorted(session.rglob("result.json"))
    groups, summary = {}, []
    for result_path in trials:
        root = result_path.parent
        result = json.loads(result_path.read_text())
        if result.get("status") != "ok":
            print(f"Excluded {root.relative_to(session)}: {result.get('error', result.get('status'))}")
            continue
        cfg = result["config"]
        # Plot repetitions as distinct traces; compare round counts at fixed settings.
        key = (cfg["num_nodes"], cfg["collection_start_view"], cfg["active_clients"],
               cfg["client_threads"], cfg["max_in_flight"], cfg["batch_size"],
               cfg["client_duration_sec"], cfg["source_commit"])
        series = client_series(sorted(root.glob("client-*/client.log")), cfg)
        start = result["client_started_at"]
        marker = result.get("first_collect_at")
        offset = None if marker is None else marker - start
        groups.setdefault(key, []).append((cfg["collection_rounds"], cfg["repetition"], offset, series))
        summary.append(dict(nodes=key[0], first_view=key[1], rounds=cfg["collection_rounds"],
                            observed=result["observed_rounds"], repetition=cfg["repetition"],
                            first_collect_sec=offset, last_slow_proposal_sec=(
                                result["last_slow_proposal_at"] - start if marker is not None else None)))
    # Older GCP folders record the requested attempts but no observed event times.
    for meta in sorted(session.glob("Collection_*/run_config.txt")):
        fields = dict(line.split("=", 1) for line in meta.read_text().splitlines() if "=" in line)
        rounds = int(fields["max_collect_attempts"])
        clients = int(fields["active_clients"])
        key = (int(fields["num_nodes"]), "legacy", clients, int(fields["client_threads_per_vm"]),
               int(fields["client_max_in_flight_per_thread"]), int(fields["server_batch_size_hint"]),
               int(fields["client_duration_sec"]), "legacy")
        cfg = {"active_clients": clients, "client_duration_sec": key[6]}
        paths = sorted(meta.parent.glob("client_*.log")) or [meta.parent / "client.log"]
        series = client_series(paths, cfg)
        groups.setdefault(key, []).append((rounds, 1, None, series))
        summary.append(dict(nodes=key[0], first_view=None, rounds=rounds, observed=None,
                            repetition=1, first_collect_sec=None, last_slow_proposal_sec=None))
    if not groups:
        raise ValueError(f"No successfully verified collection-round trials under {session}")
    font = 0.05 * 8 * 72
    style = {"font.size": font, "axes.labelsize": font, "xtick.labelsize": 0.85*font,
             "ytick.labelsize": 0.85*font, "legend.fontsize": 0.75*font,
             "lines.linewidth": 0.10*font}
    out = session / "figures"
    if save:
        out.mkdir(exist_ok=True)
    with plt.rc_context(style):
        for key, traces in groups.items():
            n, view, clients, threads, limit, batch, duration, sha = key
            tag = f"n{n}_v{view}_c{clients}_t{threads}_w{limit}_b{batch}_{duration}s_{sha[:8]}"
            for metric, label in [("throughput", "ACK throughput (ops/s)"), ("latency", "Mean ACK latency (ms)")]:
                fig, ax = plt.subplots(figsize=(12, 8))
                for rounds, rep, start, data in sorted(traces, key=lambda t: t[:2]):
                    line, = ax.plot(data.sec, data[metric], label=f"{rounds} rounds (rep {rep})")
                    if start is not None:
                        ax.axvline(start, color=line.get_color(), linestyle=":", linewidth=1.5)
                ax.set(xlabel="Time since client start (s)", ylabel=label, xlim=(0, duration))
                ax.set_ylim(bottom=0)
                ax.yaxis.set_major_formatter(FuncFormatter(
                    lambda x, _: f"{x/1000:.3g}k" if abs(x) >= 1000 else f"{x:.3g}"))
                ax.legend(frameon=False)
                fig.tight_layout()
                if save:
                    for ext in ("png", "pdf"):
                        fig.savefig(out / f"collection_rounds_{metric}_{tag}.{ext}", dpi=150, bbox_inches="tight")
                if show:
                    plt.show()
                plt.close(fig)
    table = pd.DataFrame(summary).sort_values(["nodes", "rounds", "repetition"])
    print(table.to_string(index=False))
    return table
