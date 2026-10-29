// Copyright (c) 2025-2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
// Spout plugin for Unreal Engine 5 - https://github.com/kessoning/Spout-UE5

/* ============================================================
   SpoutPlugin Docs — content data
   Source of truth: SpoutPlugin.uplugin (version) + Docs/USER_MANUAL_SOURCE.md (content).
   The version below must match the plugin descriptor; Scripts/Validate-Documentation.ps1
   fails the release when it does not.
   ============================================================ */
window.SPOUT = {};

SPOUT.facts = {
  version: "1.0.0",
  engine: "UE 5.3 - 5.8",
  module: "SpoutPlugin (Runtime)",
  platform: "Windows 64-bit",
  date: "2026-08-03"
};

/* ---- Sidebar navigation groups ---- */
SPOUT.nav = [
  { group: "Overview", items: [
    { id: "hero", num: "", label: "SpoutPlugin" },
    { id: "what", num: "01", label: "What It Is" },
    { id: "requirements", num: "02", label: "Requirements" },
    { id: "install", num: "03", label: "Installation & Setup" }
  ]},
  { group: "Quick Start", items: [
    { id: "qs-viewport", num: "04", label: "Send Game Viewport" },
    { id: "qs-rendertarget", num: "05", label: "Send Render Target" },
    { id: "qs-receive", num: "06", label: "Receive a Stream" }
  ]},
  { group: "Workflows", items: [
    { id: "sender", num: "07", label: "Sender Workflow" },
    { id: "receiver", num: "08", label: "Receiver Workflow" },
    { id: "scenecapture", num: "09", label: "SceneCapture2D & RT" },
    { id: "materials", num: "10", label: "Materials" }
  ]},
  { group: "Reference", items: [
    { id: "api", num: "11", label: "Blueprint API" },
    { id: "examples", num: "12", label: "Example Setups" },
    { id: "packaging", num: "13", label: "Packaging" },
    { id: "performance", num: "14", label: "Performance" },
    { id: "limitations", num: "15", label: "Limitations" },
    { id: "troubleshooting", num: "16", label: "Troubleshooting" },
    { id: "bestpractices", num: "17", label: "Best Practices" },
    { id: "appendix", num: "18", label: "Appendix & Glossary" }
  ]}
];

/* ---- Screenshot figures (exact filenames from PDF) ---- */
SPOUT.figures = {
  "SP-001": { file: "SP-001_plugin-enabled.png", title: "Plugin enabled in the Plugins browser", desc: "Open Edit > Plugins and search \"Spout\". Frame the Spout Plugin tile so its name, the ticked enabled checkbox, and the Rendering category are visible. Include the Beta label if shown." },
  "SP-002": { file: "SP-002_rhi-directx12.png", title: "Default RHI set to DirectX 12", desc: "Edit > Project Settings > Platforms > Windows. Scroll to Targeted RHIs / Default RHI and capture the dropdown showing DirectX 12 selected." },
  "SP-003": { file: "SP-003_plugin-folder.png", title: "SpoutPlugin folder inside project Plugins", desc: "File Explorer at MyProject/Plugins/SpoutPlugin/. Show the top-level folders (Binaries, Content, Resources, Source, ThirdParty) and SpoutPlugin.uplugin in the same view." },
  "SP-004": { file: "SP-004_spout-dll-binaries.png", title: "Spout.dll staged in Binaries/Win64", desc: "File Explorer at MyProject/Plugins/SpoutPlugin/Binaries/Win64/. Make sure both Spout.dll and UnrealEditor-SpoutPlugin.dll are visible." },
  "SP-005": { file: "SP-005_sender-viewport-graph.png", title: "Blueprint: sending the game viewport every frame", desc: "Event Tick (or timer) -> Spout Sender. Spout Name = UnrealViewport, Send Texture From = GameViewport, Texture Render Target 2D empty. Zoom so pin text is legible." },
  "SP-006": { file: "SP-006_sender-rendertarget-graph.png", title: "Blueprint: sending a TextureRenderTarget2D", desc: "Event Tick (or timer) -> Spout Sender. Send Texture From = TextureRenderTarget2D, render target asset on the pin, Spout Name visible." },
  "SP-007": { file: "SP-007_scenecapture-source.png", title: "SceneCapture2D Capture Source set correctly", desc: "Select the SceneCapture2D actor, open the Capture Source dropdown showing 'Final Color (HDR) in Linear Working Color Space'. The most important shot for avoiding black output - make it unambiguous." },
  "SP-008": { file: "SP-008_rendertarget-settings.png", title: "TextureRenderTarget2D asset settings", desc: "Open the render target asset. Capture the Render Target Format (RGBA8 sRGB recommended) and Size X / Size Y fields." },
  "SP-009": { file: "SP-009_receiver-graph.png", title: "Blueprint: receiving a stream and using the outputs", desc: "Event Tick (or timer) -> Spout Receiver. Name matches the sender, advanced pins expanded so Optional Output Render Target is visible. Out Mat / Out Texture / return-value pins wired (Branch on return; Out Mat -> Set Material)." },
  "SP-010": { file: "SP-010_material-texture-parameter.png", title: "Material with a Texture Parameter for receiver output", desc: "Open M_SpoutMaterial. Select the Texture Parameter node, show its Parameter Name (default SpoutTexture) connected to Base Color / Emissive." },
  "SP-011": { file: "SP-011_external-app-receiving.png", title: "External application receiving the Unreal stream", desc: "In TouchDesigner, add a Spout In TOP. Select the Unreal sender by name. Capture the live Unreal image with the sender name visible in the dropdown." },
  "SP-012": { file: "SP-012_spout-info-debug.png", title: "Spout Info output listing active senders", desc: "A small debug graph calling Spout Info and printing the array (name, width, height). Capture the graph or the on-screen / log output showing one or more sender entries." },
  "SP-013": { file: "SP-013_log-loaded.png", title: "Output Log confirming the plugin loaded", desc: "Window > Output Log filtered for LogSpoutPlugin. Capture lines 'Spout Plugin Loaded' and 'SpoutDX11on12 Initialized.'" },
  "SP-014": { file: "SP-014_log-dll-missing.png", title: "Output Log showing a missing Spout.dll error", desc: "Reproduce by temporarily removing Spout.dll. Capture the LogSpoutPlugin error naming the Binaries/Win64 and ThirdParty paths it checked.", optional: true },
  "SP-015": { file: "SP-015_cvar-console.png", title: "Setting r.Spout.GPUReceiver from the console", desc: "Open the editor console (tilde) in PIE. Type r.Spout.GPUReceiver and capture the printed current value, plus an example of setting it to 0 or 1." },
  "SP-016": { file: "SP-016_example-content.png", title: "The plugin example content folder", desc: "Enable 'Show Plugin Content' and open the SpoutPlugin content folder. Capture the assets: BP_SpoutSenderExample_, BP_SpoutReceiverExample, M_SpoutMaterial, RT_SpoutSenderTest_, RT_SpoutReceiverTest." },
  "SP-017": { file: "SP-017_add-sender-node.png", title: "Adding the Spout Sender node", desc: "In a Blueprint Event Graph, right-click empty canvas, type \"Spout Sender\". Capture the search results showing the node under the Spout category before placing it." },
  "SP-018": { file: "SP-018_timer-setup.png", title: "Driving the sender on a timer", desc: "Set Timer by Event (or by Function Name) calling the function that runs Spout Sender. Time = your interval (e.g. 0.033 for ~30fps), Looping = true. Capture the timer node and bound event name." }
};

/* ---- Advised capture spec per figure (aspect ratio + resolution) ---- */
SPOUT.figureSpecs = {
  "SP-001": { ar: "16:9",  res: "1920 × 1080", note: "full editor window" },
  "SP-002": { ar: "16:9",  res: "1920 × 1080", note: "full editor window" },
  "SP-003": { ar: "16:10", res: "1600 × 1000", note: "File Explorer window" },
  "SP-004": { ar: "16:10", res: "1600 × 1000", note: "File Explorer window" },
  "SP-005": { ar: "16:9",  res: "2560 × 1440", note: "zoom for legible pins" },
  "SP-006": { ar: "16:9",  res: "2560 × 1440", note: "zoom for legible pins" },
  "SP-007": { ar: "4:3",   res: "1280 × 960",  note: "Details panel + dropdown" },
  "SP-008": { ar: "4:3",   res: "1280 × 960",  note: "asset editor closeup" },
  "SP-009": { ar: "16:9",  res: "2560 × 1440", note: "zoom for legible pins" },
  "SP-010": { ar: "16:9",  res: "1920 × 1080", note: "material editor" },
  "SP-011": { ar: "16:9",  res: "1920 × 1080", note: "external app window" },
  "SP-012": { ar: "16:9",  res: "1920 × 1080", note: "graph or on-screen output" },
  "SP-013": { ar: "21:9",  res: "1920 × 820",  note: "Output Log — wide & short" },
  "SP-014": { ar: "21:9",  res: "1920 × 820",  note: "Output Log — wide & short" },
  "SP-015": { ar: "16:9",  res: "1920 × 1080", note: "console during PIE" },
  "SP-016": { ar: "16:9",  res: "1920 × 1080", note: "Content Browser" },
  "SP-017": { ar: "4:3",   res: "1280 × 960",  note: "node search popup" },
  "SP-018": { ar: "16:9",  res: "1920 × 1080", note: "graph closeup" }
};

/* image folder the placeholders resolve against */
SPOUT.imageBase = "public/docs/spout/screenshots/";

/* ---- Blueprint API nodes ---- */
SPOUT.api = [
  {
    name: "Spout Sender", cat: "sender", ret: "bool",
    sig: "bool SpoutSender(FName SpoutName, ESpoutSendTextureFrom SendTextureFrom, UTextureRenderTarget2D* TextureRenderTarget2D)",
    purpose: "Publishes a texture to Spout. The GPU copy runs on the render thread. Returns true when accepted or queued. Provide the render target when using the TextureRenderTarget2D source. Call continuously.",
    inputs: [
      ["SpoutName", "FName — unique, stable stream name. Must match the receiver exactly, including case."],
      ["SendTextureFrom", "ESpoutSendTextureFrom — GameViewport or TextureRenderTarget2D."],
      ["TextureRenderTarget2D", "UTextureRenderTarget2D* — required for the render-target source; ignored for GameViewport."]
    ],
    returns: "true when the publish was accepted or queued.",
    when: "Every frame on Event Tick, or on a looping timer when full frame rate is not needed.",
    mistakes: ["Empty render target with the TextureRenderTarget2D source.", "Calling it once instead of continuously.", "Name mismatch with the receiver."]
  },
  {
    name: "Close Sender", cat: "sender", ret: "void",
    sig: "void CloseSender(FName SpoutName)",
    purpose: "Removes the named sender and releases its handle. Scoped to senders only, so a receiver using the same name is unaffected.",
    inputs: [["SpoutName", "FName — the sender stream to close."]],
    returns: "Nothing (void).",
    when: "When you want to stop publishing a stream. Senders are not kept alive between calls, so this is only needed for an explicit teardown."
  },
  {
    name: "Spout Receiver", cat: "receiver", ret: "bool",
    sig: "bool SpoutReceiver(const FName SpoutName, UMaterialInterface* InputMaterial, FName TextureParameterName, UMaterialInstanceDynamic*& OutMat, UTexture2D*& OutTexture, UTextureRenderTarget2D* OptionalOutputRenderTarget = nullptr, ESpoutColorSpace ColorSpace = ESpoutColorSpace::Auto)",
    purpose: "Receives a stream. OptionalOutputRenderTarget is an advanced-display pin. Returns true when the sender exists and a receive was enqueued. Outputs Out Mat and Out Texture. Texture Parameter Name defaults to SpoutTexture when empty. Validate outputs before use.",
    inputs: [
      ["SpoutName", "FName — must match the sender exactly, including case."],
      ["InputMaterial", "UMaterialInterface* — optional base material; leave empty for raw texture only."],
      ["TextureParameterName", "FName — texture parameter to bind. Defaults to SpoutTexture when empty."],
      ["OptionalOutputRenderTarget", "UTextureRenderTarget2D* — advanced pin; the frame is also copied into it if provided. It must have the received texture's pixel format: the incoming one, or RGBA16f for a 10-bit sender received with sRGB."],
      ["ColorSpace", "ESpoutColorSpace — advanced pin. Auto (default) decodes 8-bit frames as sRGB, the Spout convention, and reads 10-bit and float frames as linear; Linear skips decoding (use it for a sender whose 8-bit bytes are linear, such as a Force Linear Gamma render target); sRGB decodes sRGB-encoded values, 8-bit through the sampler and 10-bit into a linear 16-bit float texture, and is refused for float frames. The format cannot tell you which applies: senders publish 8-bit video as plain _UNORM, and an Unreal viewport stream carries display values in 10 bits, so receive a viewport stream with sRGB."]
    ],
    returns: "true when the sender exists and a receive was enqueued. Branch on this.",
    when: "Every frame on Event Tick, or on a timer. Validate Out Mat / Out Texture before using them - the sender may not be running yet.",
    outputs: [
      ["Out Mat", "UMaterialInstanceDynamic (by ref) — created from Input Material on the first call, Out Texture bound to the parameter."],
      ["Out Texture", "UTexture2D (by ref) — transient texture, resized to the stream, updated each call."]
    ]
  },
  {
    name: "Spout Info", cat: "info", ret: "bool",
    sig: "bool SpoutInfo(TArray<FSenderStruct>& Senders)",
    purpose: "Fills an array with one entry per active sender. Only Name, Width, Height, and bIsAlive are populated. Returns true if the registry was queried.",
    inputs: [["Senders", "TArray<FSenderStruct>& — filled with one entry per active sender."]],
    returns: "true if the registry was queried.",
    when: "To list active sender names, widths, and heights - useful for a debug overlay or a dropdown."
  },
  {
    name: "Get Sender Info", cat: "info", ret: "bool",
    sig: "bool GetSenderInfo(FName SpoutName, FSenderStruct& OutSenderStruct)",
    purpose: "Looks up one sender by name. Fills Name, Width, Height, bIsAlive. Returns true if it exists.",
    inputs: [["SpoutName", "FName — the sender to look up."], ["OutSenderStruct", "FSenderStruct& — filled with Name, Width, Height, bIsAlive."]],
    returns: "true if a sender with that name exists.",
    when: "To query one known sender's size and alive state."
  },
  {
    name: "Set Max Senders", cat: "utility", ret: "int32",
    sig: "int32 SetMaxSenders(int32 Max)",
    purpose: "Sets the maximum sender count in Spout's registry and returns the limit actually in effect afterwards. It does not echo the requested value back.",
    inputs: [["Max", "int32 — the new maximum sender count (accepted range 1–1024)."]],
    returns: "The limit actually in effect afterwards. 0 when the request could not be applied: the requested value was outside 1–1024 or Spout is unavailable.",
    when: "When you need more (or fewer) concurrent senders than Spout's default registry size."
  },
  {
    name: "Get Max Senders", cat: "utility", ret: "void",
    sig: "void GetMaxSenders(int32& Max)",
    purpose: "Reads the current maximum sender count into Max.",
    inputs: [["Max", "int32& — receives the current maximum sender count."]],
    returns: "Nothing (void); value comes back through Max.",
    when: "To read the registry's configured sender capacity."
  },
  {
    name: "Create Texture Render Target 2D", cat: "utility", ret: "UTextureRenderTarget2D*",
    sig: "UTextureRenderTarget2D* CreateTextureRenderTarget2D(int32 Width = 1024, int32 Height = 768, EPixelFormat Format = PF_B8G8R8A8, bool bForceLinearGamma = true)",
    purpose: "Creates a render target at runtime, sets wrap addressing on both axes, and initializes its resource. The result is NOT rooted: keep a reference to it, exactly as with Create Render Target 2D.",
    inputs: [
      ["Width", "int32 = 1024 — render target width."],
      ["Height", "int32 = 768 — render target height."],
      ["Format", "EPixelFormat = PF_B8G8R8A8 — pixel format. Supported: B8G8R8A8, R8G8B8A8, A2B10G10R10, FloatRGBA."],
      ["bForceLinearGamma", "bool = true — decides what an 8-bit target's bytes contain; the sender publishes plain _UNORM either way. true stores linear values (set Color Space = Linear on the receiver); false stores sRGB-encoded bytes, the Spout convention, which a receiver on Auto reads correctly."]
    ],
    returns: "The new UTextureRenderTarget2D, or null for invalid dimensions or an unsupported pixel format. Not rooted.",
    when: "To make a render target in Blueprint at runtime instead of authoring an asset."
  }
];

SPOUT.enums = [
  { name: "ESpoutType", values: ["Sender", "Receiver"] },
  { name: "ESpoutSendTextureFrom", values: ["GameViewport", "TextureRenderTarget2D"] }
];

SPOUT.structFields = [
  ["Name", "FName", true],
  ["bIsAlive", "bool", true],
  ["SpoutType", "ESpoutType", false],
  ["Width", "int32", true],
  ["Height", "int32", true],
  ["TextureColor", "UTexture2D*", false],
  ["MaterialInstanceColor", "UMaterialInstanceDynamic*", false],
  ["UserBaseMaterial", "UMaterialInterface*", false],
  ["UserTextureParameter", "FName (default SpoutTexture)", false],
  ["OutputRenderTarget", "UTextureRenderTarget2D* (unused)", false]
];

/* ---- Troubleshooting matrix (every row from the PDF) ---- */
SPOUT.troubleshooting = [
  { symptom: "Plugin does not compile", fix: "Confirm it is in the project Plugins folder, you are on UE 5.3-5.8 with Visual Studio 2022, then regenerate project files and rebuild." },
  { symptom: "Module could not be loaded", fix: "The binary must match your exact engine version, and Spout.dll must be in Binaries/Win64. Read the Output Log for the startup diagnostic." },
  { symptom: "Spout.dll missing", fix: "Copy it from ThirdParty/Spout/lib/amd64/ to Binaries/Win64/, or rebuild from source so it is staged automatically." },
  { symptom: "Sender not visible externally", fix: "Confirm DirectX 12; confirm the sender is called continuously; one sender per name; for GameViewport the window must be actively rendering and not minimized." },
  { symptom: "Receiver returns invalid output", fix: "Exact name match; check the return value; validate outputs; the texture parameter name must match the material." },
  { symptom: "Black texture from SceneCapture2D", fix: "Set Capture Source to Final Color (HDR) in Linear Working Color Space. Capture Source, exposure and output format must be qualified together." },
  { symptom: "Wrong colors or gamma", fix: "Adjust render target format and Capture Source; try RGBA8 sRGB first, then RGB10A2. 8-bit sources are published as _UNORM whatever their gamma, so set Color Space = Linear on the receiver (Unreal or external) for a Force Linear Gamma source." },
  { symptom: "Viewport stream too bright in Unreal", fix: "The GameViewport sender publishes display (sRGB-encoded) values in 10 bits, which Auto reads as linear. Set Color Space = sRGB on the receiving Unreal; an output render target must then be RGBA16f." },
  { symptom: "Spout Demo Receiver: \"No sender detected\" for an sRGB render target", fix: "Fixed in 1.0.0. Earlier release candidates published 8-bit sRGB render targets as _UNORM_SRGB, which Spout 2.007's OpenGL receivers refuse; the sender now publishes plain _UNORM with unchanged bytes." },
  { symptom: "Log: \"shared-texture access was unavailable\"", fix: "Another application still held the shared texture after the access wait (r.Spout.AccessWaitMs, default 2 ms). The sender skips that frame and publishes the next; the receiver copies the sender's newest frame on a later call. Throttled to one line per stream every 5 seconds; Get Spout Stream Status counts every miss. The wait applies to each attempt on the render thread, not to a frame, so several locked streams add up; r.Spout.AccessWaitMs 0 never waits (zero-wait acquisition only; other fixes stay active)." },
  { symptom: "Frames skip or repeat, no access misses", fix: "Sampling, not contention: the receiver reads once per Unreal frame and a sender at the same frame rate drifts in and out of alignment with it. r.Spout.AccessWaitMs does not change this; a higher Unreal frame rate reduces it but does not guarantee every frame. See Limitations." },
  { symptom: "Stream name mismatch", fix: "Use the exact external name from Spout Info. Local FName keys do not distinguish case." },
  { symptom: "Multiple senders conflict", fix: "Use unique, stable names per stream. Only one sender can own a name at a time." },
  { symptom: "Performance drops", fix: "Keep the GPU receiver at its default; reduce resolution or update rate; prefer timers over Tick." },
  { symptom: "Crashes or instability", fix: "Update GPU drivers; avoid swapping render target resources at runtime; test resolutions; reproduce in a minimal project." },
  { symptom: "External app cannot see stream", fix: "Confirm Spout/format support and that both apps are on the same GPU." },
  { symptom: "After an engine upgrade", fix: "Do a clean rebuild: delete the four Binaries/Intermediate folders, regenerate project files, and rebuild." }
];

SPOUT.firstChecks = [
  "DX12 enabled?", "Spout.dll present?", "Exact name match?",
  "Sender called continuously?", "Same GPU?", "Tested render target format?",
  "Correct SceneCapture2D Capture Source?"
];

/* ---- Limitations (all from PDF) ---- */
SPOUT.limitations = [
  "Windows 64-bit only.",
  "DirectX 12 RHI required; the plugin refuses to initialize otherwise.",
  "Both ends must be on the same GPU.",
  "Supported formats are BGRA8, RGBA8, RGB10A2 and RGBA16F; color interpretation must match the source.",
  "No colour conversion on send. Every 8-bit source is published as plain _UNORM with its bytes unchanged; _UNORM_SRGB is never published, because Spout's OpenGL receivers refuse it.",
  "The sRGB format tag is not carried: an sRGB render target and a Force Linear Gamma render target both publish _UNORM, so receivers cannot tell the source's colour intent from the format. _UNORM does not mean linear; by the Spout convention the bytes are sRGB-encoded, and a Force Linear Gamma source needs Color Space = Linear on the receiver.",
  "A 10-bit format does not mean linear either: an Unreal viewport stream carries display (sRGB-encoded) values in 10 bits. Receive it with Color Space = sRGB, which decodes each frame into a linear 16-bit float texture. HDR back buffers are not decoded.",
  "Final Color (HDR) in Linear Working Color Space is a starting point; qualify Capture Source, exposure and output format together.",
  "One sender per name.",
  "Senders and receivers must be called continuously.",
  "Receiving samples a stream once per Unreal frame and does not queue frames. At equal frame rates the two clocks drift in and out of alignment; near alignment single frames are skipped and the previous one repeated, with no access contention involved. In 1080p 60 fps testing this lowered delivery to 87 to 89 % of distinct frames in individual 10-second windows, while whole runs delivered about 99 %. r.Spout.AccessWaitMs does not change it; a higher Unreal frame rate reduces it but does not guarantee every frame, because jitter, hitches, stalls and contention can still lose frames.",
  "The access wait (r.Spout.AccessWaitMs, default 2 ms) applies to each attempt, not to a frame, so several busy streams add up: in testing, a peer that locked eight 1080p streams added about 13 ms of render-thread waiting per frame. 0 restores zero-wait acquisition only; the plugin's other fixes stay active.",
  "A keyed-mutex sender that holds its texture for tens of milliseconds can cause runs of consecutive skipped frames, with or without the access wait. With one 1080p 60 fps keyed test sender holding its texture 20 ms on every tenth frame, delivery was 84 % with up to 14 frames skipped in a row. Qualify keyed senders with their actual workload.",
  "Spout Info and Get Sender Info populate only Name, Width, Height, bIsAlive.",
  "The descriptor sets IsBetaVersion to false; qualification is still required. Native failures are reported and transient connection failures retry with backoff."
];

/* ---- Best practices ---- */
SPOUT.bestPractices = [
  "Use stable, unique Spout names and keep sender and receiver names identical.",
  "Start at a low resolution and raise it only when needed.",
  "Use a timer when full frame rate is not required.",
  "Always validate Out Mat and Out Texture before applying them.",
  "Keep the render target format simple: RGBA8 sRGB first.",
  "For SceneCapture2D sends, start with Final Color (HDR) in Linear Working Color Space and qualify the chosen capture pipeline.",
  "Test in a minimal project before integrating into production.",
  "Record the engine version and the plugin binary version together; binaries are version-specific."
];

/* ---- Performance ---- */
SPOUT.performance = [
  "Cost scales with texture resolution on both send and receive. Start lower and raise as needed.",
  "Use a timer instead of Tick when you do not need full frame-rate output.",
  "Leave r.Spout.GPUReceiver at its default of 1. The legacy path (0) adds a full-frame copy, map, and upload each frame.",
  "The send path is a GPU copy through the interop device; there is no CPU readback on send.",
  "Keep per-frame Blueprint logic around the send and receive calls minimal.",
  "Watch large textures: a 4K stream at full frame rate is far more expensive than 1080p."
];

/* ---- Examples ---- */
SPOUT.examples = [
  ["BP_SpoutSenderExample_", "Example sender setup."],
  ["BP_SpoutReceiverExample", "Example receiver setup."],
  ["M_SpoutMaterial", "Material with a texture parameter for receiver output."],
  ["RT_SpoutSenderTest_", "Example render target for sending."],
  ["RT_SpoutReceiverTest", "Example render target for receiving."]
];

/* ---- Glossary ---- */
SPOUT.glossary = [
  ["Spout", "Windows GPU texture-sharing technology between real-time applications."],
  ["Sender", "An application publishing a shared texture."],
  ["Receiver", "An application reading a shared texture."],
  ["Render Target", "A UTextureRenderTarget2D; a texture you can render into and read back."],
  ["SceneCapture2D", "An Unreal actor that renders a view into a render target."],
  ["RHI", "Rendering Hardware Interface, Unreal's graphics API abstraction."],
  ["D3D12", "DirectX 12, the graphics API Unreal uses; required by this plugin."],
  ["D3D11-on-D3D12", "An interop layer exposing D3D12 resources to D3D11 code (here, Spout)."],
  ["Shared texture", "A GPU texture exposed via a shared handle so another process can read it."]
];
