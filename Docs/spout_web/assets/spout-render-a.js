// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/* SpoutPlugin Docs — render part A: hero -> materials */
window.RENDER_A = function () {
  const out = [];

  /* ===================== HERO ===================== */
  out.push(`
  <section class="section" id="hero" data-screen-label="hero" style="padding-top:48px">
    <div class="hero">
      <div class="hero-copy reveal">
        <div class="hero-tag mono">${ICON.pulse(14)} GPU TEXTURE SHARING / UNREAL ENGINE 5</div>
        <h1 class="hero-title">Spout<span class="hl">Plugin</span></h1>
        <p class="hero-sub">Unreal Engine 5 Spout Sender / Receiver Plugin</p>
        <p class="hero-lead">GPU texture sharing between Unreal Engine and external real-time applications such as TouchDesigner, through a D3D11-on-D3D12 bridge.</p>
        <div class="hero-btns">
          <a class="btn primary" href="#qs-viewport">${ICON.bolt(16)} Quick Start</a>
          <a class="btn" href="#api">${ICON.terminal(16)} Blueprint API</a>
          <a class="btn" href="#troubleshooting">${ICON.wrench(16)} Troubleshooting</a>
          <a class="btn ghost" href="#install">${ICON.download(16)} Download <span class="mono tiny">(Patreon)</span></a>
        </div>
        <div class="hero-pills">
          <span class="pill"><span class="dot"></span>v${C.esc(SPOUT.facts.version)}</span>
          <span class="pill"><span class="dot"></span>UE 5.3 - 5.8</span>
          <span class="pill warn"><span class="dot"></span>Windows only</span>
          <span class="pill warn"><span class="dot"></span>DX12 required</span>
          <span class="pill"><span class="dot"></span>Blueprint-first</span>
        </div>
      </div>
      <div class="hero-diagram reveal" aria-hidden="true">
        <div class="hd-label mono">LIVE TEXTURE STREAM</div>
        ${heroBridge()}
      </div>
    </div>
  </section>`);

  /* ===================== 1. WHAT IT IS ===================== */
  out.push(C.section("what", `
    ${C.secHead("01", "Overview", "What SpoutPlugin Is",
      "Spout is a Windows technology for sharing GPU textures between real-time applications with no disk or network transfer. One application publishes a texture - a <strong>sender</strong> - and another reads it live - a <strong>receiver</strong>. It is widely used to move video between tools such as TouchDesigner, Resolume, Notch, and OBS.")}
    <div class="reveal">
      <p class="sec-intro" style="margin-bottom:22px">SpoutPlugin brings Spout into Unreal Engine 5. It sends Unreal's rendered output, or any render target, to other Spout applications, and receives Spout streams back into Unreal as textures and materials. <strong>The default transfer path stays on the GPU; CPU receiver fallback uses system memory.</strong></p>
    </div>

    <div class="reveal" style="margin:24px 0">
      <div class="toggle-row" role="tablist" id="sr-toggle">
        <button class="active" data-sr="send" role="tab">${ICON.send(14)} Sender</button>
        <button data-sr="receive" role="tab">${ICON.receive(14)} Receiver</button>
      </div>
      <div id="sr-flow">${C.flow([
        { label: "Source", name: "Unreal Engine", sub: "viewport or render target" },
        { label: "Bridge", name: "D3D11-on-D3D12", sub: "shared texture" },
        { label: "Out", name: "TouchDesigner", sub: "or any Spout app" }
      ])}</div>
      <p class="hint-line mono">Sender: Unreal &rarr; external app</p>
    </div>

    <h3 class="block-title reveal">The problem it solves</h3>
    <div class="reveal">${C.callout("tip", "Why a bridge exists",
      "Spout shares <strong>DirectX 11</strong> textures, but modern UE5 runs on the <strong>DirectX 12</strong> RHI. The plugin stands up a D3D11-on-D3D12 interop device and bridges UE's D3D12 textures to Spout's D3D11 shared textures. You never touch this directly - it is why DirectX 12 is required and why the plugin is Windows-only.", ICON.layers(20))}</div>

    <div class="grid grid-2 reveal" style="margin-top:24px">
      <div class="panel">
        <h3 class="block-title" style="margin-top:0;color:var(--success)">${ICON.check(16)} What it does</h3>
        <p style="font-size:14.5px;margin:0">GPU texture send and receive over Spout, from the game viewport or a render target, into a texture, a dynamic material instance, or an optional render target.</p>
      </div>
      <div class="panel">
        <h3 class="block-title" style="margin-top:0;color:var(--warn)">${ICON.x ? ICON.x(16) : ""} What it does not do</h3>
        <p style="font-size:14.5px;margin:0">It is not a video codec, not a network streaming solution, and not a color-management system. It does not apply gamma or color correction on send. Windows-only, DirectX 12 required.</p>
      </div>
    </div>

    <h3 class="block-title reveal">Typical use cases</h3>
    <div class="reveal">${C.leadList([
      "Send an Unreal scene or camera into TouchDesigner for compositing, projection mapping, or LED output.",
      "Receive a live feed from another tool into Unreal and map it onto a mesh or use it in a material.",
      "Build a real-time pipeline between Unreal and a media-server or VJ setup."
    ])}</div>
  `));

  /* ===================== 2. REQUIREMENTS ===================== */
  const reqs = [
    ["Windows 64-bit", "The only supported platform.", "must"],
    ["Unreal Engine 5.3 - 5.8", "Binaries are built per engine version.", "must"],
    ["DirectX 12 RHI enabled", "The plugin creates a D3D11-on-D3D12 device and will not initialize on any other RHI. It logs \"SpoutPlugin requires DX12 RHI.\" and stops.", "must"],
    ["Spout.dll", "Bundled under ThirdParty/Spout/lib/amd64/ and staged to Binaries/Win64/ at build time.", "must"],
    ["Same GPU on both ends", "Spout's shared-texture model does not span GPUs.", "must"],
    ["Visual Studio 2022", "With the C++ toolchain and a matching Windows SDK - for source builds only.", "maybe"],
    ["Project-local install", "Install into your project's Plugins/ folder, the qualified consumer layout. Qualify engine-wide installations separately.", "must"]
  ];
  out.push(C.section("requirements", `
    ${C.secHead("02", "Setup", "Requirements",
      "Everything you need before the first node runs. The two hard gates are <strong>Windows</strong> and <strong>DirectX 12</strong> - the plugin refuses to initialize without them.")}
    <div class="panel reveal" style="padding:6px 0">
      ${reqs.map(r => `
        <div class="req-row">
          <div class="req-status ${r[2] === "must" ? "req" : "opt"}">${r[2] === "must" ? ICON.check(13) : ICON.info(13)}</div>
          <div class="req-main"><b>${r[0]}</b><div class="req-sub">${r[1]}</div></div>
          <span class="req-tag ${r[2]}">${r[2] === "must" ? "required" : "source build"}</span>
        </div>`).join("")}
    </div>
    <details class="xnote reveal">
      <summary>${ICON.layers(17)} Why DX12? <span class="xchev">${ICON.chevron(16)}</span></summary>
      <div class="xbody">Spout shares DirectX 11 textures, while modern UE5 runs with the DirectX 12 RHI. The plugin creates a D3D11-on-D3D12 bridge so UE's D3D12 textures can be exposed to Spout's D3D11 shared textures. You never interact with the bridge directly - it is exactly why the plugin is DX12-only and Windows-only.</div>
    </details>
  `));

  /* ===================== 3. INSTALLATION ===================== */
  out.push(C.section("install", `
    ${C.secHead("03", "Setup", "Installation & Project Setup",
      "Five short stages. Get the binaries in the right folder, switch the project to DirectX 12, enable the plugin, then confirm it loaded in the Output Log.")}
    <div class="tabs reveal" id="install-tabs">
      <div class="tab-bar" role="tablist">
        <button class="tab-btn active" data-tab="t0" role="tab"><span class="tnum">01</span>Precompiled Binary</button>
        <button class="tab-btn" data-tab="t1" role="tab"><span class="tnum">02</span>Source Build</button>
        <button class="tab-btn" data-tab="t2" role="tab"><span class="tnum">03</span>Set DirectX 12</button>
        <button class="tab-btn" data-tab="t3" role="tab"><span class="tnum">04</span>Enable Plugin</button>
        <button class="tab-btn" data-tab="t4" role="tab"><span class="tnum">05</span>Verify Loaded</button>
      </div>

      <div class="tab-panel active" id="t0">
        ${C.steps([
          { t: "Download the ZIP matching your <b>exact</b> engine version.", note: "Prebuilt binaries are on Patreon." },
          { t: "Extract the SpoutPlugin folder into <code>MyProject/Plugins/</code>." },
          { t: "Confirm <code>Spout.dll</code> is at <code>Plugins/SpoutPlugin/Binaries/Win64/Spout.dll</code>." },
          { t: "Set the project RHI to DirectX 12 (see tab 03)." },
          { t: "Open the project. If Unreal offers to rebuild due to an engine-version mismatch, use the correct ZIP or build from source." }
        ])}
        ${C.figure("SP-003")}
      </div>

      <div class="tab-panel" id="t1">
        ${C.steps([
          { t: "Copy SpoutPlugin into <code>MyProject/Plugins/</code>." },
          { t: "Set the project RHI to DirectX 12." },
          { t: "Generate Visual Studio project files from the <code>.uproject</code>." },
          { t: "Build from Visual Studio, or let the editor compile on open." }
        ])}
        ${C.callout("tip", "Automatic staging", "Spout.dll is staged automatically to the plugin's <code>Binaries/Win64</code> via Unreal's runtime dependency system. <strong>Do not copy it by hand.</strong>")}
        ${C.figure("SP-004")}
      </div>

      <div class="tab-panel" id="t2">
        ${C.callout("info", "Project Settings path", "<code>Edit &gt; Project Settings &gt; Platforms &gt; Windows &gt; Default RHI &gt; DirectX 12</code>, then restart the editor.")}
        ${C.figure("SP-002")}
      </div>

      <div class="tab-panel" id="t3">
        <p>The descriptor enables the plugin by default. Confirm in <code>Edit &gt; Plugins</code> by searching "Spout Plugin". Restart the editor if you change the enabled state.</p>
        ${C.figure("SP-001")}
      </div>

      <div class="tab-panel" id="t4">
        <p>Open the Output Log (<code>Window &gt; Output Log</code>) and look for <code>Spout Plugin Loaded</code> and, after first use of any node, <code>SpoutDX11on12 Initialized.</code></p>
        ${C.callout("warn", "If Spout.dll is missing", "The plugin logs an explicit startup error naming the paths it checked. See Troubleshooting for the exact recovery steps.")}
        ${C.figure("SP-013")}
      </div>
    </div>
  `));

  /* ===================== QUICK START (4,5,6) ===================== */
  out.push(C.section("quickstart", `
    ${C.secHead("04 - 06", "Quick Start", "Three ways in",
      "Pick a starting point. Each card expands into a full step-by-step graph with its screenshot placeholders and the one mistake people hit most.")}
    <div class="grid reveal" id="qs-cards" style="gap:14px"></div>
  `));

  /* The 3 quick-start cards are injected by app.js into #qs-cards (so each gets its own id/anchor) */

  /* ===================== 7. SENDER WORKFLOW ===================== */
  out.push(C.section("sender", `
    ${C.secHead("07", "Workflow", "Sender Workflow",
      "The sender publishes a UE texture to other Spout applications as a shared GPU texture. Two sources are available, chosen by the <strong>Send Texture From</strong> pin.")}
    <div class="reveal" style="margin:6px 0 22px">
      <div class="toggle-row" id="sender-toggle">
        <button class="active" data-mode="vp">${ICON.monitor(14)} GameViewport</button>
        <button data-mode="rt">${ICON.target(14)} TextureRenderTarget2D</button>
      </div>
      <div id="sender-mode">
        <div class="panel" data-mode-panel="vp">
          <p style="margin-top:0">Sends the active window backbuffer - what you see in Play mode. The plugin hooks the point in the frame where the window is ready to present, so the capture is correctly timed. The window must be rendering and not minimized. Leave the <code>Texture Render Target 2D</code> pin empty.</p>
        </div>
        <div class="panel" data-mode-panel="rt" hidden>
          <p style="margin-top:0">Sends a render target you provide. The flexible option: drive the render target with a SceneCapture2D, a <code>Draw Material to Render Target</code> call, or any other render-to-texture method. The render target is resolved on the game thread and copied on the render thread.</p>
        </div>
      </div>
    </div>

    <h3 class="block-title reveal">Stream name rules</h3>
    <div class="reveal">${C.leadList([
      "The name must match the receiver <strong>exactly</strong>, including case.",
      "Only one sender can own a name at a time. Two senders sharing a name conflict.",
      "Sender and receiver entries are tracked separately, so a sender and a receiver in the same project may use the same name without interfering."
    ])}</div>

    <h3 class="block-title reveal">Lifetime</h3>
    <div class="reveal"><p>Call the sender <strong>continuously</strong> (Tick or timer). Registration and the last image persist between calls until close. The shared texture is created and resized automatically when the source size or format changes. Call <code>Close Sender</code> to stop a stream; it affects only the sender entry, leaving any receiver with the same name running.</p>
    <div class="stream-status" id="stream-status">
      <span class="ss-dot"></span><span class="ss-text mono">stream alive</span>
      <span class="ss-note">pulses only while Tick / Timer is calling the sender</span>
    </div></div>

    <h3 class="block-title reveal">Performance</h3>
    <div class="reveal">${C.leadList([
      "Cost scales with resolution. Use a timer when full frame rate is not needed.",
      "The shared texture and the interop wrap are cached and reused while size and format are stable.",
      "No CPU readback happens on the send path - it is a GPU-to-GPU copy.",
      "No sender-side gamma or color correction is applied. Manage color on the source side."
    ])}</div>
  `));

  /* ===================== 8. RECEIVER WORKFLOW ===================== */
  out.push(C.section("receiver", `
    ${C.secHead("08", "Workflow", "Receiver Workflow",
      "The receiver consumes a stream and exposes it to Unreal. Call <strong>Spout Receiver</strong> continuously with a matching name.")}
    <div class="tbl-wrap reveal">
      <table class="data">
        <thead><tr><th>Output</th><th>Type</th><th>Notes</th></tr></thead>
        <tbody>
          <tr><td><code>Out Texture</code></td><td>UTexture2D (by ref)</td><td>Transient texture, resized to the stream, updated each call.</td></tr>
          <tr><td><code>Out Mat</code></td><td>UMaterialInstanceDynamic (by ref)</td><td>Created from Input Material on the first call, with Out Texture bound to the parameter.</td></tr>
          <tr><td><code>Optional Output Render Target</code></td><td>UTextureRenderTarget2D (input)</td><td>If provided, the frame is also copied into it; resized automatically.</td></tr>
        </tbody>
      </table>
    </div>

    <h3 class="block-title reveal">Finding senders</h3>
    <div class="reveal"><p>Use the <code>Spout Info</code> node to list active sender names, widths, and heights - useful for a debug overlay or a dropdown. Names must match exactly. The receiver reconnects automatically when the sender appears or changes size.</p>
    ${C.figure("SP-012")}</div>

    <h3 class="block-title reveal">Receive paths &mdash; <code>r.Spout.GPUReceiver</code></h3>
    <div class="reveal"><p>The receiver has two implementations, selected at runtime by a console variable. Both produce identical images; the difference is performance only.</p>
    <div class="cvar-grid">
      <div class="cvar-card def">
        <div class="cvar-val">1 <span class="badge">default</span></div>
        <div class="cvar-path">GPU-direct</div>
        <div class="cvar-desc">Shared texture copied straight into the UE destination via D3D11-on-12. No staging texture, no CPU readback.</div>
      </div>
      <div class="cvar-card">
        <div class="cvar-val">0 <span class="badge legacy">legacy</span></div>
        <div class="cvar-path">CPU readback</div>
        <div class="cvar-desc">Copies to a staging texture, maps it, uploads via RHIUpdateTexture2D. For debugging; more expensive.</div>
      </div>
    </div>
    ${C.callout("info", "Four ways to set it", "In the console, in <code>DefaultEngine.ini</code> under <code>[SystemSettings]</code>, with a <code>-ExecCmds</code> launch argument, or from a Blueprint <code>Execute Console Command</code>. It is read once per receive call, so changes apply on the next call with no restart.")}
    ${C.figure("SP-015")}
    ${C.callout("warn", "OutputRenderTarget field is unused", "The <code>OutputRenderTarget</code> field on <code>FSenderStruct</code> is unused. The functional render-target output is the <strong>Optional Output Render Target</strong> pin on the Spout Receiver node.")}</div>
  `));

  /* ===================== 9. SCENECAPTURE2D ===================== */
  out.push(C.section("scenecapture", `
    ${C.secHead("09", "Workflow", "SceneCapture2D & Render Target Notes",
      "The single most common cause of a black output. Get the Capture Source right and most problems disappear.")}
    <div class="reveal sc-split">
      <div class="sc-good">
        <div class="sc-tag good">${ICON.check(14)} Use this</div>
        <div class="sc-val">Final Color (HDR) in Linear Working Color Space</div>
        <div class="sc-note">A starting point to qualify with your rendering pipeline.</div>
      </div>
      <div class="sc-bad">
        <div class="sc-tag bad">${ICON.warn(14)} Qualify separately</div>
        <div class="sc-val">SceneColor (HDR) in RGB</div>
        <div class="sc-note">Output depends on capture settings, exposure and format. The plugin does not tone-map or convert pixels.</div>
      </div>
    </div>
    <div class="reveal"><p>Recommended render target format: <code>RGBA8 sRGB</code> (start here). <code>RGB10A2</code> is also tested and useful to reduce banding. The plugin shares the texture as-is and does not color-correct on send, so color correctness depends on the Capture Source and render target format. The tested combination is Final Color (HDR) in Linear Working Color Space with an RGBA8 sRGB render target.</p></div>

    <h3 class="block-title reveal">Black-output checklist</h3>
    <div class="reveal panel" id="black-checklist">
      ${["Capture Source is Final Color (HDR) in Linear Working Color Space.",
         "The render target is assigned to the SceneCapture2D Texture Target.",
         "The capture is active and pointed at visible geometry.",
         "You are sending the same render target asset you assigned to the capture.",
         "The render target uses a tested format."].map(t => `
        <label class="ts-check chk" style="display:flex;width:100%;margin-bottom:8px;border-radius:8px">
          <span class="box">${ICON.check(11)}</span><span>${t}</span>
        </label>`).join("")}
    </div>
  `));

  /* ===================== 10. MATERIALS ===================== */
  out.push(C.section("materials", `
    ${C.secHead("10", "Workflow", "Materials",
      "The receiver can hand you a ready-to-use material instance with the live texture already bound. Your material just needs a <strong>Texture Parameter</strong> whose name matches what you pass to the node.")}
    <div class="reveal">${C.steps([
      { t: "Create a material with a Texture Parameter (the example content is <code>M_SpoutMaterial</code>). Connect it to Base Color or Emissive." },
      { t: "Name the parameter. If you leave <code>Texture Parameter Name</code> empty on the node, the plugin uses <code>SpoutTexture</code> - so naming the parameter <code>SpoutTexture</code> matches the default." },
      { t: "Pass the material as <code>Input Material</code> to the Spout Receiver node, and set <code>Texture Parameter Name</code> to the parameter name." },
      { t: "On the first call with a valid Input Material, the plugin creates a dynamic material instance, binds Out Texture to the parameter, and returns it as <code>Out Mat</code>." },
      { t: "Apply <code>Out Mat</code> to a mesh with a Set Material node." }
    ])}</div>
    ${C.figure("SP-010")}
    <div class="reveal">${C.callout("tip", "No material needed?", "Leave <code>Input Material</code> empty and use <code>Out Texture</code> directly, or use the Optional Output Render Target.")}</div>
  `));

  return out.join("\n");
};

/* hero bridge SVG with animated packets */
function heroBridge() {
  const stages = [
    { x: 60, label: "UNREAL", name: "Engine" },
    { x: 215, label: "D3D12", name: "RHI" },
    { x: 370, label: "BRIDGE", name: "11-on-12" },
    { x: 525, label: "SPOUT", name: "Shared" }
  ];
  return `
  <svg viewBox="0 0 640 300" class="bridge-svg" preserveAspectRatio="xMidYMid meet">
    <defs>
      <linearGradient id="line-grad" x1="0" y1="0" x2="1" y2="0">
        <stop offset="0" stop-color="var(--accent-deep)"/>
        <stop offset="1" stop-color="var(--accent)"/>
      </linearGradient>
    </defs>
    <line x1="60" y1="150" x2="600" y2="150" stroke="var(--line)" stroke-width="1.5"/>
    <line x1="60" y1="150" x2="600" y2="150" stroke="url(#line-grad)" stroke-width="2" class="bridge-flowline"/>
    ${stages.map(s => `
      <g class="bridge-node">
        <rect x="${s.x - 42}" y="118" width="84" height="64" rx="10" fill="var(--panel)" stroke="var(--line)"/>
        <circle cx="${s.x}" cy="150" r="4" fill="var(--accent)" class="node-dot"/>
        <text x="${s.x}" y="140" text-anchor="middle" class="bn-label">${s.label}</text>
        <text x="${s.x}" y="170" text-anchor="middle" class="bn-name">${s.name}</text>
      </g>`).join("")}
    <g class="bridge-out">
      <rect x="560" y="118" width="74" height="64" rx="10" fill="var(--panel-2)" stroke="var(--accent-deep)"/>
      <text x="597" y="144" text-anchor="middle" class="bn-label" fill="var(--accent)">EXT APP</text>
      <text x="597" y="166" text-anchor="middle" class="bn-name">TD / VJ</text>
    </g>
    <g class="packets"></g>
  </svg>`;
}
