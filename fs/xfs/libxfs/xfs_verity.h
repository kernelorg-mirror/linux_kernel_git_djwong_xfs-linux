/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2022 Red Hat, Inc.
 */
#ifndef __XFS_VERITY_H__
#define __XFS_VERITY_H__

void xfs_merkle_key_to_disk(struct xfs_merkle_key *key, uint64_t pos);
uint64_t xfs_merkle_key_from_disk(const void *attr_name, int namelen);
bool xfs_verity_namecheck(unsigned int attr_flags, const void *name,
		int namelen);

#endif	/* __XFS_VERITY_H__ */
