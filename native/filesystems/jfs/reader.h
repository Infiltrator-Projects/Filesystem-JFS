/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native offline Linux JFS namespace reader, using the on-disk PXD/XAD,
 * dmap, IAG and directory slot structures of the pinned format source.
 * No journal replay; a clean exclusive host and validated allocation
 * map are prerequisites. The first namespace profile uses inline
 * directory leaf roots; external directory pages fail closed.
 */
#define PAGE 4096u
#define MAP_MAX_BLOCKS (UINT64_C(1)<<23)
#define BT_ROOT 1u
#define BT_LEAF 2u
#define BT_INTERNAL 4u
#define MODE_MASK 0170000u
#define MODE_FILE 0100000u
#define MODE_DIR 0040000u
#define MODE_LINK 0120000u
typedef struct {
 const IfsFsHostRuntimeV2 *host;
 uint64_t blocks,free;
 uint32_t flags,version,iags,stamp;
 uint8_t imap[512],bmap[512];
 uint8_t *allocated;
 int verified;
} Volume;
static uint64_t pxd_address(const uint8_t *p){return ((uint64_t)p[3]<<32)|le32(p+4);}
static uint32_t pxd_length(const uint8_t *p){return le32(p)&0xffffffu;}
static uint64_t xad_offset(const uint8_t *p){return ((uint64_t)p[3]<<32)|le32(p+4);}
static int owned(const Volume*v,uint64_t p,uint64_t n)
{
 if(!p||p>=v->blocks||!n||n>v->blocks-p)return 0;
 if(!v->verified)return 1;
 while(n--){if(!(v->allocated[p/8u]&(1u<<(p%8u))))return 0;p++;}
 return 1;
}
static int raw(const Volume *v,uint64_t block,void *out,uint32_t length)
{
 if(length>PAGE||!owned(v,block,1))return IFS_FS_CORRUPT;
 return ifs_fs_host_read_bytes_v2(v->host,block*PAGE,out,length);
}
/* The logical key, not the physical allocation address, drives XAD
 * search. Internal keys choose the last lower bound; a leaf must cover
 * the requested logical block. A gap is a hole only in file data. */
static int xmap(const Volume*v,const uint8_t *inode,uint64_t logical,uint64_t *physical)
{
 uint8_t page[PAGE];const uint8_t *p=inode+224;uint32_t cap=18;
 uint64_t visited[16];
 *physical=0;
 for(unsigned depth=0;depth<16;depth++){
  unsigned flag=p[16],next=le16(p+18),max=le16(p+20);
  if((flag&BT_ROOT)!=(depth?0u:BT_ROOT)||((flag&(BT_LEAF|BT_INTERNAL))!=BT_LEAF&&(flag&(BT_LEAF|BT_INTERNAL))!=BT_INTERNAL)||
     next<2u||next>max||max>cap)return IFS_FS_CORRUPT;
  const uint8_t *choice=NULL;uint64_t prev=0,prev_end=0;
  for(unsigned i=2;i<next;i++){
   const uint8_t *e=p+i*16u;uint64_t key=xad_offset(e),b=pxd_address(e+8);uint32_t n=pxd_length(e+8);
   if((i>2u&&(key<=prev||((flag&BT_LEAF)&&key<prev_end)))||
      key>UINT64_MAX-n||!owned(v,b,n))return IFS_FS_CORRUPT;
   if(e[0]&~8u)return IFS_FS_UNSUPPORTED;
   if((flag&BT_INTERNAL)&&(n!=1u||e[0]))return IFS_FS_CORRUPT;
   if(key<=logical)choice=e;
   prev=key;prev_end=key+n;
  }
  if(!choice)return IFS_FS_OK;
  uint64_t b=pxd_address(choice+8);
  if(flag&BT_LEAF){
   uint64_t delta=logical-xad_offset(choice);
   if(delta<pxd_length(choice+8)&&!(choice[0]&8u))*physical=b+delta;
   return IFS_FS_OK;
  }
  for(unsigned i=0;i<depth;i++)if(visited[i]==b)return IFS_FS_CORRUPT;
  visited[depth]=b;int rc=raw(v,b,page,PAGE);if(rc)return rc;
  if(pxd_address(page+24)!=b||pxd_length(page+24)!=1u)return IFS_FS_CORRUPT;
  p=page;cap=256;
 }
 return IFS_FS_CORRUPT;
}
static int read_file(const Volume*v,const uint8_t*inode,uint64_t off,void*out,uint32_t n,int holes)
{
 uint8_t *b=out;uint64_t size=le64(inode+24);
 if((!b&&n)||off>size||n>size-off)return IFS_FS_INVALID;
 while(n){
  uint64_t physical;unsigned skip=(unsigned)(off%PAGE),take=PAGE-skip;
  if(take>n)take=n;
  int rc=xmap(v,inode,off/PAGE,&physical);if(rc)return rc;
  if(!physical){if(!holes)return IFS_FS_CORRUPT;for(unsigned k=0;k<take;k++)b[k]=0;}
  else {rc=ifs_fs_host_read_bytes_v2(v->host,physical*PAGE+skip,b,take);if(rc)return rc;}
  off+=take;n-=take;b+=take;
 }
 return IFS_FS_OK;
}
static int page_of(const Volume*v,const uint8_t*inode,uint64_t number,uint8_t*out)
{
 if(number>UINT64_MAX/PAGE)return IFS_FS_CORRUPT;
 return read_file(v,inode,number*PAGE,out,PAGE,0);
}
static int aggregate_inode(const Volume*v,unsigned id,uint8_t*p)
{
 int rc=ifs_fs_host_read_bytes_v2(v->host,0xb000u+id*512u,p,512);
 if(rc)return rc;
 return le32(p+8)==id&&le32(p+40)&&(le32(p+52)&MODE_MASK)==MODE_FILE?IFS_FS_OK:IFS_FS_CORRUPT;
}
/* Reconstruct actual allocation from persistent dmap words. The clean
 * working map must agree. Validate each page's covered range and free
 * count, and reconcile the sum with the aggregate map descriptor. */
static int load_allocation(Volume*v)
{
 uint8_t page[PAGE];int rc=page_of(v,v->bmap,0,page);if(rc)return rc;
 uint64_t mapsize=le64(page),free=le64(page+8);
 if(!mapsize||mapsize>v->blocks||mapsize>MAP_MAX_BLOCKS||free>mapsize||le32(page+16)!=0u)
  return IFS_FS_UNSUPPORTED;
 v->blocks=mapsize;
 v->allocated=v->host->allocate(v->host->context,(size_t)((mapsize+7u)/8u));
 if(!v->allocated)return IFS_FS_NO_MEMORY;
 for(uint64_t i=0;i<(mapsize+7u)/8u;i++)v->allocated[i]=0;
 v->free=0;
 for(uint64_t start=0;start<mapsize;start+=8192u){
  uint64_t logical=(start>>13)+(start>>23)+(start>>33)+4u;
  rc=page_of(v,v->bmap,logical,page);if(rc)return rc;
  unsigned count=(unsigned)(mapsize-start>8192u?8192u:mapsize-start),local=0;
  if(le32(page)!=count||le64(page+8)!=start||!equal(page+2048,page+3072,1024))return IFS_FS_CORRUPT;
  for(unsigned k=0;k<count;k++){
   uint32_t word=le32(page+3072+(k/32u)*4u);
   if(word&(UINT32_C(0x80000000)>>(k%32u)))v->allocated[(start+k)/8u]|=(uint8_t)(1u<<((start+k)%8u));
   else local++;
  }
  if(local!=le32(page+4))return IFS_FS_CORRUPT;
  v->free+=local;
 }
 if(v->free!=free)return IFS_FS_CORRUPT;
 v->verified=1;
 /* Confirm the allocation map's own extents with the resulting map. */
 rc=page_of(v,v->bmap,0,page);if(rc)return rc;
 for(uint64_t start=0;start<mapsize;start+=8192u){
  rc=page_of(v,v->bmap,(start>>13)+(start>>23)+(start>>33)+4u,page);if(rc)return rc;
 }
 return IFS_FS_OK;
}
static int load_inode(const Volume*v,uint32_t id,uint8_t*out)
{
 uint32_t group=id/4096u,extent_index=(id%4096u)/32u,slot=id%32u;
 if(id<2u||group>=v->iags)return IFS_FS_CORRUPT;
 uint8_t page[PAGE];int rc=page_of(v,v->imap,(uint64_t)group+1u,page);if(rc)return rc;
 if(le32(page+8)!=group)return IFS_FS_CORRUPT;
 uint32_t mask=UINT32_C(0x80000000)>>slot;
 if(!(le32(page+2560+extent_index*4u)&mask)||
    le32(page+2048+extent_index*4u)!=le32(page+2560+extent_index*4u))
  return IFS_FS_CORRUPT;
 const uint8_t *pxd=page+3072+extent_index*8u;uint64_t b=pxd_address(pxd);
 if(pxd_length(pxd)!=4u||!owned(v,b,4u))return IFS_FS_CORRUPT;
 rc=ifs_fs_host_read_bytes_v2(v->host,b*PAGE+slot*512u,out,512);if(rc)return rc;
 if(le32(out+8)!=id||le32(out)!=v->stamp||!le32(out+40)||
    pxd_address(out+16)!=b||pxd_length(out+16)!=4u||
    le64(out+24)>v->blocks*PAGE||le64(out+32)>v->blocks)
  return IFS_FS_CORRUPT;
 return IFS_FS_OK;
}
static int name_equal(const char*a,const char*b,unsigned n)
{for(unsigned k=0;k<n;k++)if(a[k]!=b[k])return 0;return 1;}
/* Inline directory B+tree leaf. Walk the actual sorted head-slot table
 * and validate every continuation and the complete freelist. UTF-16LE
 * ASCII subset is explicit; non-ASCII names and external trees are
 * refused until their Unicode collation and slot ownership are qualified. */
static int directory(const Volume*v,const uint8_t*inode,const char*name,unsigned length,
                     uint32_t index,uint32_t *id,char*out,unsigned *outlen)
{
 if((le32(inode+52)&MODE_MASK)!=MODE_DIR)return IFS_FS_INVALID;
 const uint8_t*p=inode+224;unsigned flag=p[16],count=p[17],free=p[18],head=p[19];
 if((flag&(BT_ROOT|BT_LEAF|BT_INTERNAL))!=(BT_ROOT|BT_LEAF))return IFS_FS_UNSUPPORTED;
 if(count>8u||free>8u||!le32(p+20))return IFS_FS_CORRUPT;
 unsigned used=1;int match=0;char names[8][256];unsigned lengths[8];
 for(unsigned i=0;i<count;i++){
  unsigned slot=p[24+i];if(!slot||slot>=9u||(used&(1u<<slot)))return IFS_FS_CORRUPT;
  used|=1u<<slot;
  const uint8_t*d=p+slot*32u;unsigned n=d[5],next=d[4],at=0;
  uint32_t number=le32(d);
  if(!n||number<2u)return IFS_FS_CORRUPT;
  unsigned first=(v->flags&0x00200000u)?11u:13u;
  for(unsigned k=0;k<first&&at<n;k++){
   unsigned c=le16(d+6+k*2u);if(c<32u||c>126u||c=='/')return IFS_FS_UNSUPPORTED;
   names[i][at++]=(char)c;
  }
  while(at<n){
   if(!next||next>=9u||(used&(1u<<next)))return IFS_FS_CORRUPT;
   used|=1u<<next;d=p+next*32u;
   unsigned take=n-at>15u?15u:n-at;
   if(d[1]!=take)return IFS_FS_CORRUPT;
   for(unsigned k=0;k<take;k++){
    unsigned c=le16(d+2+k*2u);if(c<32u||c>126u||c=='/')return IFS_FS_UNSUPPORTED;
    names[i][at++]=(char)c;
   }
   next=d[0];
  }
  if(next!=255u)return IFS_FS_CORRUPT;
  names[i][n]=0;lengths[i]=n;
  for(unsigned k=0;k<i;k++)if(lengths[k]==n&&name_equal(names[k],names[i],n))return IFS_FS_CORRUPT;
  if((name&&n==length&&name_equal(name,names[i],n))||(!name&&index==i)){
   *id=number;*outlen=n;for(unsigned k=0;k<=n;k++)out[k]=names[i][k];match=1;
  }
 }
 unsigned traversed=0;
 while(head!=255u){
  if(!head||head>=9u||(used&(1u<<head))||traversed>=free)return IFS_FS_CORRUPT;
  used|=1u<<head;head=p[head*32u];traversed++;
 }
 if(traversed!=free||used!=0x1ffu)return IFS_FS_CORRUPT;
 return match?IFS_FS_OK:name?IFS_FS_NOT_FOUND:IFS_FS_END;
}
static int info(void*volume,IfsFsVolumeInfoV2*out)
{
 Volume*v=volume;if(!v||!out||out->structure_bytes<sizeof(*out))return IFS_FS_INVALID;
 out->variant=v->version;out->max_name_bytes=255;out->block_bytes=PAGE;
 out->total_bytes=v->host->volume_bytes;out->free_bytes=v->free*PAGE;return IFS_FS_OK;
}
static void destroy(Volume*v)
{
 if(v->allocated)v->host->release(v->host->context,v->allocated);
 v->host->release(v->host->context,v);
}
static int mount_volume(const IfsFsHostRuntimeV2*h,void**out,IfsFsVolumeInfoV2*vi)
{
 uint8_t s[512],backup[512],page[PAGE],root[512];uint32_t version;
 if(!out||!vi||vi->structure_bytes<sizeof(*vi)||!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
 *out=NULL;
 if((h->capabilities&(IFS_FS_HOST_READ_ONLY|IFS_FS_HOST_EXCLUSIVE))!=(IFS_FS_HOST_READ_ONLY|IFS_FS_HOST_EXCLUSIVE))return IFS_FS_ACCESS;
 int rc=identify(h,&version);if(rc)return rc;
 rc=read_bytes(h,0x8000,s,sizeof(s));if(rc)return rc;
 rc=read_bytes(h,0xf000,backup,sizeof(backup));if(rc)return rc;
 if(sb(s,h->volume_bytes)||sb(backup,h->volume_bytes))return IFS_FS_UNSUPPORTED;
 uint32_t flags=le32(s+36);
 if((version!=1u&&version!=2u)||le32(s+16)!=PAGE||le32(s+40)||le32(backup+40)||le32(s+44)||
    !(flags&0x10000000u)||(flags&0x40000000u)||!(flags&0x800u))
  return IFS_FS_UNSUPPORTED;
 Volume*v=h->allocate(h->context,sizeof(*v));if(!v)return IFS_FS_NO_MEMORY;
 v->host=h;v->blocks=h->volume_bytes/PAGE;v->free=0;v->flags=flags;v->version=version;
 v->iags=0;v->stamp=0;v->allocated=NULL;v->verified=0;
 rc=aggregate_inode(v,2,v->bmap);if(!rc)rc=aggregate_inode(v,16,v->imap);
 if(!rc)rc=load_allocation(v);
 if(!rc)rc=page_of(v,v->imap,0,page);
 if(!rc){
  v->iags=le32(page+4);v->stamp=le32(v->imap);
  if(!v->iags||v->iags>1048576u||le32(page+16)!=4u||le32(page+20)!=2u||
     (uint64_t)(v->iags+1u)*PAGE>le64(v->imap+24))rc=IFS_FS_CORRUPT;
 }
 if(!rc)rc=load_inode(v,2,root);
 if(!rc){
  char name[256];unsigned length;uint32_t id;
  rc=directory(v,root,NULL,0,UINT32_MAX,&id,name,&length);
  if(rc==IFS_FS_END)rc=0;
 }
 if(rc){destroy(v);return rc;}
 *out=v;return info(v,vi);
}
static int unmount_volume(void*volume){if(!volume)return IFS_FS_INVALID;destroy(volume);return IFS_FS_OK;}
static int resolve(const Volume*v,const char*path,uint8_t*inode,const char**name,unsigned *length)
{
 if(!v||!path||path[0]!='/')return IFS_FS_INVALID;
 int rc=load_inode(v,2,inode);if(rc)return rc;
 *name=path;*length=1;const char*p=path+1;
 for(unsigned depth=0;*p;depth++){
  if(depth>=64u)return IFS_FS_UNSUPPORTED;
  unsigned n=0;while(p[n]&&p[n]!='/'){if(n==255u)return IFS_FS_INVALID;n++;}
  if(!n||(n==1u&&p[0]=='.')||(n==2u&&p[0]=='.'&&p[1]=='.'))return IFS_FS_INVALID;
  uint32_t id;char found[256];unsigned fn;
  rc=directory(v,inode,p,n,0,&id,found,&fn);if(rc)return rc;
  rc=load_inode(v,id,inode);if(rc)return rc;
  *name=p;*length=n;p+=n;if(*p=='/'){p++;if(!*p)return IFS_FS_INVALID;}
 }
 return IFS_FS_OK;
}
static int fill(const uint8_t*i,const char*name,unsigned n,IfsFsEntryV2*e)
{
 if(!e||e->structure_bytes<sizeof(*e)||!e->name||n>=e->name_capacity)return IFS_FS_INVALID;
 switch(le32(i+52)&MODE_MASK){case MODE_DIR:e->type=IFS_FS_ENTRY_DIRECTORY;break;case MODE_FILE:e->type=IFS_FS_ENTRY_FILE;break;case MODE_LINK:e->type=IFS_FS_ENTRY_LINK;break;default:return IFS_FS_UNSUPPORTED;}
 e->size_bytes=le64(i+24);e->attributes=0;e->name_bytes=n;
 for(unsigned k=0;k<n;k++)e->name[k]=name[k];
 e->name[n]=0;
 return IFS_FS_OK;
}
static int stat_path(void*v,const char*path,IfsFsEntryV2*e)
{uint8_t i[512];const char*n;unsigned l;int rc=resolve(v,path,i,&n,&l);return rc?rc:fill(i,n,l,e);}
static int list_path(void*v,const char*path,uint32_t index,IfsFsEntryV2*e)
{
 uint8_t i[512];const char*n;unsigned l;char name[256];uint32_t id;
 int rc=resolve(v,path,i,&n,&l);
 if(!rc)rc=directory(v,i,NULL,0,index,&id,name,&l);
 if(!rc)rc=load_inode(v,id,i);
 return rc?rc:fill(i,name,l,e);
}
static int read_path(void*v,const char*path,uint64_t off,void*out,uint32_t n)
{
 uint8_t i[512];const char*name;unsigned l;int rc=resolve(v,path,i,&name,&l);
 if(rc)return rc;
 if((le32(i+52)&MODE_MASK)!=MODE_FILE)return IFS_FS_INVALID;
 return read_file(v,i,off,out,n,1);
}
static const IfsFsVolumeOpsV2 volume_ops={.header={2,sizeof(IfsFsVolumeOpsV2),IFS_FS_GROUP_VOLUME,0,IFS_FS_VOLUME_QUERY},.query=info};
