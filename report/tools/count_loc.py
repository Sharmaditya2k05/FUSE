"""Count qualifying source lines in the SchedFS repository.

Method: every C source/header, shell script, Python script and the Makefile
under the repository (excluding .git, docs/, results/, report/ and generated
files) is read; blank lines and comment-only lines are excluded. For C files,
/* ... */ block comments are removed first and lines starting with // are
skipped. For Python, shell and Make, lines starting with # are skipped.

Usage (from the repository root): python report/tools/count_loc.py .
"""
import pathlib, re, sys

def count(p):
    s = p.read_text(encoding="utf-8", errors="replace")
    if p.suffix in (".c", ".h"):
        s = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group().count("\n"), s, flags=re.S)
        keep = [l for l in s.splitlines() if l.strip() and not l.strip().startswith("//")]
    else:
        keep = [l for l in s.splitlines() if l.strip() and not l.strip().startswith("#")]
    return len(keep), len(p.read_text(encoding="utf-8", errors="replace").splitlines())

root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".")
total = raw_total = 0
for p in sorted(root.rglob("*")):
    rel = p.relative_to(root).parts
    if ".git" in rel or (rel and rel[0] in ("docs", "results", "report")) or not p.is_file():
        continue
    if p.suffix in (".c", ".h", ".py", ".sh") or p.name == "Makefile":
        n, raw = count(p)
        total += n
        raw_total += raw
        print(f"{n:5d} {raw:5d}  {p.relative_to(root).as_posix()}")
print(f"qualifying lines: {total}   physical lines: {raw_total}")
