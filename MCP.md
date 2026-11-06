# Using the LEMU MCP server

LEMU has a built-in [MCP](https://modelcontextprotocol.io/) server.
With it, an AI agent can work a VM like a person at the keyboard:

- look at the screen
- type and use the mouse
- read and write serial ports
- change CDs and floppies
- power the VM on and off, and take snapshots

It works on macOS and Linux.

## How it works

- **Every LEMU VM offers MCP.** It is on by default (`-mcp on`). The VM
  opens a socket in `/tmp/lemu-$UID/`, a folder only your user can open.
- **`lemu-mcp` is the bridge.** It is the one MCP server your agent starts.
  It finds all running VMs and passes each tool call to the VM you name.
- **VMs can come and go.** `lemu-mcp` keeps running.

## Setup

1. Build LEMU (see [README.md](README.md)). The bridge is
   `build/tools/lemu-mcp/lemu-mcp`. The ready-to-run kits do not include
   the bridge yet.
2. Add the bridge to your agent once.
   - Claude Code:
     `claude mcp add lemu -s user -- /path/to/build/tools/lemu-mcp/lemu-mcp`
   - Other MCP clients: add a stdio server. The command is the path to
     `lemu-mcp`, with no arguments.
3. Start a VM from a shell, for example a kit's `./run.sh`. Add
   `-name myvm` to give it a short name.
4. In the agent: run `vm-list`, then `vm-info` and `screenshot`.

**Headless works.** Start the VM with `-display none` and the agent still
sees the screen.

## Tools

Every tool takes `vm`: a name or PID from `vm-list`. You may leave it out
only when exactly one VM is running and you started it.

| Tool | What it does |
|---|---|
| `vm-list` | List the running VMs: name, PID, folder, machine, who started it |
| `vm-info` | Describe one VM: screens, mouse, disks, serial ports. Use it first |
| `screenshot` | Take a picture of a screen (PNG) |
| `frames` | List recent screens, to find one that went by too fast |
| `wait-screen` | Wait until the screen changes, settles or shows a colour. Use it instead of sleeping |
| `screen-text` | Read a text-mode screen (BIOS, DOS, NT setup) as text |
| `key` | Press keys and key combinations |
| `type-text` | Type a string, in the guest's keyboard layout |
| `mouse-move`, `mouse-click`, `mouse-drag`, `mouse-button`, `mouse-wheel` | Use the mouse, also in guests with a relative mouse |
| `pointer-locate` | Find where the guest's mouse pointer really is |
| `serial-read`, `serial-write`, `serial-expect` | Read, write and wait for serial port text |
| `serial-run` | Run a command in a shell on a serial port: get its output and exit status in one call |
| `media` | List, insert or eject CDs and floppies |
| `vm-power` | Pause, resume, reset, power down or quit |
| `vm-snapshot` | Save, load, list or delete whole-VM snapshots (needs qcow2 disks) |
| `hmp`, `qmp` | Run any QEMU monitor command: registers, memory, disassembly, pause and go on |

## How to drive a VM

The server sends these rules to every agent that connects:

- **Look before you act.** Take a screenshot first. The keyboard and
  mouse tools take `then`, so they can wait and look again in the same
  call.
- **Don't sleep.** Use `wait-screen`, or `serial-expect` for serial
  text. For long installs, call it in a loop.
- **No scripts.** Don't write Python or socket scripts to drive a VM.
  The tools do it; see [Without Python](#without-python).
- **Commands in a Unix guest:** log in on a serial port and use
  `serial-run`. It returns the output and the exit status. If the
  console is a graphics screen, start a getty on a serial port first.
- **Check the focus before typing.** Keys go to the focused window.
- **Old dialogs:** use Tab and Shift+Tab, then Space or Enter.
- **Mouse:** most old guests have a relative mouse. If a click misses,
  take a screenshot with `pointer=true`, or use the keyboard.
- **Old menus** may need press-and-hold: `mouse-button` down,
  `mouse-move`, `mouse-button` up.
- **Keyboard layout:** give `type-text` the guest's `layout`, for example
  `de` for German.
- **Events:** every result lists what happened in the VM since your last
  call, such as a reset or a pause.

## Without Python

Don't write scripts for the monitor socket, the serial port or waiting.
Use these:

| You want to | Use |
|---|---|
| Read the CPU registers | `hmp` `info registers` |
| Read guest memory | `hmp` `xp /16wx 0x1000` (physical) or `x /16wx ADDR` (virtual) |
| Disassemble guest code | `hmp` `x /10i ADDR` |
| Save memory to a file | `hmp` `pmemsave ADDR SIZE FILE` |
| Pause and go on | `vm-power` `pause` and `resume`, or `hmp` `stop` and `cont` |
| Any other monitor command | `hmp` (`help` lists them) or `qmp` |
| Run a command in the guest, get its output and exit status | `serial-run` (log in on a serial port first) |
| Type into a serial console | `serial-write` (old Unix wants `\r`) |
| Wait for serial text | `serial-expect` |
| Read what a serial port printed | `serial-read` |
| Wait for the screen | `wait-screen` |
| Crop or zoom a screenshot | `screenshot` with `region` and `scale` |
| Find a screen that went by | `frames` |
| Change a CD or floppy | `media` |

- **Every serial port works.** The serial tools see what the guest
  writes to every serial port, whatever it is connected to: a socket, a
  pty, a window or `mon:stdio`. You don't need a socket of your own.
- **Waiting for something outside the VM,** such as a QEMU process that
  should exit: use your agent's own way to wait in the background (in
  Claude Code, a background shell with an `until` loop), not a Python
  sleep.
- **Files and logs on the host:** `xxd`, `od`, `grep`, `jq` and `cmp` do
  the job.

Not covered yet: breakpoints, single steps, memory writes and copying
files. For guests that run `rshd`, an rsh MCP server such as
[rsh-mcp](https://github.com/tenox7/rsh-mcp) runs commands and copies
files.

## Safety

- **Other people's VMs.** `vm-list` shows which VMs an agent started
  (detected for Claude Code). All other VMs belong to the user: don't
  stop, reset or change them unless the user asks.
- **Disks.** `vm-info` shows whether disks are read-only or snapshot. If
  changes will stay, ask before risky steps, or save a `vm-snapshot`
  first.
- **Turning it off.** Start a VM with `-mcp off` to turn MCP off. MCP is
  also off under `-qtest`.

## Troubleshooting

- **A VM is missing from `vm-list`.** It was started with `-mcp off` or
  under `-qtest`, or by another user. Each VM writes
  `/tmp/lemu-$UID/<pid>.json`; check that the file is there.
- **Your own socket.** `-mcp unix:vm.mcp,server=on,wait=off` gives a VM its
  own socket instead of the default one. `lemu-mcp` still finds it. On
  macOS, Unix socket paths must stay under about 104 bytes.
