import sys, textwrap, pathlib
from PIL import Image, ImageDraw, ImageFont
src, dst = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
S=2; COLS=118
reg=ImageFont.truetype(r"C:\Windows\Fonts\consola.ttf",14*S); bold=ImageFont.truetype(r"C:\Windows\Fonts\consolab.ttf",14*S)
ui=ImageFont.truetype(r"C:\Windows\Fonts\segoeui.ttf",13*S)
lines=src.read_text(encoding="utf-8").rstrip("\n").split("\n")
while lines and not lines[-1].strip(): lines.pop()
rows=[]
for l in lines:
    l=l.replace("\t","    ")
    prompt=l.startswith("student@ubuntu:")
    parts=textwrap.wrap(l,COLS,drop_whitespace=False,replace_whitespace=False) or [""]
    for i,p in enumerate(parts): rows.append((p, prompt and i==0, prompt))
cw=reg.getlength("M"); lh=int(19*S); pad=14*S; bar=30*S
W=int(pad*2+cw*COLS); H=int(bar+pad*2+lh*len(rows))
im=Image.new("RGB",(W,H),"#300a24"); d=ImageDraw.Draw(im)
d.rectangle([0,0,W,bar],fill="#2c2c2c")
for i,c in enumerate(["#ff5f57","#febc2e","#28c840"]): x=pad+i*20*S; d.ellipse([x,bar/2-6*S,x+12*S,bar/2+6*S],fill=c)
t="student@ubuntu: ~/FUSE"; tw=d.textlength(t,font=ui); d.text(((W-tw)/2,bar/2-9*S),t,font=ui,fill="#d0d0d0")
y=bar+pad
for text,first,isprompt in rows:
    x=pad
    if first:
        a="student@ubuntu"; d.text((x,y),a,font=bold,fill="#8ae234"); x+=cw*len(a)
        d.text((x,y),":",font=reg,fill="#eeeeec"); x+=cw
        b="~/FUSE"; d.text((x,y),b,font=bold,fill="#729fcf"); x+=cw*len(b)
        rest=text[len("student@ubuntu:~/FUSE"):]; d.text((x,y),rest,font=reg,fill="#ffffff")
    else:
        col="#ffffff" if isprompt else "#e3e3e3"
        if any(k in text for k in ("passed","identical","exit status: 0")): col="#8ae234"
        if any(k in text for k in ("cannot","failed","Usage","must be","exists","too long","No such","not empty","Invalid")): col="#f2a2a2"
        d.text((x,y),text,font=reg,fill=col)
    y+=lh
im.save(dst,optimize=True); print(dst.name, im.size)
