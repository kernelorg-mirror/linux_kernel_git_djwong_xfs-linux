# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the linux ext4-filesystem routines.
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
#ccflags-y += -Wshadow					# resize/xattr/ialloc/mballoc.c are a mess
ccflags-y += $(call cc-option, -Wlogical-op)
ccflags-y += -Wmissing-field-initializers
#ccflags-y += -Wtype-limits				# breaks BUILD_BUG_ON
ccflags-y += $(call cc-option, -Wmaybe-uninitialized)
#ccflags-y += $(call cc-option, -Wunused-macros)	# dont care about cpp
ccflags-y += -Wno-error=maybe-uninitialized		# extents.c is a mess

UBSAN_SANITIZE := y

ccflags-$(CONFIG_KASAN) += -Wno-error

obj-$(CONFIG_EXT4_FS) += ext4.o

ext4-y	:= balloc.o bitmap.o block_validity.o dir.o ext4_jbd2.o extents.o \
		extents_status.o file.o fsmap.o fsync.o hash.o ialloc.o \
		indirect.o inline.o inode.o ioctl.o mballoc.o migrate.o \
		mmp.o move_extent.o namei.o page-io.o readpage.o resize.o \
		super.o symlink.o sysfs.o xattr.o xattr_hurd.o xattr_trusted.o \
		xattr_user.o fast_commit.o orphan.o

ext4-$(CONFIG_EXT4_FS_POSIX_ACL)	+= acl.o
ext4-$(CONFIG_EXT4_FS_SECURITY)		+= xattr_security.o
ext4-inode-test-objs			+= inode-test.o
obj-$(CONFIG_EXT4_KUNIT_TESTS)		+= ext4-inode-test.o
ext4-$(CONFIG_FS_VERITY)		+= verity.o
ext4-$(CONFIG_FS_ENCRYPTION)		+= crypto.o
