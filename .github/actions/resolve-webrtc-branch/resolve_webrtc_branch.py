#!/usr/bin/env python3
"""Work out which WebRTC branch-head a build workflow should check out.

WebRTC is released on the Chromium train, and every Chromium milestone has a
matching WebRTC branch (M152 -> branch-heads/7977).  The Chromium dashboard
publishes that mapping, so the workflows ask it at run time instead of carrying
a hard-coded branch number that silently rots.

Resolution order:

  1. ``--branch N``      use branch-head N as given.
  2. ``--milestone N``   ask the dashboard for milestone N's ``webrtc_branch``.
  3. otherwise           auto-detect: the highest milestone whose
                         ``schedule_phase`` is exactly ``"stable"``.

On why (3) is written the way it is -- two more obvious approaches are wrong:

  * ``fetch_releases?channel=Stable`` names the milestone that is *rolling out*,
    which runs one ahead of the milestone most users are on.  While M152 was
    stable it already answered M153.
  * taking the largest ``refs/branch-heads/*`` in the WebRTC repo is worse
    still: branch-heads are cut continuously and most are not milestones.  That
    approach returned 8043 while the stable milestone branch was 7977.

Milestones carry a ``schedule_phase`` of "beta" (next up), "stable_cut" (mid
rollout), "stable" (shipped) or "extended".  The newest "stable" one is what the
Stable column of https://chromiumdash.appspot.com/branches shows.

The resolved branch is checked against the real repository before it is
returned, so a typo costs seconds rather than an hour of runner time.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

DASHBOARD = "https://chromiumdash.appspot.com"
WEBRTC_REPO = "https://webrtc.googlesource.com/src"
TIMEOUT = 60
ATTEMPTS = 3


def fail(message: str) -> "typing.NoReturn":  # noqa: F821
    print(f"::error::{message}", file=sys.stderr)
    sys.exit(1)


def get_json(url: str):
    """GET a JSON document, retrying briefly -- the dashboard blips sometimes."""
    last = None
    for attempt in range(1, ATTEMPTS + 1):
        try:
            request = urllib.request.Request(
                url, headers={"User-Agent": "WebRTCnative-build-workflow"}
            )
            with urllib.request.urlopen(request, timeout=TIMEOUT) as response:
                return json.loads(response.read().decode("utf-8"))
        except (urllib.error.URLError, json.JSONDecodeError, OSError) as error:
            last = error
            if attempt < ATTEMPTS:
                print(f"  {url} failed ({error}); retrying...", file=sys.stderr)
                time.sleep(5)
    fail(f"Could not read {url}: {last}")


def milestone_for_branch(branch: str) -> str:
    """Best-effort reverse lookup, purely for labelling. Never fatal."""
    try:
        for entry in get_json(f"{DASHBOARD}/fetch_milestones?only_branched=true") or []:
            if isinstance(entry, dict) and str(entry.get("webrtc_branch")) == branch:
                return str(entry["milestone"])
    except SystemExit:
        pass
    return "unknown"


def resolve(branch: str, milestone: str) -> tuple[str, str, str]:
    """Return (branch, milestone, human-readable explanation of the choice)."""
    if branch:
        if not branch.isdigit():
            fail(f"--branch must be a number such as 7977, got {branch!r}.")
        return branch, milestone_for_branch(branch), f"manual override (branch {branch})"

    if milestone:
        if not milestone.isdigit():
            fail(f"--milestone must be a number such as 152, got {milestone!r}.")
        entries = get_json(f"{DASHBOARD}/fetch_milestones?mstone={milestone}")
        # An unknown milestone comes back as [null], not as an empty list.
        entry = entries[0] if isinstance(entries, list) and entries else None
        if not isinstance(entry, dict) or not entry.get("webrtc_branch"):
            fail(
                f"The Chromium dashboard has no WebRTC branch for milestone {milestone}. "
                "Check https://chromiumdash.appspot.com/branches for valid milestones."
            )
        resolved = str(entry["webrtc_branch"])
        return resolved, milestone, f"manual override (Chromium M{milestone})"

    entries = get_json(f"{DASHBOARD}/fetch_milestones") or []
    stable = [
        e for e in entries
        if isinstance(e, dict) and e.get("schedule_phase") == "stable" and e.get("milestone")
    ]
    if not stable:
        fail(
            "No milestone is in the 'stable' phase on the Chromium dashboard right now. "
            "Re-run this workflow with an explicit webrtc_branch or chromium_milestone."
        )
    newest = max(stable, key=lambda e: e["milestone"])
    if not newest.get("webrtc_branch"):
        fail(f"Milestone {newest['milestone']} is stable but carries no webrtc_branch.")
    return (
        str(newest["webrtc_branch"]),
        str(newest["milestone"]),
        "auto-detected (latest stable Chromium milestone)",
    )


def branch_exists(branch: str) -> bool:
    try:
        result = subprocess.run(
            ["git", "ls-remote", WEBRTC_REPO, f"refs/branch-heads/{branch}"],
            capture_output=True,
            text=True,
            timeout=120,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        # Do not block the build on a flaky network probe.
        print(f"::warning::Could not verify branch-heads/{branch}: {error}", file=sys.stderr)
        return True
    return result.returncode == 0 and bool(result.stdout.strip())


def emit(name: str, value: str, path_variable: str) -> None:
    path = os.environ.get(path_variable)
    if path:
        with open(path, "a", encoding="utf-8") as handle:
            handle.write(f"{name}={value}\n" if path_variable == "GITHUB_OUTPUT" else value)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--branch", default="", help="WebRTC branch-head number, e.g. 7977")
    parser.add_argument("--milestone", default="", help="Chromium milestone, e.g. 152")
    arguments = parser.parse_args()

    branch = arguments.branch.strip()
    milestone = arguments.milestone.strip()

    branch, milestone, source = resolve(branch, milestone)

    print(f"Verifying refs/branch-heads/{branch} exists in {WEBRTC_REPO} ...")
    if not branch_exists(branch):
        fail(
            f"refs/branch-heads/{branch} does not exist in the WebRTC repository. "
            "See https://chromiumdash.appspot.com/branches for the branch of each milestone."
        )

    print(f"WebRTC branch-heads/{branch} (Chromium M{milestone}) -- {source}")

    output = os.environ.get("GITHUB_OUTPUT")
    if output:
        with open(output, "a", encoding="utf-8") as handle:
            handle.write(f"branch={branch}\n")
            handle.write(f"milestone={milestone}\n")
            handle.write(f"source={source}\n")

    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as handle:
            handle.write("### WebRTC source\n\n")
            handle.write("| | |\n|---|---|\n")
            handle.write(f"| Branch | `branch-heads/{branch}` |\n")
            handle.write(f"| Chromium milestone | M{milestone} |\n")
            handle.write(f"| Resolved by | {source} |\n\n")

    return 0


if __name__ == "__main__":
    sys.exit(main())
