/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <infiltratr/filesystem_module_v2.h>
#include "../Libraries/tests/filesystem_reader_lifetime.h"
#include <assert.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define SIZE (134217728u)
static uint8_t *image;
struct context{int fd,fail;uint32_t sector,allocs;uint64_t bytes;};
static int rd(void *p,uint64_t off,void *out,uint32_t n){
 struct context *c=p;assert(n&&n<=4096u&&!(off%c->sector)&&!(n%c->sector)&&off<=c->bytes&&n<=c->bytes-off);
 if(c->fail)return -1;
 if(c->fd>=0)return pread(c->fd,out,n,(off_t)off)==(ssize_t)n?0:-1;
 memcpy(out,image+off,n);return 0;
}
static void *allocate(void *p,size_t n){struct context*c=p;void *m=malloc(n);if(m)c->allocs++;return m;}
static void release(void *p,void *m){struct context*c=p;assert(m&&c->allocs);c->allocs--;free(m);}
static inline void put16(uint8_t*p,uint32_t n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static inline void put32(uint8_t*p,uint32_t n){put16(p,n);put16(p+2,n>>16);}
static inline void put64(uint8_t*p,uint64_t n){put32(p,(uint32_t)n);put32(p+4,(uint32_t)(n>>32));}
static inline uint32_t crc32(uint32_t c,const uint8_t*p,uint32_t n){
 while(n--){c^=*p++;for(unsigned i=0;i<8u;i++)c=(c>>1)^((0u-(c&1u))&0xedb88320u);}return c;
}

static void fixture(void){
 uint8_t *p=image+0x8000;memcpy(p,"JFS1",4);put32(p+4,2);put64(p+8,SIZE/512-8192);
 put32(p+16,4096);put16(p+20,12);put16(p+22,3);put32(p+24,512);put16(p+28,9);put32(p+32,8192);
 put32(p+48,4);put32(p+52,100);put32(p+56,2);put32(p+60,104);put32(p+80,4);put32(p+84,200);
 memcpy(image+0xf000,p,512);
 p=image+0xd000;put32(p+8,16);put64(p+24,8192);put64(p+32,2);put32(p+52,0100644);put16(p+242,3);put16(p+244,18);
}
static void corrupt(void){put32(image+0xd000+8,17);}
static void absent(void){image[0x8000]=0;image[0xf000]=0;}

static void expect(const IfsFsCoreOpsV2 *core,IfsFsHostRuntimeV2*h,struct context*c,int want){
 uint32_t v=99;int rc=core->probe(h,&v);
 if(rc!=want)fprintf(stderr,"expected %d got %d at sector %u\n",want,rc,c->sector);
 assert(rc==want&&(!rc?v!=0:v==0)&&!c->allocs);
}

/* Populated native byte fixture: independently encode a clean fileset,
 * extent roots, inode allocation group, dmap ownership and slot chains.
 * This complements the separate mkfs.jfs image; it is not a substitute
 * for real externally populated filesystem images. */
#define B 4096u
#define MAP_BLOCKS 31744u
#define USED 256u
static uint8_t *bp(unsigned n){return image+(size_t)n*B;}
static void xt(uint8_t *p,unsigned count){
 p+=224;p[16]=3;put16(p+18,2+count);put16(p+20,18);
}
static void xad(uint8_t*p,unsigned slot,unsigned key,unsigned block,unsigned count,unsigned flags){
 p+=224+(slot+2)*16;p[0]=(uint8_t)flags;put32(p+4,key);put32(p+8,count);put32(p+12,block);
}
static void ino(uint8_t*p,unsigned id,unsigned mode,uint64_t size,unsigned blocks){
 put32(p,1234);put32(p+4,16);put32(p+8,id);put32(p+16,4);put32(p+20,140);
 put64(p+24,size);put64(p+32,blocks);put32(p+40,1);put32(p+52,mode);
}
static void dt(uint8_t*p,unsigned parent,unsigned count){
 p+=224;p[16]=3;p[17]=(uint8_t)count;p[18]=(uint8_t)(8-count);p[19]=(uint8_t)(count==8?255:count+1);put32(p+20,parent);
 for(unsigned i=1;i<=count;i++)p[23+i]=(uint8_t)i;
 for(unsigned i=count+1;i<=8;i++)p[i*32]=(uint8_t)(i==8?255:i+1);
}
static void dent(uint8_t*p,unsigned slot,unsigned id,const char*name){
 p+=224+slot*32;unsigned n=(unsigned)strlen(name);assert(n<=11);
 put32(p,id);p[4]=255;p[5]=(uint8_t)n;
 for(unsigned k=0;k<n;k++)put16(p+6+k*2,(unsigned char)name[k]);
}
static void populated(void){
 memset(image,0,SIZE);fixture();uint8_t*p=image+0x8000;
 put32(p+4,1);put32(p+36,0x10200800);memcpy(image+0xf000,p,512);
 p=image+0xd000;memset(p,0,512);ino(p,16,0100644,2*B,2);xt(p,1);xad(p,0,0,100,2,0);
 p=image+0xb000+2*512;ino(p,2,0100644,8*B,8);xt(p,1);xad(p,0,0,120,8,0);
 p=bp(100);put32(p+4,1);put32(p+16,4);put32(p+20,2);
 p=bp(101);put32(p+2048,0x3c000000);put32(p+2560,0x3c000000);put32(p+3072,4);put32(p+3076,140);
 p=bp(120);put64(p,MAP_BLOCKS);put64(p+8,MAP_BLOCKS-USED);
 for(unsigned start=0;start<MAP_BLOCKS;start+=8192){
  p=bp(124+start/8192);unsigned n=MAP_BLOCKS-start>8192?8192:MAP_BLOCKS-start;
  put32(p,n);put32(p+4,n-(start==0?USED:0));put64(p+8,start);
  if(!start){memset(p+2048,255,USED/8);memset(p+3072,255,USED/8);}
 }
 p=bp(140)+2*512;ino(p,2,0040755,288,0);dt(p,2,2);dent(p,1,3,"data.bin");dent(p,2,4,"sub");
 p=bp(140)+3*512;ino(p,3,0100644,3*B+17,3);xt(p,3);xad(p,0,0,150,1,0);xad(p,1,2,152,1,0);xad(p,2,3,151,1,8);
 memset(bp(150),0x41,B);memset(bp(152),0x42,B);memset(bp(151),0xcc,B);
 p=bp(140)+4*512;ino(p,4,0040755,288,0);dt(p,2,1);dent(p,1,5,"note");
 p=bp(140)+5*512;ino(p,5,0100644,5,1);xt(p,1);xad(p,0,0,153,1,0);memcpy(bp(153),"hello",5);
}
static void reader_tests(const IfsFsCoreOpsV2*core,IfsFsHostRuntimeV2*h,struct context*c){
 populated();h->capabilities|=IFS_FS_HOST_EXCLUSIVE;
 void*v=NULL;IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
 check_reader_lifetime(core, h);
 assert(core->mount(h,&v,&info)==0&&v&&info.free_bytes==(MAP_BLOCKS-USED)*B);
 char name[256];IfsFsEntryV2 e={.structure_bytes=sizeof(e),.name=name,.name_capacity=sizeof(name)};
 assert(core->directory_entry(v,"/",0,&e)==0&&!strcmp(name,"data.bin")&&e.size_bytes==3*B+17);
 assert(core->directory_entry(v,"/",1,&e)==0&&!strcmp(name,"sub"));
 assert(core->directory_entry(v,"/",2,&e)==IFS_FS_END);
 assert(core->stat(v,"/sub/note",&e)==0&&e.size_bytes==5);
 uint8_t out[2*B+18];
 assert(core->read(v,"/sub/note",0,out,5)==0&&!memcmp(out,"hello",5));
 assert(core->read(v,"/data.bin",B-1,out,sizeof(out))==0&&out[0]==0x41);
 for(unsigned i=1;i<=B;i++)assert(!out[i]);
 for(unsigned i=B+1;i<=2*B;i++)assert(out[i]==0x42);
 for(unsigned i=2*B+1;i<sizeof(out);i++)assert(!out[i]);
 assert(core->read(v,"/data.bin",3*B+17,out,1)==IFS_FS_INVALID);
 put32(bp(140)+3*512+224+32+12,300);
 assert(core->read(v,"/data.bin",0,out,1)==IFS_FS_CORRUPT);
 assert(core->unmount(v)==0&&!c->allocs);
 populated();put32(image+0x8000+40,2);put32(image+0xf000+40,2);
 assert(core->mount(h,&v,&info)==IFS_FS_UNSUPPORTED&&!v&&!c->allocs);
 populated();bp(124)[3072]^=1;
 assert(core->mount(h,&v,&info)==IFS_FS_CORRUPT&&!v&&!c->allocs);
 h->capabilities&=~IFS_FS_HOST_EXCLUSIVE;
}

int main(int argc,char **argv){
 assert(argc==2||argc==3);void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(lib);
 IfsFsModuleApiEntryV2 get=(IfsFsModuleApiEntryV2)dlsym(lib,IFS_FS_COMMON_EXPORT_V2);assert(get);
 const IfsFsModuleApiV2*a=get();assert(ifs_fs_api_v2_valid(a,"jfs"));
 const IfsFsCoreOpsV2*core=(const void*)ifs_fs_group_v2(a,IFS_FS_GROUP_CORE);
 assert(core&&core->mount&&core->read&&!core->write&&!ifs_fs_group_v2(a,IFS_FS_GROUP_DEFRAG));
 struct context c={.fd=-1,.bytes=SIZE};
 if(argc==3){struct stat st;c.fd=open(argv[2],O_RDONLY);assert(c.fd>=0&&!fstat(c.fd,&st)&&st.st_size>0);c.bytes=(uint64_t)st.st_size;}
 else {image=calloc(1,SIZE);assert(image);}
 IfsFsHostRuntimeV2 h={.abi_version=2,.structure_bytes=sizeof(h),.capabilities=IFS_FS_HOST_READ_ONLY,.volume_bytes=c.bytes,.maximum_transfer_bytes=4096,.context=&c,.read_at=rd,.allocate=allocate,.release=release};
 for(c.sector=512;c.sector<=4096;c.sector*=8){
  h.logical_sector_bytes=h.physical_sector_bytes=c.sector;
  if(argc==3){
   expect(core,&h,&c,IFS_FS_OK);
   void *v=NULL;IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
   assert(core->mount(&h,&v,&info)==IFS_FS_ACCESS&&!v);
   h.capabilities|=IFS_FS_HOST_EXCLUSIVE;
   int rc=core->mount(&h,&v,&info);
   if(rc)fprintf(stderr,"native JFS mount failed: %d\n",rc);
   assert(rc==0&&v&&info.block_bytes==4096&&info.free_bytes>0);
   char name[256];IfsFsEntryV2 e={.structure_bytes=sizeof(e),.name=name,.name_capacity=sizeof(name)};
   assert(core->stat(v,"/",&e)==0&&e.type==IFS_FS_ENTRY_DIRECTORY);
   assert(core->directory_entry(v,"/",0,&e)==IFS_FS_END);
   assert(core->stat(v,"/absent",&e)==IFS_FS_NOT_FOUND);
   assert(core->unmount(v)==0&&!c.allocs);h.capabilities&=~IFS_FS_HOST_EXCLUSIVE;
   continue;
  }
  memset(image,0,SIZE);fixture();expect(core,&h,&c,IFS_FS_OK);
  c.fail=1;expect(core,&h,&c,IFS_FS_IO_ERROR);c.fail=0;
  corrupt();expect(core,&h,&c,IFS_FS_CORRUPT);
  memset(image,0,SIZE);fixture();absent();expect(core,&h,&c,IFS_FS_NOT_FOUND);
  reader_tests(core,&h,&c);
 }
 if(c.fd>=0)assert(!close(c.fd));
 free(image);assert(!dlclose(lib));
 puts("JFS native recognition, offline namespace, sparse payload and corruption refusal PASS");return 0;
}
