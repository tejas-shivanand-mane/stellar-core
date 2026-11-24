"""Measurements shared by the local and GCP Shabdiz collection-round experiments."""
import re


def check_round_settings(delay, points):
    if type(delay) is not int or not 0 <= delay < 2**31:
        raise ValueError("Choose a nonnegative server uptime in seconds (below 2**31)")
    if (not points or any(type(n) is not int or not 0 <= n < 2**32 for n in points)
            or len(set(points)) != len(points)):
        raise ValueError("Choose distinct nonnegative 32-bit collection round counts")


def observed_rounds(logs, config, client_started_at):
    required_config = {"collection_rounds", "num_nodes", "force_collect_after_sec"}
    missing = required_config.difference(config)
    if missing:
        raise ValueError(f"Timed collection trial is missing settings: {', '.join(sorted(missing))}")
    count = config["collection_rounds"]
    logs = list(logs)
    if len(logs) != config["num_nodes"]:
        raise ValueError(f"Expected {config['num_nodes']} replica logs, found {len(logs)}")
    reports = []
    for path in logs:
        with path.open(errors="replace") as stream:
            lines = [line for line in stream if "[SHABDIZ TIMED COLLECTION]" in line]
        if len(lines) != 1:
            raise ValueError(f"{path}: expected one completed collection window, found {len(lines)}; "
                             "check server uptime and trial duration")
        r = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", lines[0])}
        required = {"delay", "first", "rounds", "collects", "proposals", "armed_us",
                    "first_us", "last_us", "committed"}
        if not required <= r.keys():
            raise ValueError(f"{path}: malformed collection summary")
        if (r["delay"] != config["force_collect_after_sec"] or r["rounds"] != count or
                r["committed"] < r["first"] + count or r["collects"] != r["proposals"] or
                r["collects"] > count or bool(r["collects"]) != bool(r["first_us"]) or
                bool(r["proposals"]) != bool(r["last_us"])):
            raise ValueError(f"{path}: incomplete or inconsistent collection summary")
        reports.append(r)
    # Each view has a unique leader. Independently started server clocks can
    # cover different view intervals, so use the counts the leaders observed.
    observed = sum(r["collects"] for r in reports)
    summary = dict(observed_rounds=observed, min_final_commit_view=min(r["committed"] for r in reports),
                   first_armed_at=min(r["armed_us"] for r in reports) / 1e6,
                   last_armed_at=max(r["armed_us"] for r in reports) / 1e6)
    if observed:
        summary.update(first_collect_at=min(r["first_us"] for r in reports if r["collects"]) / 1e6,
                       last_slow_proposal_at=max(r["last_us"] for r in reports) / 1e6)
        if client_started_at is not None and summary["first_collect_at"] < client_started_at:
            print("Collection began before client measurements started.")
    return summary
