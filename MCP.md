# Using the LEMU MCP server

LEMU has a built-in [MCP](https://modelcontextprotocol.io/) server.
With it, an AI agent can work a VM like a person at the keyboard:

- look at the screen
- type and use the mouse
- read and write serial ports, and log in over telnet
- change CDs and floppies, and copy files in and out on them
- power the VM on and off, and take snapshots

It works on macOS and Linux.

## How it works

- **Every LEMU VM offers MCP.** It is on by default (`-mcp on`). The VM
  opens a socket in `/tmp/lemu-$UID/`, a folder only your user can open.
- **`lemu-mcp` is the bridge.** It is the one MCP server your agent starts.
  It finds all running VMs and passes each tool call to the VM you name.
- **VMs can come and go.** `lemu-mcp` keeps running.

## Setup

1. Find the bridge. Every LEMU binary is also the bridge when its first
   option is `-mcp-bridge`, for example `qemu-system-ppc -mcp-bridge`.
   That includes the binaries in the ready-to-run kits made from
   November 2026 on. A LEMU build (see [README.md](README.md)) also has
   the bridge as its own program, `build/tools/lemu-mcp/lemu-mcp`.
2. Add the bridge to your agent once.
   - Claude Code:
     `claude mcp add lemu -s user -- /path/to/qemu-system-ppc -mcp-bridge`
     or `claude mcp add lemu -s user -- /path/to/build/tools/lemu-mcp/lemu-mcp`
   - Other MCP clients: add a stdio server with that command.
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
| `vm-wait` | Wait for a VM to start, end or start again, or for a guest service on a forwarded port |
| `vm-info` | Describe one VM: screens, mouse, disks, serial ports. Use it first |
| `vm-stats` | Measure how much host CPU a VM uses and how often its CPUs are halted |
| `screenshot` | Take a picture of a screen (PNG) |
| `frames` | List recent screens, to find one that went by too fast |
| `wait-screen` | Wait until the screen changes, settles, shows a colour or shows some text. Use it instead of sleeping |
| `screen-text` | Read a text-mode screen (BIOS, DOS, NT setup) as text |
| `key` | Press keys and key combinations |
| `type-text` | Type a string, in the guest's keyboard layout |
| `mouse-move`, `mouse-click`, `mouse-drag`, `mouse-button`, `mouse-wheel` | Use the mouse, also in guests with a relative mouse |
| `pointer-locate` | Find where the guest's mouse pointer really is |
| `serial-read`, `serial-write`, `serial-expect` | Read, write and wait for serial port text |
| `serial-run` | Run a command in a shell on a serial port: get its output and exit status in one call |
| `serial-connect` | Open a telnet session to the guest and use it like a serial port |
| `media` | List, insert or eject CDs and floppies; put host files on a new one, or copy the files off a floppy |
| `vm-power` | Pause, resume, reset, power down or quit |
| `vm-snapshot` | Save, load, list or delete whole-VM snapshots (needs qcow2 disks) |
| `debug-run` | Run until a CPU gets to an address (a breakpoint), touches watched memory or takes an exception, then show the instruction, the registers and memory |
| `debug-trace` | Log each arrival at an address, access to watched memory or exception, with registers and memory, while the guest runs on |
| `debug-step` | Run one CPU an instruction at a time, and show each one |
| `debug-profile` | Sample where the CPUs run, to find idle loops and hot spots |
| `memory-write` | Patch guest RAM or ROM: code or data |
| `memory-search` | Find text or bytes in guest RAM and ROM |
| `debug-symbols` | Load the names of the guest's functions and variables (from nm or a link map), to use in place of addresses |
| `register-write` | Set a register of a stopped CPU, or move it to another instruction (`pc`) |
| `hmp`, `qmp` | Run any QEMU monitor command: registers, memory, disassembly, pause and go on |
| `steps` | Run several of these tools in one call, in order; stop at the first wait that is not met |

## How to drive a VM

The server sends these rules to every agent that connects:

- **Look before you act.** Take a screenshot first. The keyboard and
  mouse tools take `then`, so they can wait and look again in the same
  call.
- **Known sequences in one call:** for a login or the next pages of an
  installer, `steps` runs several tool calls in order. It stops at the
  first wait that is not met, so put a wait after each action.
- **A black screen** in a graphical guest is often its screen saver,
  not a hang. Move the mouse, then look again.
- **Don't sleep.** Use `wait-screen`, or `serial-expect` for serial
  text. One call can wait up to 25 minutes; for a longer install, call
  it again. Let a long call finish: a call you stop returns nothing,
  not even what it saw so far. A wait that times out says what kept
  changing. On a text screen (a BIOS, DOS, a boot menu), `wait-screen`
  until `text` waits for words.
- **Starting and stopping:** after you start a VM from a shell, call
  `vm-wait`. After a guest shutdown, `vm-wait` until `gone` before you
  copy its disk.
- **No scripts.** Don't write Python or socket scripts to drive a VM.
  The tools do it; see [Without Python](#without-python).
- **Commands in a Unix guest:** log in on a serial port and use
  `serial-run`. It returns the output and the exit status. If the
  console is a graphics screen, open a telnet session with
  `serial-connect` and log in there: the serial tools work on it the
  same way. Old Unix tools lack GNU options such as `grep -A` and
  `sed -i`: use `awk`.
- **Files without a network:** `media` `insert` with `files` puts host
  files on a new CD or floppy. To get files out, insert a blank floppy
  (`files: []`), let the guest write to it, then `media` `copy-out`.
- **Check the focus before typing.** Keys go to the focused window.
- **Old dialogs:** use Tab and Shift+Tab to reach a control, then
  Space. Radio buttons and check boxes need Space. Enter presses the
  default button, which may be Cancel or Decline.
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
| Stop when the guest gets to an address (a breakpoint) | `debug-run` |
| Stop at whichever of several addresses comes first | `debug-run` with a list in `address` |
| Stop when the guest crashes or takes a trap (an exception) | `debug-run` with `exception` |
| See which exceptions the guest takes, and how often | `debug-trace` with `exception: {}` |
| Find what writes or reads some memory (a watchpoint) | `debug-run` with `watch` |
| See every call of a function, or every write to some memory, as the guest runs | `debug-trace` |
| See how the CPU got to where it stopped | `debug-run` with `history` |
| Stop at the same code in any process (any space) | `debug-run` with `address-mask` |
| Single-step | `debug-step` |
| Patch code or data | `memory-write` |
| Find a message, a value or a signature in memory | `memory-search` (`wide` for Windows NT's UTF-16 strings) |
| Use function and variable names in place of addresses | `debug-symbols` with nm's output or a link map |
| Change a register, or where the CPU goes on (the pc) | `register-write` |
| Find the idle loop, or where the guest spends its time | `debug-profile` |
| Pause and go on | `vm-power` `pause` and `resume`, or `hmp` `stop` and `cont` |
| Any other monitor command | `hmp` (`help` lists them) or `qmp` |
| Run a command in the guest, get its output and exit status | `serial-run` (log in on a serial port first) |
| Type into a serial console | `serial-write` (old Unix wants `\r`) |
| Wait for serial text | `serial-expect` |
| Read what a serial port printed | `serial-read` |
| Log in over the network, when the console is the screen | `serial-connect`, then the serial tools on the port it names |
| Wait for the screen | `wait-screen` |
| Wait for words on a text screen (a BIOS, a boot menu) | `wait-screen` until `text`, or a key's `then` with `wait: text` |
| Wait until a menu line is chosen (highlighted) | `wait-screen` until `text` with `highlighted` |
| Press keys, type, wait and look, several times, in one call | `steps` |
| Wait for a VM to start, or to end | `vm-wait` until `up` or `gone` |
| Wait for a VM to start again after a restart | `vm-wait` with `restarted` |
| Wait until a guest service answers on a forwarded port | `vm-wait` until `port` |
| See how much host CPU a VM uses, and whether the guest idles | `vm-stats` |
| Crop or zoom a screenshot | `screenshot` with `region` and `scale` |
| Find a screen that went by | `frames` |
| Change a CD or floppy | `media` |
| Put host files into a guest | `media` `insert` with `files` |
| Get files out of a guest | `media` `insert` with `files: []`, the guest writes to the floppy, then `media` `copy-out` |

- **Every serial port works.** The serial tools see what the guest
  writes to every serial port, whatever it is connected to: a socket, a
  pty, a window or `mon:stdio`. You don't need a socket of your own.
- **Telnet sessions are ports too.** `serial-connect` with `address`
  `2323` connects to 127.0.0.1:2323 and returns a port name such as
  `telnet0`. With user networking, use the host port that is forwarded
  to the guest's port 23; to add one in a running VM, use `hmp`
  `hostfwd_add tcp:127.0.0.1:2323-:23`. lemu answers the telnet options
  itself (terminal type VT100), so the guest's telnetd needs no setup.
  When the guest ends the session, the serial tools say so at once;
  `serial-connect` again opens a new one on the same port name.
- **A login in one call:** `steps` with this list types the user name,
  waits for the prompt and runs a command. If the prompt does not come,
  it stops there and says so. A command that fails does not stop it:
  to stop on a failure, join the commands with `&&` in one `serial-run`.
  All steps run on the VM the call names, and together they get the
  25 minutes of one call.

  ```json
  [{"tool": "serial-write", "args": {"text": "root\r"}},
   {"tool": "serial-expect", "args": {"pattern": "# $"}},
   {"tool": "serial-run", "args": {"command": "uname -a"}}]
  ```
- **A boot prompt that shows for seconds,** such as NT's "Press
  spacebar NOW": start QEMU with `-S`, so that it waits before the
  first instruction, and `vm-wait` until it is up. Then one `steps`
  call starts the guest, waits for the prompt and answers it, with no
  gap between the steps:

  ```json
  [{"tool": "vm-power", "args": {"action": "resume"}},
   {"tool": "wait-screen", "args": {"until": "text",
                                    "text": "Press spacebar NOW"}},
   {"tool": "key", "args": {"keys": ["spc"]}}]
  ```
- **Waiting outside the VM:** `vm-wait` covers a QEMU process that
  starts or ends, and a port that should answer. For anything else, use
  your agent's own way to wait in the background (in Claude Code, a
  background shell with an `until` loop), not a Python sleep.
- **Files and logs on the host:** `xxd`, `od`, `grep`, `jq` and `cmp` do
  the job.
- **Debugging without GDB:** `debug-run` stops the VM when a CPU gets
  to an address, and shows the next instruction, the registers and the
  memory you ask for. Look around with `hmp`, go on with `debug-step` or
  another `debug-run`, and `vm-power` `resume` when you are done.
  Addresses are virtual, as the CPU sees them (on x86: the CS base plus
  EIP). These tools need TCG, the default for the old CPUs, and no GDB
  stub.
- **Names for addresses:** give `debug-symbols` the guest's symbol
  list: nm's output (in the guest or on the host), a System.map, or a
  link map, as `text` or a `file`. Then every address a debug tool
  takes can be a name, such as `panic` or `panic+0x1c`, and stops,
  history, profiles, search matches and disassembly (also `hmp`
  `x /i`) show `0x80123456 <panic+0x1c>`. `base` moves a list to where
  a module or a DLL was loaded; `lookup` turns names into addresses and
  back. A 32-bit list works as it is on a 64-bit CPU that extends
  addresses with the sign (NT on MIPS and Alpha).
- **Memory at a register:** a `memory` address can add and take away
  registers, read when the CPU stops: `sp+16`, `r26`, or
  `0x657500000000+r26` when the register holds only the offset in a
  space or segment (take the base from the register dump). Names are
  GDB's; CPUs without names there (alpha, hppa, mips) take `rN`, which
  is the general register N on most CPUs. `string: true` shows the C
  string there.
- **What writes this memory?** `debug-run` with `watch` (`address`,
  `length`, and `write`, `read` or `access`) stops right after a CPU
  touches it, and shows the instruction that did it. With an
  `address` too, whichever comes first stops it. Only CPUs are
  watched, not devices that write memory themselves (DMA). On hppa,
  give the whole address, space and offset, as for other memory.
- **Several at once:** `address` takes a list of up to 64 addresses,
  and `watch` a list of up to 16 areas. `debug-run` stops at whichever
  a CPU gets to or touches first; `debug-trace` logs them all and
  counts the arrivals by address.
- **Exceptions (traps and faults):** `debug-run` with `exception`
  stops right after a CPU takes one, at the first instruction of the
  guest's handler, and shows the instruction that took it. The numbers
  are QEMU's for each CPU, so look first: `debug-trace` with
  `exception: {}` counts the ones the guest takes, by number and by
  instruction. Then stop at a rare one with `numbers`, or at anything
  but the everyday ones (page faults, TLB misses, system calls) with
  `skip`. Interrupts from devices are not exceptions.
- **Changing registers:** `register-write` sets a register of a stopped
  CPU by the names a `memory` address takes, and returns the old value.
  `pc` moves the CPU to another instruction: to skip one, or to run one
  again. It changes the registers that hold the pc (on x86 EIP, not the
  CS base; on SPARC npc too) and keeps the CPU's mode, such as PALcode
  or a privilege level. If it can't, it changes nothing and says what
  else would change.
- **How did it get here?** `debug-run` with `history: 16` notes the
  last 16 blocks of code each CPU runs, and shows them when it stops,
  oldest first, as start-end ranges. A block runs from where a jump
  lands to the next jump, so each block ends in the jump to the next
  one; a loop of one block shows once, with `xN`. It works the same on
  every CPU, with no stack to unwind. The guest runs slower while the
  blocks are noted.
- **The same code in any process:** where the top bits of an address
  name a space (hppa), each process runs in its own space, and a new
  process gets a new one, so a plain `address` stops in one process
  only. `address-mask: "0xffffffff"` compares only the low 32 bits:
  `debug-run` then stops in any space, and `debug-trace` counts the
  arrivals by address, one line per space. A mask works on every CPU:
  its bits are the ones that must match.
- **Every hit, with the guest running on:** `debug-trace` logs each
  arrival at `address`, or each access to `watch`, with the
  `registers` and `memory` you ask for, until `count` hits or
  `duration-ms`. For watched memory it counts which instructions
  touched it, and lists the first access by each one, so a rare
  writer behind a busy loop still shows. Each hit stops the VM for a
  moment: a busy address slows the guest while it traces.
- **TLB misses and page faults:** when the CPU has no mapping for an
  address right now, the debug tools say so instead of showing the
  instruction or the memory there. Kernels are often mapped 1:1: ask
  for that memory with `physical: true`. An instruction that takes a
  TLB miss or a page fault runs again once the guest has mapped it.
  `debug-step` shows where such an instruction sent the CPU, and
  `debug-run` marks the arrival when the CPU comes back with
  `retry: true`. Run `debug-run` again to get to the next real
  arrival. `debug-profile` gives each hot spot's `physical` address
  too, so the same address in two processes is two entries.
- **When you need GDB after all,** for example to stop only when a
  register holds some value: `hmp` `gdbserver tcp:127.0.0.1:1234`
  turns the GDB stub on in a running VM, with no restart. Don't use
  the debug tools while a GDB client is attached.

- **Copying files** needs nothing in the guest. `media` `insert` with
  `files` makes a disc for a CD-ROM drive, or a 1.44 MB floppy for
  other drives, from host files and folders. Every guest reads the 8.3
  names; Unix guests that read Rock Ridge see the real names on a disc.
  Mount it as the guest's CD or DOS file system (`hsfs`, `cdrfs`,
  `cdfs`, `iso9660`, `pcfs`, `msdos`). `copy-out` reads FAT12 and FAT16,
  with the long names Windows writes. A medium lemu made is deleted when
  its drive gets another one, so copy out first.
- **Guests with `rshd`:** an rsh MCP server such as
  [rsh-mcp](https://github.com/tenox7/rsh-mcp) runs commands and copies
  files over the network, which is faster.

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
- **"Another one may have it".** A VM takes one MCP client at a time:
  the first `lemu-mcp` that uses it, or a script on its socket. Others
  wait, and their calls fail after 5 s, with the call not run.
  `vm-list` and the tool list only look: they leave each VM to the
  session that uses it.
- **Long waits in Claude Code.** A tool call that runs longer than 2
  minutes moves to the background, and Claude Code tells the agent when
  it ends. Stopping it (TaskStop) cancels the wait in the VM. One call
  waits at most 25 minutes, under Claude Code's 30-minute limit for a
  call that sends nothing. That holds for a `steps` call too: its steps
  share the 25 minutes.
