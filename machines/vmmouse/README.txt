VMMOUSE - seamless mouse for Windows NT 3.1 to 4.0 under QEMU
=============================================================

What it does
------------
The QEMU window stops grabbing the mouse.
The pointer moves in and out of the window freely.
No more key combination to let go of the mouse.
The mouse wheel scrolls on Windows NT 4.0. (NT 3.x has no wheel
support; it just ignores the wheel.)

It uses QEMU's "vmmouse" device (the VMware mouse backdoor).
- Intel: QEMU adds it by itself on its PC machines. This part works
  with any QEMU, not only LEMU.
- MIPS, Alpha, PowerPC: these CPUs cannot use the x86 backdoor, so
  LEMU adds a memory-mapped twin of it, vmmouse-mmio
  (hw/input/vmmouse-mmio.c), to the Magnum and Pica 61 (Jazz), RM200,
  40P and Jensen boards. It is always there: run.sh needs no change.
VMware has the same backdoor, so the Intel one may work there too
(not tested).


Which file
----------
vmmouse-binaries.zip holds one installer per CPU:
    vmmouse-i386.exe    Intel     NT 3.1, 3.5, 3.51, 4.0
    vmmouse-mips.exe    MIPS      NT 3.1, 3.5, 3.51, 4.0  (Magnum/Jazz, RM200)
    vmmouse-alpha.exe   Alpha     NT 3.1, 3.5, 3.51, 4.0  (Jensen)
    vmmouse-ppc.exe     PowerPC   NT 4.0  (40P)

Unzip it on the host: NT has no unzip. Each .exe carries the driver
inside itself, so the one for your CPU is all you need to copy to a
guest. The mouse icon on the window is just so you can tell it apart.

The Intel, MIPS and Alpha ones are built with the Windows NT 3.1 SDK,
so they run on NT 3.1, 3.5, 3.51 and 4.0. The PowerPC one is built
with Visual C++ 4.1 and needs NT 4.0.


Install
-------
1. Log on as Administrator.
2. Run the .exe for your CPU.
3. Click Install.
4. When it asks, let it restart Windows NT.

If the restart hangs, quit QEMU and start it again (a cold boot).
Windows NT has already shut down cleanly by then. This happens on the
Magnum (Jazz): its firmware says "Testing Interrupt Controller...Error"
after a warm restart. It has also happened on NT 4.0 Intel.


Remove
------
1. Run the .exe again.
2. Click Remove.
3. Let it restart.


Check that it works
-------------------
Run the .exe. The "Current status" line should say:
    Absolute mode on: seamless mouse.

In the QEMU monitor, "info mice" should show:
    * Mouse #... vmmouse (absolute)

The same status line is in the registry, value State under
    HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\vmmouse


From a script, with no window
-----------------------------
    VMMSETUP /install     install, then show a message
    VMMSETUP /remove      remove, then show a message
    VMMSETUP /status      show the status
Add /q to any of these for no window at all (the errorlevel is 0 if
it worked). Example:
    start /wait VMMSETUP /install /q
"start /wait" makes cmd wait, so you get the installer's errorlevel.

Always add /q when you run it from a service, such as ntrshd. A
service has no screen: the message box is invisible and waits forever.


If Windows NT does not start
----------------------------
1. Restart.
2. When it says "Press spacebar NOW to invoke the Last Known Good menu",
   press the space bar.
3. Pick the Last Known Good configuration.
   That is the setup from before the install.


Tested (November 2026, LEMU)
----------------------------
- NT 3.1 SP3, Intel:          pointer, clicks. The wheel does nothing.
- NT 3.1, MIPS Magnum (Jazz): pointer, clicks. The wheel does nothing.
- NT 3.1, Alpha Jensen:       pointer, clicks. The wheel does nothing.
- NT 4.0 SP6, Intel:          pointer, clicks, wheel.
- NT 4.0, MIPS RM200:         pointer, wheel.
- NT 4.0, MIPS Magnum (Jazz): pointer, clicks, wheel.
- NT 4.0, Alpha Jensen:       pointer, wheel.
- NT 4.0, PowerPC 40P:        pointer, clicks, keyboard, wheel.
The wheel test: 5 notches in Notepad scroll exactly 15 lines (NT 4.0
scrolls 3 lines a notch), then 5 notches back to the top.
On Intel with no backdoor (QEMU -machine vmport=off, like a real PC)
the driver does not start, and the normal mouse works.
Not tested yet: NT 3.5, NT 3.51, the Pica 61, VMware, real PCs.


Other CPUs (MIPS, Alpha, PowerPC)
---------------------------------
One vmmouse.c builds for every CPU. On Intel it uses the IN/OUT port
backdoor. On RISC it uses vmmouse-mmio's register window instead. The
driver tries each address it knows and keeps the one that answers:
    MIPS Magnum (Jazz)   0x8000e000
    MIPS RM200           0x13ff0000
    PowerPC 40P          0xc0f00000
    Alpha Jensen         EISA memory 0x00f00000
If none answers, the driver does not start (safe).

Two Alpha lessons (both were bugs, both fixed):
- Jensen EISA memory is "sparse". The driver hands the HAL a QVA:
  0xA0000000 + the bus address. READ/WRITE_REGISTER_ULONG decode it
  and do the sparse access. Do not map it with MmMapIoSpace.
- The DDK packs IO_STACK_LOCATION to 4 bytes on every CPU except
  Alpha. So it is 0x28 bytes on Alpha and 0x24 everywhere else. With
  the wrong size, every request the driver passed down was 4 bytes off,
  and the keyboard and mouse both died. ntmini.h now matches, and the
  build stops if the size is ever wrong.


Build for MIPS, Alpha or PowerPC
--------------------------------
Build on that NT machine: its compiler only runs there. Each CPU has a
folder of batch files. They expect this folder on the guest as
T:\NTOS\vmmouse, for example on a share mapped with
    net use T: \\<host>\<share>
NT 3.1 needs the host's name there, not its IP address: put the name
in the guest's LMHOSTS file with #PRE.
For another place, change T:\NTOS\vmmouse in the batch files.

MIPS and Alpha, with the Windows NT 3.1 SDK on NT 3.1. This is how the
zip's MIPS and Alpha installers are made. Run:
    T:\NTOS\vmmouse\mipsbuild\build31.bat    on NT 3.1 for MIPS
    T:\NTOS\vmmouse\alphabuild\build31.bat   on NT 3.1 for Alpha
Each makes vmmouse.sys and vmmsetup.exe (driver inside), in
D:\TEMP\vmb31 or D:\TEMP\vmb31axp.

Visual C++ 4 on NT 4.0: PowerPC, or MIPS and Alpha for NT 4.0 only.
Run, in this order:
    T:\NTOS\vmmouse\<folder>\build.bat   makes vmmouse.sys
    T:\NTOS\vmmouse\<folder>\setup.bat   makes vmmsetup.exe, driver inside
    T:\NTOS\vmmouse\<folder>\inst.bat    installs it, no window

    folder       compiler                        output in
    mipsbuild    Visual C++ 4, D:\MSDEV          D:\TEMP\vmb
    alphabuild   Visual C++ 4.0, D:\MSDEV        D:\TEMP\vmb-axp
    ppcbuild     Visual C++ 4.1, C:\MSDEV        C:\TEMP\vmb-ppc
ppcbuild falls back to T:\NTOS\_POWERPC\msvc\setup\MSDEV if the guest
has no compiler of its own.

The .def files make the ntoskrnl import library (and the hal one on
Alpha). On MIPS and PowerPC, READ/WRITE_REGISTER_ULONG are macros in
the DDK, not exports, so mips-reg.c and ppc-reg.c supply them.

Two NT 3.1 SDK lessons:
- Its MIPS and Alpha compilers call memcpy and memset instead of
  inlining them (ntmini.h's RtlCopyMemory and RtlZeroMemory), and the
  Alpha one also calls OtsMove. The NT 3.1 kernel exports none of
  them, so build31.bat links libc.lib into the driver. On MIPS, libc's
  memcpy and memset call RtlMoveMemory and RtlFillMemory, so
  mipsbuild\ntoskrnl.def lists those too.
- NT 3.1's SET refuses a value with an equals sign in it ("The syntax
  of the command is incorrect"), so build31.bat writes the -D flags
  out on each command line.


Files
-----
README.txt            this file
vmmouse-binaries.zip  the installers, one per CPU, driver inside
SRC\                  all the source
mipsbuild\  alphabuild\  ppcbuild\    RISC build batch files
.gitattributes        keeps the CRLF line ends that the NT tools need


Build for Intel  (the SDK is enough - no DDK needed)
----------------------------------------------------
Needs only the Windows NT 3.1 Win32 SDK (cl386, link32, lib32, rc).
    cd SRC
    nmake -f vmmouse.mak
This makes vmmouse.sys (the driver) and vmmsetup.exe (the installer,
with the driver built into it). On macOS or Linux, the Docker image
tenox7/nt31sdk has this SDK.

How it is self-contained:
- ntmini.h is a small copy of just the DDK types the driver uses, so
  no DDK is needed.
- ntstubs.c + the .def files let lib32 make the ntoskrnl and hal import
  libraries, so the driver links against NT 3.1's exports.
- The driver is built against NT 3.1, so one vmmouse.sys runs on
  NT 3.1, 3.5, 3.51 and 4.0.


How it works
------------
- vmmouse.sys is a filter on top of i8042prt, the PS/2 mouse driver.
- When Windows opens the mouse, the driver switches QEMU's vmmouse
  to absolute mode. QEMU then stops grabbing the mouse.
- QEMU reports where the host pointer is (0 to 65535 on each axis).
  The driver gives that to Windows as an absolute position.
- QEMU also reports the wheel. The driver passes it on as a wheel
  event (MOUSE_WHEEL, 120 per notch), like a real IntelliMouse.
- When Windows closes the mouse, the driver turns vmmouse off again.
- On a real PC (no QEMU, no VMware) the driver does not start,
  and the mouse works as before.
- The keyboard shares i8042prt. Its data passes through unchanged.
