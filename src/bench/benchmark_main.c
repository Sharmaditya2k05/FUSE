#include "scheduler.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

typedef struct {const char*name;const uint32_t*blocks;size_t count;} workload_t;
static const uint32_t seq[]={10,11,12,13,14,15,16,17};
static const uint32_t random_w[]={145,12,98,176,32,67,4};
static const uint32_t clustered[]={90,93,96,101,104,108,111};
static const uint32_t mixed[]={10,11,12,145,13,14,67,15,16,176,17};
static const uint32_t concurrent_w[]={300,12,900,13,1500,14,2200,15,3010,16,44,17,1888,18,4000,19};
static double ms(struct timespec a,struct timespec b){return (double)(b.tv_sec-a.tv_sec)*1000.0+(double)(b.tv_nsec-a.tv_nsec)/1e6;}
static void now_time(struct timespec *t){
#ifdef _WIN32
    FILETIME ft; ULARGE_INTEGER u; GetSystemTimeAsFileTime(&ft);
    u.LowPart=ft.dwLowDateTime;u.HighPart=ft.dwHighDateTime;
    uint64_t ns=(u.QuadPart-116444736000000000ULL)*100ULL;
    t->tv_sec=(time_t)(ns/1000000000ULL);t->tv_nsec=(long)(ns%1000000000ULL);
#else
    clock_gettime(CLOCK_MONOTONIC,t);
#endif
}
static void seek_delay(uint64_t blocks){
    struct timespec start,current;now_time(&start);
    double target_ms=.010+(double)blocks*.002;
    do{now_time(&current);}while(ms(start,current)<target_ms);
}
static void run(FILE*out,const workload_t*w,sched_policy_t p){disk_request_t req[64];disk_request_t*pending[64];struct timespec base,now;now_time(&base);for(size_t i=0;i<w->count;++i){memset(&req[i],0,sizeof(req[i]));req[i].id=i+1;req[i].block=w->blocks[i];req[i].arrival=base;req[i].arrival.tv_nsec+=(long)i*1000L;while(req[i].arrival.tv_nsec>=1000000000L){req[i].arrival.tv_sec++;req[i].arrival.tv_nsec-=1000000000L;}pending[i]=&req[i];}
    scheduler_t s={p,4096,53,-1,.7,.3};uint64_t seek=0;double sum=0,max=0,sq=0;size_t n=w->count;now_time(&now);
    while(n){size_t k=scheduler_pick(&s,pending,n,now);uint32_t next=pending[k]->block;uint64_t move=s.head>next?s.head-next:next-s.head;if(p==SCHED_SCAN&&s.scan_direction<0&&next>s.head){move=(uint64_t)s.head+next;s.scan_direction=1;}else if(p==SCHED_SCAN&&s.scan_direction>0&&next<s.head){move=(uint64_t)(4095-s.head)+(4095-next);s.scan_direction=-1;}seek+=move;s.head=next;double wait=ms(pending[k]->arrival,now);if(wait<0)wait=0;sum+=wait;sq+=wait*wait;if(wait>max)max=wait;memmove(&pending[k],&pending[k+1],(n-k-1)*sizeof(*pending));n--;seek_delay(move);now_time(&now);}
    double elapsed=ms(base,now);if(elapsed<=0)elapsed=.001;double count=(double)w->count;double avg=sum/count,var=sq/count-avg*avg;if(var<0)var=0;fprintf(out,"%s,%s,%lu,%.3f,%.6f,%.6f,%.6f,%lu,%.6f,%.3f\n",w->name,scheduler_name(p),(unsigned long)seek,(double)seek/count,avg,max,sqrt(var),(unsigned long)w->count,elapsed,count/(elapsed/1000.0));}
int main(int argc,char**argv){if(argc!=2||strcmp(argv[1],"--all")){fprintf(stderr,"Usage: %s --all\n",argv[0]);return 2;}workload_t ws[]={{"sequential",seq,sizeof(seq)/sizeof(seq[0])},{"random",random_w,sizeof(random_w)/sizeof(random_w[0])},{"clustered",clustered,sizeof(clustered)/sizeof(clustered[0])},{"mixed",mixed,sizeof(mixed)/sizeof(mixed[0])},{"concurrent",concurrent_w,sizeof(concurrent_w)/sizeof(concurrent_w[0])}};FILE*f=fopen("benchmark.csv","w");if(!f){perror("benchmark.csv");return 1;}fputs("workload,policy,total_seek,average_seek,average_wait_ms,maximum_wait_ms,wait_stddev_ms,total_requests,elapsed_ms,throughput_rps\n",f);for(size_t i=0;i<sizeof(ws)/sizeof(ws[0]);++i)for(int p=SCHED_FCFS;p<=SCHED_ADAPTIVE;++p)run(f,&ws[i],(sched_policy_t)p);fclose(f);puts("Wrote benchmark.csv (measured scheduler replay; no artificial results).\nUse scripts/plot_results.py to generate graphs.");return 0;}
