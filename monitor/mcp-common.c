/*
 * MCP: what the MCP monitor and the lemu-mcp bridge share
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "monitor/mcp-common.h"
#include "qobject/qstring.h"

static const char *const mcp_protocols[] = {
    "2025-11-25", "2025-06-18", NULL
};

const char mcp_instructions[] =
    "You are operating a real emulated computer through lemu (QEMU).\n"
    "- Look before you act: take a screenshot first. After an action, use"
    " the tool's \"then\" to wait and look in the same call.\n"
    "- For a sequence you know, such as a login or the next pages of an"
    " installer, steps runs several tool calls in one. It stops at the"
    " first wait that is not met, so put a wait after each action.\n"
    "- A black screen in a graphical guest is often its screen saver, not"
    " a hang: move the mouse, then look again.\n"
    "- Use wait-screen or serial-expect instead of sleeping. Long installs:"
    " change-then-stable with a long stable-ms and a long timeout-ms (one"
    " call can wait up to 25 minutes; let it finish, since a call you stop"
    " returns nothing). If a clock or a graph keeps the"
    " screen from settling, add ignore-animation (not while text"
    " scrolls). On a text screen (BIOS, DOS, boot menus), until text"
    " waits for words.\n"
    "- Don't write Python or socket scripts to drive the VM; the tools do"
    " it. hmp runs monitor commands: registers ('info registers'), memory"
    " ('xp', 'x'), disassembly ('x /10i ADDR'). Instead of GDB scripts,"
    " use debug-run (stop at an address, when memory is written, by a CPU"
    " or by a device (DMA), or when the guest takes an exception;"
    " condition stops only when a register or memory holds a value; a"
    " watch on a device's registers, or on I/O ports with space, shows"
    " the value and the register), debug-trace (log each hit while the"
    " guest runs on), debug-step, memory-write, register-write, and"
    " debug-profile to find idle loops and hot spots. debug-coverage finds"
    " the code that runs for one action (start, do it, stop), less the"
    " code that runs anyway. memory-search finds"
    " text, bytes or a number in guest memory; to find a variable, search"
    " for its value, let it change, then search with refine for the new"
    " one. Give debug-symbols nm's output or a"
    " link map, and the debug tools take and show names (panic+0x1c)"
    " for addresses. The serial tools reach"
    " every serial port, whatever it is connected to. screenshot can crop"
    " (region) and zoom (scale). sound hears the guest's sound (start,"
    " do the thing, then wait): how long and how loud. record makes a"
    " video with sound for YouTube (start, use the tools, stop); while"
    " it records, the pointer moves like a hand and type-text types"
    " like a person.\n"
    "- To run commands in a Unix guest, log in on a serial port and use"
    " serial-run: one call types the command and returns its output and"
    " exit status. If the console is a graphics screen, open a telnet"
    " session with serial-connect and log in there: the serial tools work"
    " on it the same way. Old Unix tools lack GNU options (grep -A/-B,"
    " sed -i): use awk.\n"
    "- To move files without a network, media insert with files puts host"
    " files on a new CD or floppy. To get files out, insert a blank floppy"
    " (files: []), let the guest write to it, then media copy-out.\n"
    "- Check focus before typing (focus ring, highlighted title bar,"
    " caret). Keys go to the focused window, not to the one you are"
    " looking at.\n"
    "- In old dialogs prefer Tab / Shift+Tab to reach a control, then"
    " Space to press it; radio buttons and check boxes need Space. Enter"
    " presses the default button, which may be Cancel or Decline. Alt"
    " mnemonics can be ambiguous (Alt+C may mean Continue or Cancel).\n"
    "- Combo boxes: use Home, End and arrow keys; type-ahead moves one"
    " item per key.\n"
    "- These guests mostly have relative mice. The mouse tools home the"
    " pointer and move in small steps; the first move measures how the"
    " guest moves it: how far a mouse count goes, and how big a step the"
    " guest does not speed up. If a click misses, take a"
    " screenshot with pointer=true, or use the keyboard. If the pointer"
    " still lands too far, move with mouse-move step-px 1, then click"
    " without x and y.\n"
    "- Old menus may need press-and-hold: mouse-button down, mouse-move,"
    " mouse-button up.\n"
    "- Use type-text with the guest's keyboard layout (for example \"de\""
    " for a German keyboard). Unknown characters are rejected before"
    " anything is typed.\n"
    "- Each result lists VM events since your last call, repeats merged"
    " with a count. A reset or stop there means the guest rebooted or the"
    " VM paused.\n"
    "- vm-info tells you whether disks are snapshot or read-only. If"
    " changes will persist, ask the user before risky steps.\n"
    "- Networks: -nic user is NAT and needs nothing (hostfwd for ports)."
    " On macOS, -nic vmnet-bridged,ifname=en0 puts the VM on the user's"
    " LAN. It needs no sudo when /usr/local/bin/lemu-vmnet is installed:"
    " run it to check, and it lists the interfaces. Give each bridged VM"
    " its own mac=52:54:00:xx:xx:xx; a VM whose MAC another VM already"
    " uses does not start. vm-info lists the networks.\n"
    "- VMs not started by you belong to the user. Touch them only when"
    " the user asked. Never stop or reset them.\n";

const char mcp_tool_hmp[] =
    "{\"name\":\"hmp\","
    "\"description\":\"Run one HMP (human monitor) command line and return"
    " its text output. Use it instead of a script on the monitor socket."
    " Examples: 'info registers'; 'xp /16wx 0x1000' reads physical memory,"
    " 'x /16wx ADDR' virtual memory; 'x /10i ADDR' disassembles; 'stop'"
    " and 'cont' pause and resume the CPUs; 'pmemsave ADDR SIZE FILE'"
    " saves memory to a file; 'help' lists the rest. A VM built without"
    " HMP answers with an error.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"command\":{\"type\":\"string\","
    "\"description\":\"the command line, for example 'info registers'\"}},"
    "\"required\":[\"command\"]},"
    "\"annotations\":{\"destructiveHint\":true,\"idempotentHint\":false,"
    "\"openWorldHint\":false}}";

const char mcp_tool_qmp[] =
    "{\"name\":\"qmp\","
    "\"description\":\"Run any QMP command and return its JSON result, for"
    " example query-status or query-block. For memory, registers and"
    " disassembly the hmp tool is simpler.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"execute\":{\"type\":\"string\",\"description\":\"the QMP command\"},"
    "\"arguments\":{\"type\":\"object\","
    "\"description\":\"its arguments\"}},"
    "\"required\":[\"execute\"]},"
    "\"annotations\":{\"destructiveHint\":true,\"idempotentHint\":false,"
    "\"openWorldHint\":false}}";

const char mcp_tool_steps[] =
    "{\"name\":\"steps\","
    "\"description\":\"Run several tool calls in one, in order: keys,"
    " typing, mouse, waits, serial, media, hmp. Use it for a sequence you"
    " know, such as the next pages of an installer or a login. It stops at"
    " the first step that fails or whose wait does not get what it waits"
    " for (wait-screen or a 'then' wait that times out, serial-expect with"
    " no match, serial-run still running, debug-run that does not get to"
    " its address), and says which. A command's exit status does not stop"
    " it: join commands with && in one serial-run for that. All steps are"
    " checked before the first one runs. They run on one VM and share one"
    " call's 25 minutes."
    " The result has each step's summary and the last screenshot"
    " taken.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"steps\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":50,"
    "\"description\":\"the calls, in order\","
    "\"items\":{\"type\":\"object\",\"properties\":{"
    "\"tool\":{\"type\":\"string\",\"description\":\"a tool, such as key,"
    " type-text, mouse-click, wait-screen, serial-write or"
    " serial-expect\"},"
    "\"args\":{\"type\":\"object\",\"description\":\"its arguments, as in"
    " a call of its own\"}},"
    "\"required\":[\"tool\"]}}},"
    "\"required\":[\"steps\"]},"
    "\"annotations\":{\"destructiveHint\":true,\"idempotentHint\":false,"
    "\"openWorldHint\":false}}";

bool mcp_protocol_supported(const char *v)
{
    int i;

    for (i = 0; v && mcp_protocols[i]; i++) {
        if (!strcmp(v, mcp_protocols[i])) {
            return true;
        }
    }
    return false;
}

QList *mcp_protocol_list(void)
{
    QList *list = qlist_new();
    int i;

    for (i = 0; mcp_protocols[i]; i++) {
        qlist_append_str(list, mcp_protocols[i]);
    }
    return list;
}

GString *mcp_unescape(const char *text)
{
    GString *s = g_string_new(NULL);
    const char *p = text;

    while (*p) {
        if (p[0] != '\\' || !p[1]) {
            g_string_append_c(s, *p++);
            continue;
        }
        switch (p[1]) {
        case 'r':
            g_string_append_c(s, '\r');
            p += 2;
            break;
        case 'n':
            g_string_append_c(s, '\n');
            p += 2;
            break;
        case 't':
            g_string_append_c(s, '\t');
            p += 2;
            break;
        case 'e':
            g_string_append_c(s, 0x1b);
            p += 2;
            break;
        case '\\':
            g_string_append_c(s, '\\');
            p += 2;
            break;
        case 'x':
            if (g_ascii_isxdigit(p[2]) && g_ascii_isxdigit(p[3])) {
                g_string_append_c(s, g_ascii_xdigit_value(p[2]) * 16 +
                                     g_ascii_xdigit_value(p[3]));
                p += 4;
                break;
            }
            /* fall through */
        default:
            g_string_append_c(s, *p++);
            break;
        }
    }
    return s;
}

const char *mcp_meta_protocol(QDict *params)
{
    QDict *meta = params ? qdict_get_qdict(params, "_meta") : NULL;
    const QDictEntry *e;

    if (!meta) {
        return NULL;
    }
    for (e = qdict_first(meta); e; e = qdict_next(meta, e)) {
        QString *s = qobject_to(QString, qdict_entry_value(e));

        if (s && strstr(qdict_entry_key(e), "protocolVersion")) {
            return qstring_get_str(s);
        }
    }
    return NULL;
}
