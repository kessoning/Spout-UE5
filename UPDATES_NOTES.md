# Update Notes

Hey! These are my casual, plain-English notes on what I've been changing in the Spout UE5 plugin. No jargon, no scary technical wall of text. Just me jotting down what got better, what got fixed, and what I've been tinkering with. If you want the nitty-gritty engineering details, those live in the `CHANGELOG.md`. This is the friendly version.

---

## JULY 2026 UPDATES

A crash fix and a packaging fix month. Later in the month I also did a full audit of the plugin, hunting specifically for the things that made packaged games misbehave, and fixed what I found. Right at the end of the month I turned my attention to proving all of it, and to the documentation, which had quietly fallen a long way behind.

Late July, the "actually prove it works" pass:

- **I now package a real game and run it, on every Unreal version I support.** This is the big one. Up to now I could tell you the plugin compiled on 5.3 through 5.8, but compiling is not the same as working. So I built a tiny test project whose only job is to be packaged and launched with nobody watching. It starts up, opens a sender ten times, opens a receiver ten times, closes and instantly reopens the same stream, travels back and forth between two levels five times with a stream running the whole way, checks that nothing is left advertising itself after it closes, and then shuts down with a stream still going, which is the moment things used to fall over. It does all of that three times per Unreal version, in both the normal and the final shipping build. If any step does not happen, it fails and tells me which one.
- **I check that the right Spout file actually loaded, not just that it is sitting there.** These are different things and I had been treating them as the same. The test now asks Windows itself which Spout file is loaded into the running game and where it came from, and compares that against where I put it. If it ever picked up a different copy from somewhere else on the machine, that is now a failure instead of a mystery.
- **Speed measurements, finally.** There were no performance numbers anywhere, just opinions. Now there are nine measured scenarios: 1080p and 4K sending, 1080p and 4K receiving, several streams at once, the slower fallback mode, a receiver sitting idle while nothing new arrives, constant resizing and reconnecting, and streams starting and stopping over and over. Each one records not just the average but the worst frames, which is what you actually feel. If a scenario cannot genuinely run, it says so rather than quietly reporting zeros.
- **The documentation was wrong, and it is not wrong any more.** This one was overdue and slightly embarrassing. The manual, the PDF and the website all still said version 0.0.5 while the plugin was on 0.0.9, because the version was typed in by hand in three places instead of being read from the plugin itself. They also described a colour feature I deleted, listed a dependency I removed, told you a function protected something it does not protect, and completely left out one of the nodes you can actually use. All fixed, and the version is now read straight from the plugin so it cannot drift again.
- **A checker that stops the docs drifting again.** Rather than fix it and hope, there is now an automatic check that reads the plugin's own source and makes sure the documentation still agrees with it: the version, which Unreal versions are supported, which formats work, which nodes exist. If they ever disagree, a release simply stops.
- **The plugin now explains where its files go.** There are five genuinely different places the core Spout file ends up depending on how you are using the plugin, and confusing them is far and away the most common reason people write to me saying it will not load. All five are now written out plainly in the readme, in one table.
- **The readme ships inside the download.** Small thing, but if you unzip the plugin you now get the instructions with it instead of having to go find them.
- **Downloads cannot accidentally contain my internal stuff.** The release now refuses to build if the zip picked up test projects, build scripts, working notes or anything else that is mine rather than yours.
- **Older Unreal versions are properly supported again in writing.** The readme still said support started at 5.5 and that 5.3 and 5.4 could not be built, which stopped being true a while ago. It now says 5.3 through 5.8, which is what I actually test.

- **Packaged games actually work now.** This one was sneaky. When you package your project, Unreal puts the core Spout file in a different spot than it does in the editor, right next to your game's exe. The plugin was only looking in its usual editor spots, could not find the file, and quietly turned itself off. So your project would package fine, run fine, and then Spout would just do nothing. The plugin now checks all the right places, including next to your game's exe, so packaged builds pick it up like they should.
- **Packaging without Visual Studio is possible again for the download versions.** My release script was cleaning out a folder that looked like build junk, but it actually held the prebuilt pieces Unreal needs to package a project when you do not have a compiler installed. Those pieces now stay in every download. Quick note: for a no-compiler setup the plugin has to sit in the engine's own plugins folder rather than your project folder, that is just how Unreal works.
- **No more broken packages for other platforms.** The plugin now clearly tells Unreal it is Windows-only. Before, if you packaged your project for another platform, Unreal would still try to cook the plugin's example content there, trip over the missing Windows-only parts, and could fail your whole package. Now it just skips the plugin on those platforms.
- **Fixed a stale picture when you switch quality settings mid-stream.** If you changed the format of what you were sending, say from a regular color texture to an HDR one, without changing the resolution, the stream could silently freeze on the last old frame. The plugin now notices the change and rebuilds itself, same as the receiving side already did.
- **Sending your game view to several apps at once got a lot cheaper.** If you were sending the same game view out under two or three different names, each one used to do the full expensive round trip separately. Now they all share one, so sending to three places costs about the same as sending to one.
- **Smoother sending in general.** I found the plugin was redoing a costly setup step on almost every single frame when sending the game view, because of the way the graphics card cycles through a few different image buffers behind the scenes. It now remembers all of them, so that work happens once instead of constantly.
- **The plugin tells you when your render target is the wrong format.** If you pointed the receiver at a render target whose format did not match the incoming video, it just stayed black and you got no explanation. Now it writes a clear note in the log telling you both formats so you know exactly what to change. It leaves your asset alone rather than quietly rewriting it behind your back.
- **Senders stop when you press Stop.** Before, if you started a stream in the editor preview and then stopped it, other apps kept seeing the stream frozen on its last frame until you closed Unreal entirely. Now it shuts down properly when you stop.
- **Spring cleaning.** Removed a few hundred lines of an old unfinished color-adjustment feature that was never actually switched on. Less baggage, smaller plugin, one less thing to trip over.

Earlier in July:

- **You can build and package your own project with it now.** Someone grabbed a ready-made download and it ran fine in the editor, but the moment they tried to compile or package their own project it fell over with a confusing "could not find definition for module" error. The problem was that those free downloads were shipping without the source files, and Unreal actually needs those any time it compiles or cooks a build, even when it is not changing a thing. I put the source back into every download, so now you can drop the plugin in and compile or package your project like normal. The ready-to-run files are still bundled too, so if you only ever use it live in the editor, nothing changes for you.
- **Fixed the crash when you send with a downloaded build.** A few people hit an instant crash the moment they used a Spout Sender node, especially on Event Tick, when running one of the ready-made (non-source) downloads. Turned out two things were going wrong. First, the core Spout file that does all the actual work was not even getting bundled into the download, so it simply was not there. Second, even when it was there, the plugin waited until the very last second to go looking for it and then checked the wrong place. Both are fixed now: the file is packed into every release, and the plugin loads it up front from the right spot, so sending just works.
- **No more scary crash if that file is ever missing.** If the core Spout file somehow still is not there, the plugin now quietly switches Spout off and leaves you a clear note in the log, instead of crashing. You get a plugin that does nothing rather than an editor that falls over.
- **Safer releases from my end.** I added a step to my build process that packs that core file in and refuses to publish a release without it, so a broken download should not reach you in the first place.

---

## JUNE 2026 UPDATES

This month was all about cleaning up and tightening the loose ends.

- **New "Close Receiver" button.** You can now cleanly shut down a video feed you were pulling in. This is super handy if you're switching between lots of different sources during a long session. Before, things could quietly pile up in the background. Think of it as the partner to the "Close Sender" we already had.
- **Colors look right now.** The plugin got smarter about color. Your normal everyday feeds look exactly the same as before, but the fancier high-quality ones (10-bit and floating point) now get handled properly instead of looking a little off.
- **No more garbled picture when a source changes on the fly.** If whatever you were receiving suddenly switched its format mid-stream, you could occasionally get a garbled or dropped image. Fixed, it just quietly rebuilds itself now and keeps going.
- **Plugged a slow memory leak.** There was a spot where a little bit of memory got left behind every single time, slowly adding up. Sealed it.
- **Squashed some behind-the-scenes timing bugs.** A couple of rare hiccups that could cause glitches or a crash when things were starting up and shutting down at the same moment. Gone.
- **Made the sending side a touch smoother.** Moved a heavy operation out of a tight spot so you don't get a tiny stutter every frame.

---

## MAY 2026 UPDATES

The "make it actually work when you ship it" month.

- **The big one: packaged builds load properly now.** If you ever ran into that frustrating *"module SpoutPlugin could not be loaded"* error, this fixes it. Precompiled and packaged versions now find everything they need.
- **Friendlier when something's missing.** The plugin now loads its core file in a safer, more forgiving way, and if that file is ever missing you'll get a clear message telling you exactly what's wrong, instead of a cryptic failure that leaves you guessing.
- **Tidied up how files get bundled.** I replaced some old, fragile copy-the-file-around logic with Unreal's proper built-in way of doing it, so things land in the right place whether you're building normally, packaging, or shipping a release zip.
- **Better docs + Windows-only label.** Added troubleshooting steps to the README, and officially marked the plugin as Windows 64-bit only (which is all it's ever supported anyway).

---

## APRIL 2026 UPDATES

Performance month, all about making the receiving side faster.

- **Video takes the fast lane now.** On the receiving side, frames go straight onto the graphics card instead of taking a slow detour through the regular processor every single frame. Smoother, lighter, less overhead.
- **A safety switch.** Added a toggle (`r.Spout.GPUReceiver`) so you can flip between the new fast path and the old one, just in case you ever need to.
- **Names don't clash anymore.** Fixed a quirk where a sender and a receiver sharing the same name could trip over each other.
- **Lots of little color and stability fixes** tucked away under the hood.

---

## THE EARLY DAYS

Where it all started.

**v0.0.2**
- Some minor bug fixes.
- Added HDR capture support when using SceneCapture2D. For the best-looking results, set your Capture Source to *Final Color (HDR in Linear Working Color Space)* paired with an *RGB8 sRGB* Render Target.

**v0.0.1**
- The very first release.
