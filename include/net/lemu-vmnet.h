/*
 * lemu-vmnet: how QEMU and the lemu-vmnet helper talk (macOS)
 *
 * Only root may start a vmnet interface. When QEMU is not root, the
 * vmnet netdevs start the setuid helper lemu-vmnet (lemu-vmnet.c) with
 * one end of an AF_UNIX SOCK_DGRAM socket pair on fd LEMU_VMNET_FD.
 * QEMU sends one LemuVmnetRequest, the helper answers with one
 * LemuVmnetReply. After a reply with status LEMU_VMNET_OK, each
 * datagram, both ways, is one Ethernet frame.
 *
 * This header is also built into the helper, which does not use the
 * rest of QEMU: keep it to standard C and POSIX.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef NET_LEMU_VMNET_H
#define NET_LEMU_VMNET_H

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

/* Where QEMU looks for the helper, unless the netdev says helper=PATH */
#define LEMU_VMNET_PATH "/usr/local/bin/lemu-vmnet"
#define LEMU_VMNET_INSTALL \
    "sudo install -o root -g admin -m 4750"

/* The socket's fd in the helper */
#define LEMU_VMNET_FD 3

#define LEMU_VMNET_MAGIC 0x4e4d564cu /* "LVMN" */

/*
 * Bump it when the request or the reply changes. The helper is
 * installed once and outlives QEMU builds, so a new version means that
 * everyone has to install the helper again.
 */
#define LEMU_VMNET_VERSION 1

/*
 * Each VM that uses vmnet locks a file named after its MAC address
 * here, so that a second VM with the same MAC address does not start.
 * lemu-vmnet makes the folder: root's, mode 1777 like /tmp.
 */
#define LEMU_VMNET_LOCK_DIR "/var/run/lemu-vmnet"

#define LEMU_VMNET_IFNAMSIZ 16

/* LemuVmnetRequest.flags */
#define LEMU_VMNET_ISOLATED (1u << 0) /* vmnet_enable_isolation_key */
#define LEMU_VMNET_RANGE    (1u << 1) /* the 3 IPv4 fields are set */
#define LEMU_VMNET_NAT66    (1u << 2) /* nat66_prefix is set (shared) */
#define LEMU_VMNET_NET_UUID (1u << 3) /* net_uuid is set (host) */
#define LEMU_VMNET_FLAGS    0xfu

typedef struct LemuVmnetRequest {
    uint32_t magic;
    uint32_t version;
    uint32_t mode;              /* VMNET_HOST/SHARED/BRIDGED_MODE */
    uint32_t flags;
    char ifname[LEMU_VMNET_IFNAMSIZ]; /* bridged: host interface, NUL */
    uint8_t start_address[4];   /* IPv4, network byte order */
    uint8_t end_address[4];
    uint8_t subnet_mask[4];
    uint8_t nat66_prefix[16];   /* IPv6, network byte order */
    uint8_t net_uuid[16];
} LemuVmnetRequest;

/* LemuVmnetReply.status */
enum {
    LEMU_VMNET_OK,
    LEMU_VMNET_ERR_REQUEST,     /* the request is not valid */
    LEMU_VMNET_ERR_VERSION,     /* the helper speaks another version */
    LEMU_VMNET_ERR_NOT_ROOT,    /* the helper is not setuid root */
    LEMU_VMNET_ERR_IFNAME,      /* the interface cannot be bridged */
    LEMU_VMNET_ERR_VMNET,       /* vmnet failed, see vmnet_status */
};

typedef struct LemuVmnetReply {
    /* The first 4 fields stay the same in every version */
    uint32_t magic;
    uint32_t version;           /* the helper's LEMU_VMNET_VERSION */
    uint32_t status;
    char message[256];          /* for the user, NUL-terminated */
    uint32_t vmnet_status;      /* vmnet_return_t */
    uint32_t mtu;
    uint32_t max_packet_size;   /* the longest frame, both ways */
    uint32_t euid;              /* the helper's user after the start */
} LemuVmnetReply;

/*
 * Make LEMU_VMNET_LOCK_DIR. Only root can, so lemu-vmnet and QEMU
 * running as root call it.
 */
static inline void lemu_vmnet_make_lock_dir(void)
{
    struct stat st;
    int fd;

    if (mkdir(LEMU_VMNET_LOCK_DIR, 0755) < 0 && errno != EEXIST) {
        return;
    }
    fd = open(LEMU_VMNET_LOCK_DIR,
              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == 0 &&
        (st.st_mode & 07777) != 01777) {
        if (fchmod(fd, 01777) < 0) {
            /* the MAC check then works only for root's VMs */
        }
    }
    close(fd);
}

#endif
