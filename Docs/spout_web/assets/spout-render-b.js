// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/* SpoutPlugin Docs — render part B: quick-start cards + reference sections */

/* ---- Quick-start card data (each becomes an anchored card) ---- */
window.QS_CARDS = [
  {
    id: "qs-viewport", num: "04", kicker: "QUICK START 04", glyph: ICON.monitor(22),
    name: "Send the Game Viewport",
    intro: "The fastest way to get Unreal's rendered image into another application. It sends the game window backbuffer - exactly what you see in Play mode.",
    flow: [{label:"UE",name:"GameViewport"},{label:"NODE",name:"Spout Sender"},{label:"OUT",name:"UnrealViewport"}],
    steps: [
      { t: "Create a Blueprint Actor (or open the Level Blueprint) and open its Event Graph." },
      { t: "Right-click the canvas, search \"Spout Sender\", and place the node." },
      { t: "Connect <code>Event Tick</code> to the Spout Sender execution input. Tick calls it every frame, which requests continuous frame updates; registration persists until close." },
      { t: "Set <code>Spout Name</code> to a unique, stable name, for example <code>UnrealViewport</code>. The receiver must use this exact name." },
      { t: "Set <code>Send Texture From</code> to <code>GameViewport</code>." },
      { t: "Leave <code>Texture Render Target 2D</code> empty - it is ignored in this mode." },
      { t: "If the actor is a Blueprint Actor, place one instance in the level so its graph runs." },
      { t: "Start your external Spout receiver (e.g. a Spout In TOP in TouchDesigner), then choose <code>UnrealViewport</code> from its sender list." }
    ],
    mistake: "The game window must be actively rendering and not minimized - the viewport is captured when the window is ready to present. If nothing appears, confirm Play is running and the name matches exactly.",
    optional: "Optional: drive the sender from a looping timer instead of Tick to reduce GPU copy cost. Put the Spout Sender call in a function or custom event and trigger it with Set Timer by Event.",
    figs: ["SP-017", "SP-005", "SP-011", "SP-018"]
  },
  {
    id: "qs-rendertarget", num: "05", kicker: "QUICK START 05", glyph: ICON.target(22),
    name: "Send a Render Target",
    intro: "Sending a render target gives you control over exactly what is shared - a specific camera, a clean feed without UI, or a custom resolution. The usual way to fill a render target is with a SceneCapture2D actor.",
    flow: [{label:"CAPTURE",name:"SceneCapture2D"},{label:"RT",name:"RenderTarget2D"},{label:"NODE",name:"Spout Sender"}],
    steps: [
      { t: "Create a <code>TextureRenderTarget2D</code> asset. <code>RGBA8 sRGB</code> is recommended. You can also create one at runtime with the Create Texture Render Target 2D node." },
      { t: "Add a <code>SceneCapture2D</code> actor to the level and aim it at what you want to send." },
      { t: "In the SceneCapture2D Details, set <code>Texture Target</code> to the render target from step 1." },
      { t: "Set <code>Capture Source</code> to <b>Final Color (HDR) in Linear Working Color Space</b>. Qualify other capture sources with known pixels and matching receiver color space." },
      { t: "On Event Tick or a timer, call <code>Spout Sender</code>." },
      { t: "Set <code>Send Texture From</code> to <code>TextureRenderTarget2D</code>." },
      { t: "Set <code>Texture Render Target 2D</code> to the same render target asset." },
      { t: "Set a <code>Spout Name</code> and view the stream in your external application." }
    ],
    mistake: "If the output is black, the cause is almost always the Capture Source in step 4. See section 9 for the full black-output checklist.",
    warnMistake: true,
    figs: ["SP-008", "SP-007", "SP-006"]
  },
  {
    id: "qs-receive", num: "06", kicker: "QUICK START 06", glyph: ICON.receive(22),
    name: "Receive a Spout Stream",
    intro: "Receiving pulls a stream from any Spout application into Unreal. You get a live texture, and optionally a ready-to-use material instance and a render target copy.",
    flow: [{label:"EXT",name:"TouchDesigner"},{label:"NODE",name:"Spout Receiver"},{label:"USE",name:"Mesh / Material"}],
    reverse: false,
    steps: [
      { t: "On Event Tick or a timer, place a <code>Spout Receiver</code> node." },
      { t: "Set <code>Spout Name</code> to exactly match the sender, including case." },
      { t: "Optionally set <code>Input Material</code> to a base material and <code>Texture Parameter Name</code> to a texture parameter in it (see Materials). Leave both empty if you only need the raw texture." },
      { t: "Optionally expand the advanced pins and set <code>Optional Output Render Target</code> to copy the frame into a render target as well." },
      { t: "Read the outputs: <code>Out Texture</code> (a live UTexture2D), <code>Out Mat</code> (a dynamic material instance), and the return bool (true when the sender was found)." },
      { t: "Branch on the return value, and validate Out Mat / Out Texture before using them - the sender may not be running yet." },
      { t: "Apply <code>Out Mat</code> to a mesh (Set Material), or sample <code>Out Texture</code> directly." }
    ],
    mistake: "If the receiver returns false or the image is blank, check the name match first, then confirm the sender is actually running and being called continuously.",
    figs: ["SP-009"]
  }
];

window.renderQuickStartCards = function () {
  return QS_CARDS.map(c => `
    <article class="qs-card" id="${c.id}" data-screen-label="${c.id}">
      <div class="qs-head" role="button" tabindex="0" aria-expanded="false">
        <div class="qs-glyph">${c.glyph}</div>
        <div class="qs-meta">
          <div class="qs-kicker mono">${c.kicker}</div>
          <div class="qs-name">${c.name} <a class="anchor-link" data-anchor>#</a></div>
        </div>
        <span class="qs-chev">${ICON.chevron(20)}</span>
      </div>
      <div class="qs-body"><div><div class="qs-inner">
        <p style="margin-top:14px">${c.intro}</p>
        ${C.flow(c.flow, c.reverse)}
        <h3 class="block-title">Build it</h3>
        ${C.steps(c.steps)}
        ${C.callout(c.warnMistake ? "error" : "warn", "Common mistake", c.mistake)}
        ${c.optional ? C.callout("tip", "Optional", c.optional) : ""}
        ${c.figs.map(f => C.figure(f)).join("")}
      </div></div></div>
    </article>`).join("");
};

/* ========================================================== */
window.RENDER_B = function () {
  const out = [];

  /* ===================== 11. API ===================== */
  out.push(C.section("api", `
    ${C.secHead("11", "Reference", "Blueprint API Reference",
      "All nodes are static functions on <code>USpoutBPFunctionLibrary</code>, category Spout. The signatures below are the source of truth; Blueprint pin names match the parameter names.")}
    <div class="api-toolbar reveal">
      <div class="api-search">${ICON.search(16)}<input type="text" id="api-search-input" placeholder="Search nodes, parameters, types..." aria-label="Search API nodes"></div>
      <div class="api-filters" id="api-filters">
        <button class="fchip active" data-filter="all">All</button>
        <button class="fchip" data-filter="sender">Sender</button>
        <button class="fchip" data-filter="receiver">Receiver</button>
        <button class="fchip" data-filter="info">Info</button>
        <button class="fchip" data-filter="utility">Utility</button>
      </div>
    </div>
    <div id="api-list" class="reveal"></div>
    <p class="ts-empty" id="api-empty" style="display:none">No nodes match your search.</p>

    <h3 class="block-title reveal">Enums</h3>
    <div class="grid grid-2 reveal">
      ${SPOUT.enums.map(e => `<div class="panel"><div class="mono" style="color:var(--accent);font-size:13px;margin-bottom:8px">${e.name}</div><div style="display:flex;gap:8px;flex-wrap:wrap">${e.values.map(v => `<span class="pill">${v}</span>`).join("")}</div></div>`).join("")}
    </div>

    <h3 class="block-title reveal">Struct &mdash; <code>FSenderStruct</code></h3>
    <div class="reveal tbl-wrap">
      <table class="data">
        <thead><tr><th>Field</th><th>Type</th><th>Filled by plugin?</th></tr></thead>
        <tbody>${SPOUT.structFields.map(f => `<tr><td><code>${f[0]}</code></td><td>${C.esc(f[1])}</td><td>${f[2] ? '<span class="pill" style="color:var(--success)"><span class="dot" style="background:var(--success);box-shadow:0 0 7px rgba(var(--success-rgb),.8)"></span>Yes (Info nodes)</span>' : '<span style="color:var(--muted)">No</span>'}</td></tr>`).join("")}</tbody>
      </table>
    </div>
    <div class="reveal">${C.callout("warn", "Rely on four fields only", "From the Info nodes, rely only on <code>Name</code>, <code>Width</code>, <code>Height</code>, and <code>bIsAlive</code>. The remaining fields are available for your own Blueprint use but are not driven by the plugin.")}</div>
  `));

  /* ===================== 12. EXAMPLES ===================== */
  out.push(C.section("examples", `
    ${C.secHead("12", "Reference", "Example Blueprint Setups",
      "The plugin ships example content under its <code>Content/</code> folder. These are the actual asset names - open them to see working graphs rather than building from scratch.")}
    <div class="grid grid-2 reveal">
      ${SPOUT.examples.map(e => `<div class="panel" style="display:flex;gap:14px;align-items:flex-start;padding:18px">
        <span style="color:var(--accent);flex:none">${e[0].startsWith("BP_") ? ICON.box(20) : e[0].startsWith("M_") ? ICON.swatch(20) : ICON.target(20)}</span>
        <div><div class="mono" style="color:var(--ink);font-size:14px;font-weight:600">${e[0]}</div><div style="font-size:13.5px;color:var(--muted);margin-top:3px">${e[1]}</div></div>
      </div>`).join("")}
    </div>
    <div class="reveal">${C.callout("tip", "Show Plugin Content", "Enable <strong>Show Plugin Content</strong> in the Content Browser settings to see these assets.")}</div>
    ${C.figure("SP-016")}
  `));

  /* ===================== 13. PACKAGING ===================== */
  out.push(C.section("packaging", `
    ${C.secHead("13", "Reference", "Packaging & Distribution",
      "Spout.dll is staged to the plugin's <code>Binaries/Win64</code> via RuntimeDependencies to <code>$(BinaryOutputDir)</code> - the same place used by project builds, RunUAT BuildPlugin, and release zips. The DLL is delay-loaded so the module loads even when it resolves late.")}
    <div class="reveal pkg-diagram">
      ${["Binaries", "Content", "Resources", "Source", ".uplugin"].map((p,i) => `<div class="pkg-item" style="--d:${i*0.08}s"><span class="pkg-ic">${ICON.folder(18)}</span><span class="mono">${p}${p==="Binaries"?' <span class="pkg-note">(with Spout.dll)</span>':''}</span></div>`).join('<div class="pkg-link"></div>')}
    </div>
    <div class="reveal">${C.leadList([
      "A release folder contains <strong>Binaries</strong> (with Spout.dll), Content, Resources, Source, and SpoutPlugin.uplugin.",
      "Precompiled releases must include <code>Spout.dll</code> in <code>Binaries/Win64</code>.",
      "Releases are built per engine version with <code>RunUAT BuildPlugin ... -TargetPlatforms=Win64 -Rocket</code>, then .pdb files are removed and a clean folder is zipped per version. <code>Intermediate/Build</code> is deliberately kept, so Blueprint-only users can package a project without a compiler.",
      "The repository <code>build.ps1</code> does this for UE 5.3 through 5.8. A binary built for one engine version will not load in another."
    ])}</div>
    <h3 class="block-title reveal">Clean rebuild after an engine upgrade</h3>
    <div class="reveal"><p>Delete these four folders, regenerate project files, and rebuild from scratch:</p>
    ${C.code(["Project/Binaries", "Project/Intermediate", "Plugins/SpoutPlugin/Binaries", "Plugins/SpoutPlugin/Intermediate"].join("\n"), {raw:true})}</div>
  `));

  /* ===================== 14. PERFORMANCE ===================== */
  out.push(C.section("performance", `
    ${C.secHead("14", "Reference", "Performance Notes",
      "Cost scales with texture resolution on both send and receive. Start lower and raise as needed.")}
    <div class="reveal panel cost-meter" id="cost-meter">
      <div class="mono" style="color:var(--muted);font-size:12px;margin-bottom:14px">RELATIVE PER-FRAME COST BY RESOLUTION</div>
      <div class="cost-row"><span class="cr-label">720p</span><div class="cost-bar"><div class="cost-fill" data-w="22"></div></div><span class="cost-val">light</span></div>
      <div class="cost-row"><span class="cr-label">1080p</span><div class="cost-bar"><div class="cost-fill" data-w="40"></div></div><span class="cost-val">baseline</span></div>
      <div class="cost-row"><span class="cr-label">1440p</span><div class="cost-bar"><div class="cost-fill" data-w="64"></div></div><span class="cost-val">heavier</span></div>
      <div class="cost-row hot"><span class="cr-label">4K</span><div class="cost-bar"><div class="cost-fill" data-w="100"></div></div><span class="cost-val">far heavier</span></div>
    </div>
    <div class="reveal">${C.leadList(SPOUT.performance)}</div>
  `));

  /* ===================== 15. LIMITATIONS ===================== */
  out.push(C.section("limitations", `
    ${C.secHead("15", "Reference", "Limitations",
      "Honest constraints. None of these are bugs - they are the boundaries of what Spout and this beta can do today.")}
    <div class="grid grid-2 reveal">
      ${SPOUT.limitations.map(l => `<div class="lim-card"><span class="ld"></span><p>${l}</p></div>`).join("")}
    </div>
  `));

  /* ===================== 16. TROUBLESHOOTING ===================== */
  out.push(C.section("troubleshooting", `
    ${C.secHead("16", "Reference", "Troubleshooting",
      "Search a symptom, or run the first-checks panel. Every row clicks open to its fix.")}
    <h3 class="block-title reveal" style="margin-top:0">First checks</h3>
    <div class="ts-firstchecks reveal" id="ts-firstchecks">
      ${SPOUT.firstChecks.map(c => `<span class="ts-check" tabindex="0" role="checkbox" aria-checked="false"><span class="box">${ICON.check(11)}</span>${c}</span>`).join("")}
    </div>
    <div class="api-search reveal" style="margin:18px 0">${ICON.search(16)}<input type="text" id="ts-search" placeholder="Search symptoms..." aria-label="Search troubleshooting" style="flex:1;background:none;border:none;color:var(--ink);font-family:var(--sans);font-size:14px;outline:none"></div>
    <div id="ts-list" class="reveal"></div>
    <p class="ts-empty" id="ts-empty" style="display:none">No symptoms match.</p>
    ${C.figure("SP-014")}
  `));

  /* ===================== 17. BEST PRACTICES ===================== */
  out.push(C.section("bestpractices", `
    ${C.secHead("17", "Reference", "Best Practices",
      "Eight habits that keep a Spout pipeline stable from prototype to show.")}
    <div class="grid grid-2 reveal">
      ${SPOUT.bestPractices.map((b,i) => `<div class="bp-card"><span class="bp-num">${String(i+1).padStart(2,"0")}</span><p>${b}</p></div>`).join("")}
    </div>
  `));

  /* ===================== 18. APPENDIX ===================== */
  out.push(C.section("appendix", `
    ${C.secHead("18", "Reference", "Appendix & Glossary",
      "File structure, dependencies, attribution, license, and the terms used throughout this manual.")}
    <h3 class="block-title reveal" style="margin-top:0">File structure</h3>
    <div class="reveal panel"><div class="ftree">
<span class="dir">SpoutPlugin.uplugin</span>
<span class="dir">Source/SpoutPlugin/</span>
<span class="ind">  SpoutPlugin.Build.cs</span>
<span class="ind">  Public/</span>  <span class="note">SpoutBPFunctionLibrary.h, SpoutModule.h</span>
<span class="ind">  Private/</span> <span class="note">SpoutBPFunctionLibrary.cpp, SpoutModule.cpp,</span>
<span class="ind">          </span> <span class="note">SpoutSender.*, SpoutReceiver.*, SpoutD3DContext.*,</span>
<span class="ind">          </span> <span class="note">SpoutD3DUtils.*, SpoutSenderRegistry.*, SpoutTextureUtils.*</span>
<span class="dir">ThirdParty/Spout/</span> <span class="note">include/, lib/amd64/ (Spout.dll, Spout.lib)</span>
<span class="dir">Content/</span> <span class="note">example Blueprints, material, render targets</span>
<span class="dir">Resources/</span> <span class="note">Icon128.png</span>
<span class="dir">Docs/</span> <span class="note">documentation and this manual</span>
<span class="ind">build.ps1, build.bat</span>
    </div></div>

    <h3 class="block-title reveal">Module & dependency summary</h3>
    <div class="reveal"><p>Single Runtime module <strong>SpoutPlugin</strong>, Win64 only.</p>
    <div class="grid grid-2">
      <div class="panel"><div class="mono" style="color:var(--accent);font-size:12px;margin-bottom:8px">PUBLIC DEPENDENCIES</div><div style="display:flex;gap:6px;flex-wrap:wrap">${["Core","CoreUObject","Engine","RHI","RenderCore"].map(d=>`<span class="pill">${d}</span>`).join("")}</div></div>
      <div class="panel"><div class="mono" style="color:var(--warn);font-size:12px;margin-bottom:8px">PRIVATE DEPENDENCIES</div><div style="display:flex;gap:6px;flex-wrap:wrap">${["ApplicationCore","Slate","SlateCore","Projects","D3D12RHI","D3D11RHI"].map(d=>`<span class="pill">${d}</span>`).join("")}</div></div>
    </div>
    <p style="font-size:14px;color:var(--muted);margin-top:12px">Plus the engine DX12 and DX11 third-party libraries. Spout.dll is delay-loaded and staged as a runtime dependency.</p></div>

    <div class="grid grid-2 reveal" style="margin-top:24px">
      <div class="panel"><h3 class="block-title" style="margin-top:0">Third-party attribution</h3>
        <ul class="lead-list" style="margin:0">
          <li>Spout: <a href="http://spout.zeal.co/" target="_blank" rel="noopener">spout.zeal.co</a></li>
          <li>Based on the original repository by zuyi53: <a href="https://github.com/zuyi53/Spout-UE5" target="_blank" rel="noopener">github.com/zuyi53/Spout-UE5</a></li>
        </ul>
      </div>
      <div class="panel"><h3 class="block-title" style="margin-top:0">License</h3>
        <p style="font-size:14px;margin:0"><strong>MIT License.</strong> Commercial use is allowed, but reselling the plugin as-is as a standalone product goes against the spirit of the project.</p>
      </div>
    </div>

    <h3 class="block-title reveal">Glossary</h3>
    <div class="reveal panel">
      ${SPOUT.glossary.map(g => `<div class="gloss-item"><div class="gloss-term">${g[0]}</div><div class="gloss-def">${g[1]}</div></div>`).join("")}
    </div>

    <h3 class="block-title reveal">Figure index</h3>
    <div class="reveal tbl-wrap">
      <table class="data">
        <thead><tr><th>Figure</th><th>File</th><th>Shows</th></tr></thead>
        <tbody>${Object.keys(SPOUT.figures).map(id => { const f=SPOUT.figures[id]; return `<tr><td><code>${id}</code></td><td class="mono" style="font-size:12px">${f.file}</td><td>${f.title}${f.optional?' <span class="req-tag maybe">optional</span>':''}</td></tr>`; }).join("")}</tbody>
      </table>
    </div>

    <div class="foot">
      <div>SpoutPlugin User Manual &mdash; v${SPOUT.facts.version.split(" ")[0]} &middot; <span class="mono">${SPOUT.facts.date}</span><br><span style="color:var(--faint)">${SPOUT.facts.module} &middot; ${SPOUT.facts.platform} &middot; ${SPOUT.facts.engine}</span></div>
      <div class="mono">Reformatted from the source PDF.<br>No screenshots or asset names invented.</div>
    </div>
  `));

  return out.join("\n");
};
