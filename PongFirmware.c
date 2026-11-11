/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <sys/vnode_if.h>
#include <sys/mount.h>
#include <IOKit/IOLib.h>
#include <stdbool.h>
int sheng_fw_open(const char *path,void **handle,uint64_t *size) {
 const bool efs=!strcmp(path,"/pong/firmware/efs/modemst1.bin")||
     !strcmp(path,"/pong/firmware/efs/modemst2.bin")||
     !strcmp(path,"/pong/firmware/efs/fsg.bin")||!strcmp(path,"/pong/firmware/efs/fsc.bin");
 if(!efs&&strcmp(path,"/pong/firmware/adsp.mbn")&&strcmp(path,"/pong/firmware/modem.mbn")&&strcmp(path,"/pong/firmware/ipa_fws.mbn"))return EINVAL;
 vnode_t vnode=NULLVP;vfs_context_t ctx=vfs_context_kernel();
 int rc=vnode_open(path,FREAD,0,0,&vnode,ctx);if(rc)return rc;
 struct vnode_attr va;VATTR_INIT(&va);VATTR_WANTED(&va,va_data_size);
 rc=vnode_getattr(vnode,&va,ctx);
 uint64_t limit=efs?4*1024*1024:!strcmp(path,"/pong/firmware/modem.mbn")?128*1024*1024:
     !strcmp(path,"/pong/firmware/ipa_fws.mbn")?512*1024:64*1024*1024;
 if(rc||!VATTR_IS_SUPPORTED(&va,va_data_size)||!va.va_data_size||va.va_data_size>limit){vnode_close(vnode,FREAD,ctx);return rc?rc:EFTYPE;}
 *handle=vnode;*size=va.va_data_size;return 0;
}
int sheng_fw_read(void *handle,uint64_t offset,void *to,size_t length) {
 if(!handle||length>1024*1024||offset>128*1024*1024||length>128*1024*1024-offset)return EINVAL;
 vfs_context_t ctx=vfs_context_kernel();int resid=(int)length;
 int rc=vn_rdwr(UIO_READ,(vnode_t)handle,(caddr_t)to,(int)length,(off_t)offset,UIO_SYSSPACE,IO_NOAUTH,vfs_context_ucred(ctx),&resid,vfs_context_proc(ctx));
 return rc?rc:resid?EIO:0;
}
void sheng_fw_close(void *handle) {if(handle)vnode_close((vnode_t)handle,FREAD,vfs_context_kernel());}

/* Actual modem RFS files, separate from the PAS firmware input whitelist.
 * Firmware is read-only; writes go to a private native Data directory.
 * TFTP has no directory operations, so encode readwrite names flat to
 * preserve each logical path without creating caller-controlled parents.
 */
bool pong_rfs_ready(void) {
 vnode_t vnode=NULLVP;vfs_context_t ctx=vfs_context_kernel();
 int rc=vnode_lookup("/private/var/root/Library/PongRFS/.ready",VNODE_LOOKUP_NOFOLLOW_ANY,&vnode,ctx);
 if(rc)return false;
 const bool ready=vnode_vtype(vnode)==VREG;vnode_put(vnode);return ready;
}
int pong_rfs_open(const char *relative,bool writable,void **handle,uint64_t *size) {
 if(!relative||!handle||!size)return EINVAL;
 *handle=NULL;*size=0;
 const char *name=relative;size_t length=strnlen(name,160);
 if(!length||length>=160||name[0]=='/')return EINVAL;
 for(size_t at=0;at<length;) {
  size_t end=at;while(end<length&&name[end]!='/') {
   unsigned char c=name[end];if(c<33||c>126||c=='\\')return EINVAL;++end;
  }
  if(end==at||(end-at==1&&name[at]=='.')||(end-at==2&&name[at]=='.'&&name[at+1]=='.'))return EINVAL;
  at=end+1;
 }
 char path[256];
 bool data=!strncmp(name,"readwrite/",10);
 if(data) {
  name+=10;length-=10;if(!length||length>96)return EINVAL;
  static const char base[]="/private/var/root/Library/PongRFS/",hex[]="0123456789abcdef";
  memcpy(path,base,sizeof base-1);
  for(size_t i=0;i<length;i++){path[sizeof base-1+2*i]=hex[(unsigned char)name[i]>>4];path[sizeof base+2*i]=hex[name[i]&15];}
  path[sizeof base-1+2*length]=0;
 } else {
  if(writable||strncmp(name,"firmware/",9))return EACCES;
  name+=9;if(!*name)return EINVAL;
  if(snprintf(path,sizeof path,"/pong/firmware/rfs/%s",name)>=(int)sizeof path)return ENAMETOOLONG;
 }
 int flags=writable?(FREAD|FWRITE|O_CREAT):FREAD;
 vnode_t vnode=NULLVP;vfs_context_t ctx=vfs_context_kernel();
 // vnode_open translates NOFOLLOW_ANY into O_NOFOLLOW_ANY. Darwin
 // explicitly rejects combining it with O_NOFOLLOW (vn_open_auth).
 int rc=vnode_open(path,flags,0600,VNODE_LOOKUP_NOFOLLOW_ANY,&vnode,ctx);if(rc)return rc;
 struct vnode_attr va;VATTR_INIT(&va);VATTR_WANTED(&va,va_data_size);VATTR_WANTED(&va,va_type);
 rc=vnode_getattr(vnode,&va,ctx);
 if(rc||!VATTR_IS_SUPPORTED(&va,va_data_size)||!VATTR_IS_SUPPORTED(&va,va_type)||va.va_type!=VREG||
    va.va_data_size>(data?8ULL*1024*1024:128ULL*1024*1024)) {
  vnode_close(vnode,flags&(FREAD|FWRITE),ctx);return rc?rc:EFTYPE;
 }
 *handle=vnode;*size=va.va_data_size;return 0;
}
int pong_rfs_read(void *handle,uint64_t offset,void *to,uint32_t length,uint32_t *actual) {
 if(!handle||!to||!actual||length>8192||offset>128ULL*1024*1024||length>128ULL*1024*1024-offset)return EINVAL;
 vfs_context_t ctx=vfs_context_kernel();int resid=(int)length;
 int rc=vn_rdwr(UIO_READ,(vnode_t)handle,(caddr_t)to,(int)length,(off_t)offset,UIO_SYSSPACE,IO_NOAUTH,vfs_context_ucred(ctx),&resid,vfs_context_proc(ctx));
 if(resid<0||resid>(int)length)return EIO;
 *actual=length-resid;return rc;
}
int pong_rfs_write(void *handle,uint64_t offset,const void *data,uint32_t length,bool final) {
 if(!handle||!data||length>8192||offset>8ULL*1024*1024||length>8ULL*1024*1024-offset)return EINVAL;
 vfs_context_t ctx=vfs_context_kernel();int resid=(int)length;
 int rc=vn_rdwr(UIO_WRITE,(vnode_t)handle,(caddr_t)data,(int)length,(off_t)offset,UIO_SYSSPACE,IO_NOAUTH,vfs_context_ucred(ctx),&resid,vfs_context_proc(ctx));
 if(!rc&&resid)rc=EIO;
 if(!rc&&final)rc=VNOP_FSYNC((vnode_t)handle,MNT_WAIT,ctx);
 return rc;
}
void pong_rfs_close(void *handle,bool writable) {
 if(handle)vnode_close((vnode_t)handle,writable?(FREAD|FWRITE):FREAD,vfs_context_kernel());
}
