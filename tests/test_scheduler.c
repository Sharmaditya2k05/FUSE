#include "scheduler.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint64_t check(sched_policy_t p,const uint32_t*expected){uint32_t blocks[]={98,183,37,122,14,124,65,67};disk_request_t r[8];disk_request_t*items[8];struct timespec now={1,0};for(size_t i=0;i<8;++i){memset(&r[i],0,sizeof(r[i]));r[i].id=i+1;r[i].block=blocks[i];r[i].arrival.tv_nsec=(long)i;items[i]=&r[i];}scheduler_t s={p,200,53,-1,.7,.3};size_t n=8;uint64_t movement=0;for(size_t step=0;step<8;++step){size_t k=scheduler_pick(&s,items,n,now);assert(items[k]->block==expected[step]);uint32_t old=s.head,next=items[k]->block;if(p==SCHED_SCAN&&s.scan_direction<0&&next>old){movement+=(uint64_t)old+next;s.scan_direction=1;}else movement+=old>next?old-next:next-old;s.head=next;memmove(&items[k],&items[k+1],(--n-k)*sizeof(*items));}return movement;}
int main(void){uint32_t f[]={98,183,37,122,14,124,65,67},s[]={65,67,37,14,98,122,124,183},scan[]={37,14,65,67,98,122,124,183};assert(check(SCHED_FCFS,f)==640);assert(check(SCHED_SSTF,s)==236);assert(check(SCHED_SCAN,scan)==236);puts("scheduler tests passed");return 0;}
