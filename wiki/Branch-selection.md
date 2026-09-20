# Branch selection

Every workflow begins by deciding which WebRTC branch to build. This page explains the rule, how to
override it, and why two more obvious approaches are wrong.

## Background: WebRTC rides the Chromium train

WebRTC has no independent version number. It is released on the Chromium schedule, and every
Chromium milestone has a matching WebRTC branch:

| Chromium milestone | WebRTC branch |
|---|---|
| M153 | `branch-heads/8010` |
| M152 | `branch-heads/7977` |
| M151 | `branch-heads/7922` |

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

At the time of writing (2026-09-20, the day after M153 went stable):

| Milestone | WebRTC branch | Phase | |
|---|---|---|---|
| 155 | 8059 | `beta` | |
| 154 | 8037 | `stable_cut` | |
| **153** | **8010** | **`stable`** | ← chosen |
| 152 | 7977 | `extended` | |
| 150 | 7871 | `extended` | |

Note what a rollover does to the row above the chosen one: M152 moves from
`stable` to `extended` and the auto-detected branch changes under you, without
anything in this repository changing. That is the intent, but it means a build
dispatched before and after a rollover produces different binaries from the
same commit. Pin `webrtc_branch` when that matters — see *Rollovers are not
free*, below.

## Two tempting approaches that are wrong

**Asking for the latest stable release.** `fetch_releases?channel=Stable` returns the milestone
that is *rolling out*, which runs one ahead of the milestone most users are on. While M152 was the
broad stable release, that endpoint already answered M153.

**Taking the largest branch-head in the WebRTC repository.** Branch-heads are cut continuously and
most of them are not milestones at all. While the stable milestone branch was 7977, the largest
existing branch-head was 8043 — a branch nobody ships.

## Rollovers are not free

M152 to M153 (2026-09-19) cost four failed Windows builds and an Android
archive nothing could read, and not one of those was about WebRTC's API — the
interop shim compiled against M153 without a single source change. Every one
was the surrounding toolchain moving:

| What moved | What it looked like |
|---|---|
| Windows SDK pin, 26100 to 10.0.28000 | `gn gen` dies in `setup_toolchain.py`: *include path does not exist* |
| Runner disk, with VS 18 now in the image | `LLVM ERROR: IO failure on output stream: no space on device`, 25 minutes in |
| Android javac `--release`, 21 to 25 | the AAR builds fine and the *consumer* rejects it: *class file has wrong version 69.0* |

Two lessons worth carrying into the next one:

- **Do not work around a toolchain pin.** Pointing the checkout at the older
  SDK got past `gn gen` and then failed compiling libvpx with `unknown type
  name 'FILE_INFO_BY_HANDLE_CLASS'`. The pin was a real dependency. Install
  what it asks for.
- **A green build is not a usable artifact.** The Android archive was complete
  and correct by every check the workflow had, and still unusable, because
  nothing verified the bytecode level against what consumes it. The collect
  step now does.

## Overriding

One `workflow_dispatch` input, on every workflow:

| `webrtc_branch` | Effect |
|---|---|
| *(empty)* | Auto-detect, as described above. |
| `7977` | Build that branch-head. |

There is deliberately no separate milestone input. The mapping is one to one, so a milestone box
would be a second way of naming the same thing — and it could not express a branch the dashboard
no longer lists. The branch is also what the checkout actually uses, so there is no lookup between
what you type and what gets built.

The milestone is still *reported* — resolved by reverse lookup and shown in the log, the job
summary and the artifact name — it simply is not an input.

To reproduce an older build, set `webrtc_branch` to the number recorded in that run's job
summary.

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

The logic lives in one place, used by all ten workflows:

```
.github/actions/resolve-webrtc-branch/
├── action.yml                  # composite action wrapper
└── resolve_webrtc_branch.py    # the resolver
```

Written in Python rather than shell because every runner image ships Python, and because it can be
run and tested on a laptop:

```bash
python .github/actions/resolve-webrtc-branch/resolve_webrtc_branch.py
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
| `/fetch_milestones?only_branched=true` | reverse lookup of branch → milestone, for labelling |
