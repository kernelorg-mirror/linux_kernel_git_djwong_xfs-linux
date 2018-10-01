.. SPDX-License-Identifier: CC-BY-SA-4.0

Journaling Log
--------------

    **Note**

    Only v2 log format is covered here.

The XFS journal exists on disk as a reserved extent of blocks within the
filesystem, or as a separate journal device. The journal itself can be thought
of as a series of log records; each log record contains a part of or a whole
transaction. A transaction consists of a series of log operation headers
("log items"), formatting structures, and raw data. The first operation in
a transaction establishes the transaction ID and the last operation is a
commit record. The operations recorded between the start and commit operations
represent the metadata changes made by the transaction. If the commit
operation is missing, the transaction is incomplete and cannot be recovered.

Log Records
~~~~~~~~~~~

The XFS log is split into a series of log records. Log records seem to
correspond to an in-core log buffer, which can be up to 256KiB in size. Each
record has a log sequence number, which is the same LSN recorded in the v5
metadata integrity fields.

Log sequence numbers are a 64-bit quantity consisting of two 32-bit
quantities. The upper 32 bits are the
"cycle number", which increments every time XFS
cycles through the log.  The lower 32 bits are the "block number", which
is assigned when a transaction is committed, and should correspond to the
block offset within the log.

A log record begins with the following header, which occupies 512 bytes on
disk:

.. code:: c

    typedef struct xlog_rec_header {
         __be32            h_magicno;
         __be32            h_cycle;
         __be32            h_version;
         __be32            h_len;
         __be64            h_lsn;
         __be64            h_tail_lsn;
         __le32            h_crc;
         __be32            h_prev_block;
         __be32            h_num_logops;
         __be32            h_cycle_data[XLOG_HEADER_CYCLE_SIZE / BBSIZE];
         /* new fields */
         __be32            h_fmt;
         uuid_t            h_fs_uuid;
         __be32            h_size;
    } xlog_rec_header_t;

**h\_magicno**
    The magic number of log records, 0xfeedbabe.

**h\_cycle**
    Cycle number of this log record.

**h\_version**
    Log record version, currently 2.

**h\_len**
    Length of the log record, in bytes. Must be aligned to a 64-bit boundary.

**h\_lsn**
    Log sequence number of this record.

**h\_tail\_lsn**
    Log sequence number of the first log record with uncommitted buffers.

**h\_crc**
    Checksum of the log record header, the cycle data, and the log records
    themselves.

**h\_prev\_block**
    Block number of the previous log record.

**h\_num\_logops**
    The number of log operations in this record.

**h\_cycle\_data**
    The first u32 of each log sector must contain the cycle number. Since log
    item buffers are formatted without regard to this requirement, the
    original contents of the first four bytes of each sector in the log are
    copied into the corresponding element of this array. After that, the first
    four bytes of those sectors are stamped with the cycle number. This
    process is reversed at recovery time. If there are more sectors in this
    log record than there are slots in this array, the cycle data continues
    for as many sectors are needed; each sector is formatted as type
    xlog\_rec\_ext\_header.

**h\_fmt**
    Format of the log record. This is one of the following values:

.. list-table::
   :widths: 24 56
   :header-rows: 1

   * - Format value
     - Log format

   * - XLOG\_FMT\_UNKNOWN
     - Unknown. Perhaps this log is corrupt.

   * - XLOG\_FMT\_LINUX\_LE
     - Little-endian Linux.

   * - XLOG\_FMT\_LINUX\_BE
     - Big-endian Linux.

   * - XLOG\_FMT\_IRIX\_BE
     - Big-endian Irix.

Table: Log record formats

**h\_fs\_uuid**
    Filesystem UUID.

**h\_size**
    In-core log record size. This is somewhere between 16 and 256KiB, with
    32KiB being the default.

As mentioned earlier, if this log record is longer than 256 sectors, the cycle
data overflows into the next sector(s) in the log. Each of those sectors is
formatted as follows:

.. code:: c

    typedef struct xlog_rec_ext_header {
        __be32             xh_cycle;
        __be32             xh_cycle_data[XLOG_HEADER_CYCLE_SIZE / BBSIZE];
    } xlog_rec_ext_header_t;

**xh\_cycle**
    Cycle number of this log record. Should match h\_cycle.

**xh\_cycle\_data**
    Overflow cycle data.

Log Operations
~~~~~~~~~~~~~~

Within a log record, log operations are recorded as a series consisting of an
operation header immediately followed by a data region. The operation header
has the following format:

.. code:: c

    typedef struct xlog_op_header {
         __be32                    oh_tid;
         __be32                    oh_len;
         __u8                      oh_clientid;
         __u8                      oh_flags;
         __u16                     oh_res2;
    } xlog_op_header_t;

**oh\_tid**
    Transaction ID of this operation.

**oh\_len**
    Number of bytes in the data region.

**oh\_clientid**
    The originator of this operation. This can be one of the following:

.. list-table::
   :widths: 24 56
   :header-rows: 1

   * - Client ID
     - Originator

   * - XFS\_TRANSACTION
     - Operation came from a transaction.

   * - XFS\_VOLUME
     - ???

   * - XFS\_LOG
     - ???

Table: Log Operation Client ID

**oh\_flags**
    Specifies flags associated with this operation. This can be a combination
    of the following values (though most likely only one will be set at a
    time):

.. list-table::
   :widths: 24 56
   :header-rows: 1

   * - Flag
     - Description

   * - XLOG\_START\_TRANS
     - Start a new transaction. The next operation header should describe a
       transaction header.

   * - XLOG\_COMMIT\_TRANS
     - Commit this transaction.

   * - XLOG\_CONTINUE\_TRANS
     - Continue this trans into new log record.

   * - XLOG\_WAS\_CONT\_TRANS
     - This transaction started in a previous log record.

   * - XLOG\_END\_TRANS
     - End of a continued transaction.

   * - XLOG\_UNMOUNT\_TRANS
     - Transaction to unmount a filesystem.

Table: Log Operation Flags

**oh\_res2**
    Padding.

The data region follows immediately after the operation header and is exactly
oh\_len bytes long. These payloads are in host-endian order, which means that
one cannot replay the log from an unclean XFS filesystem on a system with a
different byte order.

Log Items
~~~~~~~~~

Following are the types of log item payloads that can follow an
xlog\_op\_header. Except for buffer data and inode cores, all log items have a
magic number to distinguish themselves. Buffer data items only appear after
xfs\_buf\_log\_format items; and inode core items only appear after
xfs\_inode\_log\_format items.

.. list-table::
   :widths: 24 12 44
   :header-rows: 1

   * - Magic
     - Hexadecimal
     - Operation Type

   * - XFS\_TRANS\_HEADER\_MAGIC
     - 0x5452414e
     - Log Transaction Header

   * - XFS\_LI\_EFI
     - 0x1236
     - Extent Freeing Intent

   * - XFS\_LI\_EFD
     - 0x1237
     - Extent Freeing Done

   * - XFS\_LI\_IUNLINK
     - 0x1238
     - Unknown?

   * - XFS\_LI\_INODE
     - 0x123b
     - Inode Updates

   * - XFS\_LI\_BUF
     - 0x123c
     - Buffer Writes

   * - XFS\_LI\_DQUOT
     - 0x123d
     - Update Quota

   * - XFS\_LI\_QUOTAOFF
     - 0x123e
     - Quota Off

   * - XFS\_LI\_ICREATE
     - 0x123f
     - Inode Creation

   * - XFS\_LI\_RUI
     - 0x1240
     - Reverse Mapping Update Intent

   * - XFS\_LI\_RUD
     - 0x1241
     - Reverse Mapping Update Done

   * - XFS\_LI\_CUI
     - 0x1242
     - Reference Count Update Intent

   * - XFS\_LI\_CUD
     - 0x1243
     - Reference Count Update Done

   * - XFS\_LI\_BUI
     - 0x1244
     - File Block Mapping Update Intent

   * - XFS\_LI\_BUD
     - 0x1245
     - File Block Mapping Update Done

Table: Log Operation Magic Numbers

Note that all log items (except for transaction headers) MUST start with the
following header structure. The type and size fields are baked into each log
item header, but there is not a separately defined header.

.. code:: c

    struct xfs_log_item {
         __uint16_t                magic;
         __uint16_t                size;
    };

Transaction Headers
^^^^^^^^^^^^^^^^^^^

A transaction header is an operation payload that starts a transaction.

.. code:: c

    typedef struct xfs_trans_header {
         uint                      th_magic;
         uint                      th_type;
         __int32_t                 th_tid;
         uint                      th_num_items;
    } xfs_trans_header_t;

**th\_magic**
    The signature of a transaction header, "TRAN" (0x5452414e). Note that
    this value is in host-endian order, not big-endian like the rest of XFS.

**th\_type**
    Transaction type. This is one of the following values:

.. list-table::
   :widths: 28 52
   :header-rows: 1

   * - Type
     - Description

   * - XFS\_TRANS\_SETATTR\_NOT\_SIZE
     - Set an inode attribute that isn’t the inode’s size.

   * - XFS\_TRANS\_SETATTR\_SIZE
     - Setting the size attribute of an inode.

   * - XFS\_TRANS\_INACTIVE
     - Freeing blocks from an unlinked inode.

   * - XFS\_TRANS\_CREATE
     - Create a file.

   * - XFS\_TRANS\_CREATE\_TRUNC
     - Unused?

   * - XFS\_TRANS\_TRUNCATE\_FILE
     - Truncate a quota file.

   * - XFS\_TRANS\_REMOVE
     - Remove a file.

   * - XFS\_TRANS\_LINK
     - Link an inode into a directory.

   * - XFS\_TRANS\_RENAME
     - Rename a path.

   * - XFS\_TRANS\_MKDIR
     - Create a directory.

   * - XFS\_TRANS\_RMDIR
     - Remove a directory.

   * - XFS\_TRANS\_SYMLINK
     - Create a symbolic link.

   * - XFS\_TRANS\_SET\_DMATTRS
     - Set the DMAPI attributes of an inode.

   * - XFS\_TRANS\_GROWFS
     - Expand the filesystem.

   * - XFS\_TRANS\_STRAT\_WRITE
     - Convert an unwritten extent or delayed-allocate some blocks to
       handle a write.

   * - XFS\_TRANS\_DIOSTRAT
     - Allocate some blocks to handle a direct I/O write.

   * - XFS\_TRANS\_WRITEID
     - Update an inode’s preallocation flag.

   * - XFS\_TRANS\_ADDAFORK
     - Add an attribute fork to an inode.

   * - XFS\_TRANS\_ATTRINVAL
     - Erase the attribute fork of an inode.

   * - XFS\_TRANS\_ATRUNCATE
     - Unused?

   * - XFS\_TRANS\_ATTR\_SET
     - Set an extended attribute.

   * - XFS\_TRANS\_ATTR\_RM
     - Remove an extended attribute.

   * - XFS\_TRANS\_ATTR\_FLAG
     - Unused?

   * - XFS\_TRANS\_CLEAR\_AGI\_BUCKET
     - Clear a bad inode pointer in the AGI unlinked inode hash bucket.

   * - XFS\_TRANS\_SB\_CHANGE
     - Write the superblock to disk.

   * - XFS\_TRANS\_QM\_QUOTAOFF
     - Start disabling quotas.

   * - XFS\_TRANS\_QM\_DQALLOC
     - Allocate a disk quota structure.

   * - XFS\_TRANS\_QM\_SETQLIM
     - Adjust quota limits.

   * - XFS\_TRANS\_QM\_DQCLUSTER
     - Unused?

   * - XFS\_TRANS\_QM\_QINOCREATE
     - Create a (quota) inode with reference taken.

   * - XFS\_TRANS\_QM\_QUOTAOFF\_END
     - Finish disabling quotas.

   * - XFS\_TRANS\_FSYNC\_TS
     - Update only inode timestamps.

   * - XFS\_TRANS\_GROWFSRT\_ALLOC
     - Grow the realtime bitmap and summary data for growfs.

   * - XFS\_TRANS\_GROWFSRT\_ZERO
     - Zero space in the realtime bitmap and summary data.

   * - XFS\_TRANS\_GROWFSRT\_FREE
     - Free space in the realtime bitmap and summary data.

   * - XFS\_TRANS\_SWAPEXT
     - Swap data fork of two inodes.

   * - XFS\_TRANS\_CHECKPOINT
     - Checkpoint the log.

   * - XFS\_TRANS\_ICREATE
     - Unknown?

   * - XFS\_TRANS\_CREATE\_TMPFILE
     - Create a temporary file.

**th\_tid**
    Transaction ID.

**th\_num\_items**
    The number of operations appearing after this operation, not including the
    commit operation. In effect, this tracks the number of metadata change
    operations in this transaction.

Intent to Free an Extent
^^^^^^^^^^^^^^^^^^^^^^^^

The next two operation types work together to handle the freeing of filesystem
blocks. Naturally, the ranges of blocks to be freed can be expressed in terms
of extents:

.. code:: c

    typedef struct xfs_extent_32 {
         __uint64_t                ext_start;
         __uint32_t                ext_len;
    } __attribute__((packed)) xfs_extent_32_t;

    typedef struct xfs_extent_64 {
         __uint64_t                ext_start;
         __uint32_t                ext_len;
         __uint32_t                ext_pad;
    } xfs_extent_64_t;

**ext\_start**
    Start block of this extent.

**ext\_len**
    Length of this extent.

The "extent freeing intent" operation comes first; it tells the log that XFS
wants to free some extents.  This record is crucial for correct log recovery
because it prevents the log from replaying blocks that are subsequently freed.
If the log lacks a corresponding "extent freeing done" operation, the
recovery process will free the extents.

.. code:: c

    typedef struct xfs_efi_log_format {
         __uint16_t                efi_type;
         __uint16_t                efi_size;
         __uint32_t                efi_nextents;
         __uint64_t                efi_id;
         xfs_extent_t              efi_extents[1];
    } xfs_efi_log_format_t;

**efi\_type**
    The signature of an EFI operation, 0x1236. This value is in host-endian
    order, not big-endian like the rest of XFS.

**efi\_size**
    Size of this log item. Should be 1.

**efi\_nextents**
    Number of extents to free.

**efi\_id**
    A 64-bit number that binds the corresponding EFD log item to this EFI log
    item.

**efi\_extents**
    Variable-length array of extents to be freed. The array length is given by
    efi\_nextents. The record type will be either xfs\_extent\_64\_t or
    xfs\_extent\_32\_t; this can be determined from the log item size
    (oh\_len) and the number of extents (efi\_nextents).

Completion of Intent to Free an Extent
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The "extent freeing done" operation complements the "extent freeing
intent" operation. This second operation indicates that the block freeing
actually happened, so that log recovery needn’t try to free the blocks.
Typically, the operations to update the free space B+trees follow immediately
after the EFD.

.. code:: c

    typedef struct xfs_efd_log_format {
         __uint16_t                efd_type;
         __uint16_t                efd_size;
         __uint32_t                efd_nextents;
         __uint64_t                efd_efi_id;
         xfs_extent_t              efd_extents[1];
    } xfs_efd_log_format_t;

**efd\_type**
    The signature of an EFD operation, 0x1237. This value is in host-endian
    order, not big-endian like the rest of XFS.

**efd\_size**
    Size of this log item. Should be 1.

**efd\_nextents**
    Number of extents to free.

**efd\_id**
    A 64-bit number that binds the corresponding EFI log item to this EFD log
    item.

**efd\_extents**
    Variable-length array of extents to be freed. The array length is given by
    efd\_nextents. The record type will be either xfs\_extent\_64\_t or
    xfs\_extent\_32\_t; this can be determined from the log item size
    (oh\_len) and the number of extents (efd\_nextents).

Reverse Mapping Updates Intent
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The next two operation types work together to handle deferred reverse mapping
updates. Naturally, the mappings to be updated can be expressed in terms of
mapping extents:

.. code:: c

    struct xfs_map_extent {
         __uint64_t                me_owner;
         __uint64_t                me_startblock;
         __uint64_t                me_startoff;
         __uint32_t                me_len;
         __uint32_t                me_flags;
    };

**me\_owner**
    Owner of this reverse mapping. See the values in the section about
    `reverse mapping <#reverse-mapping-b-tree>`__ for more information.

**me\_startblock**
    Filesystem block of this mapping.

**me\_startoff**
    Logical block offset of this mapping.

**me\_len**
    The length of this mapping.

**me\_flags**
    The lower byte of this field is a type code indicating what sort of
    reverse mapping operation we want. The upper three bytes are flag bits.

.. list-table::
   :widths: 36 44
   :header-rows: 1

   * - Value
     - Description

   * - XFS\_RMAP\_EXTENT\_MAP
     - Add a reverse mapping for file data.

   * - XFS\_RMAP\_EXTENT\_MAP\_SHARED
     - Add a reverse mapping for file data for a file with shared blocks.

   * - XFS\_RMAP\_EXTENT\_UNMAP
     - Remove a reverse mapping for file data.

   * - XFS\_RMAP\_EXTENT\_UNMAP\_SHARED
     - Remove a reverse mapping for file data for a file with shared blocks.

   * - XFS\_RMAP\_EXTENT\_CONVERT
     - Convert a reverse mapping for file data between unwritten and normal.

   * - XFS\_RMAP\_EXTENT\_CONVERT\_SHARED
     - Convert a reverse mapping for file data between unwritten and normal for
       a file with shared blocks.

   * - XFS\_RMAP\_EXTENT\_ALLOC
     - Add a reverse mapping for non-file data.

   * - XFS\_RMAP\_EXTENT\_FREE
     - Remove a reverse mapping for non-file data.

Table: Reverse mapping update log intent types

.. list-table::
   :widths: 36 44
   :header-rows: 1

   * - Value
     - Description

   * - XFS\_RMAP\_EXTENT\_ATTR\_FORK
     - Extent is for the attribute fork.

   * - XFS\_RMAP\_EXTENT\_BMBT\_BLOCK
     - Extent is for a block mapping btree block.

   * - XFS\_RMAP\_EXTENT\_UNWRITTEN
     - Extent is unwritten.

Table: Reverse mapping update log intent flags

The "rmap update intent" operation comes first; it tells the log that XFS
wants to update some reverse mappings. This record is crucial for correct log
recovery because it enables us to spread a complex metadata update across
multiple transactions while ensuring that a crash midway through the complex
update will be replayed fully during log recovery.

.. code:: c

    struct xfs_rui_log_format {
         __uint16_t                rui_type;
         __uint16_t                rui_size;
         __uint32_t                rui_nextents;
         __uint64_t                rui_id;
         struct xfs_map_extent     rui_extents[1];
    };

**rui\_type**
    The signature of an RUI operation, 0x1240. This value is in host-endian
    order, not big-endian like the rest of XFS.

**rui\_size**
    Size of this log item. Should be 1.

**rui\_nextents**
    Number of reverse mappings.

**rui\_id**
    A 64-bit number that binds the corresponding RUD log item to this RUI log
    item.

**rui\_extents**
    Variable-length array of reverse mappings to update.

Completion of Reverse Mapping Updates
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The "reverse mapping update done" operation complements the "reverse
mapping update intent" operation. This second operation indicates that the
update actually happened, so that log recovery needn’t replay the update. The
RUD and the actual updates are typically found in a new transaction following
the transaction in which the RUI was logged.

.. code:: c

    struct xfs_rud_log_format {
          __uint16_t               rud_type;
          __uint16_t               rud_size;
          __uint32_t               __pad;
          __uint64_t               rud_rui_id;
    };

**rud\_type**
    The signature of an RUD operation, 0x1241. This value is in host-endian
    order, not big-endian like the rest of XFS.

**rud\_size**
    Size of this log item. Should be 1.

**rud\_rui\_id**
    A 64-bit number that binds the corresponding RUI log item to this RUD log
    item.

Reference Count Updates Intent
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The next two operation types work together to handle reference count updates.
Naturally, the ranges of extents having reference count updates can be
expressed in terms of physical extents:

.. code:: c

    struct xfs_phys_extent {
         __uint64_t                pe_startblock;
         __uint32_t                pe_len;
         __uint32_t                pe_flags;
    };

**pe\_startblock**
    Filesystem block of this extent.

**pe\_len**
    The length of this extent.

**pe\_flags**
    The lower byte of this field is a type code indicating what sort of
    reverse mapping operation we want. The upper three bytes are flag bits.

.. list-table::
   :widths: 34 46
   :header-rows: 1

   * - Value
     - Description

   * - XFS\_REFCOUNT\_EXTENT\_INCREASE
     - Increase the reference count for this extent.

   * - XFS\_REFCOUNT\_EXTENT\_DECREASE
     - Decrease the reference count for this extent.

   * - XFS\_REFCOUNT\_EXTENT\_ALLOC\_COW
     - Reserve an extent for staging copy on write.

   * - XFS\_REFCOUNT\_EXTENT\_FREE\_COW
     - Unreserve an extent for staging copy on write.

Table: Reference count update log intent types

The "reference count update intent" operation comes first; it tells the
log that XFS wants to update some reference counts. This record is crucial for
correct log recovery because it enables us to spread a complex metadata update
across multiple transactions while ensuring that a crash midway through the
complex update will be replayed fully during log recovery.

.. code:: c

    struct xfs_cui_log_format {
         __uint16_t                cui_type;
         __uint16_t                cui_size;
         __uint32_t                cui_nextents;
         __uint64_t                cui_id;
         struct xfs_map_extent     cui_extents[1];
    };

**cui\_type**
    The signature of an CUI operation, 0x1242. This value is in host-endian
    order, not big-endian like the rest of XFS.

**cui\_size**
    Size of this log item. Should be 1.

**cui\_nextents**
    Number of reference count updates.

**cui\_id**
    A 64-bit number that binds the corresponding RUD log item to this RUI log
    item.

**cui\_extents**
    Variable-length array of reference count update information.

Completion of Reference Count Updates
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The "reference count update done" operation complements the "reference
count update intent" operation. This second operation indicates that the
update actually happened, so that log recovery needn’t replay the update. The
CUD and the actual updates are typically found in a new transaction following
the transaction in which the CUI was logged.

.. code:: c

    struct xfs_cud_log_format {
          __uint16_t               cud_type;
          __uint16_t               cud_size;
          __uint32_t               __pad;
          __uint64_t               cud_cui_id;
    };

**cud\_type**
    The signature of an RUD operation, 0x1243. This value is in host-endian
    order, not big-endian like the rest of XFS.

**cud\_size**
    Size of this log item. Should be 1.

**cud\_cui\_id**
    A 64-bit number that binds the corresponding CUI log item to this CUD log
    item.

File Block Mapping Intent
^^^^^^^^^^^^^^^^^^^^^^^^^

The next two operation types work together to handle deferred file block
mapping updates. The extents to be mapped are expressed via the
xfs\_map\_extent structure discussed in the section about `reverse mapping
intents <#reverse-mapping-updates-intent>`__.

The lower byte of the me\_flags field is a type code indicating what sort of
file block mapping operation we want. The upper three bytes are flag bits.

.. list-table::
   :widths: 32 48
   :header-rows: 1

   * - Value
     - Description

   * - XFS\_BMAP\_EXTENT\_MAP
     - Add a mapping for file data.

   * - XFS\_BMAP\_EXTENT\_UNMAP
     - Remove a mapping for file data.

Table: File block mapping update log intent types

.. list-table::
   :widths: 32 48
   :header-rows: 1

   * - Value
     - Description

   * - XFS\_BMAP\_EXTENT\_ATTR\_FORK
     - Extent is for the attribute fork.

   * - XFS\_BMAP\_EXTENT\_UNWRITTEN
     - Extent is unwritten.

Table: File block mapping update log intent flags

The "file block mapping update intent" operation comes first; it tells the
log that XFS wants to map or unmap some extents in a file. This record is
crucial for correct log recovery because it enables us to spread a complex
metadata update across multiple transactions while ensuring that a crash
midway through the complex update will be replayed fully during log recovery.

.. code:: c

    struct xfs_bui_log_format {
         __uint16_t                bui_type;
         __uint16_t                bui_size;
         __uint32_t                bui_nextents;
         __uint64_t                bui_id;
         struct xfs_map_extent     bui_extents[1];
    };

**bui\_type**
    The signature of an BUI operation, 0x1244. This value is in host-endian
    order, not big-endian like the rest of XFS.

**bui\_size**
    Size of this log item. Should be 1.

**bui\_nextents**
    Number of file mappings. Should be 1.

**bui\_id**
    A 64-bit number that binds the corresponding BUD log item to this BUI log
    item.

**bui\_extents**
    Variable-length array of file block mappings to update. There should only
    be one mapping present.

Completion of File Block Mapping Updates
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The "file block mapping update done" operation complements the "file
block mapping update intent" operation. This second operation indicates that
the update actually happened, so that log recovery needn’t replay the update.
The BUD and the actual updates are typically found in a new transaction
following the transaction in which the BUI was logged.

.. code:: c

    struct xfs_bud_log_format {
          __uint16_t               bud_type;
          __uint16_t               bud_size;
          __uint32_t               __pad;
          __uint64_t               bud_bui_id;
    };

**bud\_type**
    The signature of an BUD operation, 0x1245. This value is in host-endian
    order, not big-endian like the rest of XFS.

**bud\_size**
    Size of this log item. Should be 1.

**bud\_bui\_id**
    A 64-bit number that binds the corresponding BUI log item to this BUD log
    item.

Inode Updates
^^^^^^^^^^^^^

This operation records changes to an inode record. There are several types of
inode updates, each corresponding to different parts of the inode record.
Allowing updates to proceed at a sub-inode granularity reduces contention for
the inode, since different parts of the inode can be updated simultaneously.

The actual buffer data are stored in subsequent log items.

The inode log format header is as follows:

.. code:: c

    typedef struct xfs_inode_log_format_64 {
         __uint16_t                ilf_type;
         __uint16_t                ilf_size;
         __uint32_t                ilf_fields;
         __uint16_t                ilf_asize;
         __uint16_t                ilf_dsize;
         __uint32_t                ilf_pad;
         __uint64_t                ilf_ino;
         union {
              __uint32_t           ilfu_rdev;
              uuid_t               ilfu_uuid;
         } ilf_u;
         __int64_t                 ilf_blkno;
         __int32_t                 ilf_len;
         __int32_t                 ilf_boffset;
    } xfs_inode_log_format_64_t;

**ilf\_type**
    The signature of an inode update operation, 0x123b. This value is in
    host-endian order, not big-endian like the rest of XFS.

**ilf\_size**
    Number of operations involved in this update, including this format
    operation.

**ilf\_fields**
    Specifies which parts of the inode are being updated. This can be certain
    combinations of the following:

.. list-table::
   :widths: 24 56
   :header-rows: 1

   * - Flag
     - Inode changes to log include:

   * - XFS\_ILOG\_CORE
     - The standard inode fields.

   * - XFS\_ILOG\_DDATA
     - Data fork’s local data.

   * - XFS\_ILOG\_DEXT
     - Data fork’s extent list.

   * - XFS\_ILOG\_DBROOT
     - Data fork’s B+tree root.

   * - XFS\_ILOG\_DEV
     - Data fork’s device number.

   * - XFS\_ILOG\_UUID
     - Data fork’s UUID contents.

   * - XFS\_ILOG\_ADATA
     - Attribute fork’s local data.

   * - XFS\_ILOG\_AEXT
     - Attribute fork’s extent list.

   * - XFS\_ILOG\_ABROOT
     - Attribute fork’s B+tree root.

   * - XFS\_ILOG\_DOWNER
     - Change the data fork owner on replay.

   * - XFS\_ILOG\_AOWNER
     - Change the attr fork owner on replay.

   * - XFS\_ILOG\_TIMESTAMP
     - Timestamps are dirty, but not necessarily anything else. Should never
       appear on disk.

   * - XFS\_ILOG\_NONCORE
     - ( XFS_ILOG_DDATA \| XFS_ILOG_DEXT \| XFS_ILOG_DBROOT \|
       XFS_ILOG_DEV \| XFS_ILOG_UUID \| XFS_ILOG_ADATA \| XFS_ILOG_AEXT
       \| XFS_ILOG_ABROOT \| XFS_ILOG_DOWNER \| XFS_ILOG_AOWNER )

   * - XFS\_ILOG\_DFORK
     - ( XFS_ILOG_DDATA \| XFS_ILOG_DEXT \| XFS_ILOG_DBROOT 

   * - XFS\_ILOG\_AFORK
     - ( XFS_ILOG_ADATA \| XFS_ILOG_AEXT \| XFS_ILOG_ABROOT )


   * - XFS\_ILOG\_ALL
     - ( XFS_ILOG_CORE \| XFS_ILOG_DDATA \| XFS_ILOG_DEXT \|
       XFS_ILOG_DBROOT \| XFS_ILOG_DEV \| XFS_ILOG_UUID \|
       XFS_ILOG_ADATA \| XFS_ILOG_AEXT \| XFS_ILOG_ABROOT \|
       XFS_ILOG_TIMESTAMP \| XFS_ILOG_DOWNER \| XFS_ILOG_AOWNER )

**ilf\_asize**
    Size of the attribute fork, in bytes.

**ilf\_dsize**
    Size of the data fork, in bytes.

**ilf\_ino**
    Absolute node number.

**ilfu\_rdev**
    Device number information, for a device file update.

**ilfu\_uuid**
    UUID, for a UUID update?

**ilf\_blkno**
    Block number of the inode buffer, in sectors.

**ilf\_len**
    Length of inode buffer, in sectors.

**ilf\_boffset**
    Byte offset of the inode in the buffer.

Be aware that there is a nearly identical xfs\_inode\_log\_format\_32 which
may appear on disk. It is the same as xfs\_inode\_log\_format\_64, except that
it is missing the ilf\_pad field and is 52 bytes long as opposed to 56 bytes.

Inode Data Log Item
^^^^^^^^^^^^^^^^^^^

This region contains the new contents of a part of an inode, as described in
the `previous section <#inode-updates>`__. There are no magic numbers.

If XFS\_ILOG\_CORE is set in ilf\_fields, the correpsonding data buffer must
be in the format struct xfs\_icdinode, which has the same format as the first
96 bytes of an `inode <#on-disk-inode>`__, but is recorded in host byte order.

Buffer Log Item
^^^^^^^^^^^^^^^

This operation writes parts of a buffer to disk. The regions to write are
tracked in the data map; the actual buffer data are stored in subsequent log
items.

.. code:: c

    typedef struct xfs_buf_log_format {
         unsigned short            blf_type;
         unsigned short            blf_size;
         ushort                    blf_flags;
         ushort                    blf_len;
         __int64_t                 blf_blkno;
         unsigned int              blf_map_size;
         unsigned int              blf_data_map[XFS_BLF_DATAMAP_SIZE];
    } xfs_buf_log_format_t;

**blf\_type**
    Magic number to specify a buffer log item, 0x123c.

**blf\_size**
    Number of buffer data items following this item.

**blf\_flags**
    Specifies flags associated with the buffer item. This can be any of the
    following:

.. list-table::
   :widths: 24 56
   :header-rows: 1

   * - Flag
     - Description

   * - XFS\_BLF\_INODE\_BUF
     - Inode buffer. These must be recovered before replaying items that change
       this buffer.

   * - XFS\_BLF\_CANCEL
     - Don’t recover this buffer, blocks are being freed.

   * - XFS\_BLF\_UDQUOT\_BUF
     - User quota buffer, don’t recover if there’s a subsequent quotaoff.

   * - XFS\_BLF\_PDQUOT\_BUF
     - Project quota buffer, don’t recover if there’s a subsequent quotaoff.

   * - XFS\_BLF\_GDQUOT\_BUF
     - Group quota buffer, don’t recover if there’s a subsequent quotaoff.

**blf\_len**
    Number of sectors affected by this buffer.

**blf\_blkno**
    Block number to write, in sectors.

**blf\_map\_size**
    The size of blf\_data\_map, in 32-bit words.

**blf\_data\_map**
    This variable-sized array acts as a dirty bitmap for the logged buffer.
    Each 1 bit represents a dirty region in the buffer, and each run of 1 bits
    corresponds to a subsequent log item containing the new contents of the
    buffer area. Each bit represents (blf\_len \* 512) / (blf\_map\_size \*
    NBBY) bytes.

Buffer Data Log Item
^^^^^^^^^^^^^^^^^^^^

This region contains the new contents of a part of a buffer, as described in
the `previous section <#buffer-log-item>`__. There are no magic numbers.

Update Quota File
^^^^^^^^^^^^^^^^^

This updates a block in a quota file. The buffer data must be in the next log
item.

.. code:: c

    typedef struct xfs_dq_logformat {
         __uint16_t                qlf_type;
         __uint16_t                qlf_size;
         xfs_dqid_t                qlf_id;
         __int64_t                 qlf_blkno;
         __int32_t                 qlf_len;
         __uint32_t                qlf_boffset;
    } xfs_dq_logformat_t;

**qlf\_type**
    The signature of an inode create operation, 0x123e. This value is in
    host-endian order, not big-endian like the rest of XFS.

**qlf\_size**
    Size of this log item. Should be 2.

**qlf\_id**
    The user/group/project ID to alter.

**qlf\_blkno**
    Block number of the quota buffer, in sectors.

**qlf\_len**
    Length of the quota buffer, in sectors.

**qlf\_boffset**
    Buffer offset of the quota data to update, in bytes.

Quota Update Data Log Item
^^^^^^^^^^^^^^^^^^^^^^^^^^

This region contains the new contents of a part of a buffer, as described in
the `previous section <#quota-update-data-log-item>`__. There are no magic numbers.

Disable Quota Log Item
^^^^^^^^^^^^^^^^^^^^^^

A request to disable quota controls has the following format:

.. code:: c

    typedef struct xfs_qoff_logformat {
         unsigned short            qf_type;
         unsigned short            qf_size;
         unsigned int              qf_flags;
         char                      qf_pad[12];
    } xfs_qoff_logformat_t;

**qf\_type**
    The signature of an inode create operation, 0x123d. This value is in
    host-endian order, not big-endian like the rest of XFS.

**qf\_size**
    Size of this log item. Should be 1.

**qf\_flags**
    Specifies which quotas are being turned off. Can be a combination of the
    following:

.. list-table::
   :widths: 20 60
   :header-rows: 1

   * - Flag
     - Quota type to disable

   * - XFS\_UQUOTA\_ACCT
     - User quotas.

   * - XFS\_PQUOTA\_ACCT
     - Project quotas.

   * - XFS\_GQUOTA\_ACCT
     - Group quotas.

Inode Creation Log Item
^^^^^^^^^^^^^^^^^^^^^^^

This log item is created when inodes are allocated in-core. When replaying
this item, the specified inode records will be zeroed and some of the inode
fields populated with default values.

.. code:: c

    struct xfs_icreate_log {
         __uint16_t                icl_type;
         __uint16_t                icl_size;
         __be32                    icl_ag;
         __be32                    icl_agbno;
         __be32                    icl_count;
         __be32                    icl_isize;
         __be32                    icl_length;
         __be32                    icl_gen;
    };

**icl\_type**
    The signature of an inode create operation, 0x123f. This value is in
    host-endian order, not big-endian like the rest of XFS.

**icl\_size**
    Size of this log item. Should be 1.

**icl\_ag**
    AG number of the inode chunk to create.

**icl\_agbno**
    AG block number of the inode chunk.

**icl\_count**
    Number of inodes to initialize.

**icl\_isize**
    Size of each inode, in bytes.

**icl\_length**
    Length of the extent being initialized, in blocks.

**icl\_gen**
    Inode generation number to write into the new inodes.

xfs\_logprint Example
~~~~~~~~~~~~~~~~~~~~~

Here’s an example of dumping the XFS log contents with xfs\_logprint:

::

    # xfs_logprint /dev/sda
    xfs_logprint: /dev/sda contains a mounted and writable filesystem
    xfs_logprint:
        data device: 0xfc03
        log device: 0xfc03 daddr: 900931640 length: 879816

    cycle: 48   version: 2      lsn: 48,0   tail_lsn: 47,879760
    length of Log Record: 19968 prev offset: 879808     num ops: 53
    uuid: 24afeec2-f418-46a2-a573-10091f5e200e   format: little endian linux
    h_size: 32768

This is the log record header.

::

    Oper (0): tid: 30483aec  len: 0  clientid: TRANS  flags: START

This operation indicates that we’re starting a transaction, so the next
operation should record the transaction header.

::

    Oper (1): tid: 30483aec  len: 16  clientid: TRANS  flags: none
    TRAN:    type: CHECKPOINT       tid: 30483aec       num_items: 50

This operation records a transaction header. There should be fifty operations
in this transaction and the transaction ID is 0x30483aec.

::

    Oper (2): tid: 30483aec  len: 24  clientid: TRANS  flags: none
    BUF:  #regs: 2   start blkno: 145400496 (0x8aaa2b0)  len: 8  bmap size: 1  flags: 0x2000
    Oper (3): tid: 30483aec  len: 3712  clientid: TRANS  flags: none
    BUF DATA
    ...
    Oper (4): tid: 30483aec  len: 24  clientid: TRANS  flags: none
    BUF:  #regs: 3   start blkno: 59116912 (0x3860d70)  len: 8  bmap size: 1  flags: 0x2000
    Oper (5): tid: 30483aec  len: 128  clientid: TRANS  flags: none
    BUF DATA
     0 43544241 49010000 fa347000 2c357000 3a40b200 13000000 2343c200 13000000
     8 3296d700 13000000 375deb00 13000000 8a551501 13000000 56be1601 13000000
    10 af081901 13000000 ec741c01 13000000 9e911c01 13000000 69073501 13000000
    18 4e539501 13000000  6549501 13000000 5d0e7f00 14000000 c6908200 14000000

    Oper (6): tid: 30483aec  len: 640  clientid: TRANS  flags: none
    BUF DATA
     0 7f47c800 21000000 23c0e400 21000000 2d0dfe00 21000000 e7060c01 21000000
     8 34b91801 21000000 9cca9100 22000000 26e69800 22000000 4c969900 22000000
    ...
    90 1cf69900 27000000 42f79c00 27000000  6a99e00 27000000  6a99e00 27000000
    98  6a99e00 27000000  6a99e00 27000000  6a99e00 27000000  6a99e00 27000000

Operations 4-6 describe two updates to a single dirty buffer at disk address
59,116,912. The first chunk of dirty data is 128 bytes long. Notice how the
first four bytes of the first chunk is 0x43544241? Remembering that log items
are in host byte order, reverse that to 0x41425443, which is the magic number
for the free space B+tree ordered by size.

The second chunk is 640 bytes. There are more buffer changes, so we’ll skip
ahead a few operations:

::

    Oper (19): tid: 30483aec  len: 56  clientid: TRANS  flags: none
    INODE: #regs: 2   ino: 0x63a73b4e  flags: 0x1   dsize: 40
            blkno: 1412688704  len: 16  boff: 7168
    Oper (20): tid: 30483aec  len: 96  clientid: TRANS  flags: none
    INODE CORE
    magic 0x494e mode 0100600 version 2 format 3
    nlink 1 uid 1000 gid 1000
    atime 0x5633d58d mtime 0x563a391b ctime 0x563a391b
    size 0x109dc8 nblocks 0x111 extsize 0x0 nextents 0x1b
    naextents 0x0 forkoff 0 dmevmask 0x0 dmstate 0x0
    flags 0x0 gen 0x389071be

This is an update to the core of inode 0x63a73b4e. There were similar inode
core updates after this, so we’ll skip ahead a bit:

::

    Oper (32): tid: 30483aec  len: 56  clientid: TRANS  flags: none
    INODE: #regs: 3   ino: 0x4bde428  flags: 0x5   dsize: 16
            blkno: 79553568  len: 16  boff: 4096
    Oper (33): tid: 30483aec  len: 96  clientid: TRANS  flags: none
    INODE CORE
    magic 0x494e mode 0100644 version 2 format 2
    nlink 1 uid 1000 gid 1000
    atime 0x563a3924 mtime 0x563a3931 ctime 0x563a3931
    size 0x1210 nblocks 0x2 extsize 0x0 nextents 0x1
    naextents 0x0 forkoff 0 dmevmask 0x0 dmstate 0x0
    flags 0x0 gen 0x2829c6f9
    Oper (34): tid: 30483aec  len: 16  clientid: TRANS  flags: none
    EXTENTS inode data

This inode update changes both the core and also the data fork. Since we’re
changing the block map, it’s unsurprising that one of the subsequent
operations is an EFI:

::

    Oper (37): tid: 30483aec  len: 32  clientid: TRANS  flags: none
    EFI:  #regs: 1    num_extents: 1  id: 0xffff8801147b5c20
    (s: 0x720daf, l: 1)
    \----------------------------------------------------------------------------
    Oper (38): tid: 30483aec  len: 32  clientid: TRANS  flags: none
    EFD:  #regs: 1    num_extents: 1  id: 0xffff8801147b5c20
    \----------------------------------------------------------------------------
    Oper (39): tid: 30483aec  len: 24  clientid: TRANS  flags: none
    BUF:  #regs: 2   start blkno: 8 (0x8)  len: 8  bmap size: 1  flags: 0x2800
    Oper (40): tid: 30483aec  len: 128  clientid: TRANS  flags: none
    AGF Buffer: XAGF
    ver: 1  seq#: 0  len: 56308224
    root BNO: 18174905  CNT: 18175030
    level BNO: 2  CNT: 2
    1st: 41  last: 46  cnt: 6  freeblks: 35790503  longest: 19343245
    \----------------------------------------------------------------------------
    Oper (41): tid: 30483aec  len: 24  clientid: TRANS  flags: none
    BUF:  #regs: 3   start blkno: 145398760 (0x8aa9be8)  len: 8  bmap size: 1  flags: 0x2000
    Oper (42): tid: 30483aec  len: 128  clientid: TRANS  flags: none
    BUF DATA
    Oper (43): tid: 30483aec  len: 128  clientid: TRANS  flags: none
    BUF DATA
    \----------------------------------------------------------------------------
    Oper (44): tid: 30483aec  len: 24  clientid: TRANS  flags: none
    BUF:  #regs: 3   start blkno: 145400224 (0x8aaa1a0)  len: 8  bmap size: 1  flags: 0x2000
    Oper (45): tid: 30483aec  len: 128  clientid: TRANS  flags: none
    BUF DATA
    Oper (46): tid: 30483aec  len: 3584  clientid: TRANS  flags: none
    BUF DATA
    \----------------------------------------------------------------------------
    Oper (47): tid: 30483aec  len: 24  clientid: TRANS  flags: none
    BUF:  #regs: 3   start blkno: 59066216 (0x3854768)  len: 8  bmap size: 1  flags: 0x2000
    Oper (48): tid: 30483aec  len: 128  clientid: TRANS  flags: none
    BUF DATA
    Oper (49): tid: 30483aec  len: 768  clientid: TRANS  flags: none
    BUF DATA

Here we see an EFI, followed by an EFD, followed by updates to the AGF and the
free space B+trees. Most probably, we just unmapped a few blocks from a file.

::

    Oper (50): tid: 30483aec  len: 56  clientid: TRANS  flags: none
    INODE: #regs: 2   ino: 0x3906f20  flags: 0x1   dsize: 16
            blkno: 59797280  len: 16  boff: 0
    Oper (51): tid: 30483aec  len: 96  clientid: TRANS  flags: none
    INODE CORE
    magic 0x494e mode 0100644 version 2 format 2
    nlink 1 uid 1000 gid 1000
    atime 0x563a3938 mtime 0x563a3938 ctime 0x563a3938
    size 0x0 nblocks 0x0 extsize 0x0 nextents 0x0
    naextents 0x0 forkoff 0 dmevmask 0x0 dmstate 0x0
    flags 0x0 gen 0x35ed661
    \----------------------------------------------------------------------------
    Oper (52): tid: 30483aec  len: 0  clientid: TRANS  flags: COMMIT

One more inode core update and this transaction commits.
