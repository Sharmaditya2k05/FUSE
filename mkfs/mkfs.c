#include "layout.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int parse_size(const char *s, uint64_t *out) {
    char *end; errno=0; unsigned long long n=strtoull(s,&end,10);
    if(errno||end==s||n==0)return -1;
    uint64_t mul=1;if(!strcmp(end,"M")||!strcmp(end,"MiB"))mul=1024u*1024u;
    else if(!strcmp(end,"G")||!strcmp(end,"GiB"))mul=1024u*1024u*1024u;
    else if(*end)return -1;
    if(n>UINT64_MAX/mul)return -1;
    *out=(uint64_t)n*mul;
    return 0;
}
static void setbit(uint8_t *b,uint32_t n){b[n/8u]|=(uint8_t)(1u<<(n%8u));}
static int write_full(int fd,const void *buf,size_t n,off_t off){size_t done=0;while(done<n){ssize_t rc=pwrite(fd,(const char*)buf+done,n-done,off+(off_t)done);if(rc<0&&errno==EINTR)continue;if(rc<=0)return -1;done+=(size_t)rc;}return 0;}
int main(int argc,char **argv){
    if(argc!=3){fprintf(stderr,"Usage: %s IMAGE SIZE (example: disk.img 16M)\n",argv[0]);return 2;}
    uint64_t bytes;if(parse_size(argv[2],&bytes)||bytes%SFS_BLOCK_SIZE){fprintf(stderr,"size must be a positive multiple of 4096 (M/MiB/G/GiB accepted)\n");return 2;}
    uint64_t blocks64=bytes/SFS_BLOCK_SIZE;if(blocks64<SFS_MIN_BLOCKS||blocks64>UINT32_MAX){fprintf(stderr,"unsupported image size\n");return 2;}uint32_t blocks=(uint32_t)blocks64;
    uint32_t bitmap_blocks=(blocks+SFS_BLOCK_SIZE*8u-1u)/(SFS_BLOCK_SIZE*8u),inode_blocks=(SFS_DEFAULT_INODES*(uint32_t)sizeof(sfs_inode_t)+SFS_BLOCK_SIZE-1u)/SFS_BLOCK_SIZE;
    uint32_t data_start=1u+bitmap_blocks+inode_blocks;if(data_start>=blocks){fprintf(stderr,"image too small for metadata\n");return 2;}
    int fd=open(argv[1],O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0644);if(fd<0){perror("open (remove an old image explicitly before formatting)");return 1;}
    if(ftruncate(fd,(off_t)bytes)){perror("ftruncate");close(fd);unlink(argv[1]);return 1;}
    uint8_t block[SFS_BLOCK_SIZE]={0};sfs_superblock_t sb={0};sb.magic=SFS_MAGIC;sb.version=SFS_VERSION;sb.block_size=SFS_BLOCK_SIZE;sb.total_blocks=blocks;sb.total_inodes=SFS_DEFAULT_INODES;sb.bitmap_start=1;sb.bitmap_blocks=bitmap_blocks;sb.inode_start=1u+bitmap_blocks;sb.inode_blocks=inode_blocks;sb.data_start=data_start;sb.root_inode=SFS_ROOT_INO;sb.free_blocks=blocks-data_start-1u;sb.free_inodes=SFS_DEFAULT_INODES-1u;memcpy(block,&sb,sizeof(sb));
    if(write_full(fd,block,sizeof(block),0)){perror("superblock");goto fail;}
    uint8_t *bitmap=calloc(bitmap_blocks,SFS_BLOCK_SIZE);if(!bitmap)goto fail;for(uint32_t b=0;b<=data_start;++b)setbit(bitmap,b);
    if(write_full(fd,bitmap,(size_t)bitmap_blocks*SFS_BLOCK_SIZE,(off_t)SFS_BLOCK_SIZE)){perror("bitmap");free(bitmap);goto fail;}free(bitmap);
    memset(block,0,sizeof(block));sfs_inode_t root={0};root.mode=(uint16_t)(S_IFDIR|0755);root.links=2;root.uid=(uint32_t)getuid();root.gid=(uint32_t)getgid();root.size=2u*sizeof(sfs_dirent_t);root.atime=root.mtime=root.ctime=(uint64_t)time(NULL);root.direct[0]=data_start;memcpy(block,&root,sizeof(root));
    if(write_full(fd,block,sizeof(block),(off_t)sb.inode_start*SFS_BLOCK_SIZE)){perror("inode table");goto fail;}
    memset(block,0,sizeof(block));sfs_dirent_t *e=(sfs_dirent_t*)block;e[0].ino=e[1].ino=SFS_ROOT_INO;strcpy(e[0].name,".");strcpy(e[1].name,"..");if(write_full(fd,block,sizeof(block),(off_t)data_start*SFS_BLOCK_SIZE)){perror("root directory");goto fail;}
    if(fsync(fd)){perror("fsync");goto fail;}close(fd);printf("Formatted %s: %u blocks, %u inodes, data starts at block %u\n",argv[1],blocks,SFS_DEFAULT_INODES,data_start);return 0;
fail: close(fd);unlink(argv[1]);return 1;
}
