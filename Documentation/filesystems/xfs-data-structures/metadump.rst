.. SPDX-License-Identifier: CC-BY-SA-4.0

Metadata Dumps
--------------

The xfs\_metadump and xfs\_mdrestore tools are used to create a sparse
snapshot of a live file system and to restore that snapshot onto a block
device for debugging purposes. Only the metadata are captured in the snapshot,
and the metadata blocks may be obscured for privacy reasons.

A metadump file starts with a xfs\_metablock that records the addresses of the
blocks that follow. Following that are the metadata blocks captured from the
filesystem. The first block following the first superblock must be the
superblock from AG 0. If the metadump has more blocks than can be pointed to
by the xfs\_metablock.mb\_daddr area, the sequence of xfs\_metablock followed
by metadata blocks is repeated.

**Metadata Dump Format.**

.. code:: c

    struct xfs_metablock {
        __be32      mb_magic;
        __be16      mb_count;
        uint8_t     mb_blocklog;
        uint8_t     mb_reserved;
        __be64      mb_daddr[];
    };

**mb\_magic**
    The magic number, "XFSM" (0x5846534d).

**mb\_count**
    Number of blocks indexed by this record. This value must not exceed (1 <<
    mb\_blocklog) - sizeof(struct xfs\_metablock).

**mb\_blocklog**
    The log size of a metadump block. This size of a metadump block 512 bytes,
    so this value should be 9.

**mb\_reserved**
    Reserved. Should be zero.

**mb\_daddr**
    An array of disk addresses. Each of the mb\_count blocks (of size (1 <<
    mb\_blocklog) following the xfs\_metablock should be written back to the
    address pointed to by the corresponding mb\_daddr entry.

Dump Obfuscation
~~~~~~~~~~~~~~~~

Unless explicitly disabled, the xfs\_metadump tool obfuscates empty block
space and naming information to avoid leaking sensitive information into the
metadump file. xfs\_metadump does not copy user data blocks.

The obfuscation policy is as follows:

-  File and extended attribute names are both considered "names".

-  Names longer than 8 characters are totally rewritten with a name that
   matches the hash of the old name.

-  Names between 5 and 8 characters are partially rewritten to match the hash
   of the old name.

-  Names shorter than 5 characters are not obscured at all.

-  Names that cross a block boundary are not obscured at all.

-  Extended attribute values are zeroed.

-  Empty parts of metadata blocks are zeroed.
