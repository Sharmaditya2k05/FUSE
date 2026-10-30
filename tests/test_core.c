#include "filesystem.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { sfs_t *fs; int id; int rc; } worker_arg_t;
static void *worker(void *opaque) {
    worker_arg_t *a=opaque;char path[32];snprintf(path,sizeof(path),"/thread-%d",a->id);
    a->rc=sfs_create(a->fs,path,0644,NULL);if(a->rc)return NULL;
    size_t n=64u*1024u;unsigned char *data=malloc(n);if(!data){a->rc=-1;return NULL;}
    memset(data,'A'+a->id,n);ssize_t written=sfs_write(a->fs,path,data,n,0);if(written!=(ssize_t)n)a->rc=written<0?(int)written:-1;free(data);return NULL;
}
static void verify(sfs_t *fs,int id){char path[32];snprintf(path,sizeof(path),"/thread-%d",id);size_t n=64u*1024u;unsigned char *data=malloc(n);assert(data);assert(sfs_read(fs,path,data,n,0)==(ssize_t)n);for(size_t i=0;i<n;++i)assert(data[i]==(unsigned char)('A'+id));free(data);}
int main(int argc,char **argv){if(argc!=2){fprintf(stderr,"usage: %s image\n",argv[0]);return 2;}sfs_t fs;assert(sfs_mount_image(&fs,argv[1],SCHED_SSTF,.7,.3,false)==0);struct stat st;assert(sfs_getattr(&fs,"/",&st)==0&&S_ISDIR(st.st_mode));assert(sfs_mkdir(&fs,"/docs",0755)==0);assert(sfs_create(&fs,"/docs/hello",0644,NULL)==0);const char text[]="persistent scheduled I/O";assert(sfs_write(&fs,"/docs/hello",text,sizeof(text),0)==(ssize_t)sizeof(text));char got[sizeof(text)];assert(sfs_read(&fs,"/docs/hello",got,sizeof(got),0)==(ssize_t)sizeof(got));assert(!memcmp(text,got,sizeof(text)));assert(sfs_truncate(&fs,"/docs/hello",20000)==0);memset(got,1,sizeof(got));assert(sfs_read(&fs,"/docs/hello",got,sizeof(got),4096)==(ssize_t)sizeof(got));for(size_t i=0;i<sizeof(got);++i)assert(got[i]==0);
    pthread_t threads[4];worker_arg_t args[4];for(int i=0;i<4;++i){args[i]=(worker_arg_t){&fs,i,0};assert(!pthread_create(&threads[i],NULL,worker,&args[i]));}for(int i=0;i<4;++i){pthread_join(threads[i],NULL);assert(args[i].rc==0);verify(&fs,i);}sfs_unmount_image(&fs);
    assert(sfs_mount_image(&fs,argv[1],SCHED_ADAPTIVE,.7,.3,false)==0);assert(sfs_read(&fs,"/docs/hello",got,sizeof(got),0)==(ssize_t)sizeof(got));assert(!memcmp(text,got,sizeof(text)));for(int i=0;i<4;++i)verify(&fs,i);assert(sfs_unlink(&fs,"/docs/hello")==0);assert(sfs_rmdir(&fs,"/docs")==0);for(int i=0;i<4;++i){char p[32];snprintf(p,sizeof(p),"/thread-%d",i);assert(sfs_unlink(&fs,p)==0);}sfs_unmount_image(&fs);puts("core persistence and concurrency tests passed");return 0;}
