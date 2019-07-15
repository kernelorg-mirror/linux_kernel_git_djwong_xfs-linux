# SPDX-License-Identifier: GPL-2.0-or-newer
#
# Copyright (c) 2019 Oracle.
# All Rights Reserved.
#

ccflags-y += -I $(srctree)/$(src)/..

obj-$(CONFIG_FS_IOMAP)		+= iomap.o

iomap-y				+= \
					apply.o \
					direct-io.o \
					buffered-io.o \
					fiemap.o \
					seek.o

iomap-$(CONFIG_MIGRATION)	+= migrate.o
iomap-$(CONFIG_SWAP)		+= swapfile.o
