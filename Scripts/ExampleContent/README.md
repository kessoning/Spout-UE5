# Example content tool

`SpoutExampleAssetTool` is an editor-only module that reads and changes the shipped example Blueprints. None of it is
part of the plugin or of a release archive.

The examples in `Content/Content` are binary Unreal packages. They are changed only through Unreal's own Blueprint
editing API, by this module, and saved by Unreal's package saver; no asset byte is written by hand. The module goes into
a throwaway host project as an editor module (listed in the project's `Modules` and its editor target's
`ExtraModuleNames`), next to a copy of the plugin, and is never added to the plugin. Saved packages are then copied back
to `Content/Content`.

**Engine.** Save with the oldest supported engine (UE 5.3), so that every later engine can load the package. A package
saved by a newer engine cannot be opened by an older one.

Commandlet `-run=SpoutExampleAsset -Mode=<mode> -Out=<report.json>`:

| Mode | What it does |
|---|---|
| `Inspect` | Reports both example Blueprints in full: variables and their class defaults, every graph node with its pins, pin values and links, the components and what they change from their class, and the compile result. Also the two shipped materials (domain, shading, parameters and their defaults) and the two example render targets (format, size, mips). Changes nothing. |
| `Update` | Changes the receiver example `BP_SpoutReceiverExample`: adds the instance-editable variables Spout Name (default `UESpout_Test_Example`, the sender example's name) and Output Render Target (default none) and connects them to the receive and Close Receiver nodes in place of the fixed `Spout` name and the fixed `RT_SpoutReceiverTest`; makes the display material `M_SpoutDisplayUnlit` (the material instance and the plane's material); sets the material's `HasSignal` to 1 once a receive is accepted; adds a comment. Compiles, requires no errors and no warnings and the two class defaults above, then saves with the running engine. Refuses an example that already has Spout Name. |

The automation test `SpoutPlugin.P2.ShippedAssetsLoadAndCompile` checks the result on every engine, and
`SpoutPlugin.P2.ExamplesConnectWithTheirDefaults` runs both examples against each other.
