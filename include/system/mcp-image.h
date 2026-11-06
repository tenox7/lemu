/*
 * Disc and floppy images made from host files, and files read back from
 * a floppy, for the MCP "media" tool
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SYSTEM_MCP_IMAGE_H
#define SYSTEM_MCP_IMAGE_H

#include "qapi/qapi-builtin-types.h"

/* What an image holds, or what was copied out of one */
typedef struct McpImageInfo {
    int files;
    int folders;
    uint64_t bytes;
    GString *renamed;           /* "GUEST.NAM (host name)", for each */
    int n_renamed;
    GString *skipped;           /* what was left out, and why */
} McpImageInfo;

void mcp_image_info_init(McpImageInfo *info);
void mcp_image_info_free(McpImageInfo *info);

/*
 * Write an image to the empty file @fd from the host files and folders
 * in @files; a folder brings what is in it.  @disc: ISO 9660 with Rock
 * Ridge, for a CD-ROM drive; else a 1.44 MB FAT12 floppy, blank if
 * @files is empty.
 */
bool mcp_image_make(int fd, bool disc, strList *files, McpImageInfo *info,
                    Error **errp);

/* Reads @len bytes at @offset of a medium */
typedef bool McpImageRead(void *opaque, uint64_t offset, void *buf,
                          size_t len, Error **errp);

/*
 * Copy the files on a FAT12 or FAT16 medium of @size bytes into the
 * host folder @dir, which is made if needed
 */
bool mcp_image_copy_out(McpImageRead *read, void *opaque, uint64_t size,
                        const char *dir, McpImageInfo *info, Error **errp);

#endif
