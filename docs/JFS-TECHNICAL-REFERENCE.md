# JFS: technical reference

## History and scope

IBM's Journaled File System originated on AIX. The later JFS family developed for OS/2 was ported to Linux and released as open source in 2000. This reference concerns the JFS1-magic disk format of the Linux and related OS/2 implementation, not arbitrary earlier AIX JFS media. Its principal structures are an aggregate containing filesets, extent-based file B+trees, Unicode directory B+trees, dynamically backed inode allocation groups, hierarchical free-space maps and a metadata transaction log. Platform and directory-index flags affect interoperability.

## Media geometry and bootstrap layout

The reference format stores little-endian fields and uses 512-byte inodes, 4096-byte metadata pages and filesystem block sizes from 512 through 4096 bytes. Do not confuse physical-sector units, aggregate blocks and metadata pages. Fixed partition-relative offsets: reserved area 0..0x7fff; primary superblock 0x8000; aggregate inode-map control page 0x9000; first inode-map extent follows; primary aggregate inode table 0xb000 (16 KiB); secondary superblock 0xf000; initial block-map area 0x10000. The aggregate inode table's first extent permits bootstrap before general inode-map traversal. Validate all offsets against declared geometry and image capacity.

## Superblock field inventory

Relative to the superblock: ASCII JFS1 at 0, version at 4, 64-bit aggregate size in physical/LVM blocks at 8; filesystem block bytes 16, log2 block size 20 (16-bit), log2 ratio to physical blocks 22, physical block size 24 and its exponent 28. Allocation-group size 32, flags 36, state 40, compression field 44, secondary aggregate inode table extent at 48 and map extent at 56. Log device and serial at 64/68, inline log extent 72, checker workspace extent 80, update time 88, fsck log length 96, active log selector 100 and legacy 11-byte name 101. Version-two fields: extend size 112, extend fsck/log extents 120/128, UUID 136, label 152 and log UUID 168. Validate size conversion, log identity and state; never assume a dirty volume is safe to mutate.

The JFS1 signature is a Linux/OS2 lineage marker, not proof of every historical AIX variant. Check block-size/log2 relationships, aggregate and fileset boundaries and allocation/inode-map consistency before interpreting trees.

## Packed extent descriptors

A physical extent descriptor (PXD) is eight bytes: little-endian word at 0 contains length in low 24 bits and the high eight bits of a 40-bit block address in its high byte; low address word at 4. Extent length and address use filesystem blocks. An XAD is sixteen bytes: flags at 0, reserved 1..2, high eight logical offset bits at 3, low logical offset word at 4 and PXD at 8. A data descriptor (DXD) has flag 0, three reserved bytes, byte length at 4 and PXD at 8. Decode the masks explicitly. XAD flag 0x08 marks allocated but unrecorded data; never expose stale payload from such an extent. Compression and other historical flags require separate support.

## Aggregate and fileset inode lookup

Aggregate inode 1 describes the aggregate inode map; 2 describes block allocation; 3 inline log; 4 bad blocks; 16 is the first fileset inode map. In the primary bootstrap extent, inode 16 begins at 0xb000+16*512 = 0xd000. Read its extent tree to locate the fileset inode allocation map. Root of the fileset is inode 2. Do not confuse aggregate inode 2 with user root inode 2. An inode allocation group (IAG) covers 4096 inode numbers, up to 128 extents of 32 inodes each. The control page is logical page zero; IAG g is page g+1, addressed through the inode-map extent tree.

## IAG layout and worked inode address

An IAG is 4096 bytes. The 64-bit allocation-group start is at 0, IAG number 8; free-list links at 12..31; inode and extent summary maps at 32 and 48; free inode/extent counts at 64/68. Working allocation map at 2048 (128 words), persistent map at 2560 and 128 physical extent descriptors at 3072. For inode n, g=floor(n/4096), e=floor((n mod 4096)/32), slot=n mod 32. Resolve IAG page g+1, validate its group and allocation state and take PXD e. Inode 4170 is group 1, extent 2, slot 10. If that extent starts at filesystem block 900 and block bytes are 4096, the inode begins at 900*4096+10*512=3691520. Check extent backing and persistent allocation before reading.

## Dinode layout

A dinode is 512 bytes. The 128-byte base holds stamp, fileset, number, generation at 0/4/8/12; inode-extent PXD at 16; size 24 and allocated blocks 32 (64-bit); link count, UID, GID and mode at 40/44/48/52. Four eight-byte timestamps at 56,64,72,80 hold seconds and nanoseconds. ACL and EA descriptors at 88 and 104; directory next index 120, ACL type 124. The extension is type-dependent. Directory table occupies 128..223, directory tree root starts 224; ordinary file extent-tree root also starts at 224. Inline symlink data can occupy 256..511 subject to EA state. Inspect mode and flags before decoding unions. Preserve OS/2 attribute bits and native metadata.

## Extent tree lookup

The xtree header occupies 32 bytes: next and previous sibling addresses at 0/8; flags 16; next index 18, maximum index 20 (16-bit); self PXD 24. XADs begin at slot 2 (offset 32). The inode root admits up to 18 slots including header; an external 4096-byte page admits 256. Use the node flags to distinguish internal branch keys from leaf extents, and validate nextindex against actual capacity. Descend sorted logical starts, then check a leaf run covers the requested logical block. For a run logical 120, physical 800, length 12, logical block 125 maps to 805. Missing extents may represent sparse ranges; allocated-but-unrecorded extents read as zeros. Check sibling/parent coverage, cycle bounds, extent length and EOF.

## Directory tree and slot chains

Directory roots have 32-byte slots, nine in the inode. Root header includes DASD information 0..15, flags 16, sorted-table count 17, free count 18, free-list head 19, parent inode 20 and eight sorted slot indices 24..31. External pages have sibling addresses 0/8, flags 16, count 17, free count 18, free-list 19, maximum slots 20, sorted-table first slot 21 and self PXD 24. Page sizes can be 512,1024,2048 or 4096 bytes. The sorted index points to entry head slots, not contiguous entries. A continuation slot has signed next at 0, count at 1 and fifteen UTF-16LE code units from 2. Next -1 terminates. Validate all slot identities and disjoint ownership of free, sorted-table and occupied slots.

## Directory entries and name semantics

Internal head slot: child PXD 0, signed next 8, name length 9, eleven UTF-16LE name units at 10. Modern leaf head: inode 0, next 4, name length 5, eleven UTF-16LE units at 6, persistent index 28. Legacy leaf has thirteen name units and no index. A modern 30-unit name requires a head plus ceil((30-11)/15)=2 continuation slots. Assemble names by following the chain, never by treating adjacent slots as contiguous. The B+tree's ordering depends on platform case/collation rules; verify native Unicode comparison. The persistent directory index is a separate mapping for stable enumeration, not a substitute for namespace ownership. A mutation must update the tree, index and parent relationships consistently.

## Allocation, maps and free space

The aggregate block map uses dmap bitmaps and hierarchical summary trees to find free runs. A base dmap covers 8192 filesystem blocks; each bit records allocation, with distinct working and persistent maps. Summary buddy trees encode maximum aligned free-run order, permitting larger allocation search without scanning the entire disk. Free counts and per-allocation-group control values must agree with persistent ownership. Allocate inode extents through IAG summaries, then individual inode bits. A free inode slot and a free physical inode extent are different resources. Reconstruct all control, tree, log and data allocations before treating apparent free blocks as reusable.

## Journal, mutation and recovery

JFS records metadata transactions in a log that can be inline or on an external logical volume. Distinguish aggregate mount serial and log UUID, transaction commit records, redo records and no-redo/free-space semantics. Modified pages and persistent maps are published under transaction management; working allocation may differ until commit. Recovery scans the validated log and applies only committed changes using the native redo ordering and page-range semantics. Unlink, truncate and rename must preserve link counts and prevent premature reuse. Defrag must copy and verify a physical extent, update its XAD transactionally and free the old run only after safe publication. Exact log record layouts and replay coverage must be established before independent writes.

## Implementation and qualification boundary

The independent provider uses Common V2: https://github.com/Infiltrator-Projects/Libraries/blob/main/docs/FILESYSTEM_COMMON_ABI_V2.md . Negotiate actual operation masks and media profiles. Distinguish unsupported, corrupt, I/O and stale-handle results. Require bounded host I/O, exclusive unmounted ownership and durable flush before mutation. Test Media and Defragmenter consume the same engine, not separate format parsers. Hosted and freestanding binaries, relocation, recovery, BOOT, kernel integration and packages require separate exact-source evidence. Current support belongs to the repository README and qualification records. The intended package paths are /System/Filesystems/ and /System/Resources/Documentation/; the reference is not an installation or release-status claim.

## References and independent verification

Pinned layout sources: https://github.com/torvalds/linux/tree/v6.12/fs/jfs (jfs_superblock.h, jfs_filsys.h, jfs_types.h, jfs_dinode.h, jfs_imap.h, jfs_xtree.h, jfs_dtree.h, jfs_dmap.h, jfs_logmgr.h). User-space tools: https://jfs.sourceforge.net/ . Test independently created Linux JFS and relevant OS/2 images, 40-bit address decoding, IAG boundaries, missing backing extents, unrecorded ranges, extent-tree splits, long-name slot chains, indexed versus legacy directories, map disagreement, external-log identity and interrupted metadata transactions. Exact buddy-tree layout, platform collation and journal redo encoding need further field-level source and fixture validation. The historical provider documents are preserved below; no runtime qualification is inferred.

Use jfs_dinode.h for packed inode layout; Linux in-memory objects are not persisted structures.

Retained source: https://github.com/Infiltrator-Projects/Filesystems/blob/101026021d9088d80b7f0f472d9061f0e2c688a3/native/filesystems/jfs/linux/jfs_superblock.h (blob 93402c4207).
