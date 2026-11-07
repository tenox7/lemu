/*
 * lemu-vmnet: vmnet networking for LEMU VMs without sudo (macOS)
 *
 * vmnet.framework connects a VM to a network: bridged to a host
 * interface such as en0 (the VM is on the LAN), shared (NAT) or host
 * only. Apple lets only root start a vmnet interface, so this small
 * helper is installed setuid root, once:
 *
 *   sudo install -o root -g admin -m 4750 lemu-vmnet /usr/local/bin/
 *
 * After that, the Mac's administrators (group admin) and the programs
 * they run, AI agents included, can use vmnet without sudo. Rebuilding
 * QEMU does not touch the helper.
 *
 * When QEMU is not root, the vmnet-bridged, vmnet-shared and
 * vmnet-host netdevs run "lemu-vmnet --qemu" with a socket on fd 3
 * (include/net/lemu-vmnet.h). The helper reads one request, starts the
 * vmnet interface as root, gives up root for good, answers, and then
 * passes Ethernet frames between the socket and vmnet. It stops when
 * QEMU closes the socket or exits.
 *
 * Root is held only for the start. The helper takes no file names,
 * clears its environment, closes the files it does not need, checks
 * every field of the request and logs each start and stop to the
 * system log (log show --predicate 'process == "lemu-vmnet"').
 *
 * Run it without arguments to see whether it is ready and which
 * interfaces can be bridged.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <arpa/inet.h>
#include <crt_externs.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <inttypes.h>
#include <libproc.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>
#include <vmnet/vmnet.h>

#include "net/lemu-vmnet.h"

#define BATCH 32                /* frames per vmnet_read() */
#define REQUEST_TIMEOUT_S 10
#define START_TIMEOUT_S 30
#define MAX_FRAME 65535

static const int sock = LEMU_VMNET_FD;
static interface_ref iface;
static dispatch_queue_t queue;
static dispatch_source_t read_source;
static uint32_t max_packet;
static uint8_t *rx_buf;
static struct vmpktdesc tx_pkt[BATCH];
static struct iovec tx_iov[BATCH];
static unsigned long long lost_to_qemu, lost_to_net;
static bool stopping;

static const char *vmnet_error(vmnet_return_t st)
{
    static char buf[32];

    switch (st) {
    case VMNET_SUCCESS:
        return "success";
    case VMNET_FAILURE:
        return "general failure";
    case VMNET_MEM_FAILURE:
        return "out of memory";
    case VMNET_INVALID_ARGUMENT:
        return "invalid argument";
    case VMNET_SETUP_INCOMPLETE:
        return "setup incomplete";
    case VMNET_INVALID_ACCESS:
        return "permission denied";
    case VMNET_PACKET_TOO_BIG:
        return "packet too big";
    case VMNET_BUFFER_EXHAUSTED:
        return "buffers exhausted";
    case VMNET_TOO_MANY_PACKETS:
        return "too many packets";
    case VMNET_SHARING_SERVICE_BUSY:
        return "the sharing service is busy";
    default:
        snprintf(buf, sizeof(buf), "vmnet error %d", (int)st);
        return buf;
    }
}

static const char *mode_name(uint32_t mode)
{
    switch (mode) {
    case VMNET_HOST_MODE:
        return "host";
    case VMNET_SHARED_MODE:
        return "shared";
    case VMNET_BRIDGED_MODE:
        return "bridged";
    default:
        return NULL;
    }
}

/* The usual care for a setuid program, before anything else */
static void sanitize(void)
{
    static char *empty_env[] = { NULL };
    int fd;

    umask(022);
    *_NSGetEnviron() = empty_env;
    /* 0, 1 and 2 must be open, or a file we open could become one */
    for (fd = 0; fd < 3; fd++) {
        if (fcntl(fd, F_GETFD) < 0 && open("/dev/null", O_RDWR) != fd) {
            _exit(126);
        }
    }
}

/* For good: real, effective and saved user and group */
static bool drop_root(void)
{
    uid_t uid = getuid();
    gid_t gid = getgid();

    if (setgid(gid) < 0 || setuid(uid) < 0) {
        return false;
    }
    if (uid != 0 && (setuid(0) == 0 || seteuid(0) == 0)) {
        return false;
    }
    return geteuid() == uid && getegid() == gid;
}

/* The interfaces that vmnet can bridge, as "en0 en1" */
static void bridgeable(char *buf, size_t size, const char *want, bool *found)
{
    xpc_object_t list = vmnet_copy_shared_interface_list();
    size_t i, n = list ? xpc_array_get_count(list) : 0;

    buf[0] = '\0';
    if (found) {
        *found = false;
    }
    for (i = 0; i < n; i++) {
        const char *name = xpc_array_get_string(list, i);

        if (!name) {
            continue;
        }
        if (want && found && strcmp(name, want) == 0) {
            *found = true;
        }
        if (buf[0]) {
            strlcat(buf, " ", size);
        }
        strlcat(buf, name, size);
    }
    if (list) {
        xpc_release(list);
    }
}

/* This program's own path, for messages */
static void self_path(char *buf, size_t size)
{
    char path[PATH_MAX], real[PATH_MAX];
    uint32_t len = sizeof(path);

    if (_NSGetExecutablePath(path, &len) == 0 && realpath(path, real)) {
        strlcpy(buf, real, size);
    } else {
        strlcpy(buf, "lemu-vmnet", size);
    }
}

static int show_status(bool root)
{
    char list[256], self[PATH_MAX];

    self_path(self, sizeof(self));
    bridgeable(list, sizeof(list), NULL, NULL);
    printf("lemu-vmnet %d: lets LEMU VMs use vmnet networking "
           "without sudo.\n"
           "QEMU starts it for the vmnet-bridged, vmnet-shared and "
           "vmnet-host netdevs\n"
           "when QEMU is not root, for example:\n"
           "  -nic vmnet-bridged,ifname=en0,model=e1000,"
           "mac=52:54:00:12:34:56\n",
           LEMU_VMNET_VERSION);
    if (root) {
        printf("Ready: %s runs as root.\n", self);
        if (strcmp(self, LEMU_VMNET_PATH) != 0) {
            printf("QEMU looks for %s, or give the netdev helper=%s\n",
                   LEMU_VMNET_PATH, self);
        }
    } else {
        printf("Not ready: %s does not run as root. Install it once "
               "with:\n  " LEMU_VMNET_INSTALL " %s /usr/local/bin/\n",
               self, self);
    }
    printf("Interfaces you can bridge: %s\n", list[0] ? list : "none");
    return root ? 0 : 1;
}

static bool is_dgram_socket(int fd)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    socklen_t tlen;
    int type = 0;

    tlen = sizeof(type);
    return getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tlen) == 0 &&
           type == SOCK_DGRAM &&
           getsockname(fd, (struct sockaddr *)&ss, &len) == 0 &&
           ss.ss_family == AF_UNIX;
}

static void close_other_fds(void)
{
    int fd, max = getdtablesize();

    if (max > (1 << 20)) {
        max = 1 << 20;
    }
    for (fd = LEMU_VMNET_FD + 1; fd < max; fd++) {
        close(fd);
    }
}

static bool reply(LemuVmnetReply *rep, uint32_t status, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* false if QEMU did not get it */
static bool reply(LemuVmnetReply *rep, uint32_t status, const char *fmt, ...)
{
    va_list ap;
    bool sent;

    rep->magic = LEMU_VMNET_MAGIC;
    rep->version = LEMU_VMNET_VERSION;
    rep->status = status;
    va_start(ap, fmt);
    vsnprintf(rep->message, sizeof(rep->message), fmt, ap);
    va_end(ap);
    sent = send(sock, rep, sizeof(*rep), 0) == sizeof(*rep);
    if (!sent) {
        fprintf(stderr, "lemu-vmnet: cannot answer QEMU: %s\n",
                strerror(errno));
    }
    if (status != LEMU_VMNET_OK) {
        syslog(LOG_NOTICE, "not started for uid %d: %s", (int)getuid(),
               rep->message);
    }
    return sent;
}

/* Names like en0: letters and digits only */
static bool valid_ifname(const char *s)
{
    size_t i, n = strnlen(s, LEMU_VMNET_IFNAMSIZ);

    if (n == 0 || n == LEMU_VMNET_IFNAMSIZ) {
        return false;
    }
    for (i = 0; i < n; i++) {
        char c = s[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9'))) {
            return false;
        }
    }
    return true;
}

/* NULL if the request is good, else what is wrong with it */
static const char *check_request(const LemuVmnetRequest *req)
{
    uint32_t mode_flags = 0;

    switch (req->mode) {
    case VMNET_HOST_MODE:
        mode_flags = LEMU_VMNET_RANGE | LEMU_VMNET_NET_UUID;
        break;
    case VMNET_SHARED_MODE:
        mode_flags = LEMU_VMNET_RANGE | LEMU_VMNET_NAT66;
        break;
    case VMNET_BRIDGED_MODE:
        if (!valid_ifname(req->ifname)) {
            return "the interface name is not valid";
        }
        break;
    default:
        return "unknown vmnet mode";
    }
    if (req->flags & ~(LEMU_VMNET_ISOLATED | mode_flags)) {
        return "an option does not fit the vmnet mode";
    }
    if (req->mode != VMNET_BRIDGED_MODE && req->ifname[0]) {
        return "only bridged mode takes an interface";
    }
    return NULL;
}

static xpc_object_t make_desc(const LemuVmnetRequest *req)
{
    xpc_object_t d = xpc_dictionary_create(NULL, NULL, 0);
    char a[INET6_ADDRSTRLEN];

    xpc_dictionary_set_uint64(d, vmnet_operation_mode_key, req->mode);
    xpc_dictionary_set_bool(d, vmnet_allocate_mac_address_key, false);
    xpc_dictionary_set_bool(d, vmnet_enable_isolation_key,
                            req->flags & LEMU_VMNET_ISOLATED);
    if (req->mode == VMNET_BRIDGED_MODE) {
        xpc_dictionary_set_string(d, vmnet_shared_interface_name_key,
                                  req->ifname);
    }
    if (req->flags & LEMU_VMNET_RANGE) {
        xpc_dictionary_set_string(d, vmnet_start_address_key,
            inet_ntop(AF_INET, req->start_address, a, sizeof(a)));
        xpc_dictionary_set_string(d, vmnet_end_address_key,
            inet_ntop(AF_INET, req->end_address, a, sizeof(a)));
        xpc_dictionary_set_string(d, vmnet_subnet_mask_key,
            inet_ntop(AF_INET, req->subnet_mask, a, sizeof(a)));
    }
    if (req->flags & LEMU_VMNET_NAT66) {
        xpc_dictionary_set_string(d, vmnet_nat66_prefix_key,
            inet_ntop(AF_INET6, req->nat66_prefix, a, sizeof(a)));
    }
    if (req->flags & LEMU_VMNET_NET_UUID) {
        xpc_dictionary_set_uuid(d, vmnet_network_identifier_key,
                                req->net_uuid);
    }
    return d;
}

static vmnet_return_t start_vmnet(xpc_object_t desc, uint64_t *mtu,
                                  uint64_t *maxp)
{
    /* Never released: the block may still run after a timeout */
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block vmnet_return_t status = VMNET_FAILURE;
    __block uint64_t m = 0, p = 0;

    iface = vmnet_start_interface(desc, queue,
        ^(vmnet_return_t st, xpc_object_t param) {
            if (st == VMNET_SUCCESS && param) {
                m = xpc_dictionary_get_uint64(param, vmnet_mtu_key);
                p = xpc_dictionary_get_uint64(param,
                                              vmnet_max_packet_size_key);
            }
            status = st;
            dispatch_semaphore_signal(done);
        });
    if (!iface) {
        return VMNET_FAILURE;
    }
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW,
                                START_TIMEOUT_S * NSEC_PER_SEC))) {
        return VMNET_FAILURE;
    }
    *mtu = m;
    *maxp = p;
    return status;
}

/* Runs on queue, like everything below */
static void stop(const char *why)
{
    dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);

    if (stopping) {
        return;
    }
    stopping = true;
    syslog(LOG_NOTICE, "stop: %s; frames dropped: %llu to QEMU, "
           "%llu to the network", why, lost_to_qemu, lost_to_net);
    dispatch_source_cancel(read_source);
    vmnet_interface_set_event_callback(iface,
        VMNET_INTERFACE_PACKETS_AVAILABLE, NULL, NULL);
    vmnet_stop_interface(iface, q, ^(vmnet_return_t st) {
        _exit(0);
    });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), q,
                   ^{ _exit(0); });
}

/* vmnet -> QEMU */
static void from_vmnet(void)
{
    int i, n;

    while (!stopping) {
        for (i = 0; i < BATCH; i++) {
            tx_iov[i].iov_len = max_packet;
            tx_pkt[i].vm_pkt_size = max_packet;
            tx_pkt[i].vm_pkt_iov = &tx_iov[i];
            tx_pkt[i].vm_pkt_iovcnt = 1;
            tx_pkt[i].vm_flags = 0;
        }
        n = BATCH;
        if (vmnet_read(iface, tx_pkt, &n) != VMNET_SUCCESS || n <= 0) {
            return;
        }
        for (i = 0; i < n; i++) {
            if (send(sock, tx_iov[i].iov_base, tx_pkt[i].vm_pkt_size,
                     0) < 0) {
                if (errno == ENOBUFS || errno == EAGAIN ||
                    errno == EINTR) {
                    lost_to_qemu++;     /* QEMU is behind */
                    continue;
                }
                stop("QEMU closed the socket");
                return;
            }
        }
        if (n < BATCH) {
            return;
        }
    }
}

/* QEMU -> vmnet */
static void from_qemu(void)
{
    struct vmpktdesc pkt;
    struct iovec iov;
    ssize_t len;
    int i, count;

    for (i = 0; i < BATCH && !stopping; i++) {
        len = recv(sock, rx_buf, max_packet + 1, 0);
        if (len < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN) {
                stop("QEMU closed the socket");
            }
            return;
        }
        if (len == 0) {
            continue;
        }
        if ((size_t)len > max_packet) {
            lost_to_net++;
            continue;
        }
        iov.iov_base = rx_buf;
        iov.iov_len = len;
        pkt.vm_pkt_size = len;
        pkt.vm_pkt_iov = &iov;
        pkt.vm_pkt_iovcnt = 1;
        pkt.vm_flags = 0;
        count = 1;
        if (vmnet_write(iface, &pkt, &count) != VMNET_SUCCESS ||
            count != 1) {
            lost_to_net++;
        }
    }
}

static void on_signal(int sig)
{
    dispatch_source_t src;

    signal(sig, SIG_IGN);
    src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, sig, 0,
                                 queue);
    if (src) {
        dispatch_source_set_event_handler(src, ^{ stop("signal"); });
        dispatch_resume(src);
    }
}

static int serve_qemu(void)
{
    union {
        LemuVmnetRequest req;
        uint8_t raw[sizeof(LemuVmnetRequest) + 1];
    } in;
    LemuVmnetRequest req;
    LemuVmnetReply rep;
    struct timeval tv = { .tv_sec = REQUEST_TIMEOUT_S };
    int one = 1, sndbuf = 128 * 1024, rcvbuf = 1024 * 1024;
    uint64_t mtu = 0, maxp = 0;
    char list[256], self[PATH_MAX], qemu[64] = "";
    const char *mode, *bad;
    dispatch_source_t proc_source;
    pid_t parent = getppid();
    xpc_object_t desc;
    vmnet_return_t st;
    bool found, oom;
    ssize_t n;
    int i;

    memset(&rep, 0, sizeof(rep));
    if (!is_dgram_socket(sock)) {
        fprintf(stderr, "lemu-vmnet: QEMU starts it with --qemu; run it "
                "without arguments to see whether it is ready\n");
        return 2;
    }
    close_other_fds();
    signal(SIGPIPE, SIG_IGN);
    setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    openlog("lemu-vmnet", LOG_PID, LOG_USER);
    self_path(self, sizeof(self));

    do {
        n = recv(sock, &in, sizeof(in), 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        fprintf(stderr, "lemu-vmnet: no request from QEMU: %s\n",
                strerror(errno));
        return 2;
    }
    if (n < 8 || in.req.magic != LEMU_VMNET_MAGIC) {
        reply(&rep, LEMU_VMNET_ERR_REQUEST,
              "lemu-vmnet: this is not a request from LEMU");
        return 2;
    }
    if (in.req.version != LEMU_VMNET_VERSION) {
        /* QEMU says what to install; this is for older QEMUs */
        reply(&rep, LEMU_VMNET_ERR_VERSION,
              "%s speaks version %d, this QEMU version %u. Install the "
              "lemu-vmnet built with this QEMU.",
              self, LEMU_VMNET_VERSION, in.req.version);
        return 2;
    }
    if (n != sizeof(req)) {
        reply(&rep, LEMU_VMNET_ERR_REQUEST,
              "lemu-vmnet: the request has the wrong size");
        return 2;
    }
    memcpy(&req, &in.req, sizeof(req));
    bad = check_request(&req);
    if (bad) {
        reply(&rep, LEMU_VMNET_ERR_REQUEST, "lemu-vmnet: %s", bad);
        return 2;
    }
    mode = mode_name(req.mode);
    if (geteuid() != 0) {
        reply(&rep, LEMU_VMNET_ERR_NOT_ROOT,
              "%s is not setuid root. Install it once with: "
              LEMU_VMNET_INSTALL " %s /usr/local/bin/", self, self);
        return 1;
    }
    if (req.mode == VMNET_BRIDGED_MODE) {
        bridgeable(list, sizeof(list), req.ifname, &found);
        if (!found) {
            reply(&rep, LEMU_VMNET_ERR_IFNAME,
                  "%s cannot be bridged. These can: %s", req.ifname,
                  list[0] ? list : "none");
            return 1;
        }
    }
    proc_name(parent, qemu, sizeof(qemu));

    /* As root */
    lemu_vmnet_make_lock_dir();
    queue = dispatch_queue_create("lemu-vmnet", DISPATCH_QUEUE_SERIAL);
    desc = make_desc(&req);
    st = start_vmnet(desc, &mtu, &maxp);
    xpc_release(desc);
    if (!drop_root()) {
        syslog(LOG_ERR, "cannot give up root, stopping");
        reply(&rep, LEMU_VMNET_ERR_VMNET,
              "lemu-vmnet could not give up root, so it stopped");
        _exit(1);
    }
    /* Root is gone for good */

    if (st != VMNET_SUCCESS || maxp == 0 || maxp > MAX_FRAME) {
        rep.vmnet_status = st;
        reply(&rep, LEMU_VMNET_ERR_VMNET,
              "vmnet could not start the %s%s%s interface: %s", mode,
              req.ifname[0] ? " " : "", req.ifname,
              st == VMNET_SUCCESS ? "bad frame size" : vmnet_error(st));
        return 1;
    }
    max_packet = maxp;
    rx_buf = malloc(max_packet + 1);
    oom = !rx_buf;
    for (i = 0; i < BATCH; i++) {
        tx_iov[i].iov_base = malloc(max_packet);
        oom |= !tx_iov[i].iov_base;
    }
    if (oom) {
        reply(&rep, LEMU_VMNET_ERR_VMNET, "lemu-vmnet: out of memory");
        return 1;
    }
    syslog(LOG_NOTICE, "start: %s%s%s for uid %d, QEMU pid %d (%s), "
           "mtu %" PRIu64, mode, req.ifname[0] ? " " : "", req.ifname,
           (int)getuid(), (int)parent, qemu, mtu);

    rep.vmnet_status = st;
    rep.mtu = mtu;
    rep.max_packet_size = max_packet;
    rep.euid = geteuid();
    if (!reply(&rep, LEMU_VMNET_OK, "started")) {
        syslog(LOG_NOTICE, "stop: QEMU did not take the answer");
        _exit(1);
    }

    /* From here on, everything runs on queue */
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
    read_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, sock, 0,
                                         queue);
    proc_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, parent,
                                         DISPATCH_PROC_EXIT, queue);
    if (!read_source || !proc_source) {
        syslog(LOG_ERR, "stop: cannot watch the socket and QEMU");
        _exit(1);
    }
    dispatch_source_set_event_handler(read_source, ^{ from_qemu(); });
    vmnet_interface_set_event_callback(iface,
        VMNET_INTERFACE_PACKETS_AVAILABLE, queue,
        ^(interface_event_t ev, xpc_object_t e) { from_vmnet(); });
    dispatch_resume(read_source);
    dispatch_source_set_event_handler(proc_source, ^{ stop("QEMU exited"); });
    dispatch_resume(proc_source);
    on_signal(SIGTERM);
    on_signal(SIGINT);
    on_signal(SIGHUP);
    dispatch_async(queue, ^{
        if (getppid() != parent) {
            stop("QEMU exited");
        }
        from_vmnet();
    });
    dispatch_main();
}

int main(int argc, char **argv)
{
    bool root;

    sanitize();
    if (argc == 2 && strcmp(argv[1], "--qemu") == 0) {
        return serve_qemu();
    }
    root = geteuid() == 0;
    if (!drop_root()) {
        return 1;
    }
    if (argc > 1) {
        fprintf(stderr, "usage: lemu-vmnet\n"
                "Shows whether lemu-vmnet is ready. QEMU runs it as "
                "\"lemu-vmnet --qemu\".\n");
        return 2;
    }
    return show_status(root);
}
