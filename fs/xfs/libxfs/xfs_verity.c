/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Red Hat, Inc.
 */
#include "xfs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_inode.h"
#include "xfs_log_format.h"
#include "xfs_attr.h"
#include "xfs_verity.h"

/* Set a merkle tree pos in preparation for setting merkle tree attrs. */
void
xfs_merkle_key_to_disk(
	struct xfs_merkle_key	*key,
	uint64_t		pos)
{
	key->mk_pos = cpu_to_be64(pos);
}

/* Retrieve the merkle tree pos from the attr data. */
uint64_t
xfs_merkle_key_from_disk(
	const void		*attr_name,
	int			namelen)
{
	const struct xfs_merkle_key *key = attr_name;

	ASSERT(namelen == sizeof(struct xfs_merkle_key));

	return be64_to_cpu(key->mk_pos);
}

/* Return true if verity attr name is valid. */
bool
xfs_verity_namecheck(
	unsigned int		attr_flags,
	const void		*name,
	int			namelen)
{
	if (!(attr_flags & XFS_ATTR_VERITY))
		return false;

	/*
	 * Merkle tree pages are stored under u64 indexes; verity descriptor
	 * blocks are held in a named attribute.
	 */
	if (namelen != sizeof(struct xfs_merkle_key) &&
	    namelen != XFS_VERITY_DESCRIPTOR_NAME_LEN)
		return false;

	return true;
}

/*
 * Compute name hash for a verity attribute.  For merkle tree blocks, we want
 * to use the merkle tree block offset as the hash value to avoid collisions
 * between blocks unless the merkle tree becomes larger than 2^32 blocks.
 */
xfs_dahash_t
xfs_verity_hashname(
	const uint8_t		*name,
	unsigned int		namelen)
{
	if (namelen != sizeof(struct xfs_merkle_key))
		return xfs_attr_hashname(name, namelen);

	return xfs_merkle_key_from_disk(name, namelen) >> XFS_VERITY_HASH_SHIFT;
}
