# SPDX-License-Identifier: GPL-2.0-or-later
#
# Copyright (c) 2019 Oracle.
# All Rights Reserved.
#

# Generate more debugging info
ccflags-y += -femit-struct-debug-detailed=any
ccflags-y += -fstack-usage

# Base warnings(?)
ccflags-y += -Wall
ccflags-y += -Wno-pointer-sign		# dont trip on atomic64 cmpxchg
ccflags-y += -Wuninitialized
ccflags-y += -Wno-error=format=		# dont trip on trace_printk

# Fedora wants to modernize C...
ccflags-y += -Wimplicit-int
ccflags-y += -Wimplicit-function-declaration
ccflags-y += -Wint-conversion
ccflags-y += -Wstrict-prototypes
ccflags-y += -Wold-style-definition

# W=e
ifneq ($(CONFIG_KASAN),y)
ccflags-y += -Werror
endif

# W=1
ccflags-y += -Wextra -Wunused -Wno-unused-parameter
ccflags-y += -Wmissing-declarations
ccflags-y += -Wmissing-format-attribute
ccflags-y += -Wmissing-prototypes
ccflags-y += -Wold-style-definition
ccflags-y += -Wmissing-include-dirs
ccflags-y += $(call cc-option, -Wunused-but-set-variable)
ccflags-y += $(call cc-option, -Wunused-const-variable)
ccflags-y += $(call cc-option, -Wpacked-not-aligned)
ccflags-y += $(call cc-option, -Wstringop-truncation)
# The following turn off the warnings enabled by -Wextra
ccflags-y += -Wno-missing-field-initializers
ccflags-y += -Wno-sign-compare
ccflags-y += -Wno-type-limits
ccflags-y += -Wno-shift-negative-value

# W=2
ccflags-y += -Wdisabled-optimization
ccflags-y += -Wshadow					# wait_event shadows intentionally
ccflags-y += $(call cc-option, -Wlogical-op)
ccflags-y += -Wmissing-field-initializers
#ccflags-y += -Wtype-limits				# breaks BUILD_BUG_ON
ccflags-y += $(call cc-option, -Wmaybe-uninitialized)
ccflags-y += $(call cc-option, -Wunused-macros)	# dont care about cpp

UBSAN_SANITIZE := y

ccflags-y += -I $(srctree)/$(src)		# needed for trace events

obj-$(CONFIG_FS_IOMAP)		+= iomap.o

iomap-y				+= trace.o \
				   iter.o
iomap-$(CONFIG_BLOCK)		+= buffered-io.o \
				   direct-io.o \
				   fiemap.o \
				   seek.o
iomap-$(CONFIG_SWAP)		+= swapfile.o
