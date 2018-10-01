.. SPDX-License-Identifier: CC-BY-SA-4.0

About this Book
===============

XFS is a high performance filesystem which was designed to maximize
parallel throughput and to scale up to extremely large 64-bit storage
systems. Originally developed by SGI in October 1993 for IRIX, XFS can
handle large files, large filesystems, many inodes, large directories,
large file attributes, and large allocations. Filesystems are optimized
for parallel access by splitting the storage device into semi-autonomous
allocation groups. XFS employs branching trees (B+ trees) to facilitate
fast searches of large lists; it also uses delayed extent-based
allocation to improve data contiguity and IO performance.

This document describes the on-disk layout of an XFS filesystem and how
to use the debugging tools ``xfs_db`` and ``xfs_logprint`` to inspect
the metadata structures. It also describes how on-disk metadata relates
to the higher level design goals.

This book’s source code is available in the Linux kernel git tree.
Feedback should be sent to the XFS mailing list, currently at:
``linux-xfs@vger.kernel.org``.

    **Note**

    All fields in XFS metadata structures are in big-endian byte order
    except for log items which are formatted in host order.

Copyright
---------
© Copyright 2006 Silicon Graphics Inc. All rights reserved.  Permission is
granted to copy, distribute, and/or modify this document under the terms of the
Creative Commons Attribution-Share Alike, Version 3.0 or any later version
published by the Creative Commons Corp. A copy of the license is available at
http://creativecommons.org/licenses/by-sa/3.0/us/ .

Change Log
----------

.. list-table::
   :widths: 8 12 14 46
   :header-rows: 1

   * - Version
     - Date
     - Author
     - Description

   * - 0.1
     - 2006
     - Silicon Graphics, Inc.
     - Initial Release

   * - 1.0
     - Fri Jul 03 2009
     - Ryan Lerch
     - Publican Conversion

   * - 1.1
     - March 2010
     - Eric Sandeen
     - Community Release

   * - 1.99
     - February 2014
     - Dave Chinner
     - AsciiDoc Conversion

   * - 3.0
     - October 2015
     - Darrick J. Wong
     - Miscellaneous fixes.
       Add missing field definitions.
       Add some missing xfs_db examples.
       Add an overview of XFS.
       Document the journal format.
       Document the realtime device.

   * - 3.1
     - October 2015
     - Darrick J. Wong
     - Add v5 fields.
       Discuss metadata integrity.
       Document the free inode B+tree.
       Create an index of magic numbers.
       Document sparse inodes.

   * - 3.14
     - January 2016
     - Darrick J. Wong
     - Document disk format change testing.

   * - 3.141
     - June 2016
     - Darrick J. Wong
     - Document the reverse-mapping btree.
       Move the b+tree info to a separate chapter.
       Discuss overlapping interval b+trees.
       Discuss new log items for atomic updates.
       Document the reference-count btree.
       Discuss block sharing, reflink, &amp; deduplication.

   * - 3.1415
     - July 2016
     - Darrick J. Wong
     - Document the real-time reverse-mapping btree.

   * - 3.14159
     - June 2017
     - Darrick J. Wong
     - Add the metadump file format.

   * - 3.141592
     - May 2018
     - Darrick J. Wong
     - Incorporate Dave Chinner's log design document.
       Incorporate Dave Chinner's self-describing metadata design document.

   * - 4.20
     - September 2018
     - Darrick J. Wong
     - Convert to RestructuredText and move to the kernel source tree.
