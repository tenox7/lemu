/*
 * vmnet-common.m - network client wrapper for Apple vmnet.framework
 *
 * Copyright(c) 2022 Vladislav Yaroshchuk <vladislav.yaroshchuk@jetbrains.com>
 * Copyright(c) 2021 Phillip Tennen <phillip@axleos.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/log.h"
#include "qemu/cutils.h"
#include "qapi/qapi-types-net.h"
#include "vmnet_int.h"
#include "clients.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "system/runstate.h"
#include "system/system.h"
#include "net/eth.h"
#include "net/lemu-vmnet.h"

#include <arpa/inet.h>
#include <mach-o/dyld.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <vmnet/vmnet.h>
#include <dispatch/dispatch.h>


static void vmnet_send_completed(NetClientState *nc, ssize_t len);


const char *vmnet_status_map_str(vmnet_return_t status)
{
    switch (status) {
    case VMNET_SUCCESS:
        return "success";
    case VMNET_FAILURE:
        return "general failure (possibly not enough privileges)";
    case VMNET_MEM_FAILURE:
        return "memory allocation failure";
    case VMNET_INVALID_ARGUMENT:
        return "invalid argument specified";
    case VMNET_SETUP_INCOMPLETE:
        return "interface setup is not complete";
    case VMNET_INVALID_ACCESS:
        return "invalid access, permission denied";
    case VMNET_PACKET_TOO_BIG:
        return "packet size is larger than MTU";
    case VMNET_BUFFER_EXHAUSTED:
        return "buffers exhausted in kernel";
    case VMNET_TOO_MANY_PACKETS:
        return "packet count exceeds limit";
    case VMNET_SHARING_SERVICE_BUSY:
        return "conflict, sharing service is in use";
    default:
        return "unknown vmnet error";
    }
}


/*
 * lemu-vmnet
 *
 * Only root may start a vmnet interface. When QEMU is not root, the
 * setuid helper lemu-vmnet (lemu-vmnet.c) starts it, gives up root and
 * passes the frames over a socket pair: see include/net/lemu-vmnet.h.
 */

static const char *vmnet_kind(uint64_t mode)
{
    switch (mode) {
    case VMNET_HOST_MODE:
        return "host";
    case VMNET_SHARED_MODE:
        return "shared";
    case VMNET_BRIDGED_MODE:
        return "bridged";
    default:
        return "unknown";
    }
}

/* nat66-prefix: an IPv6 address, maybe followed by /64 */
static bool vmnet_parse_nat66(const char *str, uint8_t *prefix)
{
    g_autofree char *s = g_strdup(str);
    char *slash = strchr(s, '/');

    if (slash) {
        if (strcmp(slash, "/64") != 0) {
            return false;
        }
        *slash = '\0';
    }
    return inet_pton(AF_INET6, s, prefix) == 1;
}

/* Turn the interface description into a request for the helper */
static bool vmnet_helper_request(xpc_object_t if_desc,
                                 LemuVmnetRequest *req, Error **errp)
{
    __block const char *bad = NULL, *bad_key = NULL;
    __block int range = 0;

    memset(req, 0, sizeof(*req));
    req->magic = LEMU_VMNET_MAGIC;
    req->version = LEMU_VMNET_VERSION;
    xpc_dictionary_apply(if_desc, ^bool(const char *key, xpc_object_t v) {
        const char *str = xpc_get_type(v) == XPC_TYPE_STRING ?
                          xpc_string_get_string_ptr(v) : "";

        if (!strcmp(key, vmnet_operation_mode_key)) {
            req->mode = xpc_uint64_get_value(v);
        } else if (!strcmp(key, vmnet_enable_isolation_key)) {
            if (xpc_bool_get_value(v)) {
                req->flags |= LEMU_VMNET_ISOLATED;
            }
        } else if (!strcmp(key, vmnet_allocate_mac_address_key)) {
            /* lemu-vmnet sets it to false, like vmnet_if_create() */
        } else if (!strcmp(key, vmnet_shared_interface_name_key)) {
            if (strlen(str) >= sizeof(req->ifname)) {
                bad = "ifname is too long";
            }
            pstrcpy(req->ifname, sizeof(req->ifname), str);
        } else if (!strcmp(key, vmnet_start_address_key)) {
            range |= 1;
            if (inet_pton(AF_INET, str, req->start_address) != 1) {
                bad = "start-address must be an IPv4 address";
            }
        } else if (!strcmp(key, vmnet_end_address_key)) {
            range |= 2;
            if (inet_pton(AF_INET, str, req->end_address) != 1) {
                bad = "end-address must be an IPv4 address";
            }
        } else if (!strcmp(key, vmnet_subnet_mask_key)) {
            range |= 4;
            if (inet_pton(AF_INET, str, req->subnet_mask) != 1) {
                bad = "subnet-mask must be an IPv4 address";
            }
        } else if (!strcmp(key, vmnet_nat66_prefix_key)) {
            req->flags |= LEMU_VMNET_NAT66;
            if (!vmnet_parse_nat66(str, req->nat66_prefix)) {
                bad = "nat66-prefix must be an IPv6 prefix like fd00:1::";
            }
        } else if (!strcmp(key, vmnet_network_identifier_key) &&
                   xpc_get_type(v) == XPC_TYPE_UUID) {
            req->flags |= LEMU_VMNET_NET_UUID;
            memcpy(req->net_uuid, xpc_uuid_get_bytes(v),
                   sizeof(req->net_uuid));
        } else {
            bad_key = key;
        }
        return !bad && !bad_key;
    });
    if (range == 7) {
        req->flags |= LEMU_VMNET_RANGE;
    } else if (range && !bad) {
        bad = "start-address, end-address and subnet-mask go together";
    }
    if (bad_key) {
        error_setg(errp, "vmnet: lemu-vmnet cannot pass on %s; "
                   "run QEMU as root to use it", bad_key);
        return false;
    }
    if (bad) {
        error_setg(errp, "vmnet: %s", bad);
        return false;
    }
    return true;
}

/* The lemu-vmnet built next to this QEMU binary, for install hints */
static char *vmnet_helper_built(void)
{
    char path[PATH_MAX], real[PATH_MAX];
    uint32_t len = sizeof(path);
    g_autofree char *dir = NULL;
    char *built;

    if (_NSGetExecutablePath(path, &len) != 0 || !realpath(path, real)) {
        return g_strdup("lemu-vmnet");
    }
    dir = g_path_get_dirname(real);
    built = g_build_filename(dir, "lemu-vmnet", NULL);
    if (!g_file_test(built, G_FILE_TEST_IS_EXECUTABLE)) {
        g_free(built);
        return g_strdup("lemu-vmnet");
    }
    return built;
}

/* The helper exits when its socket closes; give it 2 s, then kill it */
static void vmnet_helper_reap(pid_t pid)
{
    int i, status;

    for (i = 0; i < 200; i++) {
        pid_t r = waitpid(pid, &status, WNOHANG);

        if (r == pid || (r < 0 && errno != EINTR)) {
            return;
        }
        g_usleep(10 * 1000);
    }
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        /* again */
    }
}

static pid_t vmnet_helper_spawn(const char *path, int fd, Error **errp)
{
    char *const argv[] = { (char *)"lemu-vmnet", (char *)"--qemu", NULL };
    char *const envp[] = { NULL };
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    sigset_t none, dflt;
    pid_t pid;
    int err;

    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    if (fcntl(2, F_GETFD) >= 0) {
        posix_spawn_file_actions_addinherit_np(&fa, 2);
    } else {
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    }
    if (fd == LEMU_VMNET_FD) {
        posix_spawn_file_actions_addinherit_np(&fa, fd);
    } else {
        posix_spawn_file_actions_adddup2(&fa, fd, LEMU_VMNET_FD);
    }
    posix_spawnattr_init(&attr);
    sigemptyset(&none);
    sigfillset(&dflt);
    sigdelset(&dflt, SIGKILL);
    sigdelset(&dflt, SIGSTOP);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &dflt);
    /* Its own process group: ^C and ^Z in QEMU's terminal are not for it */
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT |
                             POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF |
                             POSIX_SPAWN_SETPGROUP);
    err = posix_spawn(&pid, path, &fa, &attr, argv, envp);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);
    if (err) {
        error_setg(errp, "vmnet: cannot run %s: %s%s", path, strerror(err),
                   err == EACCES ? " (only group admin may run it)" : "");
        return -1;
    }
    return pid;
}

static int vmnet_helper_start(VmnetState *s, xpc_object_t if_desc,
                              const char *path, Error **errp)
{
    LemuVmnetRequest req;
    LemuVmnetReply rep;
    g_autofree char *built = NULL;
    int sndbuf = 128 * 1024, rcvbuf = 4 * 1024 * 1024, one = 1;
    struct pollfd pfd;
    struct stat st;
    int sv[2], r;
    ssize_t n;
    pid_t pid;

    if (!vmnet_helper_request(if_desc, &req, errp)) {
        return -1;
    }
    if (stat(path, &st) < 0 || st.st_uid != 0 || !(st.st_mode & S_ISUID)) {
        bool missing = access(path, F_OK) < 0;

        built = vmnet_helper_built();
        error_setg(errp, "vmnet-%s needs root, or the lemu-vmnet helper, "
                   "and %s %s. Install it once with: " LEMU_VMNET_INSTALL
                   " %s /usr/local/bin/", s->kind, path,
                   missing ? "is missing" : "is not setuid root", built);
        return -1;
    }

    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) {
        error_setg_errno(errp, errno, "vmnet: socketpair");
        return -1;
    }
    qemu_set_cloexec(sv[0]);
    qemu_set_cloexec(sv[1]);
    /* The defaults hold one frame and 2 frames */
    setsockopt(sv[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    while (setsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &rcvbuf,
                      sizeof(rcvbuf)) < 0 && rcvbuf > 256 * 1024) {
        rcvbuf /= 2;
    }
    pid = vmnet_helper_spawn(path, sv[1], errp);
    close(sv[1]);
    if (pid < 0) {
        close(sv[0]);
        return -1;
    }

    if (send(sv[0], &req, sizeof(req), 0) != sizeof(req)) {
        error_setg_errno(errp, errno, "vmnet: cannot talk to %s", path);
        goto fail;
    }
    pfd.fd = sv[0];
    pfd.events = POLLIN;
    do {
        r = poll(&pfd, 1, 30 * 1000);
    } while (r < 0 && errno == EINTR);
    memset(&rep, 0, sizeof(rep));
    n = r > 0 ? recv(sv[0], &rep, sizeof(rep), 0) : -1;
    if (n < (ssize_t)offsetof(LemuVmnetReply, vmnet_status) ||
        rep.magic != LEMU_VMNET_MAGIC) {
        error_setg(errp, "vmnet: %s gave no answer; run it without "
                   "arguments to see why", path);
        goto fail;
    }
    rep.message[sizeof(rep.message) - 1] = '\0';
    if (rep.status == LEMU_VMNET_ERR_VERSION) {
        built = vmnet_helper_built();
        error_setg(errp, "vmnet: %s speaks version %u of its protocol, this "
                   "QEMU version %d. Install the lemu-vmnet built with this "
                   "QEMU: " LEMU_VMNET_INSTALL " %s /usr/local/bin/", path,
                   rep.version, LEMU_VMNET_VERSION, built);
        goto fail;
    }
    if (rep.status != LEMU_VMNET_OK) {
        error_setg(errp, "vmnet-%s: %s", s->kind, rep.message);
        goto fail;
    }
    if (n != sizeof(rep) || rep.version != LEMU_VMNET_VERSION ||
        !rep.max_packet_size || rep.max_packet_size > 65535) {
        built = vmnet_helper_built();
        error_setg(errp, "vmnet: %s gave an answer that this QEMU does not "
                   "know. Install the lemu-vmnet built with this QEMU: "
                   LEMU_VMNET_INSTALL " %s /usr/local/bin/", path, built);
        goto fail;
    }

    s->helper = true;
    s->helper_fd = sv[0];
    s->helper_pid = pid;
    s->mtu = rep.mtu;
    s->max_packet_size = rep.max_packet_size;
    s->helper_buf = g_malloc(s->max_packet_size);
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    return 0;

fail:
    close(sv[0]);
    vmnet_helper_reap(pid);
    return -1;
}

static void vmnet_helper_read(void *opaque);

static void vmnet_helper_read_poll(VmnetState *s, bool on)
{
    qemu_set_fd_handler(s->helper_fd, on ? vmnet_helper_read : NULL, NULL,
                        s);
}

static void vmnet_helper_dead(VmnetState *s, int err)
{
    NetClientState *peer = s->nc.peer;
    g_autofree char *info = NULL;
    char *pid;

    if (s->helper_dead) {
        return;
    }
    s->helper_dead = true;
    vmnet_helper_read_poll(s, false);
    error_report("vmnet: the lemu-vmnet helper stopped (%s); the VM's "
                 "network cable is now unplugged", strerror(err));

    /* Make sure that it stops, and leave no zombie */
    close(s->helper_fd);
    s->helper_fd = -1;
    vmnet_helper_reap(s->helper_pid);
    s->helper_pid = 0;

    /* vm-info shows it */
    info = g_strdup(s->nc.info_str);
    pid = strstr(info, "lemu-vmnet pid");
    if (pid) {
        *pid = '\0';
        qemu_set_info_str(&s->nc, "%slemu-vmnet stopped", info);
    }

    s->nc.link_down = true;
    if (peer) {
        peer->link_down = true;
        if (peer->info->link_status_changed) {
            peer->info->link_status_changed(peer);
        }
    }
}

static void vmnet_helper_sent(NetClientState *nc, ssize_t len)
{
    VmnetState *s = DO_UPCAST(VmnetState, nc, nc);

    if (!s->helper_dead) {
        vmnet_helper_read_poll(s, true);
    }
}

/* Frames from the helper to the guest */
static void vmnet_helper_read(void *opaque)
{
    VmnetState *s = opaque;
    uint8_t min_pkt[ETH_ZLEN];
    size_t min_pktsz, size;
    const uint8_t *pkt;
    ssize_t len;
    int i;

    for (i = 0; i < VMNET_PACKETS_LIMIT; i++) {
        len = recv(s->helper_fd, s->helper_buf, s->max_packet_size, 0);
        if (len < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN) {
                vmnet_helper_dead(s, errno);
            }
            return;
        }
        if (len == 0) {
            continue;
        }
        pkt = s->helper_buf;
        size = len;
        if (net_peer_needs_padding(&s->nc)) {
            min_pktsz = sizeof(min_pkt);
            if (eth_pad_short_frame(min_pkt, &min_pktsz, pkt, size)) {
                pkt = min_pkt;
                size = min_pktsz;
            }
        }
        if (qemu_send_packet_async(&s->nc, pkt, size,
                                   vmnet_helper_sent) == 0) {
            /* The guest is busy: wait for vmnet_helper_sent() */
            vmnet_helper_read_poll(s, false);
            return;
        }
    }
}

/* A frame from the guest to the helper */
static ssize_t vmnet_helper_receive(VmnetState *s, const uint8_t *buf,
                                    size_t size)
{
    ssize_t n;

    if (s->helper_dead) {
        return size;
    }
    do {
        n = send(s->helper_fd, buf, size, 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0 && errno != ENOBUFS && errno != EAGAIN) {
        vmnet_helper_dead(s, errno);
    }
    /* When the helper is behind, the frame is lost, as on a busy LAN */
    return size;
}


/*
 * Two VMs with one MAC address on one network break each other's
 * network. An agent that starts a copy of a VM with the VM's own run
 * script does just that. So each vmnet VM locks a file named after its
 * MAC address in LEMU_VMNET_LOCK_DIR, and a second VM with the same
 * address stops before its guest runs.
 */
static void vmnet_claim_mac(Notifier *n, void *data)
{
    VmnetState *s = container_of(n, VmnetState, machine_done);
    NetClientState *peer = s->nc.peer;
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
    g_autofree char *mac = NULL;
    g_autofree char *path = NULL;
    const uint8_t *a;
    struct stat st;
    int fd;

    if (s->mac_locked || !peer ||
        peer->info->type != NET_CLIENT_DRIVER_NIC) {
        return;
    }
    a = qemu_get_nic(peer)->conf->macaddr.a;
    mac = qemu_mac_strdup_printf(a);
    path = g_strdup_printf("%s/%s-%02x%02x%02x%02x%02x%02x",
                           LEMU_VMNET_LOCK_DIR, s->kind,
                           a[0], a[1], a[2], a[3], a[4], a[5]);
    fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
              0666);
    if (fd >= 0) {
        /* Every user's VMs lock the same file, whatever the umask */
        if (fchmod(fd, 0666) < 0) {
            /* then only this user's VMs can check it */
        }
    } else if (errno == EEXIST) {
        fd = open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    }
    /* Never touch what someone else put there, such as a hard link */
    if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
        st.st_nlink != 1) {
        warn_report("vmnet: cannot check whether MAC address %s is in "
                    "use: %s: %s", mac, path,
                    fd < 0 ? strerror(errno) : "not a plain file");
        if (fd >= 0) {
            close(fd);
        }
        return;
    }
    if (fcntl(fd, F_SETLK, &fl) < 0) {
        if (errno != EAGAIN && errno != EACCES) {
            warn_report("vmnet: cannot check whether MAC address %s is in "
                        "use: %s: %s", mac, path, strerror(errno));
            close(fd);
            return;
        }
        fl.l_type = F_WRLCK;
        if (fcntl(fd, F_GETLK, &fl) < 0 || fl.l_type == F_UNLCK) {
            fl.l_pid = 0;
        }
        error_report("vmnet: MAC address %s is already in use by another "
                     "LEMU VM (pid %d). Two VMs with one MAC address break "
                     "each other's network. Give this VM its own, for "
                     "example mac=52:54:00:%02x:%02x:%02x", mac,
                     (int)fl.l_pid, g_random_int_range(0, 256),
                     g_random_int_range(0, 256), g_random_int_range(0, 256));
        exit(1);
    }
    s->mac_lock_fd = fd;
    s->mac_locked = true;
}

/* The NIC and its MAC address are known once the machine is made */
static void vmnet_watch_mac(VmnetState *s)
{
    s->machine_done.notify = vmnet_claim_mac;
    s->watching_mac = true;
    qemu_add_machine_init_done_notifier(&s->machine_done);
}

static void vmnet_unwatch_mac(VmnetState *s)
{
    if (s->watching_mac) {
        qemu_remove_machine_init_done_notifier(&s->machine_done);
        s->watching_mac = false;
    }
    if (s->mac_locked) {
        close(s->mac_lock_fd);
        s->mac_locked = false;
    }
}


/**
 * Write packets from QEMU to vmnet interface.
 *
 * vmnet.framework supports iov, but writing more than
 * one iov into vmnet interface fails with
 * 'VMNET_INVALID_ARGUMENT'. Collecting provided iovs into
 * one and passing it to vmnet works fine. That's the
 * reason why receive_iov() left unimplemented. But it still
 * works with good performance having .receive() only.
 */
ssize_t vmnet_receive_common(NetClientState *nc,
                             const uint8_t *buf,
                             size_t size)
{
    VmnetState *s = DO_UPCAST(VmnetState, nc, nc);
    struct vmpktdesc packet;
    struct iovec iov;
    int pkt_cnt;
    vmnet_return_t if_status;

    if (size > s->max_packet_size) {
        warn_report("vmnet: packet is too big, %zu > %" PRIu64,
            size,
            s->max_packet_size);
        return -1;
    }

    if (s->helper) {
        return vmnet_helper_receive(s, buf, size);
    }

    iov.iov_base = (char *) buf;
    iov.iov_len = size;

    packet.vm_pkt_iovcnt = 1;
    packet.vm_flags = 0;
    packet.vm_pkt_size = size;
    packet.vm_pkt_iov = &iov;
    pkt_cnt = 1;

    if_status = vmnet_write(s->vmnet_if, &packet, &pkt_cnt);
    if (if_status != VMNET_SUCCESS) {
        error_report("vmnet: write error: %s",
                     vmnet_status_map_str(if_status));
        return -1;
    }

    if (pkt_cnt) {
        return size;
    }
    return 0;
}


/**
 * Read packets from vmnet interface and write them
 * to temporary buffers in VmnetState.
 *
 * Returns read packets number (may be 0) on success,
 * -1 on error
 */
static int vmnet_read_packets(VmnetState *s)
{
    assert(s->packets_send_current_pos == s->packets_send_end_pos);

    struct vmpktdesc *packets = s->packets_buf;
    vmnet_return_t status;
    int i;

    /* Read as many packets as present */
    s->packets_send_current_pos = 0;
    s->packets_send_end_pos = VMNET_PACKETS_LIMIT;
    for (i = 0; i < s->packets_send_end_pos; ++i) {
        packets[i].vm_pkt_size = s->max_packet_size;
        packets[i].vm_pkt_iovcnt = 1;
        packets[i].vm_flags = 0;
    }

    status = vmnet_read(s->vmnet_if, packets, &s->packets_send_end_pos);
    if (status != VMNET_SUCCESS) {
        error_printf("vmnet: read failed: %s\n",
                     vmnet_status_map_str(status));
        s->packets_send_current_pos = 0;
        s->packets_send_end_pos = 0;
        return -1;
    }
    return s->packets_send_end_pos;
}


/**
 * Write packets from temporary buffers in VmnetState
 * to QEMU.
 */
static void vmnet_write_packets_to_qemu(VmnetState *s)
{
    uint8_t *pkt;
    size_t pktsz;
    uint8_t min_pkt[ETH_ZLEN];
    size_t min_pktsz;
    ssize_t size;

    while (s->packets_send_current_pos < s->packets_send_end_pos) {
        pkt = s->iov_buf[s->packets_send_current_pos].iov_base;
        pktsz = s->packets_buf[s->packets_send_current_pos].vm_pkt_size;

        if (net_peer_needs_padding(&s->nc)) {
            min_pktsz = sizeof(min_pkt);

            if (eth_pad_short_frame(min_pkt, &min_pktsz, pkt, pktsz)) {
                pkt = min_pkt;
                pktsz = min_pktsz;
            }
        }

        size = qemu_send_packet_async(&s->nc, pkt, pktsz,
                                      vmnet_send_completed);

        if (size == 0) {
            /* QEMU is not ready to consume more packets -
             * stop and wait for completion callback call */
            return;
        }
        ++s->packets_send_current_pos;
    }
}


/**
 * Bottom half callback that transfers packets from vmnet interface
 * to QEMU.
 *
 * The process of transferring packets is three-staged:
 * 1. Handle vmnet event;
 * 2. Read packets from vmnet interface into temporary buffer;
 * 3. Write packets from temporary buffer to QEMU.
 *
 * QEMU may suspend this process on the last stage, returning 0 from
 * qemu_send_packet_async function. If this happens, we should
 * respectfully wait until it is ready to consume more packets,
 * write left ones in temporary buffer and only after this
 * continue reading more packets from vmnet interface.
 *
 * Packets to be transferred are stored into packets_buf,
 * in the window [packets_send_current_pos..packets_send_end_pos)
 * including current_pos, excluding end_pos.
 *
 * Thus, if QEMU is not ready, buffer is not read and
 * packets_send_current_pos < packets_send_end_pos.
 */
static void vmnet_send_bh(void *opaque)
{
    NetClientState *nc = (NetClientState *) opaque;
    VmnetState *s = DO_UPCAST(VmnetState, nc, nc);

    /*
     * Do nothing if QEMU is not ready - wait
     * for completion callback invocation
     */
    if (s->packets_send_current_pos < s->packets_send_end_pos) {
        return;
    }

    /* Read packets from vmnet interface */
    if (vmnet_read_packets(s) > 0) {
        /* Send them to QEMU */
        vmnet_write_packets_to_qemu(s);
    }
}


/**
 * Completion callback to be invoked by QEMU when it becomes
 * ready to consume more packets.
 */
static void vmnet_send_completed(NetClientState *nc, ssize_t len)
{
    VmnetState *s = DO_UPCAST(VmnetState, nc, nc);

    /* Callback is invoked eq queued packet is sent */
    ++s->packets_send_current_pos;

    /* Complete sending packets left in VmnetState buffers */
    vmnet_write_packets_to_qemu(s);

    /* And read new ones from vmnet if VmnetState buffer is ready */
    if (s->packets_send_current_pos < s->packets_send_end_pos) {
        qemu_bh_schedule(s->send_bh);
    }
}


static void vmnet_bufs_init(VmnetState *s)
{
    struct vmpktdesc *packets = s->packets_buf;
    struct iovec *iov = s->iov_buf;
    int i;

    for (i = 0; i < VMNET_PACKETS_LIMIT; ++i) {
        iov[i].iov_len = s->max_packet_size;
        iov[i].iov_base = g_malloc0(iov[i].iov_len);
        packets[i].vm_pkt_iov = iov + i;
    }
}

/**
 * Called on state change to un-register/re-register handlers
 */
static void vmnet_vm_state_change_cb(void *opaque, bool running, RunState state)
{
    VmnetState *s = opaque;

    if (s->helper) {
        if (!s->helper_dead) {
            vmnet_helper_read_poll(s, running);
        }
        return;
    }

    if (running) {
        vmnet_interface_set_event_callback(
            s->vmnet_if,
            VMNET_INTERFACE_PACKETS_AVAILABLE,
            s->if_queue,
            ^(interface_event_t event_id, xpc_object_t event) {
                assert(event_id == VMNET_INTERFACE_PACKETS_AVAILABLE);
                /*
                 * This function is being called from a non qemu thread, so
                 * we only schedule a BH, and do the rest of the io completion
                 * handling from vmnet_send_bh() which runs in a qemu context.
                 */
                qemu_bh_schedule(s->send_bh);
            });
    } else {
        vmnet_interface_set_event_callback(
            s->vmnet_if,
            VMNET_INTERFACE_PACKETS_AVAILABLE,
            NULL,
            NULL);
    }
}

int vmnet_if_create(NetClientState *nc,
                    xpc_object_t if_desc,
                    const char *helper,
                    Error **errp)
{
    VmnetState *s = DO_UPCAST(VmnetState, nc, nc);
    const char *ifname = xpc_dictionary_get_string(
        if_desc, vmnet_shared_interface_name_key);
    g_autoptr(GString) info = g_string_new(NULL);
    dispatch_semaphore_t if_created_sem;
    __block vmnet_return_t if_status;

    s->kind = vmnet_kind(xpc_dictionary_get_uint64(if_desc,
                                                   vmnet_operation_mode_key));
    if (ifname) {
        g_string_append_printf(info, "ifname=%s", ifname);
    }

    if (geteuid() != 0) {
        if (vmnet_helper_start(s, if_desc, helper ? helper : LEMU_VMNET_PATH,
                               errp) < 0) {
            return -1;
        }
        g_string_append_printf(info, "%slemu-vmnet pid %d",
                               info->len ? "," : "", (int)s->helper_pid);
        qemu_set_info_str(nc, "%s", info->str);
        vmnet_helper_read_poll(s, true);
        s->change = qemu_add_vm_change_state_handler(vmnet_vm_state_change_cb,
                                                     s);
        vmnet_watch_mac(s);
        return 0;
    }

    lemu_vmnet_make_lock_dir();
    if_created_sem = dispatch_semaphore_create(0);
    s->if_queue = dispatch_queue_create(
        "org.qemu.vmnet.if_queue",
        DISPATCH_QUEUE_SERIAL
    );

    xpc_dictionary_set_bool(
        if_desc,
        vmnet_allocate_mac_address_key,
        false
    );

#ifdef DEBUG
    qemu_log("vmnet.start.interface_desc:\n");
    xpc_dictionary_apply(if_desc,
                         ^bool(const char *k, xpc_object_t v) {
                             char *desc = xpc_copy_description(v);
                             qemu_log("  %s=%s\n", k, desc);
                             free(desc);
                             return true;
                         });
#endif /* DEBUG */

    s->vmnet_if = vmnet_start_interface(
        if_desc,
        s->if_queue,
        ^(vmnet_return_t status, xpc_object_t interface_param) {
            if_status = status;
            if (status != VMNET_SUCCESS || !interface_param) {
                dispatch_semaphore_signal(if_created_sem);
                return;
            }

#ifdef DEBUG
            qemu_log("vmnet.start.interface_param:\n");
            xpc_dictionary_apply(interface_param,
                                 ^bool(const char *k, xpc_object_t v) {
                                     char *desc = xpc_copy_description(v);
                                     qemu_log("  %s=%s\n", k, desc);
                                     free(desc);
                                     return true;
                                 });
#endif /* DEBUG */

            s->mtu = xpc_dictionary_get_uint64(
                interface_param,
                vmnet_mtu_key);
            s->max_packet_size = xpc_dictionary_get_uint64(
                interface_param,
                vmnet_max_packet_size_key);

            dispatch_semaphore_signal(if_created_sem);
        });

    if (s->vmnet_if == NULL) {
        dispatch_release(s->if_queue);
        dispatch_release(if_created_sem);
        error_setg(errp,
                   "unable to create interface with requested params");
        return -1;
    }

    dispatch_semaphore_wait(if_created_sem, DISPATCH_TIME_FOREVER);
    dispatch_release(if_created_sem);

    if (if_status != VMNET_SUCCESS) {
        dispatch_release(s->if_queue);
        error_setg(errp,
                   "cannot create vmnet interface: %s",
                   vmnet_status_map_str(if_status));
        return -1;
    }

    s->send_bh = aio_bh_new(qemu_get_aio_context(), vmnet_send_bh, nc);
    vmnet_bufs_init(s);

    s->packets_send_current_pos = 0;
    s->packets_send_end_pos = 0;

    vmnet_vm_state_change_cb(s, 1, RUN_STATE_RUNNING);

    s->change = qemu_add_vm_change_state_handler(vmnet_vm_state_change_cb, s);

    if (info->len) {
        qemu_set_info_str(nc, "%s", info->str);
    }
    vmnet_watch_mac(s);
    return 0;
}


void vmnet_cleanup_common(NetClientState *nc)
{
    VmnetState *s = DO_UPCAST(VmnetState, nc, nc);
    dispatch_semaphore_t if_stopped_sem;

    vmnet_unwatch_mac(s);

    if (s->helper) {
        qemu_del_vm_change_state_handler(s->change);
        if (s->helper_fd >= 0) {
            qemu_set_fd_handler(s->helper_fd, NULL, NULL, NULL);
            close(s->helper_fd);
        }
        if (s->helper_pid > 0) {
            vmnet_helper_reap(s->helper_pid);
        }
        qemu_purge_queued_packets(nc);
        g_free(s->helper_buf);
        s->helper = false;
        return;
    }

    if (s->vmnet_if == NULL) {
        return;
    }

    vmnet_vm_state_change_cb(s, 0, RUN_STATE_SHUTDOWN);
    qemu_del_vm_change_state_handler(s->change);
    if_stopped_sem = dispatch_semaphore_create(0);
    vmnet_stop_interface(
        s->vmnet_if,
        s->if_queue,
        ^(vmnet_return_t status) {
            assert(status == VMNET_SUCCESS);
            dispatch_semaphore_signal(if_stopped_sem);
        });
    dispatch_semaphore_wait(if_stopped_sem, DISPATCH_TIME_FOREVER);

    qemu_purge_queued_packets(nc);

    qemu_bh_delete(s->send_bh);
    dispatch_release(if_stopped_sem);
    dispatch_release(s->if_queue);

    for (int i = 0; i < VMNET_PACKETS_LIMIT; ++i) {
        g_free(s->iov_buf[i].iov_base);
    }
}
