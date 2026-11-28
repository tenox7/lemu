---
name: upstream-sync
description: >-
  Merge upstream QEMU's master into LEMU's master (never rebase) and
  prove nothing broke: a trial merge, conflict rules for the files LEMU
  replaced, a build, feature lists, qtests and guest boots, then land
  and push only after the user says yes. Use whenever the user wants to
  sync, update, pull or merge from upstream QEMU, asks how far behind
  QEMU LEMU is or whether upstream still merges cleanly, or asks for a
  "rebase" (onto QEMU, onto upstream, or just "rebase"): here that word
  means this merge.
---

# Syncing LEMU with upstream QEMU

LEMU's `master` is QEMU's `master` plus LEMU's own commits. To take in
new QEMU work, merge QEMU's `master` into LEMU's `master`.

- Do it about once a week. Many small merges are much easier than one
  big one. After a QEMU release a lot lands at once, so sync more often
  then.
- Only `master`. QEMU's stable branches carry fixes that are in QEMU's
  `master` already.

## Rules

- **Merge, never rebase.** Never rebase, reset or force-push `master`.
  It is public, the kits print its commit ids, and other agents work on
  top of it.
- **"Rebase" means this merge.** The user may ask for a "rebase" when
  they mean taking in new QEMU code. Do the merge, and say in one line
  that it is a merge, not a rebase, and why. If they still want a real
  rebase, tell them what it breaks and ask before you do anything.
- **Not with GitHub's buttons.** Don't use "Sync fork": its "Discard
  commits" deletes LEMU's work. Don't use `gh repo sync --force`: it
  resets `master` to QEMU.
- **Work in the sync worktree.** Don't merge in the main checkout. Don't
  touch other worktrees or build folders: VMs may run from them.
- **No blanket strategies.** Never use `-s ours`, `-X ours` or
  `-X theirs`. They silently throw away one side.
- **Ask first.** Land on `master` and push only after the user says yes.
- **Nothing goes to QEMU.** Never push to `upstream`.
- **Not during a release.** If the user is building or testing kits, ask
  before you start.

## Check only

`check.sh` fetches QEMU's `master` and tries the merge in memory. It
changes no branch, file or worktree, so it is safe to run at any time:

```sh
sh .agents/skills/upstream-sync/check.sh
```

It shows the number of new QEMU commits, whether the merge is clean,
each conflict, the files both sides changed, the QEMU commits that touch
files LEMU changed or deleted, the files QEMU deleted, and new entries
in QEMU's lists of deprecated and removed features. Exit code: 0 clean
or nothing new, 1 conflicts, 2 error.

If the user only wants to know how things stand, stop here and report.

## Full sync

The sync worktree is the main checkout's folder with `-sync` added, for
example `lemu-sync` next to `lemu`. Shell variables don't last from one
command to the next, so each block below finds it again.

### 1. Set up

Once per clone, if they are missing:

```sh
git remote add upstream https://gitlab.com/qemu-project/qemu.git
git remote set-url --push upstream DISABLED
git config rerere.enabled true
```

`rerere` remembers how you fixed each conflict, and fixes it the same
way when it comes back.

Each time:

```sh
SYNC=$(git worktree list --porcelain | sed -n '1s/^worktree //p')-sync
git fetch upstream master
ls -d "$SYNC"
```

- **It exists:** `git -C "$SYNC" status --short` and
  `git log --oneline master..upstream-sync` must both print nothing.
  Otherwise an earlier sync is still open: ask the user. Then reset it:
  `git -C "$SYNC" switch -C upstream-sync master`.
- **It doesn't:** `git worktree add -B upstream-sync "$SYNC" master`.

`upstream-sync` is a local helper branch: never push it. Keep the
worktree when you are done. Its build folder makes the next sync faster.

### 2. Build master and take a snapshot

Build the unmerged `master` like the daily build:

```sh
SYNC=$(git worktree list --porcelain | sed -n '1s/^worktree //p')-sync
mkdir -p "$SYNC/build" && cd "$SYNC/build"
[ -f build.ninja ] || ../configure --enable-sdl --enable-cocoa \
  --target-list=ppc-softmmu,ppc64-softmmu,mips64el-softmmu,mips64-softmmu,alpha-softmmu,i386-softmmu,sparc-softmmu
make -j"$(getconf _NPROCESSORS_ONLN)"
sh ../.agents/skills/upstream-sync/snapshot.sh old
```

- On Linux, leave out `--enable-cocoa`.
- The first build in a new worktree downloads QEMU's subprojects.
- `snapshot.sh` saves each binary's feature list: displays, sound,
  network, disk formats, machines, devices, the monitor and the git id.
- It also runs the qtests that start every machine and every device:
  `qom-test`, `test-hmp`, `device-introspect-test` and `qmp-cmd-test`
  for each target, and `mcp-test`. This takes a while.
- Some tests may fail here already. Only new failures count.

### 3. Merge

```sh
SYNC=$(git worktree list --porcelain | sed -n '1s/^worktree //p')-sync
cd "$SYNC"
git -c merge.conflictStyle=zdiff3 merge --no-ff --no-commit upstream/master
```

- `--no-commit` leaves room to fix conflicts and the build before the
  merge commit is made.
- `zdiff3` also shows the common base in each conflict, so you can see
  what each side changed.
- When `rerere` fixed a file, git says "Resolved ... using previous
  resolution". Check that fix too.
- After you fix a file, `git add` it (or `git rm` it).

| Where | Why it conflicts | What to do |
|---|---|---|
| `AGENTS.md` | QEMU has its own agent rules, and LEMU replaced them | Keep LEMU's: `git checkout --ours AGENTS.md`. Carry over only a real rule change, such as a security one. |
| Files LEMU deleted, such as `hw/alpha/dp264.c` | QEMU changed a file LEMU removed | Keep it deleted: `git rm` it. If QEMU's change is an API change, make the same change in LEMU's replacement. |
| `target/alpha/`, `hw/alpha/` | LEMU replaced QEMU's Alpha code with its own (Jensen, ES40) | Keep LEMU's side. Read each QEMU commit there and port by hand what still applies. |
| `hw/m68k/next-cube.c`, `hw/display/next-fb.c`, `include/hw/m68k/next-cube.h`, `tests/functional/m68k/test_nextcube.py`; `hw/m68k/next-kbd.c`, which LEMU deleted (`hw/input/next-kms.c` replaced it) | LEMU took over QEMU's NeXTcube and made it the board of all seven NeXT machines (`machines/next/`). QEMU deprecated it in 11.2 and may delete it | Keep LEMU's side, and keep the files if QEMU deletes them; keep `next-kbd.c` deleted. Port by hand any API change QEMU made there. |
| `meson.build`, `Kconfig`, `trace-events`, `qapi/*.json`, `*.hx`, `system/vl.c`, `monitor/` | Both sides add entries | Keep both. Check commas and order. |
| Any other QEMU file LEMU changed | Both sides changed the same lines | Understand both. Keep LEMU's behaviour and QEMU's cleanup. |

Commands that help while the merge is open:

```sh
base=$(git merge-base HEAD MERGE_HEAD)
git diff --name-only --diff-filter=D "$base" HEAD          # files LEMU deleted
git log -p "$base..MERGE_HEAD" -- target/alpha hw/alpha     # QEMU's Alpha changes
git log --oneline -S'some_name' "$base..MERGE_HEAD"         # who changed some_name
```

### 4. Build and compare

```sh
SYNC=$(git worktree list --porcelain | sed -n '1s/^worktree //p')-sync
cd "$SYNC/build"
make -j"$(getconf _NPROCESSORS_ONLN)"
sh ../.agents/skills/upstream-sync/snapshot.sh new
```

A clean merge can still fail to build. QEMU renamed or changed
something and fixed its own code, but not LEMU's own files. Find the
QEMU commit with `git log -S` and make the same change in LEMU's file.

- A small fix that only follows a QEMU change goes into the merge
  commit. Name it in the commit message.
- A bigger change gets its own commit after the merge.

`snapshot.sh new` compares with the snapshot from step 2:

- **LOST:** a machine, device, display, sound or network backend, disk
  format, the monitor or the git id is gone. QEMU sometimes removes old
  things on purpose, and `check.sh` lists its removals. If a LEMU
  machine or guest needs it, stop and ask the user.
- **NEW FAIL:** a qtest that passed before now fails. Fix it.
- **GONE:** a qtest that passed before no longer runs. A conflict fix
  may have dropped LEMU's line from a `meson.build`.

### 5. Boot the guests

Boot one guest for each machine in the table in `README.md`, with the
new binaries and copies of the disks (`cp -c` on macOS). Follow "Using
LEMU VMs" in `AGENTS.md`, and `MCP.md` to drive them.

- A kit picks its binary at the top of `run.sh`. In a copy of the kit,
  point that line at the new `build/qemu-system-...`.
- Get to the desktop or the login prompt. Move the mouse a few times,
  once by more than 127 pixels, and click something that shows a change.
  Then check that the monitor still answers (`info status`).
- Look at every screen, not only the main one.
- `machines/<machine>/release/README.md` has notes for some kits.

If a guest fails, try the same copy with the kit's own binary. If that
works, the merge broke it: see "Finding the QEMU commit that broke
something" below.

### 6. Commit

```sh
git commit -F - <<'EOF'
upstream: merge qemu master up to 56afd82bd4

Merges 125 QEMU commits, 7f36b1fbeb..56afd82bd4.
No conflicts.
EOF
```

- That is an example: use your own ids and numbers.
- List each conflict and each build fix, one plain line each.
- Follow the commit rules in `AGENTS.md`: no `Signed-off-by` or
  `Co-Authored-By` lines.

### 7. Report and ask

Tell the user:

- the QEMU range and the number of commits;
- each conflict and build fix, and how you fixed it;
- anything lost, deprecated or removed that LEMU uses;
- the qtests before and after;
- which guests you booted and what you saw;
- the merge commit id.

Then ask whether to land it on `master` and push.

### 8. Land and push

Only after the user says yes. In the main checkout:

```sh
cd "$(git worktree list --porcelain | sed -n '1s/^worktree //p')"
git merge --ff-only upstream-sync
```

- **"Not possible to fast-forward":** other agents committed to `master`
  in the meantime. In the sync worktree, run
  `git merge -m "upstream: take in master's new commits" master`, build,
  run `snapshot.sh -q new`, then try again.
- **"Your local changes would be overwritten":** another agent is
  working in the main checkout. Stop and tell the user. Never stash,
  reset or commit someone else's work.

Before the push, check only LEMU's own new commits. QEMU's commits carry
`Signed-off-by` lines, and that is fine:

```sh
git fetch origin master
git merge-base --is-ancestor origin/master master && echo fast-forward
git log -i --grep='^signed-off-by:' --grep='^co-authored-by:' \
    --format='%h %an: %s' origin/master..master --not upstream/master
git log --name-only --format= origin/master..master --not upstream/master |
    grep . | sort -u | git check-ignore --no-index --stdin
git diff --name-only --diff-filter=A \
    "$(git merge-base master upstream/master)" master -- contrib/
```

- The second line must print `fast-forward`.
- The first `git log` must print nothing, except commits by other people
  that keep their own `Signed-off-by`.
- The last two must print nothing: no files that git is told to ignore,
  such as private notes, and no new files in `contrib/`.

Then `git push origin master`. If GitHub refuses it because QEMU changed
`.github/workflows`, the token needs the `workflow` scope:
`gh auth refresh -s workflow`.

## Finding the QEMU commit that broke something

Bisect the new QEMU commits in a second worktree. At each step, merge
LEMU into that QEMU commit and test the result. First, in the sync
worktree, run `git rerere` to save your conflict fixes. Then:

```sh
SYNC=$(git worktree list --porcelain | sed -n '1s/^worktree //p')-sync
git worktree add --detach "$SYNC-bisect" upstream/master
cd "$SYNC-bisect"
git bisect start upstream/master "$(git merge-base master upstream/master)"
```

At each step:

```sh
git merge --no-ff --no-commit master    # this QEMU commit plus LEMU
# build in ./build as in step 2, then test
git merge --abort
git bisect good                         # or: git bisect bad
```

`rerere` repeats your conflict fixes at each step. If a step has a
conflict it doesn't know, fix it or run `git bisect skip`. At the end,
run `git bisect reset`, then `git worktree remove --force "$SYNC-bisect"`.
