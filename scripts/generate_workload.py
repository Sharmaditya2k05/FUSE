#!/usr/bin/env python3
import argparse, random
p=argparse.ArgumentParser();p.add_argument("--kind",choices=["sequential","random","clustered","mixed"],default="random");p.add_argument("--count",type=int,default=64);p.add_argument("--blocks",type=int,default=4096);p.add_argument("--seed",type=int,default=312);a=p.parse_args();r=random.Random(a.seed)
if a.kind=="sequential": data=list(range(10,10+a.count))
elif a.kind=="random": data=[r.randrange(a.blocks) for _ in range(a.count)]
elif a.kind=="clustered":
    center=r.randrange(a.blocks);data=[max(0,min(a.blocks-1,center+r.randrange(-16,17))) for _ in range(a.count)]
else: data=[10+i if i%8 else r.randrange(a.blocks) for i in range(a.count)]
print("\n".join(map(str,data)))
