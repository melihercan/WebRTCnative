# Branch selection

Every workflow begins by deciding which WebRTC branch to build. This page explains the rule, the
three ways to influence it, and why two more obvious approaches are wrong.

## Background: WebRTC rides the Chromium train

WebRTC has no independent version number. It is released on the Chromium schedule, and every
Chromium milestone has a matching WebRTC branch:

| Chromium milestone | WebRTC branch |
|---|---|
| M152 | `branch-heads/7977` |
| M151 | `branch-heads/7922` |
| M150 | `branch-heads/7871` |

The mapping is published at <https://chromiumdash.appspot.com/branches>, and the numbers have been
identical on both sides for a long time — but the workflows read the dashboard's `webrtc_branch`
field rather than assuming that, because it is the field that is actually guaranteed to be right.

## The rule

> Build the WebRTC branch of the **highest Chromium milestone whose `schedule_phase` is exactly
> `"stable"`**.

The dashboard gives every branched milestone a phase:

| Phase | Meaning |
|---|---|
| `beta` | next milestone, still in beta |
| `stable_cut` | branched for stable and rolling out, not yet the broad release |
| `stable` | shipped and current |
| `extended` | older, still on extended support |

Taking the newest `stable` entry is what matches the **Stable** column of the dashboard's own
branches page.

At the time of writing:

| Milestone | WebRTC branch | Phase | |
|---|---|---|---|
| 154 | 8037 | `beta` | |
| 153 | 8010 | `stable_cut` | |
| **152** | **7977** | **`stable`** | ← chosen |
| 151 | 7922 | `stable` | |
| 150 | 7871 | `extended` | |

## Two tempting approaches that are wrong

**Asking for the latest stable release.** `fetch_releases?channel=Stable` returns the milestone
that is *rolling out*, which runs one ahead of the milestone most users are on. While M152 was the
broad stable release, that endpoint already answered M153.

**Taking the largest branch-head in the WebRTC repository.** Branch-heads are cut continuously and
most of them are not milestones at all. While the stable milestone branch was 7977, the largest
existing branch-head was 8043 — a branch nobody ships.

## Overriding

Two `workflow_dispatch` inputs, on every workflow:

| Input | Example | Effect |
|---|---|---|
| `webrtc_branch` | `7977` | Build that branch-head. Highest priority. |
| `chromium_milestone` | `152` | Look up that milestone's WebRTC branch. |
| *(both empty)* | | Auto-detect, as described above. |

`webrtc_branch` wins if both are given. To reproduce an older build, set `webrtc_branch` to the
number recorded in that run's job summary.

### Why the default is empty rather than a number

GitHub renders `workflow_dispatch` defaults as static strings — a default cannot be computed when
the form is shown. Prefilling `7977` would just move the hard-coded version from the checkout step
into the input box, where it would rot the moment M153 goes stable. An empty box that means "latest
stable" is the only default that stays correct without maintenance.

The resolved branch is always printed in the log and written to the job summary, so a run is never
ambiguous about what it built.

## Validation

Before any build starts, the resolved branch is checked against the real repository:

```
git ls-remote https://webrtc.googlesource.com/src refs/branch-heads/7977
```

A typo therefore fails in seconds rather than forty minutes into a checkout that was never going
to work.

## Implementation

The logic lives in one place, used by all eight workflows:

```
.github/actions/resolve-webrtc-branch/
├── action.yml                  # composite action wrapper
└── resolve_webrtc_branch.py    # the resolver
```

Written in Python rather than shell because every runner image ships Python, and because it can be
run and tested on a laptop:

```bash
python .github/actions/resolve-webrtc-branch/resolve_webrtc_branch.py
python .github/actions/resolve-webrtc-branch/resolve_webrtc_branch.py --milestone 151
python .github/actions/resolve-webrtc-branch/resolve_webrtc_branch.py --branch 7977
```

Outputs consumed by the calling workflow:

| Output | Example |
|---|---|
| `branch` | `7977` |
| `milestone` | `152` |
| `source` | `auto-detected (latest stable Chromium milestone)` |

`milestone` is best-effort when an explicit `webrtc_branch` is given — a branch the dashboard does
not track reports `unknown`, which is not an error.

## Dashboard endpoints used

| Endpoint | Purpose |
|---|---|
| `/fetch_milestones` | all milestones with `schedule_phase`; drives auto-detection |
| `/fetch_milestones?mstone=N` | one milestone; drives `chromium_milestone` |
| `/fetch_milestones?only_branched=true` | reverse lookup of branch → milestone, for labelling |

An unknown milestone returns the JSON body `[null]` rather than an empty list, which the resolver
handles explicitly.
