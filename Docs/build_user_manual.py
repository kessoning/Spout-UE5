#!/usr/bin/env python3
# Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
# SPDX-License-Identifier: MIT
# Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

"""Builds the SpoutPlugin User Manual PDF (Docs/Manual/SpoutPlugin_User_Manual.pdf) with reportlab Platypus.

The manual is rendered from Markdown, not written here: Docs/USER_MANUAL_SOURCE.md holds the manual's own sections and
its chapter list (one `<!-- include: Docs/X.md -->` line per chapter), and every included page is the same
documentation page that ships with the plugin. This script only lays the text out, so the PDF cannot drift from the
documentation.

Run: python Docs/build_user_manual.py
Writes the PDF and, beside it, <pdf>.inputs.json: the PDF's SHA-256 and the content hash of every file it was made from,
which Scripts/Validate-Documentation.ps1 uses to decide whether the PDF is current.
"""
import hashlib
import json
import os
import re
from xml.sax.saxutils import escape

from reportlab import rl_config
rl_config.invariant = 1  # no timestamps or random IDs in the PDF: the same inputs give the same bytes

from reportlab.lib.pagesizes import A4
from reportlab.lib.units import cm
from reportlab.lib import colors
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib.enums import TA_CENTER
from reportlab.platypus import (
    BaseDocTemplate, PageTemplate, Frame, Paragraph, Spacer, Table, TableStyle, PageBreak, NextPageTemplate,
    CondPageBreak
)
from reportlab.platypus.tableofcontents import TableOfContents

ROOT = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(ROOT)
OUT = os.path.join(ROOT, "Manual", "SpoutPlugin_User_Manual.pdf")
SOURCE = os.path.join(ROOT, "USER_MANUAL_SOURCE.md")
SHOTS = os.path.join(ROOT, "manual_screenshots")
INCLUDE_RE = re.compile(r"^<!--\s*include:\s*(\S+)\s*-->\s*$")


def _content_sha256(path):
    """SHA-256 of a file's content; text with CRLF read as LF, so a checkout's line endings do not matter."""
    data = open(path, "rb").read()
    if not path.lower().endswith((".png", ".jpg", ".jpeg", ".pdf")):
        data = data.replace(b"\r\n", b"\n")
    return hashlib.sha256(data).hexdigest()


def _read_text(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read().replace("\r\n", "\n")


def _included_documents():
    """The chapter pages named by the include lines of the manual source, in order (repository-relative)."""
    names = []
    for line in _read_text(SOURCE).split("\n"):
        match = INCLUDE_RE.match(line.strip())
        if match:
            names.append(match.group(1).replace("\\", "/"))
    return names


def _input_files():
    """Every file the manual is generated from: this script, the manual source, every included page, the descriptor
    (version) and build.ps1 (engine list)."""
    files = [os.path.abspath(__file__), SOURCE]
    files += [os.path.join(REPO, *name.split("/")) for name in _included_documents()]
    files += [os.path.join(REPO, "SpoutPlugin.uplugin"), os.path.join(REPO, "build.ps1")]
    return files


def _tree_state():
    return {p: _content_sha256(p) for p in _input_files() if os.path.isfile(p)}


_STATE_BEFORE = _tree_state()
for _missing in [p for p in _input_files() if not os.path.isfile(p)]:
    raise SystemExit("Manual input missing: %s" % _missing)


def _read_plugin_version():
    """Reads VersionName from SpoutPlugin.uplugin: derived, never restated, so the manual cannot claim another version."""
    with open(os.path.join(REPO, "SpoutPlugin.uplugin"), "r", encoding="utf-8") as handle:
        return json.load(handle)["VersionName"]


def _read_supported_engines():
    """Reads the declared engine matrix from build.ps1, for the same reason as the version."""
    text = _read_text(os.path.join(REPO, "build.ps1"))
    match = re.search(r"^\s*\$DeclaredEngineVersions\s*=\s*@\(([^)]*)\)", text, re.MULTILINE)
    if not match:
        raise RuntimeError("Could not read $DeclaredEngineVersions from build.ps1")
    versions = re.findall(r"'(\d+\.\d+)'", match.group(1))
    if not versions:
        raise RuntimeError("build.ps1 declares no engine versions")
    return versions


def _read_documentation_date():
    """Reads the documentation date from USER_MANUAL_SOURCE.md, the single place it is stated."""
    match = re.search(r"^- Documentation date:\s*(\d{4}-\d{2}-\d{2})\s*$", _read_text(SOURCE), re.MULTILINE)
    if not match:
        raise RuntimeError("Could not read the documentation date from USER_MANUAL_SOURCE.md")
    return match.group(1)


VERSION = _read_plugin_version()
ENGINES = _read_supported_engines()
DOC_DATE = _read_documentation_date()
CONTENT_W = A4[0] - 4 * cm

INK = colors.HexColor("#1f2933"); ACCENT = colors.HexColor("#3b4a6d"); ACCENT2 = colors.HexColor("#5a6b8c")
LIGHT = colors.HexColor("#eef1f6"); RULE = colors.HexColor("#c7cedb"); MUTE = colors.HexColor("#5b6675")
LINK = colors.HexColor("#2f5aa8")

styles = getSampleStyleSheet()


def S(name, **kw):
    base = kw.pop("parent", styles["Normal"])
    return ParagraphStyle(name, parent=base, **kw)


body = S("Body", fontName="Helvetica", fontSize=9.5, leading=13.6, spaceAfter=6, textColor=INK)
h1 = S("H1", fontName="Helvetica-Bold", fontSize=18, leading=22, spaceBefore=0, spaceAfter=12, textColor=ACCENT,
       keepWithNext=True)
h2 = S("H2", fontName="Helvetica-Bold", fontSize=12.5, leading=16, spaceBefore=12, spaceAfter=5, textColor=INK,
       keepWithNext=True)
h3 = S("H3", fontName="Helvetica-Bold", fontSize=10.5, leading=13.5, spaceBefore=9, spaceAfter=4, textColor=ACCENT2,
       keepWithNext=True)
h4 = S("H4", fontName="Helvetica-BoldOblique", fontSize=9.8, leading=13, spaceBefore=7, spaceAfter=3, textColor=INK,
       keepWithNext=True)
cell = S("Cell", fontName="Helvetica", fontSize=8, leading=10.2, textColor=colors.HexColor("#243044"))
cell_head = S("CellHead", parent=cell, fontName="Helvetica-Bold", textColor=colors.white)
toc_title = S("TocTitle", fontName="Helvetica-Bold", fontSize=18, textColor=ACCENT, spaceAfter=12)
CODE_FONT, CODE_SIZE = "Courier", 7.6
CODE_CHARS = int((CONTENT_W - 14) / (0.6 * CODE_SIZE))


def list_style(level, numbered):
    indent = 14 + 13 * level + (4 if numbered else 0)
    return S("List%d%s" % (level, "n" if numbered else "b"), parent=body, leftIndent=indent,
             bulletIndent=indent - (14 if numbered else 10), spaceAfter=2.5)


# --- inline Markdown ---------------------------------------------------------------------------------------------

ANCHORS = {}        # "<doc>#<slug>" -> PDF destination name
CHAPTER_TITLES = {}  # "<doc>" -> "chapter <n>, <title>", the text that replaces a link labelled with a file name


def slugify(text):
    text = re.sub(r"`", "", text.strip().lower())
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


def resolve_link(target, doc):
    """A link target -> (kind, value): ('url', url), ('dest', name) or ('none', None)."""
    if re.match(r"^(https?:|mailto:)", target):
        return "url", target
    path, _, frag = target.partition("#")
    if path:
        name = os.path.normpath(os.path.join(os.path.dirname(doc), path)).replace("\\", "/")
        if name not in CHAPTER_TITLES:
            return "none", None
        doc = name
    if frag and "%s#%s" % (doc, frag) in ANCHORS:
        return "dest", ANCHORS["%s#%s" % (doc, frag)]
    if "%s#" % doc in ANCHORS:
        return "dest", ANCHORS["%s#" % doc]
    return "none", None


def inline(text, doc):
    """Markdown inline syntax -> reportlab paragraph markup."""
    codes = []

    def keep_code(match):
        codes.append(match.group(1))
        return "\x00%d\x00" % (len(codes) - 1)

    text = re.sub(r"``\s?(.+?)\s?``|`([^`]+)`", lambda m: keep_code(re.match(r"(.*)", m.group(1) or m.group(2))), text)
    text = escape(text)

    def link(match):
        label, target = match.group(1), match.group(2).strip().strip("<>")
        kind, value = resolve_link(target, doc)
        plain = re.sub(r"\x00(\d+)\x00", lambda m: codes[int(m.group(1))], label)
        if kind == "dest" and re.fullmatch(r"[\w./-]+\.md(#[\w-]*)?", plain.strip()):
            path = target.partition("#")[0]
            name = os.path.normpath(os.path.join(os.path.dirname(doc), path)).replace("\\", "/") if path else doc
            label = escape(CHAPTER_TITLES.get(name, plain))
        if kind == "url":
            return '<a href="%s" color="#2f5aa8">%s</a>' % (escape(value, {'"': "&quot;"}), label)
        if kind == "dest":
            return '<a href="#%s" color="#2f5aa8">%s</a>' % (value, label)
        return label

    text = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", link, text)
    text = re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", text)
    text = re.sub(r"(?<![\w*])\*(?!\s)(.+?)(?<!\s)\*(?![\w*])", r"<i>\1</i>", text)

    def put_code(match):
        return '<font face="Courier" size="8.4">%s</font>' % escape(codes[int(match.group(1))])

    return re.sub(r"\x00(\d+)\x00", put_code, text)


# --- block Markdown ----------------------------------------------------------------------------------------------

story = []
CHAPTER_NO = [0]
DEST_NO = [0]
USED_DESTS = set()


def new_dest():
    DEST_NO[0] += 1
    return "d%d" % DEST_NO[0]


class Heading(Paragraph):
    """A heading paragraph that knows its outline level (0 = chapter)."""

    def __init__(self, text, style, level, outline):
        Paragraph.__init__(self, text, style)
        self.outline_level = level
        self.outline_text = outline


def code_block(lines):
    rows = []
    for line in lines:
        line = line.replace("\t", "    ")
        while len(line) > CODE_CHARS:
            cut = line.rfind(" ", 0, CODE_CHARS)
            cut = cut if cut > CODE_CHARS // 2 else CODE_CHARS
            rows.append([line[:cut]])
            line = "    " + line[cut:].lstrip()
        rows.append([line if line else " "])
    t = Table(rows, colWidths=[CONTENT_W], hAlign="LEFT")
    t.setStyle(TableStyle([("FONT", (0, 0), (-1, -1), CODE_FONT, CODE_SIZE, CODE_SIZE * 1.32),
                           ("TEXTCOLOR", (0, 0), (-1, -1), INK), ("BACKGROUND", (0, 0), (-1, -1), LIGHT),
                           ("LEFTPADDING", (0, 0), (-1, -1), 7), ("RIGHTPADDING", (0, 0), (-1, -1), 7),
                           ("TOPPADDING", (0, 0), (-1, -1), 0.4), ("BOTTOMPADDING", (0, 0), (-1, -1), 0.4),
                           ("TOPPADDING", (0, 0), (-1, 0), 5), ("BOTTOMPADDING", (0, -1), (-1, -1), 5)]))
    story.append(Spacer(1, 2))
    story.append(t)
    story.append(Spacer(1, 7))


def split_row(line):
    line = line.strip()
    if line.startswith("|"):
        line = line[1:]
    if line.endswith("|") and not line.endswith("\\|"):
        line = line[:-1]
    cells, cur, in_code = [], "", False
    for ch in line:
        if ch == "`":
            in_code = not in_code
        if ch == "|" and not in_code:
            cells.append(cur.strip())
            cur = ""
        else:
            cur += ch
    cells.append(cur.strip())
    return cells


def md_table(rows, doc):
    ncols = max(len(r) for r in rows)
    rows = [r + [""] * (ncols - len(r)) for r in rows]
    plain = [[re.sub(r"[`*]|\[([^\]]*)\]\([^)]*\)", r"\1", c) for c in r] for r in rows]
    weights = []
    for i in range(ncols):
        longest = max(max((len(w) for w in r[i].split()), default=0) for r in plain)
        total = max(len(r[i]) for r in plain)
        weights.append(max(longest * 1.15, min(total, 70) * 0.55, 6))
    scale = CONTENT_W / sum(weights)
    widths = [w * scale for w in weights]
    data = [[Paragraph(inline(c, doc), cell_head if ri == 0 else cell) for c in r] for ri, r in enumerate(rows)]
    t = Table(data, colWidths=widths, repeatRows=1, hAlign="LEFT")
    t.setStyle(TableStyle([("VALIGN", (0, 0), (-1, -1), "TOP"),
                           ("TOPPADDING", (0, 0), (-1, -1), 4), ("BOTTOMPADDING", (0, 0), (-1, -1), 4),
                           ("LEFTPADDING", (0, 0), (-1, -1), 5), ("RIGHTPADDING", (0, 0), (-1, -1), 5),
                           ("LINEBELOW", (0, 0), (-1, -1), 0.4, RULE), ("BACKGROUND", (0, 0), (-1, 0), ACCENT),
                           ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, colors.HexColor("#f6f8fb")])]))
    story.append(Spacer(1, 2))
    story.append(t)
    story.append(Spacer(1, 8))


def heading(level, text, doc, numbered=True):
    """level 0 = chapter (new page), 1..3 = sections."""
    slug = slugify(text)
    dest = ANCHORS.get("%s#%s" % (doc, slug))
    if not dest or dest in USED_DESTS:
        dest = new_dest()
    USED_DESTS.add(dest)
    label = inline(text, doc)
    if level == 0:
        if numbered:
            CHAPTER_NO[0] += 1
            label = "%d. %s" % (CHAPTER_NO[0], label)
        story.append(PageBreak())
        para = Heading('<a name="%s"/>%s' % (dest, label), h1, 0, None)
    else:
        para = Heading('<a name="%s"/>%s' % (dest, label), (h2, h3, h4)[min(level, 3) - 1], level, None)
    para.outline_text = re.sub(r"<[^>]+>", "", label).replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">")
    story.append(para)


def render_markdown(text, doc, level_shift, skip_first_h1=False, chapter_numbered=True):
    """Render one Markdown page. level_shift maps '#' levels to heading levels (0 = chapter)."""
    lines = text.split("\n")
    i, para, first_h1_done = 0, [], not skip_first_h1
    list_stack = []   # [(indent, numbered)]
    item = None       # [level, numbered, number, text]

    def flush_para():
        if para:
            story.append(Paragraph(inline(" ".join(l.strip() for l in para), doc), body))
            para.clear()

    def flush_item():
        nonlocal item
        if item:
            lvl, numbered, number, txt = item
            style = list_style(lvl, numbered)
            story.append(Paragraph(inline(txt, doc), style,
                                   bulletText=("%s." % number) if numbered else "•"))
            item = None

    def end_list():
        flush_item()
        if list_stack:
            story.append(Spacer(1, 4))
        list_stack.clear()

    while i < len(lines):
        raw = lines[i]
        line = raw.rstrip()
        stripped = line.strip()

        fence = re.match(r"^(\s*)```", line)
        if fence:
            flush_para(); end_list()
            j = i + 1
            block = []
            ind = len(fence.group(1))
            while j < len(lines) and not re.match(r"^\s*```", lines[j]):
                block.append(lines[j][ind:] if lines[j][:ind].strip() == "" else lines[j])
                j += 1
            code_block(block)
            i = j + 1
            continue

        if stripped.startswith("<!--"):
            while i < len(lines) and "-->" not in lines[i]:
                i += 1
            i += 1
            continue

        h = re.match(r"^(#{1,6})\s+(.*?)\s*#*\s*$", line)
        if h:
            flush_para(); end_list()
            depth = len(h.group(1))
            if depth == 1 and not first_h1_done:
                first_h1_done = True
                heading(0, h.group(2), doc, chapter_numbered)
            else:
                heading(max(depth - 1 + level_shift, 1 if depth > 1 or level_shift > 0 else 0), h.group(2), doc,
                        chapter_numbered)
            i += 1
            continue

        if re.match(r"^\s*(-{3,}|\*{3,}|_{3,})\s*$", line):
            flush_para(); end_list()
            story.append(Spacer(1, 6))
            i += 1
            continue

        if stripped.startswith("|") and i + 1 < len(lines) and re.match(r"^\s*\|?\s*:?-{2,}", lines[i + 1]):
            flush_para(); end_list()
            rows = [split_row(line)]
            j = i + 2
            while j < len(lines) and lines[j].strip().startswith("|"):
                rows.append(split_row(lines[j]))
                j += 1
            md_table(rows, doc)
            i = j
            continue

        li = re.match(r"^(\s*)([-*+]|\d+[.)])\s+(.*)$", line)
        if li:
            flush_para()
            indent = len(li.group(1))
            numbered = li.group(2)[0].isdigit()
            while list_stack and indent < list_stack[-1][0]:
                list_stack.pop()
            if not list_stack or indent > list_stack[-1][0]:
                list_stack.append((indent, numbered))
            flush_item()
            number = int(re.match(r"\d+", li.group(2)).group(0)) if numbered else 0
            item = [len(list_stack) - 1, numbered, number, li.group(3).strip()]
            i += 1
            continue

        if not stripped:
            flush_para()
            # A blank line inside a list continues it when the next text is indented.
            nxt = i + 1
            while nxt < len(lines) and not lines[nxt].strip():
                nxt += 1
            if item and nxt < len(lines) and re.match(r"^\s{2,}\S", lines[nxt]) and not re.match(r"^\s*```", lines[nxt]):
                item[3] += "<br/><br/>"
            elif item and nxt < len(lines) and re.match(r"^(\s*)([-*+]|\d+[.)])\s+", lines[nxt]):
                pass
            else:
                end_list()
            i += 1
            continue

        if item is not None and (raw.startswith(" ") or not para):
            item[3] += ("" if item[3].endswith("<br/><br/>") else " ") + stripped
            i += 1
            continue

        end_list()
        para.append(line)
        i += 1

    flush_para(); end_list()


def register_anchors(text, doc, chapter_title):
    """Pre-assign destinations for every heading of a page, so links work in both directions."""
    CHAPTER_TITLES[doc] = chapter_title
    ANCHORS["%s#" % doc] = new_dest()
    in_code = False
    for line in text.split("\n"):
        if re.match(r"^\s*```", line):
            in_code = not in_code
            continue
        m = None if in_code else re.match(r"^(#{1,6})\s+(.*?)\s*#*\s*$", line)
        if m:
            key = "%s#%s" % (doc, slugify(m.group(2)))
            if key not in ANCHORS:
                ANCHORS[key] = ANCHORS["%s#" % doc] if len(m.group(1)) == 1 else new_dest()


# --- document ----------------------------------------------------------------------------------------------------

class ManualDoc(BaseDocTemplate):
    _last_outline = -1

    def afterFlowable(self, flowable):
        if isinstance(flowable, Heading) and flowable.outline_level <= 2:
            key = "o%d" % id(flowable)
            level = min(flowable.outline_level, self._last_outline + 1)  # the PDF outline cannot skip a level
            self._last_outline = level
            self.canv.bookmarkPage(key)
            self.canv.addOutlineEntry(flowable.outline_text, key, level=level, closed=level > 0)
            if flowable.outline_level <= 1:
                self.notify("TOCEntry", (flowable.outline_level, flowable.outline_text, self.page))


def footer(canvas, doc):
    canvas.saveState()
    canvas.setStrokeColor(RULE); canvas.setLineWidth(0.5)
    canvas.line(2 * cm, 1.5 * cm, A4[0] - 2 * cm, 1.5 * cm)
    canvas.setFont("Helvetica", 8); canvas.setFillColor(MUTE)
    canvas.drawString(2 * cm, 1.05 * cm, "SpoutPlugin User Manual  -  v%s" % VERSION)
    canvas.drawCentredString(A4[0] / 2, 1.05 * cm, "Unreal Engine %s - %s  -  Windows  -  DirectX 12" % (ENGINES[0], ENGINES[-1]))
    canvas.drawRightString(A4[0] - 2 * cm, 1.05 * cm, "Page %d" % doc.page)
    canvas.restoreState()


def blank(canvas, doc):
    pass


def manual_sections(text):
    """The manual source split at its '## ' sections (the text above the first one is the file's own preamble)."""
    parts = re.split(r"(?m)^(?=## )", text)
    return [p for p in parts[1:]]


source_text = _read_text(SOURCE)

# Chapter titles and anchors first, so every cross-reference can be resolved while rendering.
plan = []   # ("source", section_text) or ("doc", name, text)
for block in re.split(r"(?m)^(<!--\s*include:\s*\S+\s*-->)\s*$", source_text.split("\n---\n", 1)[1]):
    inc = INCLUDE_RE.match(block.strip())
    if inc:
        name = inc.group(1).replace("\\", "/")
        plan.append(("doc", name, _read_text(os.path.join(REPO, *name.split("/")))))
    else:
        for section in manual_sections(block):
            plan.append(("source", section))

number = 0
for entry in plan:
    if entry[0] == "doc":
        number += 1
        title = re.search(r"(?m)^#\s+(.*)$", entry[2]).group(1).strip()
        register_anchors(entry[2], entry[1], "chapter %d, %s" % (number, title))
    else:
        title = re.match(r"## (.*)", entry[1]).group(1).strip()
        if title not in ("About This Manual", "Appendix"):
            number += 1

# Cover
story.append(Spacer(1, 3.0 * cm))
story.append(Paragraph("SpoutPlugin", S("Cover", fontName="Helvetica-Bold", fontSize=46, leading=50, textColor=ACCENT,
                                         alignment=TA_CENTER)))
story.append(Spacer(1, 8))
story.append(Paragraph("Spout sender and receiver for Unreal Engine 5",
                       S("Sub", fontName="Helvetica", fontSize=15, leading=20, textColor=INK, alignment=TA_CENTER)))
story.append(Spacer(1, 6))
story.append(Paragraph("User Manual, Blueprint API and C++ API Reference",
                       S("Sub2", fontName="Helvetica-Oblique", fontSize=12, textColor=MUTE, alignment=TA_CENTER)))
story.append(Spacer(1, 1.8 * cm))
meta = [["Plugin version", VERSION], ["Unreal Engine", "%s to %s" % (ENGINES[0], ENGINES[-1])],
        ["Modules", "SpoutPlugin (Runtime), SpoutPluginShaders (Runtime), SpoutPluginEditor (Editor)"],
        ["Platform", "Windows 64-bit, DirectX 12 RHI"], ["Documentation date", DOC_DATE]]
mt = Table([[a, Paragraph(b, S("MetaVal", fontName="Helvetica", fontSize=10, leading=13, textColor=INK))] for a, b in meta],
           colWidths=[4.6 * cm, 9.4 * cm], hAlign="CENTER")
mt.setStyle(TableStyle([("FONT", (0, 0), (0, -1), "Helvetica-Bold", 10), ("TEXTCOLOR", (0, 0), (0, -1), ACCENT2),
                        ("VALIGN", (0, 0), (-1, -1), "TOP"), ("TOPPADDING", (0, 0), (-1, -1), 6),
                        ("BOTTOMPADDING", (0, 0), (-1, -1), 6), ("LINEBELOW", (0, 0), (-1, -1), 0.4, RULE),
                        ("LEFTPADDING", (0, 0), (-1, -1), 10)]))
story.append(mt)
story.append(Spacer(1, 2.0 * cm))
story.append(Paragraph("GPU texture sharing between Unreal Engine and other Windows applications such as TouchDesigner, "
                       "Resolume and OBS, bridging Direct3D 12 and Spout's Direct3D 11 shared textures.",
                       S("CoverNote", fontName="Helvetica", fontSize=9.5, leading=14, textColor=MUTE, alignment=TA_CENTER)))
story.append(NextPageTemplate("body"))
story.append(PageBreak())

story.append(Paragraph("Contents", toc_title))
toc = TableOfContents()
toc.dotsMinLevel = 0
toc.levelStyles = [S("TOC0", fontName="Helvetica-Bold", fontSize=10.5, leading=16, textColor=INK),
                   S("TOC1", fontName="Helvetica", fontSize=8.8, leading=12.5, leftIndent=16, textColor=MUTE)]
story.append(toc)

for entry in plan:
    if entry[0] == "doc":
        render_markdown(entry[2], entry[1], level_shift=0, skip_first_h1=True)
    else:
        title = re.match(r"## (.*)", entry[1]).group(1).strip()
        # A manual section: '##' is the chapter, '###' its sections.
        heading(0, title, "Docs/USER_MANUAL_SOURCE.md", numbered=title not in ("About This Manual", "Appendix"))
        render_markdown(entry[1].split("\n", 1)[1] if "\n" in entry[1] else "", "Docs/USER_MANUAL_SOURCE.md",
                        level_shift=-1)

os.makedirs(os.path.dirname(OUT), exist_ok=True)
frame_body = Frame(2 * cm, 2 * cm, A4[0] - 4 * cm, A4[1] - 3.5 * cm, id="body")
frame_title = Frame(2 * cm, 2 * cm, A4[0] - 4 * cm, A4[1] - 3 * cm, id="title")
doc = ManualDoc(OUT + ".tmp", pagesize=A4, title="SpoutPlugin User Manual", author="Kesson",
                subject="SpoutPlugin %s: user manual, Blueprint API and C++ API reference" % VERSION,
                creator="Docs/build_user_manual.py")
doc.addPageTemplates([PageTemplate(id="title", frames=[frame_title], onPage=blank),
                      PageTemplate(id="body", frames=[frame_body], onPage=footer)])
doc.multiBuild(story)
os.replace(OUT + ".tmp", OUT)
print("WROTE", OUT, os.path.getsize(OUT), "bytes,", doc.page, "pages,", len(_included_documents()), "included pages")

# Nothing the PDF was made from may have changed while it was made: otherwise the record would name content the PDF
# does not reflect. The PDF is then left as written, without a record that names it.
if _tree_state() != _STATE_BEFORE:
    raise SystemExit("An input of the manual changed while it was generated; no record was written. Run the generator again.")


# The record Scripts/Validate-Documentation.ps1 judges the PDF's freshness by: this PDF's bytes and the content of every
# file it was generated from. The manual embeds no screenshots, so the list of looked-for, absent screenshots is empty.
def _rel(p):
    return os.path.relpath(p, REPO).replace(os.sep, "/")


_record = json.dumps({"what": "inputs of the generated user manual (Docs/build_user_manual.py); Scripts/Validate-Documentation.ps1 checks them",
                      "basis": "generated",
                      "pdf": {"path": _rel(OUT), "sha256": _content_sha256(OUT)},
                      "inputs": {_rel(p): h for p, h in _STATE_BEFORE.items()},
                      "absent": []}, indent=1, sort_keys=True) + "\n"
with open(OUT + ".inputs.json.tmp", "w", encoding="utf-8", newline="\n") as _f:
    _f.write(_record)
os.replace(OUT + ".inputs.json.tmp", OUT + ".inputs.json")
print("WROTE", OUT + ".inputs.json")
