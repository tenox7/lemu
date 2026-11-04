@echo off
rem build31.bat - make vmmouse.sys and vmmsetup.exe for Alpha with the
rem Windows NT 3.1 SDK (claxp, link32 2.43). Run it on NT 3.1 for Alpha.
rem What it makes runs on NT 3.1 to 4.0.
rem It expects this folder as T:\NTOS\vmmouse and builds in D:\TEMP\vmb31axp.
rem NT 3.1 SET refuses a value with an equals sign in it, so the -D flags
rem are written out on each command line.
if not exist D:\TEMP\vmb31axp\nul md D:\TEMP\vmb31axp
D:
cd \TEMP\vmb31axp
if exist vmmouse.sys del vmmouse.sys
if exist vmmsetup.exe del vmmsetup.exe
for %%f in (ntmini.h vmmouse.c vmmouse.h vmmouse.ico vmmsetup.c vmmsetup.h vmmsetup.rc) do copy T:\NTOS\vmmouse\SRC\%%f %%f >nul
copy T:\NTOS\vmmouse\alphabuild\ntoskrnl.def ntoskrnl.def >nul
copy T:\NTOS\vmmouse\alphabuild\hal.def hal.def >nul
echo === vmmouse.sys ===
claxp -c -W3 -Ox -D_ALPHA_=1 -DDBG=0 vmmouse.c
lib32 -machine:alpha -def:ntoskrnl.def -out:ntoskrnl.lib
lib32 -machine:alpha -def:hal.def -out:hal.lib
rem libc.lib has memcpy, memset and OtsMove: NT 3.1 does not export them.
link32 -out:vmmouse.sys -machine:alpha -subsystem:native -align:0x200 -base:0x10000 -entry:DriverEntry -debug:none vmmouse.obj ntoskrnl.lib hal.lib libc.lib
echo === vmmsetup.exe ===
claxp -c -W3 -Ox -D_ALPHA_=1 -DWIN32 vmmsetup.c
rc -r -fo vmmsetup.res vmmsetup.rc
cvtres -ALPHA vmmsetup.res -o vmmsetup.rbj
link32 -out:vmmsetup.exe -subsystem:windows -entry:WinMainCRTStartup vmmsetup.obj vmmsetup.rbj libc.lib kernel32.lib user32.lib advapi32.lib
if exist vmmouse.sys echo SYS_BUILT
if exist vmmsetup.exe echo EXE_BUILT
dir vmmouse.sys vmmsetup.exe
