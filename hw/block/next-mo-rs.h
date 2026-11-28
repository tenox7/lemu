/*
 * NeXT magneto-optical disk: Reed-Solomon code
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef HW_BLOCK_NEXT_MO_RS_H
#define HW_BLOCK_NEXT_MO_RS_H

/* Encode 1024 bytes in place to the 1296 bytes of a sector on the disk */
void next_mo_rs_encode(uint8_t *sector);

/*
 * Decode a 1296-byte sector in place to its 1024 bytes. Returns the number
 * of bytes corrected, or -1 when the errors are too many.
 */
int next_mo_rs_decode(uint8_t *sector);

#endif
