# LEMU

LEMU is a fork of [QEMU](https://www.qemu.org/).
It emulates **old computers**, and it is made to be used **with AI agents**.

## How LEMU differs from QEMU

- **Old machines.** The focus is complete system emulation of legacy
  platforms and their operating systems. Modern systems, KVM and cloud
  virtualization are not the goal.
- **Made for AI.** QEMU does not accept AI-generated code. LEMU is the
  opposite: it is written with AI agents and built for them.
- **Built-in MCP server.** An AI agent can see the screen, type, use the
  mouse and serial ports, change disks, power the VM and take snapshots.
  See [MCP.md](MCP.md).
- **Agent guides and skills.** [AGENTS.md](AGENTS.md) is for agents that
  work on LEMU. Skills are in [.agents/skills](.agents/skills).
- **Ready-to-run kits.** One portable folder per guest: LEMU for macOS,
  Linux and Windows, the disk, the firmware and `run.sh` / `run.cmd`.
  The scripts that build the kits are in [machines/](machines).
- **Seamless mouse for Windows NT.** [machines/vmmouse](machines/vmmouse)
  is a mouse driver for NT 3.1 to 4.0 on the PC, the Magnum, the RM200,
  the Jensen and the 40P. The window stops grabbing the mouse, and
  NT 4.0 gets a scroll wheel.

## Machines

Each guest links to its `run.sh`, which shows the QEMU options it needs.
The PC has no kit, so its `run.sh` is an example.

| Machine | Guests |
|---|---|
| Siemens Nixdorf RM200 and RM300 (MIPS) | [Windows NT 4.0](machines/rm200/nt40/run.sh) ([2 CPUs](machines/rm200/nt40smp/run.sh), [Server with 4 CPUs](machines/rm200/nt40srv/run.sh)), [SINIX-N 5.45](machines/rm200/sinix545/run.sh) |
| MIPS Magnum (Jazz) | [Windows NT 3.1](machines/jazz/nt31/run.sh) |
| DEC Jensen (DECpc AXP 150, Alpha) | [Windows NT 3.1](machines/jensen/nt31/run.sh), [Windows NT 4.0](machines/jensen/nt40/run.sh) |
| Intel PC with an S3 86C928 card | [Windows NT 3.1](machines/pc/nt31/run.sh) |
| IBM 40P (7020-40P, PReP, PowerPC 604) | [AIX 4.1.5](machines/40p/aix41/run.sh), [AIX 4.3.3](machines/40p/aix43/run.sh), [AIX 5.1](machines/40p/aix51/run.sh), [Windows NT 4.0](machines/40p/nt/run.sh), [OS/2 Warp](machines/40p/os2/run.sh), [Solaris 2.5.1](machines/40p/solaris/run.sh) |
| IBM RS/6000 7006-41T (PowerPC 601) | [AIX 3.2.5](machines/7006/aix325/run.sh) |
| IBM pSeries (POWER7, POWER8) | [AIX 5.3](machines/pseries/aix53/run.sh), [AIX 6.1](machines/pseries/aix61/run.sh), [AIX 7.2](machines/pseries/aix72/run.sh) |
| HP B160L (PA-RISC 1.1, PA-7300LC) | [HP-UX 10.20](machines/hppa/hpux1020/run.sh) (4 CPUs) |
| HP 9000/715 (PA-RISC 1.1, PA-7100LC) | [HP-UX 9.05](machines/hppa/hpux905/run.sh) |

## Build

    ./configure --target-list=mips64el-softmmu,mips64-softmmu,alpha-softmmu,i386-softmmu,ppc-softmmu,ppc64-softmmu,hppa-softmmu
    make

- You need the same tools as for QEMU, and Python 3.12 or newer.
- Leave out `--target-list` to build every target.

## Use it with an agent

1. Build LEMU, or take a kit made from November 2026 on. Every LEMU
   binary is also the MCP bridge when its first option is `-mcp-bridge`.
2. Add the bridge to your agent once. For Claude Code:
   `claude mcp add lemu -s user -- /path/to/qemu-system-ppc -mcp-bridge`
3. Start a VM from a shell, for example a kit's `./run.sh`.
4. Ask the agent to list the VMs and take a screenshot.

The MCP server works on macOS and Linux. All the details are in
[MCP.md](MCP.md).

## Rules

- LEMU code does not go back to QEMU.
- Please don't send LEMU patches or LEMU bug reports to the QEMU project.

## License

GPL version 2, like QEMU. See [COPYING](COPYING).
QEMU's own README is [README.rst](README.rst).
