.. SPDX-License-Identifier: CC-BY-SA-4.0

Variable Length Record B+trees
------------------------------

Directories and extended attributes are implemented as a simple key-value
record store inside the blocks pointed to by the data or attribute fork of a
file. Blocks referenced by either data structure are block offsets of an inode
fork, not physical blocks.

Directory and attribute data are stored as a linear array of variable-length
records in the low blocks of a fork. Both data types share the property that
record keys and record values are both arbitrary and unique sequences of
bytes. See the respective sections about `directories <#directories>`__ or
`attributes <#extended-attributes>`__ for more information about the exact
record formats.

The dir/attr b+tree (or "dabtree"), if present, computes a hash of the record
key to produce the b+tree key, and b+tree keys are used to index the fork
block in which the record may be found. Unlike the fixed-length b+trees, the
variable length b+trees can index the same key multiple times. B+tree
keypointers and records both take this format:

::

    +---------+--------------+
    | hashval | before_block |
    +---------+--------------+

The "before block" is the block offset in the inode fork of the block in which
we can find the record whose hashed key is "hashval". The hash function is as
follows:

.. code:: c

    #define rol32(x,y)             (((x) << (y)) | ((x) >> (32 - (y))))

    xfs_dahash_t
    xfs_da_hashname(const uint8_t *name, int namelen)
    {
        xfs_dahash_t hash;

        /*
         * Do four characters at a time as long as we can.
         */
        for (hash = 0; namelen >= 4; namelen -= 4, name += 4)
            hash = (name[0] << 21) ^ (name[1] << 14) ^ (name[2] << 7) ^
                   (name[3] << 0) ^ rol32(hash, 7 * 4);

        /*
         * Now do the rest of the characters.
         */
        switch (namelen) {
        case 3:
            return (name[0] << 14) ^ (name[1] << 7) ^ (name[2] << 0) ^
                   rol32(hash, 7 * 3);
        case 2:
            return (name[0] << 7) ^ (name[1] << 0) ^ rol32(hash, 7 * 2);
        case 1:
            return (name[0] << 0) ^ rol32(hash, 7 * 1);
        default: /* case 0: */
            return hash;
        }
    }

.. _directory-attribute-block-header:

Block Headers
~~~~~~~~~~~~~

-  Tree nodes, leaf and node `directories <#directories>`__, and leaf and node
   `extended attributes <#extended-attributes>`__ use the xfs\_da\_blkinfo\_t
   filesystem block header. The structure appears as follows:

.. code:: c

    typedef struct xfs_da_blkinfo {
         __be32                     forw;
         __be32                     back;
         __be16                     magic;
         __be16                     pad;
    } xfs_da_blkinfo_t;

**forw**
    Logical block offset of the previous B+tree block at this level.

**back**
    Logical block offset of the next B+tree block at this level.

**magic**
    Magic number for this directory/attribute block.

**pad**
    Padding to maintain alignment.

-  On a v5 filesystem, the leaves use the struct xfs\_da3\_blkinfo\_t
   filesystem block header. This header is used in the same place as
   xfs\_da\_blkinfo\_t:

.. code:: c

    struct xfs_da3_blkinfo {
         /* these values are inside xfs_da_blkinfo */
         __be32                     forw;
         __be32                     back;
         __be16                     magic;
         __be16                     pad;

         __be32                     crc;
         __be64                     blkno;
         __be64                     lsn;
         uuid_t                     uuid;
         __be64                     owner;
    };

**forw**
    Logical block offset of the previous B+tree block at this level.

**back**
    Logical block offset of the next B+tree block at this level.

**magic**
    Magic number for this directory/attribute block.

**pad**
    Padding to maintain alignment.

**crc**
    Checksum of the directory/attribute block.

**blkno**
    Block number of this directory/attribute block.

**lsn**
    Log sequence number of the last write to this block.

**uuid**
    The UUID of this block, which must match either sb\_uuid or sb\_meta\_uuid
    depending on which features are set.

**owner**
    The inode number that this directory/attribute block belongs to.

.. _directory-attribute-internal-node:

Internal Nodes
~~~~~~~~~~~~~~

The nodes of a dabtree have the following format:

.. code:: c

    typedef struct xfs_da_intnode {
         struct xfs_da_node_hdr {
               xfs_da_blkinfo_t     info;
               __uint16_t           count;
               __uint16_t           level;
         } hdr;
         struct xfs_da_node_entry {
               xfs_dahash_t         hashval;
               xfs_dablk_t          before;
         } btree[1];
    } xfs_da_intnode_t;

**info**
    Directory/attribute block info. The magic number is XFS\_DA\_NODE\_MAGIC
    (0xfebe).

**count**
    Number of node entries in this block.

**level**
    The level of this block in the B+tree. Levels start at 1 for blocks that
    point to directory or attribute data blocks and increase towards the root.

**hashval**
    The hash value of a particular record.

**before**
    The directory/attribute logical block containing all entries up to the
    corresponding hash value.

    -  On a v5 filesystem, the directory/attribute node blocks have the
       following structure:

.. code:: c

    struct xfs_da3_intnode {
         struct xfs_da3_node_hdr {
               struct xfs_da3_blkinfo    info;
               __uint16_t                count;
               __uint16_t                level;
               __uint32_t                pad32;
         } hdr;
         struct xfs_da_node_entry {
               xfs_dahash_t              hashval;
               xfs_dablk_t               before;
         } btree[1];
    };

**info**
    Directory/attribute block info. The magic number is XFS\_DA3\_NODE\_MAGIC
    (0x3ebe).

**count**
    Number of node entries in this block.

**level**
    The level of this block in the B+tree. Levels start at 1 for blocks that
    point to directory or attribute data blocks, and increase towards the
    root.

**pad32**
    Padding to maintain alignment.

**hashval**
    The hash value of a particular record.

**before**
    The directory/attribute logical block containing all entries up to the
    corresponding hash value.
