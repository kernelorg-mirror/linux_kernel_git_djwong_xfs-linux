Real-Time Reverse-Mapping B+tree
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

    **Note**

    This data structure is under construction! Details may change.

If the reverse-mapping B+tree and real-time storage device features are
enabled, the real-time device has its own reverse block-mapping B+tree.

As mentioned in the chapter about `reconstruction <#metadata-reconstruction>`__, this
data structure is another piece of the puzzle necessary to reconstruct the
data or attribute fork of a file from reverse-mapping records; we can also use
it to double-check allocations to ensure that we are not accidentally
cross-linking blocks, which can cause severe damage to the filesystem.

This B+tree is only present if the XFS\_SB\_FEAT\_RO\_COMPAT\_RMAPBT feature
is enabled and a real time device is present. The feature requires a version 5
filesystem.

The real-time reverse mapping B+tree is rooted in an inode’s data fork; the
inode number is given by the sb\_rrmapino field in the superblock. The B+tree
blocks themselves are stored in the regular filesystem. The structures used
for an inode’s B+tree root are:

.. code:: c

    struct xfs_rtrmap_root {
         __be16                     bb_level;
         __be16                     bb_numrecs;
    };

-  On disk, the B+tree node starts with the xfs\_rtrmap\_root header followed
   by an array of xfs\_rtrmap\_key values and then an array of
   xfs\_rtrmap\_ptr\_t values. The size of both arrays is specified by the
   header’s bb\_numrecs value.

-  The root node in the inode can only contain up to 10 key/pointer pairs for
   a standard 512 byte inode before a new level of nodes is added between the
   root and the leaves. di\_forkoff should always be zero, because there are
   no extended attributes.

Each record in the real-time reverse-mapping B+tree has the following
structure:

.. code:: c

    struct xfs_rtrmap_rec {
         __be64                     rm_startblock;
         __be64                     rm_blockcount;
         __be64                     rm_owner;
         __be64                     rm_fork:1;
         __be64                     rm_bmbt:1;
         __be64                     rm_unwritten:1;
         __be64                     rm_unused:7;
         __be64                     rm_offset:54;
    };

**rm\_startblock**
    Real-time device block number of this record.

**rm\_blockcount**
    The length of this extent, in real-time blocks.

**rm\_owner**
    A 64-bit number describing the owner of this extent. This must be an inode
    number, because the real-time device is for file data only.

**rm\_fork**
    If rm\_owner describes an inode, this can be 1 if this record is for an
    attribute fork. This value will always be zero for real-time extents.

**rm\_bmbt**
    If rm\_owner describes an inode, this can be 1 to signify that this record
    is for a block map B+tree block. In this case, rm\_offset has no meaning.
    This value will always be zero for real-time extents.

**rm\_unwritten**
    A flag indicating that the extent is unwritten. This corresponds to the
    flag in the `extent record <#data-extents>`__ format which means
    XFS\_EXT\_UNWRITTEN.

**rm\_offset**
    The 54-bit logical file block offset, if rm\_owner describes an inode.

    **Note**

    The single-bit flag values rm\_unwritten, rm\_fork, and rm\_bmbt are
    packed into the larger fields in the C structure definition.

The key has the following structure:

.. code:: c

    struct xfs_rtrmap_key {
         __be64                     rm_startblock;
         __be64                     rm_owner;
         __be64                     rm_fork:1;
         __be64                     rm_bmbt:1;
         __be64                     rm_reserved:1;
         __be64                     rm_unused:7;
         __be64                     rm_offset:54;
    };

-  All block numbers are 64-bit real-time device block numbers.

-  The bb\_magic value is "MAPR" (0x4d415052).

-  The xfs\_btree\_lblock\_t header is used for intermediate B+tree node as
   well as the leaves.

-  Each pointer is associated with two keys. The first of these is the "low
   key", which is the key of the smallest record accessible through the
   pointer. This low key has the same meaning as the key in all other btrees.
   The second key is the high key, which is the maximum of the largest key
   that can be used to access a given record underneath the pointer. Recall
   that each record in the real-time reverse mapping b+tree describes an
   interval of physical blocks mapped to an interval of logical file block
   offsets; therefore, it makes sense that a range of keys can be used to find
   to a record.

xfs\_db rtrmapbt Example
""""""""""""""""""""""""

This example shows a real-time reverse-mapping B+tree from a freshly populated
root filesystem:

::

    xfs_db> sb 0
    xfs_db> addr rrmapino
    xfs_db> p
    core.magic = 0x494e
    core.mode = 0100000
    core.version = 3
    core.format = 5 (rtrmapbt)
    ...
    u3.rtrmapbt.level = 3
    u3.rtrmapbt.numrecs = 1
    u3.rtrmapbt.keys[1] = [startblock,owner,offset,attrfork,bmbtblock,startblock_hi,
                   owner_hi,offset_hi,attrfork_hi,bmbtblock_hi]
        1:[1,132,1,0,0,1705337,133,54431,0,0]
    u3.rtrmapbt.ptrs[1] = 1:671
    xfs_db> addr u3.rtrmapbt.ptrs[1]
    xfs_db> p
    magic = 0x4d415052
    level = 2
    numrecs = 8
    leftsib = null
    rightsib = null
    bno = 5368
    lsn = 0x400000000
    uuid = 98bbde42-67e7-46a5-a73e-d64a76b1b5ce
    owner = 131
    crc = 0x2560d199 (correct)
    keys[1-8] = [startblock,owner,offset,attrfork,bmbtblock,startblock_hi,owner_hi,
             offset_hi,attrfork_hi,bmbtblock_hi]
        1:[1,132,1,0,0,17749,132,17749,0,0]
        2:[17751,132,17751,0,0,35499,132,35499,0,0]
        3:[35501,132,35501,0,0,53249,132,53249,0,0]
        4:[53251,132,53251,0,0,1658473,133,7567,0,0]
        5:[1658475,133,7569,0,0,1667473,133,16567,0,0]
        6:[1667475,133,16569,0,0,1685223,133,34317,0,0]
        7:[1685225,133,34319,0,0,1694223,133,43317,0,0]
        8:[1694225,133,43319,0,0,1705337,133,54431,0,0]
    ptrs[1-8] = 1:134 2:238 3:345 4:453 5:795 6:563 7:670 8:780

We arbitrarily pick pointer 7 (twice) to traverse downwards:

::

    xfs_db> addr ptrs[7]
    xfs_db> p
    magic = 0x4d415052
    level = 1
    numrecs = 36
    leftsib = 563
    rightsib = 780
    bno = 5360
    lsn = 0
    uuid = 98bbde42-67e7-46a5-a73e-d64a76b1b5ce
    owner = 131
    crc = 0x6807761d (correct)
    keys[1-36] = [startblock,owner,offset,attrfork,bmbtblock,startblock_hi,owner_hi,
              offset_hi,attrfork_hi,bmbtblock_hi]
        1:[1685225,133,34319,0,0,1685473,133,34567,0,0]
        2:[1685475,133,34569,0,0,1685723,133,34817,0,0]
        3:[1685725,133,34819,0,0,1685973,133,35067,0,0]
        ...
        34:[1693475,133,42569,0,0,1693723,133,42817,0,0]
        35:[1693725,133,42819,0,0,1693973,133,43067,0,0]
        36:[1693975,133,43069,0,0,1694223,133,43317,0,0]
    ptrs[1-36] = 1:669 2:672 3:674...34:722 35:723 36:725
    xfs_db> addr ptrs[7]
    xfs_db> p
    magic = 0x4d415052
    level = 0
    numrecs = 125
    leftsib = 678
    rightsib = 681
    bno = 5440
    lsn = 0
    uuid = 98bbde42-67e7-46a5-a73e-d64a76b1b5ce
    owner = 131
    crc = 0xefce34d4 (correct)
    recs[1-125] = [startblock,blockcount,owner,offset,extentflag,attrfork,bmbtblock]
        1:[1686725,1,133,35819,0,0,0]
        2:[1686727,1,133,35821,0,0,0]
        3:[1686729,1,133,35823,0,0,0]
        ...
        123:[1686969,1,133,36063,0,0,0]
        124:[1686971,1,133,36065,0,0,0]
        125:[1686973,1,133,36067,0,0,0]

Several interesting things pop out here. The first record shows that inode 133
has mapped real-time block 1,686,725 at offset 35,819. We confirm this by
looking at the block map for that inode:

::

    xfs_db> inode 133
    xfs_db> p core.realtime
    core.realtime = 1
    xfs_db> bmap
    data offset 35817 startblock 1686723 (1/638147) count 1 flag 0
    data offset 35819 startblock 1686725 (1/638149) count 1 flag 0
    data offset 35821 startblock 1686727 (1/638151) count 1 flag 0

Notice that inode 133 has the real-time flag set, which means that its data
blocks are all allocated from the real-time device.
