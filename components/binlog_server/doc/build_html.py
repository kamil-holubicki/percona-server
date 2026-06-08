#!/usr/bin/env python3
"""
Convert components/binlog_server/doc/md/*.md to components/binlog_server/doc/html/*.html.

Design notes:

* Uses the standard `markdown` package with `tables`, `fenced_code`,
  `codehilite`, and `toc` extensions.
* `` ```mermaid ... ``` `` blocks are passed through to the HTML as
  `<div class="mermaid">...</div>` so the Mermaid runtime (loaded
  client-side from a CDN) renders them in-page.
* Inter-document links of the form `xxx.md` are rewritten to
  `xxx.html` so the rendered docs cross-link correctly.
* The output is wrapped in a tiny self-contained shell (sidebar nav +
  content) that pulls `style.css` and the Mermaid CDN script.
"""

from __future__ import annotations

import hashlib
import html
import re
from pathlib import Path
from typing import Iterable

import markdown


HERE = Path(__file__).resolve().parent
SRC = HERE / "md"
OUT = HERE / "html"

PAGES = [
    ("index.md",        "Binlog Server",      "Documentation home"),
    ("overview.md",     "Overview",           "What it is, why it exists, who it's for"),
    ("user_guide.md",   "User guide",         "Installation, syntax, day-2 operations"),
    ("architecture.md", "Architecture",       "Components, data flow, threading, scalability"),
    ("design.md",       "Low-level design",   "Locking, on-disk format, internals, error codes"),
]

MERMAID_FENCE = re.compile(
    r"^```mermaid\s*\n(?P<body>.*?)\n```\s*$",
    re.MULTILINE | re.DOTALL,
)

HTML_HREF_MD_REWRITE = re.compile(
    r'(?P<pre>href=")(?P<name>[a-zA-Z0-9_]+)\.md(?P<frag>#[^"]+)?(?P<post>")'
)


def extract_mermaid_blocks(src: str) -> tuple[str, list[str]]:
    """
    Replace each ```mermaid ... ``` block with a placeholder; return the
    transformed source and the list of block bodies in order.
    """
    blocks: list[str] = []

    def _capture(m: re.Match) -> str:
        idx = len(blocks)
        blocks.append(m.group("body"))
        return f'<div class="__mermaid_placeholder__" data-idx="{idx}"></div>'

    return MERMAID_FENCE.sub(_capture, src), blocks


def reinject_mermaid(html_body: str, blocks: list[str]) -> str:
    """
    Replace each placeholder with the actual <div class="mermaid">.
    """

    def _swap(m: re.Match) -> str:
        idx = int(m.group("idx"))
        body = blocks[idx]
        return (
            f'<div class="diagram">\n'
            f'<div class="mermaid">\n{body}\n</div>\n'
            f'</div>'
        )

    return re.sub(
        r'<div class="__mermaid_placeholder__" data-idx="(?P<idx>\d+)"></div>',
        _swap,
        html_body,
    )


def rewrite_md_links(html_body: str) -> str:
    """
    Rewrite href="xxx.md" / href="xxx.md#anchor" to href="xxx.html".
    """

    def _swap(m: re.Match) -> str:
        name = m.group("name")
        frag = m.group("frag") or ""
        return f'{m.group("pre")}{name}.html{frag}{m.group("post")}'

    return HTML_HREF_MD_REWRITE.sub(_swap, html_body)


def make_sidebar(current_md: str, pages: Iterable[tuple[str, str, str]]) -> str:
    items = []
    for md_name, title, tagline in pages:
        href = md_name.replace(".md", ".html")
        cls = ' class="current"' if md_name == current_md else ""
        items.append(
            f'      <li><a href="{href}"{cls}>{html.escape(title)}</a></li>'
        )
    return "\n".join(items)


SHELL_TEMPLATE = """<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{title} &middot; Binlog Server documentation</title>
  <link rel="stylesheet" href="style.css?v={css_version}">
  <script src="https://cdn.jsdelivr.net/npm/mermaid@10/dist/mermaid.min.js"></script>
  <script>
    document.addEventListener("DOMContentLoaded", function () {{
      if (window.mermaid) {{
        window.mermaid.initialize({{
          startOnLoad: true,
          theme: "default",
          themeVariables: {{
            fontFamily: '-apple-system, BlinkMacSystemFont, "Segoe UI", "Helvetica Neue", Arial, system-ui',
            primaryColor: "#fff8e1",
            primaryBorderColor: "#f9a825",
            primaryTextColor: "#5d4037",
            lineColor: "#7b8a9b",
            secondaryColor: "#e3f2fd",
            tertiaryColor: "#e8f5e9"
          }},
          flowchart: {{
            htmlLabels: true,
            curve: "basis",
            useMaxWidth: false,
            nodeSpacing: 60,
            rankSpacing: 70,
            padding: 14
          }},
          sequence: {{
            useMaxWidth: false,
            actorMargin: 70,
            boxMargin: 12,
            messageMargin: 36
          }},
          classDiagram: {{ useMaxWidth: false }},
          stateDiagram: {{ useMaxWidth: false }},
          gantt: {{ useMaxWidth: false }}
        }});
      }}
    }});
  </script>
</head>
<body>
<div class="layout">
  <aside class="nav">
    <a href="index.html" class="brand">Binlog Server</a>
    <div class="tagline">Percona Server 9.6 binlog collector: receive and persist.</div>
    <div class="navhead">Documentation</div>
    <ul>
{sidebar}
    </ul>
    <div class="navhead">Source</div>
    <ul>
      <li><a href="../../">components/binlog_server/</a></li>
      <li><a href="../md/">markdown sources</a></li>
    </ul>
  </aside>
  <main>
{content}
    <footer class="docfoot">
      <span>Binlog Server documentation</span>
      <span>Percona Server 9.6 &middot; GPL v2</span>
    </footer>
  </main>
</div>
</body>
</html>
"""


def compute_css_version() -> str:
    css_path = OUT / "style.css"
    if not css_path.exists():
        return "0"
    return hashlib.sha1(css_path.read_bytes()).hexdigest()[:10]


def render_one(md_name: str, title: str, css_version: str) -> str:
    src = (SRC / md_name).read_text(encoding="utf-8")
    src_no_mermaid, blocks = extract_mermaid_blocks(src)

    md = markdown.Markdown(
        extensions=["tables", "fenced_code", "codehilite", "toc", "sane_lists"],
        extension_configs={
            "codehilite": {
                "css_class": "codehilite",
                "guess_lang": False,
                "noclasses": False,
            },
            "toc": {"permalink": "&#x00B6;"},
        },
        output_format="html",
    )
    body = md.convert(src_no_mermaid)
    body = reinject_mermaid(body, blocks)
    body = rewrite_md_links(body)

    sidebar = make_sidebar(md_name, PAGES)
    return SHELL_TEMPLATE.format(
        title=html.escape(title),
        sidebar=sidebar,
        content=body,
        css_version=css_version,
    )


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    css_version = compute_css_version()
    for md_name, title, _ in PAGES:
        out_path = OUT / md_name.replace(".md", ".html")
        out_path.write_text(render_one(md_name, title, css_version), encoding="utf-8")
        print(f"wrote {out_path}  (css v={css_version})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
