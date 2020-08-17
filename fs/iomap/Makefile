# SPDX-License-Identifier: GPL-2.0-or-later
#
# Copyright (c) 2019 Oracle.
# All Rights Reserved.
#
ccflags-$(CONFIG_KASAN) += -Wno-error
ccflags-y += -g \
-Werror \
-femit-struct-debug-detailed=any \
-Wunused-but-set-variable \
-Wuninitialized \
-Wno-pointer-sign \
-Wall \
-Wextra \
-Wno-unused-parameter \
-fstack-usage \
-Wno-sign-compare \
-Wno-ignored-qualifiers \
-Wno-error=unused-but-set-variable \
-Wno-error=format=

UBSAN_SANITIZE := y

ccflags-y += -I $(srctree)/$(src)		# needed for trace events

obj-$(CONFIG_FS_IOMAP)		+= iomap.o

iomap-y				+= trace.o \
				   apply.o \
				   buffered-io.o \
				   direct-io.o \
				   fiemap.o \
				   seek.o
iomap-$(CONFIG_SWAP)		+= swapfile.o
