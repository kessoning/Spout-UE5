# Update Notes

Hey! These are my casual, plain-English notes on what I've been changing in the Spout UE5 plugin. No jargon, no scary technical wall of text. Just me jotting down what got better, what got fixed, and what I've been tinkering with. If you want the nitty-gritty engineering details, those live in the `CHANGELOG.md`. This is the friendly version.

---

## OCTOBER 2026 UPDATES

This month is mostly groundwork. One change is on for everyone, the new way pictures travel between
Unreal and Spout; everything else stays switched off unless you turn it on.

I also did my release testing this month on my gaming laptop, an RTX 2060 machine, because it's
much closer to what a lot of you have. The good news: the standard tests, sending and receiving at
normal and 4K sizes, several streams at once, and starting and stopping, all hold a smooth 60
frames per second there, meet their delivery targets and never showed a damaged picture. I didn't
relax any of the pass marks to get that.

The honest news: in a heavier test scene that already keeps the laptop's graphics card busy, sending a
stream out of Unreal costs real frame rate. That scene ran at about 40 frames per second on its own,
and about 35 while sending. Receiving cost much less, about half a frame per second. If you build heavy scenes on
a laptop, measure with and without your streams. I also found that some sending apps hold on to the
shared picture until their own graphics work is done, and on a busy card that makes Unreal skip a few
frames (it shows the previous one, nothing breaks). Both are written up in the limitations document.
No code changed for any of this, just the documentation.

The big one first. Spout and Unreal speak slightly different graphics dialects, so the plugin has
always needed a go-between to hand pictures from one to the other. Until now that go-between worked
directly on Unreal's own pictures, which tied it closely to what Unreal was doing at the same moment.
Now the plugin keeps its own little set of holding pictures and hands images through those instead,
so it never reaches into Unreal's pictures directly. The pixels you get are the same, colours and
transparency included. Every wait along the way has a short time limit, so a stuck picture can't
freeze things for long: it just gets skipped and the next one goes through. The old go-between is
still there as a fallback setting if you ever need it, and the log tells you which one a session is
using.

While testing the release I also caught a small slowdown in how the plugin cleans up after a rare
hiccup. In those unusual moments it asks the graphics card whether some earlier work is done, and on
my NVIDIA laptop that question could sit unanswered for about three seconds. Now it asks in a way the
graphics card answers right away, about a hundredth of a second in my test. You would only ever
notice this if something had already gone wrong, but it's one less thing to wait on.

I've been chasing the last bit of stutter you can get when Unreal and the other app both run at the
same frame rate. Their timing drifts, and every now and then Unreal asks for the picture a hair
before the other app has finished the next one, so you see the same frame twice and then one gets
skipped. I added a few experimental switches that try to help with exactly that: Unreal can wait a
very short moment for a frame that is about to arrive, and it double-checks what the other app has
published at the moment it actually grabs the picture.

They are all switched off by default, and with them off none of the new waiting or double-checking
runs. I checked that by counting, in a test run, how often the new steps happened: zero. I haven't
done a side-by-side speed comparison with last month's version, so I won't promise it's identical,
only that the new parts stay out of the way. I'm still measuring whether they really help and what
they cost. If you like to experiment, the switches are listed in the limitations document, with
honest notes about what they can and can't do.
They don't make frame delivery perfect and they don't remove delay. Two apps on separate clocks
sharing one picture will always miss a frame now and then.

One safety rail went in with them: that short wait can never be longer than a thousandth of a second
per frame, whatever number you type into the setting. I still have to run this last change through
the full set of tests, so treat it as freshly added.

I also tidied up how the plugin decides that a frame has really arrived. A frame only counts once
the copy has actually finished, and if anything goes wrong on the way (the other app is busy, the
copy fails), the plugin simply tries again on the next frame instead of forgetting about it.

There's a new checkbox too, and it's off unless you tick it: Async Receive (Beta). You'll find it
on the example receiver actor, under Spout and then Experimental, and as an extra option on both
receive nodes. When it's on, the picture is fetched in the background instead of making Unreal's
drawing work wait for it. In my tests under heavy load that meant Unreal waited far less and no
frame from the other app went missing. But it isn't a free speed boost: in the same test the frame
times got a bit spikier and the picture arrived a little later. That's why it says Beta, why it
stays off for everyone who doesn't ask for it, and why I'd only turn it on after trying it in your
own project.

It works per stream name. Everything in your project that receives the same name shares one
connection, so they all have to agree: the first one to connect decides, and anything asking for
the other setting is politely turned away with a message in the log, without disturbing the ones
already running. Different names can use different settings side by side. To switch, close that
receiver and open it again (or just set it before you hit Play). If the background mode can't be
used for a stream, for example because the other app sends a picture format it doesn't support,
the receiver keeps working the normal way, and the log and the stream status tell you which mode is
really running and why.

---

## SEPTEMBER 2026 UPDATES

The biggest find from my final hands-on test: pictures sent from Unreal with the normal sRGB colour
setting didn't show up at all in the Spout demo receiver. It just said "No sender detected", even
for the example sender that comes with the plugin. It turns out the Spout demo apps, and other apps
built on the same part of Spout, only accept a fixed list of picture types, and the label I was
putting on Unreal's sRGB pictures wasn't on that list. The plugin now sends them with the plain
label instead, and the picture itself is exactly the same, pixel for pixel. My Unreal outputs now
show up in the Spout demo receiver, and other apps built the same way should accept them too (the
demo receiver is the one I've actually tested so far).

There's one trade-off worth knowing about. The stream used to carry a little tag saying "these
colours are sRGB", and now it doesn't, because that tag is exactly what those apps were refusing.
For most setups nothing changes, since apps already assume that's what they're getting. But if your
render target uses Force Linear Gamma (which is switched on by default in the Create Texture Render
Target 2D node), its colours are stored differently, and the receiving app can no longer tell. So
you have to tell it yourself: in Unreal, set the receiver's Color Space to Linear, or pick the
matching option in the other app. Or just switch Force Linear Gamma off on the sending side.
Unreal to Unreal looks exactly the same as before.

Then my long test run turned up something that looked like random stutter. When Unreal and the
other app both run at 60 frames a second, their timing slowly drifts, and every so often it lines
up so that Unreal keeps asking for the shared picture at the exact moment the other app is busy
writing it. The plugin used to give up instantly when that happened, so it could miss frame after
frame. In my tests the received picture froze for up to about half a second. Now the plugin waits a
tiny moment (2 milliseconds by default) for the other app to finish, and in the same test the worst
hiccup dropped to a couple of frames. If you ever want it to never wait, set r.Spout.AccessWaitMs
to 0. That only switches off the waiting, though: all the other fixes in this update stay on. The
wait can't go above 10 milliseconds, and it only happens when the other app is actually busy with
the picture. One thing to know: the 2 milliseconds is per stream, not per frame. If you have lots
of streams and another app keeps them all locked up, those little waits add up. In my test, an app
that held eight streams at once added about 13 milliseconds of waiting to every frame, which is a
big chunk of a 60 fps frame, and Unreal's frame times went up until the other app let go.

I also rewrote the warning messages that go with this. The log used to say it had "timed out
waiting", which made it sound like the plugin was stuck. The message now says plainly that the
other app still had the picture after the short wait, so the frame was skipped (or, when receiving,
the plugin tries again next time and grabs whatever picture is newest by then), and that the next
one goes through as soon as it's free.

There's one thing the wait can't fix, and I've written it up as a known limitation. The receiver
looks at the incoming stream once per Unreal frame and grabs whatever picture is newest. Spout
doesn't keep a queue, so if the other app swaps in two new pictures between two of Unreal's looks,
one of them is never seen. When both sides run at 60 frames a second, their timing drifts, and
every now and then it lines up almost exactly. During that stretch the odd frame gets skipped and
the one before it shows twice. Nothing freezes and nobody is fighting over the picture, so the wait
setting makes no difference. In my tests, in a 10 second stretch like that about 87 to 89 out of
every 100 frames made it through, while whole runs of a few minutes still got about 99. Running
Unreal faster than the other app makes it rarer, but I can't promise every single frame, because a
hiccup on either side can still drop one.

One more: some apps protect the shared picture with a different kind of lock than the Spout demo
apps use. If one of those holds on to the picture for a long time, Unreal can miss several frames
in a row, wait or no wait. In my test, an app that held the picture for 20 milliseconds on every
tenth frame got about 84 out of 100 frames across, with up to 14 missed in a row. If you use an app
like that, test it with your real setup.

While testing the fix I found one more small bug. If you made a render target with the plugin's
Create Texture Render Target 2D node and drew into it straight away, in that same frame, the
drawing could vanish, because the plugin finished setting the target up a moment later and wiped
it clean. That's fixed now: whatever you draw right after creating it stays there, just like with
Unreal's own render target node.

A correction from my final hands-on test: I used to say the downloads let you package a game
without Visual Studio. That was wrong, and I've fixed the docs. You can install the prebuilt plugin
and open your project in the editor without a compiler, but packaging a game needs Visual Studio
with the C++ tools, and that's the setup I support. One gotcha I hit: with the plugin in the
Plugins folder of a Blueprint-only project, my test game got packaged without the plugin and
complained at startup that it couldn't find it. Adding any C++ class to the project and packaging
again fixed it, so that's the workaround I recommend for now.

While running the final checks, one test launch out of roughly 180 reported broken pictures from the
example receiver. I dug into it, and it was the test being too strict, not the plugin sending bad
pictures. When the incoming picture changes size, the plugin resizes your output picture, and if the
other app happens to be busy at that exact moment, the output shows its plain background colour for
one update before the new picture lands. The test mistook that plain colour for damage. I taught it
to recognise that case, kept it strict about everything else, and added a line to the docs so nobody
is surprised by it.

I'm wrapping up the last checks before my own final hands-on test of the release.
The 1.0.0 version I prepared back in August never actually went out, so everything from this month
is going into 1.0.0 too. I also added a calmer restart test to the release checks: it closes and
reopens a stream every two seconds and has to keep the picture flowing the whole time without
running out of memory budget. The tougher test that restarts twice a second is still in there and
still runs out of budget, because a closed picture keeps its memory booked until Unreal's cleanup
comes around. I'm keeping that result visible rather than hiding it, and the docs now say plainly
that there isn't one safe restart speed that fits every project.

I finally tracked down the restart slowdown. Every time a stream was closed and reopened, the
plugin was setting up a small helper object from the Spout library twice over, and on my machine
each setup quietly took about fifteen milliseconds. That is most of a frame, and rapid restarts
paid it twice. The plugin now keeps those helpers around and reuses them, so a restart costs
about as much as a normal frame. I also left permanent timing markers in the send and receive
paths, so the next time something feels slow I can see exactly which step it is.

One thing this does not change: when you close a receiver, the picture it gave you stays valid
until Unreal cleans it up, and that cleanup runs about once a minute in a packaged game. If you
close and reopen receivers many times a second for a long time, the memory budget still fills up
until that cleanup happens. Keeping the receiver open while a sender restarts avoids it entirely.

I've cleaned up another part of the release checks. They now keep a clearer record of the exact
Unreal build used and double-check the performance measurements before accepting a pass. A short
test or a result with inconsistent numbers can no longer slip through just by saying it succeeded.
If an engine folder points to a different Unreal version than requested, the build now stops early
with a clear explanation instead of producing a package under the wrong version label.
This improves how I verify releases; the remaining restart slowdown still needs its own work.

I've been working through more of the fixes from the big review of the plugin. Streams now have
limits on queued work and retained pictures, and the new status nodes make skipped requests and
busy connections easier to see. Looking for a missing sender no longer leaves unused records behind.

I've also tightened the release checks: they install the actual download archive into a fresh
project, check its documentation links and exercise the included examples. Build tools now stop
cleanly when a process times out, even if it refuses to exit. These changes are still unreleased.

Those checks found two missing connections in the examples. I've connected the sender's picture
source and fixed how the receiver creates its material. Both examples now close their stream when
they leave play, and I've checked their pictures against a separate Spout process.

I've also kept working through the fixes from that review. Several screens can now receive the
same picture reliably, even when a sender is paused or a screen is recreated. Closing an old
receiver also leaves newer material changes alone.

I've added a receiver node that takes a material you create once and reuse. The release checks now
look for changing pictures during performance runs and keep each run's results separate, with a
record of exactly which build produced them. These changes are still unreleased.
I've also added a comparison run with Spout disabled. The new checks caught a slowdown when
streams are repeatedly closed and reopened, so I'm keeping that failed result visible for the
next performance pass instead of treating it as release-ready.
The failure checks also caught an older Unreal version returning a successful process exit after
a failed test. I've fixed that reporting path so build tools can see the failure too.

A fresh, careful review of the whole plugin, checking whether it's ready for real productions,
turned up a list of things to fix, and I've started working through them. These changes are still
unreleased.

The plugin now refuses to take over another sender's name, recovers more cleanly when another
program crashes while sharing a picture, and handles opening or resizing streams more carefully.
Closing a stream also has a stronger guarantee that queued setup cannot bring it back.

I found two more problems by checking real pictures with the graphics tools enabled. Those fixes
make Unreal wait until the picture is ready before sharing it. That extra waiting can affect frame
time, especially on Unreal 5.3, so performance still needs checking in the scene you plan to ship.
The graphics checks also caught a problem in how older Unreal versions prepare a picture for
sharing. I've added the missing step for Unreal 5.3 through 5.5.

There are now tests that compare every pixel against a known picture, including pictures sent by
a separate program. The rest of the review's list is still to do; this first batch does not mean
the whole release is ready.

## AUGUST 2026 UPDATES

**Version 1.0.0 is here.** After a long stretch of testing, fixing and proving things, the plugin is
out of beta.

I want to be straight about what that means. It does not mean the plugin suddenly does more than it
did last month. It means I can now show you why I trust it. Every supported version of Unreal gets a
real packaged game built and launched automatically, streams get opened and closed hundreds of
times, and an independent checker outside the plugin confirms the picture is genuinely being
published. If any of that stops being true, the release stops. That is the difference between beta
and 1.0.0 for me.

The big things this month:

- **Sending your game viewport is now properly tested, not just assumed to work.** Sending a render
  target has been well covered for a while, but sending the actual game window went untested, which
  is a bit embarrassing given it is one of the two things people use this for. There is now a test
  that opens a viewport sender in a real packaged game, waits for actual frames to be presented, and
  then checks from the outside that the stream is really there, really the right size, and really
  updating. Not "the function returned true", but "another program can see it".
- **Resizing your game window while sending the viewport no longer crashes it.** This one was a real
  bug, and the new test above is exactly what found it. If you changed resolution, resized the
  window, or went fullscreen while streaming your viewport, the game could die on the spot. It was
  the plugin holding on to the old window buffers for a fraction of a second too long. It lets go at
  the right moment now, and resizing mid-stream is safe.
- **A threading fix specific to Unreal 5.3.** On 5.3 only, the way the plugin hooked into Unreal's
  frame-presentation callback could in theory collide with the engine doing the same thing at the
  same moment. Rare, but real, and the sort of thing that produces a crash nobody can reproduce.
  Fixed for 5.3, and left alone on 5.4 and newer where the engine already handles it.
- **The documentation now matches what the plugin actually does.** A few pages had drifted and were
  describing behaviour that had changed. The colour space options, what happens when you hand the
  receiver a render target that does not match, what Set Max Senders gives you back, and where the
  Spout file ends up for each way of installing. All corrected, and there is now an automatic check
  that fails the release if the docs and the code ever disagree again.

Smaller things worth mentioning: publishing your viewport to several Spout names at once got a lot
cheaper, streams left running when you quit no longer risk taking the process down with them, and
every release archive is now opened back up and inspected before it goes anywhere near you.

Same scope as always: Windows, DirectX 12, Unreal 5.3 through 5.8.

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
