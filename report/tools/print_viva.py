"""Print the SchedFS viva guide HTML to an A4 PDF with a centred page footer."""
import pathlib
import sys

from playwright.sync_api import sync_playwright

src = pathlib.Path(sys.argv[1]).resolve()
out = pathlib.Path(sys.argv[2]).resolve()
footer = ('<div style="width:100%;text-align:center;font-family:Arial,sans-serif;'
          'font-size:7px;color:#8f8780;">SchedFS Viva Guide &middot; '
          '<span class="pageNumber"></span> / <span class="totalPages"></span></div>')

with sync_playwright() as p:
    browser = p.chromium.launch(channel="chrome")
    page = browser.new_page()
    page.goto(src.as_uri(), wait_until="networkidle")
    page.evaluate("document.fonts.ready")
    page.pdf(path=str(out), format="A4", print_background=True,
             display_header_footer=True, header_template="<div></div>",
             footer_template=footer,
             margin={"top": "18mm", "bottom": "18mm", "left": "17mm", "right": "17mm"},
             prefer_css_page_size=False)
    browser.close()
print("written", out)
