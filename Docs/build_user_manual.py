#!/usr/bin/env python3
"""Builds the SpoutPlugin User Manual PDF with reportlab Platypus.

Screenshots: drop PNG/JPG files named exactly as listed in FIGURES into
Docs/manual_screenshots/ and re-run this script. Any figure whose file exists is
embedded automatically; any missing figure renders an instruction placeholder.
"""
import json
import os
import re
from reportlab.lib.pagesizes import A4
from reportlab.lib.units import cm
from reportlab.lib import colors
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib.enums import TA_CENTER
from reportlab.lib.utils import ImageReader
from reportlab.platypus import (
    BaseDocTemplate, PageTemplate, Frame, Paragraph, Spacer, Table, TableStyle,
    PageBreak, NextPageTemplate, KeepTogether, Image
)
from reportlab.platypus.tableofcontents import TableOfContents

ROOT = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(ROOT, "2026-06-05-SpoutPlugin-User-Manual.pdf")
SHOTS = os.path.join(ROOT, "manual_screenshots")


def _read_plugin_version():
    """Reads VersionName from SpoutPlugin.uplugin.

    Derived rather than restated: a hard-coded VERSION here is how a manual ends up claiming one
    version while the plugin descriptor says another. Failing loudly is deliberate - a manual that
    cannot establish which version it documents should not be produced at all.
    """
    descriptor = os.path.join(os.path.dirname(ROOT), "SpoutPlugin.uplugin")
    with open(descriptor, "r", encoding="utf-8") as handle:
        return json.load(handle)["VersionName"]


def _read_supported_engines():
    """Reads the declared engine matrix from build.ps1, for the same reason as the version."""
    build_script = os.path.join(os.path.dirname(ROOT), "build.ps1")
    with open(build_script, "r", encoding="utf-8") as handle:
        text = handle.read()
    match = re.search(r"^\s*\$DeclaredEngineVersions\s*=\s*@\(([^)]*)\)", text, re.MULTILINE)
    if not match:
        raise RuntimeError("Could not read $DeclaredEngineVersions from build.ps1")
    versions = re.findall(r"'(\d+\.\d+)'", match.group(1))
    if not versions:
        raise RuntimeError("build.ps1 declares no engine versions")
    return versions


VERSION = _read_plugin_version()
ENGINES = _read_supported_engines()
ENGINE_RANGE = "UE %s through UE %s" % (ENGINES[0], ENGINES[-1])
CONTENT_W = A4[0] - 4*cm

INK=colors.HexColor("#1f2933"); ACCENT=colors.HexColor("#3b4a6d"); ACCENT2=colors.HexColor("#5a6b8c")
LIGHT=colors.HexColor("#eef1f6"); RULE=colors.HexColor("#c7cedb"); MUTE=colors.HexColor("#5b6675")
PHBG=colors.HexColor("#f3f6fb"); PHEDGE=colors.HexColor("#9fb0cc"); PHHEAD=colors.HexColor("#42557d")

styles = getSampleStyleSheet()
def S(name, **kw):
    base = kw.pop("parent", styles["Normal"]); return ParagraphStyle(name, parent=base, **kw)
body=S("Body",fontName="Helvetica",fontSize=10,leading=15,spaceAfter=7,textColor=INK)
bullet=S("Bullet",parent=body,leftIndent=14,bulletIndent=2,spaceAfter=3)
step=S("Step",parent=body,leftIndent=16,bulletIndent=2,spaceAfter=4)
h1=S("H1",fontName="Helvetica-Bold",fontSize=18,leading=22,spaceBefore=8,spaceAfter=10,textColor=ACCENT)
h2=S("H2",fontName="Helvetica-Bold",fontSize=13,leading=17,spaceBefore=12,spaceAfter=6,textColor=INK)
h3=S("H3",fontName="Helvetica-Bold",fontSize=11,leading=14,spaceBefore=8,spaceAfter=4,textColor=ACCENT2)
code=S("Code",fontName="Courier",fontSize=8.5,leading=11.5,textColor=INK,backColor=LIGHT,borderPadding=6,spaceBefore=4,spaceAfter=8)
note=S("Note",parent=body,fontSize=9.5,leading=14,backColor=colors.HexColor("#fbf6e9"),borderPadding=7,spaceBefore=4,spaceAfter=9)
figcap=S("FigCap",parent=body,fontSize=9,leading=12,textColor=MUTE,alignment=TA_CENTER,spaceBefore=3,spaceAfter=10)
ph_head=S("PhHead",fontName="Helvetica-Bold",fontSize=9,leading=12,textColor=colors.white)
ph_empty=S("PhEmpty",fontName="Helvetica",fontSize=9.5,leading=14,textColor=ACCENT2,alignment=TA_CENTER)
ph_body=S("PhBody",fontName="Helvetica",fontSize=8.5,leading=12,textColor=INK)
toc_title=S("TocTitle",fontName="Helvetica-Bold",fontSize=18,textColor=ACCENT,spaceAfter=12)

FIGURES = {
"SP-001": ("SP-001_plugin-enabled.png","The Spout Plugin enabled in the Plugins browser.",
    ["Open Edit &gt; Plugins and type \"Spout\" in the search box.",
     "Frame the Spout Plugin tile so its name, the enabled checkbox (ticked), and the Rendering category are visible.",
     "Include the 'Beta' label if shown."]),
"SP-002": ("SP-002_rhi-directx12.png","Default RHI set to DirectX 12 in Project Settings.",
    ["Open Edit &gt; Project Settings &gt; Platforms &gt; Windows.",
     "Scroll to the Targeted RHIs / Default RHI section.",
     "Capture the Default RHI dropdown showing DirectX 12 selected."]),
"SP-003": ("SP-003_plugin-folder.png","The SpoutPlugin folder inside the project Plugins directory.",
    ["Open Windows File Explorer at MyProject/Plugins/SpoutPlugin/.",
     "Show the top-level folders (Binaries, Content, Resources, Source, ThirdParty) and SpoutPlugin.uplugin in the same view."]),
"SP-004": ("SP-004_spout-dll-binaries.png","Spout.dll staged next to the module binary in Binaries/Win64.",
    ["Open File Explorer at MyProject/Plugins/SpoutPlugin/Binaries/Win64/.",
     "Make sure both Spout.dll and UnrealEditor-SpoutPlugin.dll are visible in the listing."]),
"SP-017": ("SP-017_add-sender-node.png","Adding the Spout Sender node from the Blueprint context menu.",
    ["In a Blueprint Event Graph, right-click on empty canvas.",
     "Type \"Spout Sender\" in the action search box.",
     "Capture the search results showing the Spout Sender node under the Spout category before placing it."]),
"SP-005": ("SP-005_sender-viewport-graph.png","Blueprint: sending the game viewport every frame.",
    ["Build the graph: Event Tick (or a timer) -&gt; Spout Sender.",
     "Set Spout Name to a readable value such as UnrealViewport.",
     "Set Send Texture From to GameViewport. Leave Texture Render Target 2D empty.",
     "Capture the full node with all pins and values readable. Zoom so pin text is legible."]),
"SP-018": ("SP-018_timer-setup.png","Driving the sender on a timer instead of Event Tick.",
    ["Show a Set Timer by Event (or Set Timer by Function Name) node configured to call the function that runs Spout Sender.",
     "Set Time to your interval (for example 0.033 for ~30 fps) and Looping = true.",
     "Capture the timer node and the bound event/function name."]),
"SP-006": ("SP-006_sender-rendertarget-graph.png","Blueprint: sending a TextureRenderTarget2D.",
    ["Build the graph: Event Tick (or timer) -&gt; Spout Sender.",
     "Set Send Texture From to TextureRenderTarget2D.",
     "Connect or select your render target asset on the Texture Render Target 2D pin.",
     "Capture the node with the render target reference and Spout Name visible."]),
"SP-007": ("SP-007_scenecapture-source.png","SceneCapture2D Capture Source set correctly.",
    ["Select the SceneCapture2D actor and open its Details panel.",
     "Open the Capture Source dropdown.",
     "Capture it showing 'Final Color (HDR) in Linear Working Color Space' selected.",
     "This is the most important screenshot for avoiding black output - make the value unambiguous."]),
"SP-008": ("SP-008_rendertarget-settings.png","TextureRenderTarget2D asset settings.",
    ["Double-click the render target asset to open it, or select it and view Details.",
     "Capture the Render Target Format (RGBA8 sRGB recommended) and Size X / Size Y fields."]),
"SP-009": ("SP-009_receiver-graph.png","Blueprint: receiving a stream and using the outputs.",
    ["Build the graph: Event Tick (or timer) -&gt; Spout Receiver.",
     "Set Spout Name to match the sender exactly.",
     "Expand the advanced pins so Optional Output Render Target is visible.",
     "Show Out Mat, Out Texture, and the return-value pins wired to something (for example a Branch on the return bool, and Out Mat into a Set Material node).",
     "Capture the whole node with pin names legible."]),
"SP-010": ("SP-010_material-texture-parameter.png","Material with a Texture Parameter for receiver output.",
    ["Open M_SpoutMaterial (or your own material).",
     "Select the Texture Parameter node and show its Parameter Name in the Details panel.",
     "Capture so the parameter name (default SpoutTexture) is readable and is connected to Base Color / Emissive."]),
"SP-011": ("SP-011_external-app-receiving.png","An external application receiving the Unreal stream.",
    ["In TouchDesigner (or another Spout receiver), add a Spout In TOP.",
     "Select the Unreal sender by name from its sender list.",
     "Capture the live Unreal image in the external app with the sender name visible in the dropdown."]),
"SP-012": ("SP-012_spout-info-debug.png","Spout Info output listing active senders.",
    ["Build a small debug graph that calls Spout Info and prints the resulting array (name, width, height).",
     "Capture either the Blueprint graph or the on-screen / log output showing one or more sender entries."]),
"SP-013": ("SP-013_log-loaded.png","Output Log confirming the plugin loaded and initialized.",
    ["Open Window &gt; Output Log and filter for LogSpoutPlugin.",
     "Capture lines 'Spout Plugin Loaded' and 'SpoutDX11on12 Initialized.'"]),
"SP-014": ("SP-014_log-dll-missing.png","Output Log showing a missing Spout.dll startup error.",
    ["Reproduce by temporarily removing Spout.dll, or capture from a past failure.",
     "Capture the LogSpoutPlugin error naming the Binaries/Win64 and ThirdParty paths it checked.",
     "Optional figure - include only if you can reproduce it cleanly."]),
"SP-015": ("SP-015_cvar-console.png","Setting r.Spout.GPUReceiver from the console.",
    ["Open the editor console (tilde key) in PIE.",
     "Type r.Spout.GPUReceiver and capture the printed current value, plus an example of setting it to 0 or 1."]),
"SP-016": ("SP-016_example-content.png","The plugin example content folder.",
    ["In the Content Browser, enable 'Show Plugin Content' and open the SpoutPlugin content folder.",
     "Capture the assets: BP_SpoutSenderExample_, BP_SpoutReceiverExample, M_SpoutMaterial, RT_SpoutSenderTest_, RT_SpoutReceiverTest."]),
}

def scaled_image(path, max_w):
    ir=ImageReader(path); iw,ih=ir.getSize(); ratio=ih/float(iw)
    w=max_w; h=w*ratio; max_h=19*cm
    if h>max_h: h=max_h; w=h/ratio
    return Image(path,width=w,height=h)

def figure(fid):
    fn,cap,instr=FIGURES[fid]; path=os.path.join(SHOTS,fn)
    if os.path.exists(path):
        img=scaled_image(path,CONTENT_W); img.hAlign="CENTER"
        return [KeepTogether([img, Paragraph("<b>Figure %s.</b> %s"%(fid,cap), figcap)])]
    header=Paragraph("SCREENSHOT PLACEHOLDER &nbsp; &bull; &nbsp; FIGURE %s"%fid, ph_head)
    empty=Paragraph("provide file: <font face='Courier' color='#42557d'>Docs/manual_screenshots/%s</font><br/>"
                    "<font size='8'><i>image not added yet - drop this file in and rebuild</i></font>"%fn, ph_empty)
    cuetxt="<b>What this figure must show:</b> %s<br/>"%cap + "<br/>".join("&bull;&nbsp; "+s for s in instr)
    cue=Paragraph(cuetxt, ph_body)
    inner=Table([[header],[empty],[cue]], colWidths=[CONTENT_W], rowHeights=[0.6*cm, 2.0*cm, None])
    inner.setStyle(TableStyle([
        ("BACKGROUND",(0,0),(0,0),PHHEAD),("BACKGROUND",(0,1),(0,1),colors.white),("BACKGROUND",(0,2),(0,2),PHBG),
        ("BOX",(0,0),(-1,-1),0.8,PHEDGE),("LINEBELOW",(0,0),(0,0),0.8,PHEDGE),("LINEBELOW",(0,1),(0,1),0.8,PHEDGE),
        ("VALIGN",(0,0),(0,0),"MIDDLE"),("VALIGN",(0,1),(0,1),"MIDDLE"),("VALIGN",(0,2),(0,2),"TOP"),
        ("LEFTPADDING",(0,0),(-1,-1),9),("RIGHTPADDING",(0,0),(-1,-1),9),
        ("TOPPADDING",(0,0),(0,0),3),("BOTTOMPADDING",(0,0),(0,0),3),
        ("TOPPADDING",(0,2),(0,2),7),("BOTTOMPADDING",(0,2),(0,2),8)]))
    return [KeepTogether([inner, Spacer(1,10)])]

class ManualDoc(BaseDocTemplate):
    def afterFlowable(self, f):
        if isinstance(f, Paragraph):
            if f.style.name=="H1": self.notify("TOCEntry",(0,f.getPlainText(),self.page))
            elif f.style.name=="H2": self.notify("TOCEntry",(1,f.getPlainText(),self.page))

def footer(c, d):
    c.saveState(); c.setStrokeColor(RULE); c.setLineWidth(0.5)
    c.line(2*cm,1.5*cm,A4[0]-2*cm,1.5*cm)
    c.setFont("Helvetica",8); c.setFillColor(MUTE)
    c.drawString(2*cm,1.05*cm,"SpoutPlugin User Manual  -  v%s"%VERSION)
    c.drawRightString(A4[0]-2*cm,1.05*cm,"Page %d"%d.page)
    c.drawCentredString(A4[0]/2,1.05*cm,"Unreal Engine 5  -  Windows  -  DirectX 12")
    c.restoreState()
def blank(c,d): pass

story=[]
def P(t,st=body): story.append(Paragraph(t,st))
def B(t): story.append(Paragraph("&bull;&nbsp;&nbsp;"+t,bullet))
def STEP(n,t): story.append(Paragraph("<b>%s.</b>&nbsp;&nbsp;%s"%(n,t),step))
def CODE(t):
    t=t.replace("&","&amp;").replace("<","&lt;").replace(">","&gt;").replace("\n","<br/>")
    story.append(Paragraph(t,code))
def H1(t): story.append(Paragraph(t,h1))
def H2(t): story.append(Paragraph(t,h2))
def H3(t): story.append(Paragraph(t,h3))
def SP(h=6): story.append(Spacer(1,h))
def NOTE(t): story.append(Paragraph(t,note))
def FIG(fid):
    for f in figure(fid): story.append(f)
def table(data,cw,header=True):
    t=Table(data,colWidths=cw,repeatRows=1 if header else 0)
    cmds=[("FONT",(0,0),(-1,-1),"Helvetica",8.5),("TEXTCOLOR",(0,0),(-1,-1),INK),("VALIGN",(0,0),(-1,-1),"TOP"),
        ("TOPPADDING",(0,0),(-1,-1),5),("BOTTOMPADDING",(0,0),(-1,-1),5),
        ("LEFTPADDING",(0,0),(-1,-1),7),("RIGHTPADDING",(0,0),(-1,-1),7),
        ("LINEBELOW",(0,0),(-1,-1),0.4,RULE),("ROWBACKGROUNDS",(0,1),(-1,-1),[colors.white,colors.HexColor("#f6f8fb")])]
    if header:
        cmds+=[("BACKGROUND",(0,0),(-1,0),ACCENT),("TEXTCOLOR",(0,0),(-1,0),colors.white),("FONT",(0,0),(-1,0),"Helvetica-Bold",8.5)]
    t.setStyle(TableStyle(cmds)); story.append(t)

# TITLE
story.append(Spacer(1,3.2*cm))
story.append(Paragraph("SpoutPlugin",S("Cover",fontName="Helvetica-Bold",fontSize=46,leading=50,textColor=ACCENT,alignment=TA_CENTER)))
story.append(Spacer(1,6))
story.append(Paragraph("Unreal Engine 5 Spout Sender / Receiver Plugin",S("Sub",fontName="Helvetica",fontSize=15,leading=20,textColor=INK,alignment=TA_CENTER)))
story.append(Spacer(1,4))
story.append(Paragraph("User Manual",S("Sub2",fontName="Helvetica-Oblique",fontSize=12,textColor=MUTE,alignment=TA_CENTER)))
story.append(Spacer(1,1.8*cm))
meta=[["Plugin version",VERSION+"  (beta)"],["Engine target","Unreal Engine 5.3 - 5.8"],
      ["Module","SpoutPlugin (Runtime)"],["Platform","Windows 64-bit, DirectX 12 RHI"],["Documentation date","2026-06-05"]]
mt=Table(meta,colWidths=[5*cm,8*cm],hAlign="CENTER")
mt.setStyle(TableStyle([("FONT",(0,0),(0,-1),"Helvetica-Bold",10),("FONT",(1,0),(1,-1),"Helvetica",10),
    ("TEXTCOLOR",(0,0),(0,-1),ACCENT2),("TEXTCOLOR",(1,0),(1,-1),INK),
    ("TOPPADDING",(0,0),(-1,-1),6),("BOTTOMPADDING",(0,0),(-1,-1),6),
    ("LINEBELOW",(0,0),(-1,-1),0.4,RULE),("LEFTPADDING",(0,0),(-1,-1),10)]))
story.append(mt); story.append(Spacer(1,2.0*cm))
story.append(Paragraph("GPU texture sharing between Unreal Engine and external real-time applications such as TouchDesigner, via a D3D11-on-D3D12 bridge.",S("CoverNote",fontName="Helvetica",fontSize=9.5,leading=14,textColor=MUTE,alignment=TA_CENTER)))
story.append(NextPageTemplate("body")); story.append(PageBreak())

story.append(Paragraph("Table of Contents",toc_title))
toc=TableOfContents()
toc.levelStyles=[S("TOC0",fontName="Helvetica-Bold",fontSize=11,leading=18,textColor=INK),
                 S("TOC1",fontName="Helvetica",fontSize=9.5,leading=15,leftIndent=16,textColor=MUTE)]
story.append(toc); story.append(PageBreak())

H1("About This Manual")
P("This manual explains how to use SpoutPlugin in Unreal Engine 5, mainly through Blueprints. It is written for Unreal users and technical artists. You do not need to write C++ to use the plugin. Short function signatures appear only in the Blueprint API reference, as the source of truth for each node's inputs and outputs.")
H3("About the screenshots")
P("Because the plugin is driven almost entirely from Blueprint, this manual relies on screenshots of the node graphs and editor settings. Where a screenshot has not been added yet, you will see a labelled placeholder box that names the exact file to provide and describes what the image must show.")
P("To add the images: capture each screenshot, name it exactly as the placeholder states, and place it in the folder below. Then rebuild the PDF and each placeholder is replaced automatically by the embedded image with a numbered caption.")
CODE("Docs/manual_screenshots/   (PNG or JPG, named per the placeholder, for example SP-005_sender-viewport-graph.png)")
NOTE("Figure IDs (SP-001, SP-002, ...) are stable. A full list with capture notes is in Docs/SCREENSHOT_CHECKLIST.md. Optional figures are marked in their capture notes.")

H1("1. What SpoutPlugin Is")
P("Spout is a Windows technology for sharing GPU textures between real-time applications with no disk or network transfer. One application publishes a texture (a <b>sender</b>) and another reads it live (a <b>receiver</b>). It is widely used to move video between tools such as TouchDesigner, Resolume, Notch, and OBS.")
P("SpoutPlugin brings Spout into Unreal Engine 5. It sends Unreal's rendered output, or any render target, to other Spout applications, and receives Spout streams back into Unreal as textures and materials. Everything stays on the GPU.")
P("The technical problem it solves: Spout shares DirectX 11 textures, but modern UE5 runs on the DirectX 12 RHI. The plugin stands up a D3D11-on-D3D12 interop device and bridges UE's D3D12 textures to Spout's D3D11 shared textures. You do not interact with any of this directly - it is why DirectX 12 is required and why the plugin is Windows-only.")
H3("Typical use cases")
B("Send an Unreal scene or camera into TouchDesigner for compositing, projection mapping, or LED output.")
B("Receive a live feed from another tool into Unreal and map it onto a mesh or use it in a material.")
B("Build a real-time pipeline between Unreal and a media-server or VJ setup.")
H3("What it does and does not do")
P("<b>Does:</b> GPU texture send and receive over Spout, from the game viewport or a render target, into a texture, a dynamic material instance, or an optional render target.")
P("<b>Does not:</b> it is not a video codec, not a network streaming solution, and not a color-management system. It does not apply gamma or color correction on send. It is Windows-only and requires DirectX 12.")

H1("2. Requirements")
B("Windows 64-bit.")
B("Unreal Engine 5 (5.3 through 5.8).")
B("DirectX 12 RHI enabled. The plugin creates a D3D11-on-D3D12 device and will not initialize on any other RHI; it logs <font face='Courier'>SpoutPlugin requires DX12 RHI.</font> and stops.")
B("<font face='Courier'>Spout.dll</font>, bundled under <font face='Courier'>ThirdParty/Spout/lib/amd64/</font> and staged to the plugin's <font face='Courier'>Binaries/Win64/</font> at build time.")
B("For source builds: Visual Studio 2022 with the C++ toolchain and a matching Windows SDK.")
B("The plugin is project-local. Install it into your project's <font face='Courier'>Plugins/</font> folder, not the engine plugins folder.")
P("Both applications sharing a stream must run on the same GPU. Spout's shared-texture model does not span GPUs.")

H1("3. Installation and Project Setup")
H2("Precompiled binary")
STEP(1,"Download the ZIP matching your exact engine version (prebuilt binaries are on Patreon).")
STEP(2,"Extract the <font face='Courier'>SpoutPlugin</font> folder into <font face='Courier'>MyProject/Plugins/</font>.")
STEP(3,"Confirm <font face='Courier'>Spout.dll</font> is at <font face='Courier'>Plugins/SpoutPlugin/Binaries/Win64/Spout.dll</font>.")
STEP(4,"Set the project RHI to DirectX 12 (below).")
STEP(5,"Open the project. If Unreal offers to rebuild because of an engine-version mismatch, use the correct ZIP or build from source.")
FIG("SP-003")
H2("Source build")
STEP(1,"Copy <font face='Courier'>SpoutPlugin</font> into <font face='Courier'>MyProject/Plugins/</font>.")
STEP(2,"Set the project RHI to DirectX 12.")
STEP(3,"Generate Visual Studio project files from the <font face='Courier'>.uproject</font>.")
STEP(4,"Build from Visual Studio, or let the editor compile on open.")
P("<font face='Courier'>Spout.dll</font> is staged automatically to the plugin's <font face='Courier'>Binaries/Win64</font> via Unreal's runtime dependency system. Do not copy it by hand.")
FIG("SP-004")
H2("Set DirectX 12")
P("Edit &gt; Project Settings &gt; Platforms &gt; Windows &gt; Default RHI &gt; DirectX 12, then restart the editor.")
FIG("SP-002")
H2("Enable the plugin")
P("The descriptor enables the plugin by default. Confirm in Edit &gt; Plugins by searching \"Spout Plugin\". Restart the editor if you change the enabled state.")
FIG("SP-001")
H2("Verify it loaded")
P("Open the Output Log (Window &gt; Output Log) and look for <font face='Courier'>Spout Plugin Loaded</font> and, after first use of any node, <font face='Courier'>SpoutDX11on12 Initialized.</font>. If <font face='Courier'>Spout.dll</font> is missing, the plugin logs an explicit startup error naming the paths it checked.")
FIG("SP-013")

H1("4. Quick Start: Sending the Game Viewport")
P("This is the fastest way to get Unreal's rendered image into another application. It sends the game window backbuffer - exactly what you see in Play mode.")
H3("Build the Blueprint")
STEP(1,"Create a Blueprint Actor (or open the Level Blueprint) and open its Event Graph.")
STEP(2,"Right-click the canvas, search \"Spout Sender\", and place the node.")
FIG("SP-017")
STEP(3,"Connect Event Tick to the Spout Sender execution input. Tick calls it every frame, which keeps the stream alive.")
STEP(4,"Set <b>Spout Name</b> to a unique, stable name, for example <font face='Courier'>UnrealViewport</font>. The receiver must use this exact name.")
STEP(5,"Set <b>Send Texture From</b> to <font face='Courier'>GameViewport</font>.")
STEP(6,"Leave <b>Texture Render Target 2D</b> empty - it is ignored in this mode.")
STEP(7,"If the actor is a Blueprint Actor, place one instance in the level so its graph runs.")
FIG("SP-005")
H3("Receive it in another application")
P("Start your external Spout receiver (for example a Spout In TOP in TouchDesigner), then choose <font face='Courier'>UnrealViewport</font> from its sender list. You should see the live Unreal image.")
FIG("SP-011")
NOTE("The viewport is captured when the window is ready to present, so the game window must be actively rendering and not minimized. If nothing appears, confirm Play is running and the name matches exactly.")
H3("Optional: send on a timer instead of every frame")
P("If you do not need full frame-rate output, drive the sender from a looping timer instead of Tick. This reduces GPU copy cost. Put the Spout Sender call in a function or custom event and trigger it with Set Timer by Event.")
FIG("SP-018")

H1("5. Quick Start: Sending a Render Target")
P("Sending a render target gives you control over exactly what is shared - a specific camera, a clean feed without UI, or a custom resolution. The usual way to fill a render target is with a SceneCapture2D actor.")
H3("Set up the capture")
STEP(1,"Create a <font face='Courier'>TextureRenderTarget2D</font> asset. RGBA8 sRGB is the recommended format. You can also create one at runtime with the Create Texture Render Target 2D node.")
FIG("SP-008")
STEP(2,"Add a <font face='Courier'>SceneCapture2D</font> actor to the level and aim it at what you want to send.")
STEP(3,"In the SceneCapture2D Details, set <b>Texture Target</b> to the render target from step 1.")
STEP(4,"Set <b>Capture Source</b> to <b>Final Color (HDR) in Linear Working Color Space</b>. Do not use SceneColor (HDR) - it sends black.")
FIG("SP-007")
H3("Send the render target")
STEP(5,"On Event Tick or a timer, call <b>Spout Sender</b>.")
STEP(6,"Set <b>Send Texture From</b> to <font face='Courier'>TextureRenderTarget2D</font>.")
STEP(7,"Set <b>Texture Render Target 2D</b> to the same render target asset.")
STEP(8,"Set a <b>Spout Name</b> and view the stream in your external application.")
FIG("SP-006")
NOTE("If the output is black, the cause is almost always the Capture Source in step 4. See section 9 for the full black-output checklist.")

H1("6. Quick Start: Receiving a Spout Stream")
P("Receiving pulls a stream from any Spout application into Unreal. You get a live texture, and optionally a ready-to-use material instance and a render target copy.")
H3("Build the Blueprint")
STEP(1,"On Event Tick or a timer, place a <b>Spout Receiver</b> node.")
STEP(2,"Set <b>Spout Name</b> to exactly match the sender, including case.")
STEP(3,"Optionally set <b>Input Material</b> to a base material and <b>Texture Parameter Name</b> to a texture parameter in it (see section 10). Leave both empty if you only need the raw texture.")
STEP(4,"Optionally expand the advanced pins and set <b>Optional Output Render Target</b> to copy the frame into a render target as well.")
STEP(5,"Read the outputs: <b>Out Texture</b> (a live UTexture2D), <b>Out Mat</b> (a dynamic material instance), and the return <b>bool</b> (true when the sender was found).")
STEP(6,"Branch on the return value, and validate Out Mat / Out Texture before using them - the sender may not be running yet.")
STEP(7,"Apply Out Mat to a mesh (Set Material), or sample Out Texture directly.")
FIG("SP-009")
NOTE("If the receiver returns false or the image is blank, check the name match first, then confirm the sender is actually running and being called continuously.")

H1("7. Sender Workflow")
P("The sender publishes a UE texture to other Spout applications as a shared GPU texture. Two sources are available, chosen by the Send Texture From pin.")
H3("GameViewport")
P("Sends the active window backbuffer - what you see in Play mode. The plugin hooks the point in the frame where the window is ready to present, so the capture is correctly timed. The window must be rendering and not minimized. Leave the Texture Render Target 2D pin empty.")
H3("TextureRenderTarget2D")
P("Sends a render target you provide. This is the flexible option: drive the render target with a SceneCapture2D, a Draw Material to Render Target call, or any other render-to-texture method. The render target is resolved on the game thread and copied on the render thread.")
H3("Stream name rules")
B("The name must match the receiver exactly, including case.")
B("Only one sender can own a name at a time. Two senders sharing a name conflict.")
B("Sender and receiver entries are tracked separately, so a sender and a receiver in the same project may use the same name without interfering.")
H3("Lifetime")
P("Call the sender continuously (Tick or timer). Nothing keeps the stream alive between calls. The shared texture is created and resized automatically when the source size or format changes. Call <b>Close Sender</b> to stop a stream; it affects only the sender entry, leaving any receiver with the same name running.")
H3("Performance")
B("Cost scales with resolution. Use a timer when full frame rate is not needed.")
B("The shared texture and the interop wrap are cached and reused while size and format are stable.")
B("No CPU readback happens on the send path - it is a GPU-to-GPU copy.")
B("No sender-side colour correction is applied. What the sender advertises is decided by the render target's own gamma setting: a Force Linear Gamma target publishes a plain _UNORM format, a non-linear one publishes _UNORM_SRGB. Pair a linear sender with Color Space = Linear on the receiver.")

H1("8. Receiver Workflow")
P("The receiver consumes a stream and exposes it to Unreal. Call Spout Receiver continuously with a matching name.")
table([["Output","Type","Notes"],
    ["Out Texture","UTexture2D (by ref)","Transient texture, resized to the stream, updated each call."],
    ["Out Mat","UMaterialInstanceDynamic (by ref)","Created from Input Material on the first call, with Out Texture bound to the parameter."],
    ["Optional Output Render Target","UTextureRenderTarget2D (input)","If provided, the frame is also copied into it; resized automatically."]],
    [3.6*cm,4.2*cm,8.2*cm])
SP(6)
P("<b>Finding senders:</b> use the <b>Spout Info</b> node to list active sender names, widths, and heights - useful for a debug overlay or a dropdown. Names must match exactly. The receiver reconnects automatically when the sender appears or changes size.")
FIG("SP-012")
H3("Receive paths (CVar r.Spout.GPUReceiver)")
P("The receiver has two implementations, selected at runtime by a console variable. Both produce identical images; the difference is performance only.")
table([["Value","Path","Behavior"],
    ["1 (default)","GPU-direct","Shared texture copied straight into the UE destination via D3D11-on-12. No staging texture, no CPU copy."],
    ["0","Legacy CPU readback","Copies to a staging texture, maps it, uploads via RHIUpdateTexture2D. For debugging; more expensive at high resolution."]],
    [2.4*cm,3.2*cm,10.4*cm])
SP(6)
P("Set it in the console, in <font face='Courier'>DefaultEngine.ini</font> under <font face='Courier'>[SystemSettings]</font>, with a <font face='Courier'>-ExecCmds</font> launch argument, or from a Blueprint Execute Console Command. It is read once per receive call, so changes apply on the next call with no restart.")
FIG("SP-015")
NOTE("The <font face='Courier'>OutputRenderTarget</font> field on FSenderStruct is unused. The functional render-target output is the Optional Output Render Target pin on the Spout Receiver node.")

H1("9. SceneCapture2D and Render Target Notes")
P("<b>Recommended Capture Source:</b> Final Color (HDR) in Linear Working Color Space.")
P("<b>Do not use:</b> SceneColor (HDR) in RGB - it produces a black image on the receiver. This is a known limitation, not something tunable through other Capture Source values.")
P("<b>Recommended render target format:</b> RGBA8 sRGB (start here). RGB10A2 is also tested and useful to reduce banding.")
P("The plugin shares the texture as-is and does not color-correct on send, so color correctness depends on the Capture Source and render target format. The tested combination is Final Color (HDR) in Linear Working Color Space with an RGBA8 sRGB render target.")
H3("Black-output checklist")
B("Capture Source is Final Color (HDR) in Linear Working Color Space.")
B("The render target is assigned to the SceneCapture2D Texture Target.")
B("The capture is active and pointed at visible geometry.")
B("You are sending the same render target asset you assigned to the capture.")
B("The render target uses a tested format.")

H1("10. Materials")
P("The receiver can hand you a ready-to-use material instance with the live texture already bound. To use it, your material needs a Texture Parameter whose name matches what you pass to the node.")
STEP(1,"Create a material with a <b>Texture Parameter</b> (the example content is M_SpoutMaterial). Connect it to Base Color or Emissive.")
STEP(2,"Name the parameter. If you leave Texture Parameter Name empty on the node, the plugin uses <font face='Courier'>SpoutTexture</font>, so naming the parameter <font face='Courier'>SpoutTexture</font> matches the default.")
FIG("SP-010")
STEP(3,"Pass the material as <b>Input Material</b> to the Spout Receiver node, and set <b>Texture Parameter Name</b> to the parameter name.")
STEP(4,"On the first call with a valid Input Material, the plugin creates a dynamic material instance, binds Out Texture to the parameter, and returns it as <b>Out Mat</b>.")
STEP(5,"Apply Out Mat to a mesh with a Set Material node.")
P("If you do not need a material, leave Input Material empty and use Out Texture directly, or use the Optional Output Render Target.")

H1("11. Blueprint API Reference")
P("All nodes are static functions on <font face='Courier'>USpoutBPFunctionLibrary</font>, category <font face='Courier'>Spout</font>. The function signatures below are the source of truth; Blueprint pin names match the parameter names.")
table([["Node","Returns"],
    ["Spout Sender","bool"],["Close Sender","void"],["Spout Receiver","bool"],["Close Receiver","void"],
    ["Spout Info","bool"],["Get Sender Info","bool"],["Set Max Senders","int32"],
    ["Get Max Senders","void"],["Create Texture Render Target 2D","UTextureRenderTarget2D*"]],
    [9*cm,7*cm])
SP(8)
def api(title,sig,desc):
    story.append(Paragraph(title,h3)); CODE(sig); P(desc)
api("Spout Sender","bool SpoutSender(FName SpoutName, ESpoutSendTextureFrom SendTextureFrom, UTextureRenderTarget2D* TextureRenderTarget2D)",
    "Publishes a texture to Spout; the GPU copy runs on the render thread. Returns true when accepted or queued. Provide the render target when using the TextureRenderTarget2D source. Call continuously. Common mistakes: empty render target with the render-target source; calling once; name mismatch.")
api("Close Sender","void CloseSender(FName SpoutName)",
    "Removes the named sender and releases its handle. Scoped to senders only, so a receiver using the same name is unaffected.")
api("Spout Receiver","bool SpoutReceiver(const FName SpoutName, UMaterialInterface* InputMaterial,\n                   FName TextureParameterName, UMaterialInstanceDynamic*& OutMat,\n                   UTexture2D*& OutTexture,\n                   UTextureRenderTarget2D* OptionalOutputRenderTarget = nullptr)",
    "Receives a stream. OptionalOutputRenderTarget is an advanced-display pin. Returns true when the sender exists and a receive was enqueued. Outputs Out Mat and Out Texture, which are OUTPUTS and not ownership: the received texture belongs to the plugin, which keeps it alive while the receiver is open. Keep the values this node returns and pass them back on later calls and to Close Receiver. Texture Parameter Name defaults to SpoutTexture when empty. The frame is only copied when the sender published a new one.")
api("Close Receiver","void CloseReceiver(FName SpoutName, UTexture2D*& ReceivedTexture,\n                   UMaterialInstanceDynamic*& ReceivedMaterial)",
    "Closes the receiver session for that name. There is one session per sender name, so this closes it for every graph receiving it, and it detaches the Spout texture parameter from every material the plugin bound it into. The NAME decides what is released, not the pointers: pass back the Out Texture and Out Mat this plugin gave you and both are cleared; pass anything else and it is left untouched with a warning. This node will not destroy a texture it did not create.")
api("Spout Info","bool SpoutInfo(TArray<FSenderStruct>& Senders)",
    "Fills an array with one entry per active sender. Only Name, Width, Height, and bIsAlive are populated. Returns true if the registry was queried.")
api("Get Sender Info","bool GetSenderInfo(FName SpoutName, FSenderStruct& OutSenderStruct)",
    "Looks up one sender by name; fills Name, Width, Height, bIsAlive. Returns true if it exists.")
api("Set Max Senders","int32 SetMaxSenders(int32 Max)",
    "Sets the maximum sender count in Spout's registry and returns the limit actually in effect. Returns 0 when nothing changed, either because the requested value was outside the accepted range (1-1024) or because Spout is unavailable. It does not echo the requested value back.")
api("Get Max Senders","void GetMaxSenders(int32& Max)",
    "Reads the current maximum sender count into Max.")
api("Create Texture Render Target 2D","UTextureRenderTarget2D* CreateTextureRenderTarget2D(int32 Width = 1024, int32 Height = 768,\n                        EPixelFormat Format = PF_B8G8R8A8, bool bForceLinearGamma = true)",
    "Creates a render target at runtime, sets wrap addressing on both axes, and initializes its resource. The result is NOT rooted: keep a reference to it exactly as you would with Create Render Target 2D, or it will be collected. Returns null for invalid dimensions or for a pixel format the plugin cannot share (supported: B8G8R8A8, R8G8B8A8, A2B10G10R10, FloatRGBA). Force Linear Gamma decides what colour space the sender advertises for 8-bit formats: true publishes a plain _UNORM (pair it with Color Space = Linear on the receiver), false publishes _UNORM_SRGB.")
H3("Enums")
P("<b>ESpoutType:</b> Sender, Receiver. &nbsp; <b>ESpoutSendTextureFrom:</b> GameViewport, TextureRenderTarget2D.")
H3("Struct: FSenderStruct")
table([["Field","Type","Filled by plugin?"],
    ["Name","FName","Yes (Info nodes)"],["bIsAlive","bool","Yes (Info nodes)"],
    ["SpoutType","ESpoutType","No"],["Width","int32","Yes (Info nodes)"],
    ["Height","int32","Yes (Info nodes)"],["TextureColor","UTexture2D*","No"],
    ["MaterialInstanceColor","UMaterialInstanceDynamic*","No"],["UserBaseMaterial","UMaterialInterface*","No"],
    ["UserTextureParameter","FName (default SpoutTexture)","No"],["OutputRenderTarget","UTextureRenderTarget2D* (unused)","No"]],
    [4.6*cm,7.4*cm,4*cm])
SP(4)
P("Rely only on Name, Width, Height, and bIsAlive from the Info nodes. The remaining fields are available for your own Blueprint use but are not driven by the plugin.")

H1("12. Example Blueprint Setups")
P("The plugin ships example content under its <font face='Courier'>Content/</font> folder. These are the actual asset names; open them to see working graphs rather than building from scratch.")
table([["Asset","Purpose"],
    ["BP_SpoutSenderExample_","Example sender setup."],["BP_SpoutReceiverExample","Example receiver setup."],
    ["M_SpoutMaterial","Material with a texture parameter for receiver output."],
    ["RT_SpoutSenderTest_","Example render target for sending."],["RT_SpoutReceiverTest","Example render target for receiving."]],
    [7*cm,9*cm])
P("Enable 'Show Plugin Content' in the Content Browser settings to see them.")
FIG("SP-016")

H1("13. Packaging and Distribution")
P("<font face='Courier'>Spout.dll</font> ends up in a different place depending on how the plugin is being used, and conflating them is a common source of "
  "&quot;module could not be loaded&quot; reports. In an editor or other modular build, RuntimeDependencies stages it to the plugin's own "
  "<font face='Courier'>Binaries/Win64</font>. In a packaged game the plugin module is linked into the game executable, so the same "
  "<font face='Courier'>$(BinaryOutputDir)</font> target resolves to the PROJECT's <font face='Courier'>Binaries/Win64</font> beside the .exe, and the plugin's own "
  "Binaries folder is not staged at all. Raw <font face='Courier'>RunUAT BuildPlugin -Package</font> output does NOT receive the DLL from RuntimeDependencies; it stays under "
  "<font face='Courier'>ThirdParty/Spout/lib/amd64</font>, which is why <font face='Courier'>build.ps1</font> copies it into the release archive explicitly. The DLL is delay-loaded, "
  "and the module loads it by full path at startup so the delay-load thunks bind by name.")
P("A release folder contains Binaries (with <font face='Courier'>Spout.dll</font>), Content, Resources, Source, and <font face='Courier'>SpoutPlugin.uplugin</font>. Precompiled releases must include <font face='Courier'>Spout.dll</font> in <font face='Courier'>Binaries/Win64</font>.")
P("Releases are built per engine version with <font face='Courier'>RunUAT BuildPlugin ... -TargetPlatforms=Win64 -Rocket</font>. Debug symbols are removed, but "
  "<font face='Courier'>Intermediate/Build</font> is deliberately KEPT: it carries the precompiled UnrealGame Development and Shipping objects and the "
  "<font face='Courier'>.precompiled</font> manifests that let Blueprint-only users package a project without Visual Studio. Removing it makes such packaging impossible. "
  "Each archive is then reopened and validated before it is published. The repository <font face='Courier'>build.ps1</font> does this for " + ENGINE_RANGE +
  ". A binary built for one engine version will not load in another.")
H3("Clean rebuild after an engine upgrade")
CODE("Project/Binaries\nProject/Intermediate\nPlugins/SpoutPlugin/Binaries\nPlugins/SpoutPlugin/Intermediate")
P("Delete those, regenerate project files, and rebuild from scratch.")

H1("14. Performance Notes")
B("Cost scales with texture resolution on both send and receive. Start lower and raise as needed.")
B("Use a timer instead of Tick when you do not need full frame-rate output.")
B("Leave <font face='Courier'>r.Spout.GPUReceiver</font> at its default of 1. The legacy path (0) adds a full-frame copy, map, and upload each frame.")
B("The send path is a GPU copy through the interop device; there is no CPU readback on send.")
B("Keep per-frame Blueprint logic around the send and receive calls minimal.")
B("Watch large textures: a 4K stream at full frame rate is far more expensive than 1080p.")

H1("15. Limitations")
B("Windows 64-bit only.")
B("DirectX 12 RHI required; the plugin refuses to initialize otherwise.")
B("Both ends must be on the same GPU.")
B("Tested render target formats are limited to RGBA8 sRGB and RGB10A2.")
B("No colour conversion on send. The advertised colour space follows the render target's gamma setting; see section 6.")
B("SceneCapture2D must use Final Color (HDR) in Linear Working Color Space; SceneColor (HDR) is black.")
B("One sender per name.")
B("Senders and receivers must be called continuously.")
B("Spout Info and Get Sender Info populate only Name, Width, Height, bIsAlive.")
B("Marked beta; not guaranteed production-ready without testing. Error handling favors visible, logged failures over silent recovery.")

H1("16. Troubleshooting")
table([["Symptom","What to do"],
    ["Plugin does not compile","Confirm it is in the project Plugins folder, you are on UE 5.3-5.8 with VS 2022; regenerate project files; clean rebuild after engine upgrades."],
    ["Module could not be loaded","Binary must match your exact engine; Spout.dll must be in Binaries/Win64. Read the log for the startup error."],
    ["Spout.dll missing","Copy it from ThirdParty/Spout/lib/amd64/ to Binaries/Win64/, or rebuild from source."],
    ["Sender not visible externally","Confirm DirectX 12; confirm continuous calls; one sender per name; for GameViewport the window must be rendering; look for SpoutDX11on12 Initialized."],
    ["Receiver returns invalid output","Exact name match; check the return value; validate outputs; texture parameter name must match the material (default SpoutTexture)."],
    ["Black texture from SceneCapture2D","Set Capture Source to Final Color (HDR) in Linear Working Color Space."],
    ["Wrong colors or gamma","Adjust render target format and Capture Source; try RGBA8 sRGB first, then RGB10A2."],
    ["Stream name mismatch","Names are case-sensitive; use Spout Info to read exact names."],
    ["Multiple senders conflict","Unique, stable names per stream."],
    ["Performance drops","Keep the GPU receiver default; reduce resolution or update rate; prefer timers."],
    ["Crashes or instability","Update GPU drivers; avoid swapping render target resources at runtime; test resolutions; reproduce in a minimal project."],
    ["External app cannot see stream","Confirm Spout/format support and that both apps are on the same GPU."],
    ["After an engine upgrade","Clean rebuild (delete the four Binaries/Intermediate folders), regenerate, rebuild."]],
    [4.8*cm,11.2*cm])
SP(6)
P("If you can reproduce a missing-DLL failure, the startup log line is a useful diagnostic to keep:")
FIG("SP-014")

H1("17. Best Practices")
B("Use stable, unique Spout names and keep sender and receiver names identical.")
B("Start at a low resolution and raise it only when needed.")
B("Use a timer when full frame rate is not required.")
B("Always validate Out Mat and Out Texture before applying them.")
B("Keep the render target format simple: RGBA8 sRGB first.")
B("For SceneCapture2D sends, always use Final Color (HDR) in Linear Working Color Space.")
B("Test in a minimal project before integrating into production.")
B("Record the engine version and the plugin binary version together; binaries are version-specific.")

H1("18. Appendix")
H3("File structure")
CODE("SpoutPlugin.uplugin\nSource/SpoutPlugin/\n  SpoutPlugin.Build.cs\n  Public/   SpoutBPFunctionLibrary.h, SpoutModule.h\n  Private/  SpoutBPFunctionLibrary.cpp, SpoutModule.cpp,\n            SpoutSender.*, SpoutReceiver.*, SpoutD3DContext.*,\n            SpoutD3DUtils.*, SpoutSenderRegistry.*, SpoutTextureUtils.*\nThirdParty/Spout/   include/, lib/amd64/ (Spout.dll, Spout.lib)\nContent/            example Blueprints, material, render targets\nResources/          Icon128.png\nDocs/               documentation and this manual\nbuild.ps1, build.bat")
H3("Module and dependency summary")
P("Single Runtime module <font face='Courier'>SpoutPlugin</font>, Win64 only. Public dependencies: Core, CoreUObject, Engine, RHI, RenderCore. Private: ApplicationCore, Slate, SlateCore, Projects, D3D12RHI, D3D11RHI, plus the engine DX12 and DX11 third-party libraries. Spout.dll is delay-loaded and staged as a runtime dependency.")
H3("Third-party attribution")
B("Spout: http://spout.zeal.co/")
B("Based on the original repository by zuyi53: https://github.com/zuyi53/Spout-UE5")
H3("License")
P("MIT License. Commercial use is allowed, but reselling the plugin as-is as a standalone product goes against the spirit of the project.")
H3("Glossary")
table([["Term","Meaning"],
    ["Spout","Windows GPU texture-sharing technology between real-time applications."],
    ["Sender","An application publishing a shared texture."],
    ["Receiver","An application reading a shared texture."],
    ["Render Target","A UTextureRenderTarget2D; a texture you can render into and read back."],
    ["SceneCapture2D","An Unreal actor that renders a view into a render target."],
    ["RHI","Rendering Hardware Interface, Unreal's graphics API abstraction."],
    ["D3D12","DirectX 12, the graphics API Unreal uses; required by this plugin."],
    ["D3D11-on-D3D12","An interop layer exposing D3D12 resources to D3D11 code (here, Spout)."],
    ["Shared texture","A GPU texture exposed via a shared handle so another process can read it."]],
    [4*cm,12*cm])
H3("Figure index")
fig_rows=[["Figure","File","Shows"]]
for fid in ["SP-001","SP-002","SP-003","SP-004","SP-017","SP-005","SP-018","SP-006","SP-007",
            "SP-008","SP-009","SP-010","SP-011","SP-012","SP-013","SP-014","SP-015","SP-016"]:
    fn,cap,_=FIGURES[fid]; fig_rows.append([fid,fn,cap])
table(fig_rows,[1.8*cm,6.6*cm,7.6*cm])

frame_body=Frame(2*cm,2*cm,A4[0]-4*cm,A4[1]-3.5*cm,id="body")
frame_title=Frame(2*cm,2*cm,A4[0]-4*cm,A4[1]-3*cm,id="title")
doc=ManualDoc(OUT,pagesize=A4,title="SpoutPlugin User Manual",author="Kesson",subject="SpoutPlugin v%s User Manual"%VERSION)
doc.addPageTemplates([PageTemplate(id="title",frames=[frame_title],onPage=blank),
                      PageTemplate(id="body",frames=[frame_body],onPage=footer)])
doc.multiBuild(story)
embedded=sum(1 for fid in FIGURES if os.path.exists(os.path.join(SHOTS,FIGURES[fid][0])))
print("WROTE",OUT,os.path.getsize(OUT),"bytes |",embedded,"of",len(FIGURES),"screenshots embedded")
