.. SPDX-License-Identifier: CC-BY-SA-4.0

Common XFS Types
----------------

All the following XFS types can be found in xfs\_types.h. NULL values are
always -1 on disk (ie. all bits for the value set to one).

**xfs\_ino\_t**
    Unsigned 64 bit absolute `inode number <#inode-numbers>`__.

**xfs\_off\_t**
    Signed 64 bit file offset.

**xfs\_daddr\_t**
    Signed 64 bit disk address (sectors).

**xfs\_agnumber\_t**
    Unsigned 32 bit `AG number <#allocation-groups>`__.

**xfs\_agblock\_t**
    Unsigned 32 bit AG relative block number.

**xfs\_extlen\_t**
    Unsigned 32 bit `extent <#data-extents>`__ length in blocks.

**xfs\_extnum\_t**
    Signed 32 bit number of extents in a data fork.

**xfs\_aextnum\_t**
    Signed 16 bit number of extents in an attribute fork.

**xfs\_dablk\_t**
    Unsigned 32 bit block number for `directories <#directories>`__ and
    `extended attributes <#extended-attributes>`__.

**xfs\_dahash\_t**
    Unsigned 32 bit hash of a directory file name or extended attribute name.

**xfs\_fsblock\_t**
    Unsigned 64 bit filesystem block number combining `AG
    number <#allocation-groups>`__ and block offset into the AG.

**xfs\_rfsblock\_t**
    Unsigned 64 bit raw filesystem block number.

**xfs\_rtblock\_t**
    Unsigned 64 bit extent number in the `real-time <#real-time-devices>`__
    sub-volume.

**xfs\_fileoff\_t**
    Unsigned 64 bit block offset into a file.

**xfs\_filblks\_t**
    Unsigned 64 bit block count for a file.

**uuid\_t**
    16-byte universally unique identifier (UUID).

**xfs\_fsize\_t**
    Signed 64 bit byte size of a file.
