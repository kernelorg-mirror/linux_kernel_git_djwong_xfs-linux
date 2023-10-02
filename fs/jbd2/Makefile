# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the linux journaling routines.
#

obj-$(CONFIG_JBD2) += jbd2.o

ifeq ($(CONFIG_JBD2_GCOV_ALL),y)
GCOV_PROFILE := y
endif

ifeq ($(CONFIG_JBD2_GCOV_NONE),y)
GCOV_PROFILE := n
endif

jbd2-objs := transaction.o commit.o recovery.o checkpoint.o revoke.o journal.o
