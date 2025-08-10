# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the FUSE filesystem.
#

# Needed for trace events
ccflags-y = -I$(src)

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
#ccflags-y += -Wmissing-field-initializers		# fuse_ctl_fill_super
#ccflags-y += -Wtype-limits				# breaks BUILD_BUG_ON
ccflags-y += $(call cc-option, -Wmaybe-uninitialized)
ccflags-y += $(call cc-option, -Wno-error=maybe-uninitialized)	# control.c in gcc 14.2
ccflags-y += $(call cc-option, -Wunused-macros)	# dont care about cpp

# W=3
#ccflags-y += -Wbad-function-cast			# pagecache
ccflags-y += -Wcast-align				# char* to struct* casts on riscv
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

obj-$(CONFIG_FUSE_FS) += fuse.o
obj-$(CONFIG_CUSE) += cuse.o
obj-$(CONFIG_VIRTIO_FS) += virtiofs.o

fuse-y := trace.o	# put trace.o first so we see ftrace errors sooner
fuse-y += dev.o dir.o file.o inode.o control.o xattr.o acl.o readdir.o ioctl.o
fuse-y += iomode.o
fuse-$(CONFIG_FUSE_DAX) += dax.o
fuse-$(CONFIG_FUSE_PASSTHROUGH) += passthrough.o backing.o
fuse-$(CONFIG_SYSCTL) += sysctl.o
fuse-$(CONFIG_FUSE_IO_URING) += dev_uring.o

virtiofs-y := virtio_fs.o
