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

# Derived from scripts/Makefile.extrawarn on 2023-09-12 (v6.6-rc1)

# W=e
ifneq ($(CONFIG_KASAN),y)
ccflags-y += -Werror
endif

# W=1
ccflags-y += -Wextra -Wunused -Wno-unused-parameter
ccflags-y += -Wmissing-declarations
ccflags-y += $(call cc-option, -Wrestrict)
ccflags-y += -Wmissing-format-attribute
ccflags-y += -Wmissing-prototypes
ccflags-y += -Wold-style-definition
ccflags-y += -Wmissing-include-dirs
ccflags-y += $(call cc-option, -Wunused-but-set-variable)
ccflags-y += $(call cc-option, -Wunused-const-variable)
ccflags-y += $(call cc-option, -Wpacked-not-aligned)
ccflags-y += $(call cc-option, -Wformat-overflow)
ccflags-y += $(call cc-option, -Wformat-truncation)
ccflags-y += $(call cc-option, -Wstringop-overflow)
ccflags-y += $(call cc-option, -Wstringop-truncation)

ccflags-y += -Wundef

# W=2
ccflags-y += -Wdisabled-optimization
#ccflags-y += -Wshadow					# wait_event shadows intentionally
ccflags-y += $(call cc-option, -Wlogical-op)
ccflags-y += -Wmissing-field-initializers
#ccflags-y += -Wtype-limits				# breaks BUILD_BUG_ON
ccflags-y += $(call cc-option, -Wmaybe-uninitialized)
ccflags-y += $(call cc-option, -Wunused-macros)	# dont care about cpp

# W=3
#ccflags-y += -Wbad-function-cast			# pagecache
#ccflags-y += -Wcast-align				# char* to struct* casts on riscv
#ccflags-y += -Wcast-qual				# rhashtable
#ccflags-y += -Wconversion				# everywhere
#ccflags-y += -Wpacked					# this whines about unnecesary __packed??
#ccflags-y += -Wpadded					# maple tree
#ccflags-y += -Wpointer-arith				# container_of
#ccflags-y += -Wredundant-decls				# ftrace
#ccflags-y += -Wsign-compare				# percpu
#ccflags-y += -Wswitch-default				# everywhere
ccflags-y += $(call cc-option, -Wpacked-bitfield-compat)

UBSAN_SANITIZE := y

ccflags-y += -I $(src)		# needed for trace events

ifeq ($(CONFIG_IOMAP_GCOV_ALL),y)
GCOV_PROFILE := y
endif

ifeq ($(CONFIG_IOMAP_GCOV_NONE),y)
GCOV_PROFILE := n
endif

obj-$(CONFIG_FS_IOMAP)		+= iomap.o

iomap-y				+= trace.o \
				   iter.o \
				   buffered-io.o
iomap-$(CONFIG_BLOCK)		+= direct-io.o \
				   ioend.o \
				   fiemap.o \
				   seek.o \
				   bio.o
iomap-$(CONFIG_SWAP)		+= swapfile.o
