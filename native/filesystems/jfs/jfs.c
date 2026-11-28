/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native bounded recognition using the documented on-disk structures and
 * the pinned Linux v6.12 format reference. No kernel, libc or tool backend.
 * Recognition is not evidence of a safe mounted namespace or mutation.
 */
#include <infiltratr/filesystem_module_v2.h>
static inline uint32_t le16(const uint8_t *p){return p[0]|((uint32_t)p[1]<<8);}
static inline uint32_t le32(const uint8_t *p){return le16(p)|(le16(p+2)<<16);}
static inline uint64_t le64(const uint8_t *p){return le32(p)|((uint64_t)le32(p+4)<<32);}
static inline int equal(const uint8_t *a,const uint8_t *b,uint32_t n){
 for(uint32_t i=0;i<n;i++)if(a[i]!=b[i])return 0;
 return 1;
}
static inline uint32_t crc32(uint32_t c,const uint8_t *p,uint32_t n){
 while(n--){c^=*p++;for(unsigned i=0;i<8u;i++)c=(c>>1)^((0u-(c&1u))&0xedb88320u);}return c;
}
static int read_bytes(const IfsFsHostRuntimeV2 *h,uint64_t off,void *p,uint32_t n){
 if(off>h->volume_bytes||n>h->volume_bytes-off)return IFS_FS_NOT_FOUND;
 return ifs_fs_host_read_bytes_v2(h,off,p,n);
}

static int extent(const uint8_t *p,uint64_t limit,uint32_t min)
{
    uint32_t n=le32(p)&0xffffffu;uint64_t a=((uint64_t)p[3]<<32)|le32(p+4);
    return n>=min && a && a<limit && n<=limit-a;
}
static int sb(const uint8_t *p,uint64_t media)
{
    static const uint8_t magic[4]={'J','F','S','1'};
    if(!equal(p,magic,4))return IFS_FS_NOT_FOUND;
    uint32_t v=le32(p+4), b=le32(p+16), phys=le32(p+24), ag=le32(p+32);
    unsigned l=le16(p+20), pl=le16(p+28), factor=le16(p+22);
    if(v<1u||v>2u)return IFS_FS_UNSUPPORTED;
    if(l<9u||l>12u||pl<9u||pl>l||factor!=l-pl||b!=(1u<<l)||phys!=(1u<<pl)||
       !le64(p+8)||le64(p+8)>media/phys||!ag||(ag&(ag-1u))||le32(p+40)>15u)
        return IFS_FS_CORRUPT;
    uint64_t blocks=media/b;
    if(!extent(p+48,blocks,16384u/b)||!extent(p+56,blocks,1u)||!extent(p+80,blocks,1u))
        return IFS_FS_CORRUPT;
    if(le32(p+72)&0xffffffu && !extent(p+72,blocks,1u))return IFS_FS_CORRUPT;
    return IFS_FS_OK;
}
static int identify(const IfsFsHostRuntimeV2 *h,uint32_t *variant)
{
    uint8_t a[512],b[512],i[512];
    int r=read_bytes(h,0x8000,a,sizeof(a));if(r)return r;
    r=read_bytes(h,0xf000,b,sizeof(b));if(r)return r;
    int x=sb(a,h->volume_bytes),y=sb(b,h->volume_bytes);
    if(x&&y)return x==IFS_FS_NOT_FOUND?y:x;
    const uint8_t *p=x?b:a;
    if(!x&&!y && (!equal(a+4,b+4,32)||(le32(p+4)==2u&&!equal(a+136,b+136,16))))return IFS_FS_CORRUPT;
    /* The primary AIT's fileset-map inode is a fixed native bootstrap.
     * Corroborate its identity and tree bounds without interpreting a
     * dirty aggregate as a mountable fileset. */
    r=read_bytes(h,0xd000,i,sizeof(i));if(r)return r;
    if(le32(i+8)!=16u || !le64(i+24) || !le64(i+32) ||
       (le32(i+52)&0170000u)!=0100000u ||
       le16(i+224+18)<2u || le16(i+224+18)>18u ||
       le16(i+224+20)<le16(i+224+18) || le16(i+224+20)>18u)
        return IFS_FS_CORRUPT;
    *variant=le32(p+4);return IFS_FS_OK;
}
static int probe(const IfsFsHostRuntimeV2 *h,uint32_t *variant){
 if(!variant||!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
 *variant=0;
 if(h->volume_bytes%h->logical_sector_bytes)return IFS_FS_INVALID;
 return identify(h,variant);
}
#include <infiltratr/filesystem_session_v2.h>
#include "reader.h"
static const IfsFsCoreOpsV2 core={
 .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsCoreOpsV2),IFS_FS_GROUP_CORE,0,IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT|IFS_FS_CORE_STAT|IFS_FS_CORE_LIST|IFS_FS_CORE_READ},.probe=probe,.mount=mount_volume,.unmount=unmount_volume,.stat=mounted_stat,.directory_entry=mounted_directory_entry,.read=mounted_read
};
static const IfsFsGroupHeaderV2 *query(uint32_t id,uint32_t min,uint32_t max){
 if(min>2u||max<2u)return NULL;
 if(id==IFS_FS_GROUP_CORE)return &core.header;
 return id==IFS_FS_GROUP_VOLUME?&volume_ops.header:NULL;
}
static const IfsFsModuleApiV2 api={
 .abi_version=2,.structure_bytes=sizeof(IfsFsModuleApiV2),.format_id="jfs",
#ifdef IFS_FREESTANDING
 .runtime_profiles=IFS_FS_PROFILE_FREESTANDING,
#else
 .runtime_profiles=IFS_FS_PROFILE_HOSTED,
#endif
 .group_mask=IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME),.query_group=query
};
__attribute__((visibility("default"))) const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void){return &api;}
