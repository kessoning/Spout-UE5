# Async Receive: example asset and validation tools

Validation and authoring tools for the per-receiver Async Receive setting. The folder and some names still say
"Beta" from when the setting was an opt-in beta. None of this is part of the plugin or of a release archive.

## `SpoutAsyncBetaAssetTool` (editor-only module)

The shipped receiver example `Content/Content/BP_SpoutReceiverExample.uasset` is a binary Unreal package.
It is changed only through Unreal's own Blueprint editing API, by this module, and saved by Unreal's
package saver. No asset byte is written by hand. The module goes into a throwaway host project as an
editor module, next to a plugin export of the working tree, and is never added to the plugin.

Commandlet `-run=SpoutAsyncBetaAsset`:

| Mode | What it does |
|---|---|
| `Inspect` | Reports the example's variables, the generated property's editor flags and metadata (DisplayName, Category, ToolTip, instance-editable), its class default, how the receive node's `bAsyncReceive` input is connected, the compile result, and which engine saved the package. Reconstructs the receive nodes against the compiled plugin first, so an unmigrated asset shows what an old graph becomes when opened (the new pin at the plugin's default). With `-Map=`, also the saved values of the example actors in that level; with `-Asset=`, another Blueprint than the example (each receive node's input value, creation default and links are reported). |
| `Migrate` | Adds the instance-editable variable `AsyncReceiveBeta` (display name "Async Receive (Beta)", default false, category `Spout\|Experimental`, the setting's tooltip plus the apply rule) and connects it to the `Async Receive (Beta)` input of every receive node of the example. Compiles, and saves with the running engine. Refuses an already migrated asset. |
| `MakeLevel` | Saves a validation level with one example actor (the example always receives the name `Spout`): with `-AsyncOn` the placed instance has the setting set to true, with `-AsyncOff` set to false, and with neither it keeps the class default. A value equal to the class default is not saved with the level. |
| `Promote` | Makes Async Receive the example's default: the example's variable keeps its name (`AsyncReceiveBeta`) and its connections; its display name becomes "Async Receive", its category Spout, its tooltip the one without "Beta", and its class default true. Compiles, checks that the class default is true, and saves with the running engine. Refuses an example without the variable or one already promoted. |
| `MakeFixtures` | Run with an earlier plugin: saves `/Game/Migration/BP_MigrationReceive`, an actor Blueprint with receive nodes in each state of the input, named by their comment (`unchecked`, `checked`, `connected` to a bool variable, `material-unchecked`). `Inspect -Asset=/Game/Migration/BP_MigrationReceive` under a later plugin then reports each node's input after Unreal's reconstruction (value, creation default, links). |

The editor console command `SpoutAsyncBeta.DetailsShot <map> <actor label> <out.png> [quit]`, run in the
full editor, renders the editor's own Details panel of that actor's Spout categories to a PNG. The
Experimental sub-group starts collapsed, so the command first opens the path to the setting the way the
editor does when it points at a property, and the row appears highlighted.

**Engine.** The example is saved by the oldest supported engine (UE 5.3), so that every engine from 5.3 to
5.8 can load it. A package saved by a newer engine cannot be opened by an older one.

## Packaged check

`Tests/SpoutHostTest` has the console command `SpoutHostTest.AsyncBetaCheck <report.json> <seconds> <spout name>`. Run it
in a packaged host whose level holds example actors with a saved Async Receive (Beta) value. It publishes a
pattern under the given name (the example receives `Spout`) with the plugin's own sender, and refuses a name
that another sender already holds. It then reports each example's saved
value, the requested and effective receive mode from `GetSpoutStreamStatus`, and whether the example's
received texture holds exactly a published pattern. The process exits with 0 only when every example's
effective mode equals its saved request and its image arrived.
