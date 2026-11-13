"""Report statistics: word counts, figures, tables, listings and test cases.

Usage: python check_report.py OS_Project_Report.tex
Counts words in the Abstract and the Scope narrative (Section 4 text before
the 4(a) subsection), and counts figures, tables, listings and test cases.
"""
import re
import sys

s = open(sys.argv[1], encoding="utf-8").read()


def plain_words(t):
    t = "\n".join(l for l in t.splitlines() if not l.lstrip().startswith("%"))
    t = re.sub(r"\\cite\{[^}]*\}", "", t)
    t = re.sub(r"\\(?:code|texttt|emph|textbf)\{([^}]*)\}", r"\1", t)
    t = re.sub(r"\\[a-zA-Z]+\*?", " ", t)
    t = re.sub(r"[{}~$]", " ", t).replace("---", " ")
    return len([w for w in t.split() if re.search(r"[A-Za-z0-9]", w)])


abstract = s.split(r"\section{ABSTRACT OF THE PROJECT}")[1].split(r"\section{SCOPE OF THE PROJECT}")[0]
scope = s.split(r"\section{SCOPE OF THE PROJECT}")[1].split(r"\renewcommand{\thesubsection}")[0]
print("abstract words:", plain_words(abstract))
print("scope narrative words:", plain_words(scope))
print("figure labels:", len(re.findall(r"\\label\{fig:", s)))
print("tikzpicture environments:", s.count(r"\begin{tikzpicture}"))
print("includegraphics:", s.count(r"\includegraphics"))
print("listings:", s.count(r"\lstinputlisting") + s.count(r"\begin{lstlisting}"))
print("test cases:", len(re.findall(r"^TC\d\d &", s, flags=re.M)))
print("functional requirements:", len(re.findall(r"^FR\d+ &", s, flags=re.M)))
print("non-functional requirements:", len(re.findall(r"^NFR\d+ &", s, flags=re.M)))
print("modules:", len(re.findall(r"^\\moddesc\{M\d+", s, flags=re.M)))
