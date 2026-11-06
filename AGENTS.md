# Agent guide for LEMU

LEMU is a fork of QEMU for old computers. It is made to be worked on and
used by AI agents, so AI-written code is welcome here.

## Using LEMU VMs

- **Read [MCP.md](MCP.md).** The MCP server also sends its rules to every
  agent that connects.
- **No Python to drive VMs.** The MCP tools do it: `hmp` for the monitor,
  memory and registers, `serial-run` for commands in the guest, the
  other serial tools for serial consoles, `wait-screen` and `vm-wait`
  instead of sleeping, `vm-stats` instead of `ps` and `top`. See
  [MCP.md](MCP.md#without-python).
- **Only touch your own VMs.** Never stop, reset or change a VM you did
  not start, unless the user asks.
- **Work on copies.** Test with copies of disk images and NVRAM files
  (`cp -c` on macOS, `cp --reflink` on Linux), never with the originals.
- **Start VMs from a shell,** then call `vm-wait`. There is no tool to
  start a VM. On macOS, keep Unix socket paths under about 104 bytes:
  use short relative names.

## Working on LEMU

- **Branch `master` only.** The machine branches are being folded into
  `master`. Don't commit to them.
- **One folder per machine:** `machines/<machine>/` holds notes, guest
  tools, a `run.sh` per guest system and the release scripts.
  `machines/release/` builds QEMU once for the kits of all machines.
  Don't add new files to `contrib/`; upstream's own folders there stay.
- **Window title:** every `run.sh` and `run.cmd` gives QEMU a `-name` with
  the guest system, the CPU and the machine, such as
  `-name "Windows NT 4.0 ALPHA JENSEN"` or
  `-name "Windows NT 4.0 MIPS RM200"`, so each window says which guest it
  runs.
- **Build:** `./configure`, then `make`. Python 3.12 or newer is needed.
  With `--without-default-features`, also give `--enable-hmp`, or
  `-monitor stdio` will not work.
- **Commit messages:** `subsystem: lowercase summary`, then a short body
  in plain English. No `Signed-off-by` or `Co-Authored-By` lines.
- **Ask first** before you push, delete branches or change GitHub
  settings.
- **Before a release,** run the feature check (see
  `machines/<machine>/release/README.md`). A guest that boots can still
  be missing a window, its sound or the git id.
- **Finding your way in the QEMU code:** use the skill in
  `.agents/skills/qemu-codebase`.

## Upstream QEMU

- **Taking in new QEMU code:** merge QEMU's `master` into `master` about
  once a week. Never rebase or force-push `master`. Use the skill in
  `.agents/skills/upstream-sync`.
- LEMU code never goes to QEMU. QEMU does not accept AI-generated code;
  see `docs/devel/code-provenance.rst`.
- Security bugs in upstream QEMU code follow QEMU's own process
  (`docs/system/security.rst`). Don't report them in public.
