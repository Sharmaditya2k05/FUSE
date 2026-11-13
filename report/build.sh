#!/bin/sh
# Rebuild OS_Project_Report.pdf from OS_Project_Report.tex (pdfLaTeX + BibTeX).
set -e
cd "$(dirname "$0")"
pdflatex -interaction=nonstopmode OS_Project_Report.tex >/dev/null
bibtex OS_Project_Report >/dev/null
pdflatex -interaction=nonstopmode OS_Project_Report.tex >/dev/null
pdflatex -interaction=nonstopmode OS_Project_Report.tex
