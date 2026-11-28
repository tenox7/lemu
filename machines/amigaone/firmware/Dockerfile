# Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
# SPDX-License-Identifier: GPL-2.0-or-later
#
# PowerPC cross compiler for the 2010 era U-Boot of the AmigaOne and
# the Sam boards.
FROM debian:bookworm
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
        --no-install-recommends gcc-powerpc-linux-gnu \
        binutils-powerpc-linux-gnu make gcc libc6-dev bison flex bc perl \
        python3 && rm -rf /var/lib/apt/lists/*
