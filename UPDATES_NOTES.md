# Update Notes

Hey! These are my casual, plain-English notes on what I've been changing in the Spout UE5 plugin. No jargon, no scary technical wall of text. Just me jotting down what got better, what got fixed, and what I've been tinkering with. If you want the nitty-gritty engineering details, those live in the `CHANGELOG.md`. This is the friendly version.

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
