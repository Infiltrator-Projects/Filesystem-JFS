# JFS Common V2 migration

Native read-only namespace implementation for the clean Linux revision 1/2 4 KiB inline-log profile. CORE probe/mount/stat/list/read and VOLUME query are implemented directly against native extent trees, persistent block maps and inode allocation groups. The directory scope is inline ASCII leaf roots; non-ASCII names and external directory pages fail closed. Sparse and unrecorded file regions return zeros. Only stable exclusive read-only host access is admitted.

[Independent formatter qualification](https://github.com/Infiltrator-Projects/Filesystem-JFS/actions/runs/46131371330) and [populated native structural fixture qualification](https://github.com/Infiltrator-Projects/Filesystem-JFS/actions/runs/46131701101) passed, including hosted and import-free builds, allocation corruption refusal and sanitizer checks. Independently populated external file payloads remain unqualified. Runtime publication is gated on exact-source CI and package installation/purge tests; it does not establish physical BOOT support.

Remaining: external directory B+trees, full Unicode, independently populated payload verification, complete allocation ownership reconciliation, log replay, mutation, format, repair, defragmentation and physical kernel/BOOT integration. Reference source: [pinned native JFS](https://github.com/Infiltrator-Projects/Filesystems/tree/101026021d9088d80b7f0f472d9061f0e2c688a3/native/filesystems/jfs). New code GPL-3.0-or-later; inherited references retain their notices.

### Mounted volume lifetime

The provider copies the caller's size-versioned host descriptor and assigns each mount an opaque generation. An operation reserves its mount until its callback returns. A concurrent unmount or second operation on that mount returns `IFS_FS_BUSY`. Independent mounts remain usable. Unmount removes the generation before releasing native state, so a closed handle returns `IFS_FS_INVALID` even after a new mount reuses the same memory. The Common V2 regression holds a directory read inside a host I/O callback while a second thread queries and unmounts both volumes.
