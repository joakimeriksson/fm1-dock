# Refreshing B3 ORGAN from upstream

B3 ORGAN is R-Jeb's firmware ([setBfree-fm1](https://github.com/R-Jeb/setBfree-fm1)) taken over almost unchanged: its
organ (`organ_*.h`), its screen code (`gfx.c`) and its app (`dinghy.c`, split into `app_init` / `app_loop`). What is ours is
small: `b3_platform.inc` (what B3's `platform.c` gave its app, on the dock's host) and `b3_module.c` (the module).

    modules/b3/refresh.py --tag v0.6.1 --fwsc <package.fwsc> --shot

does the mechanical part and checks it. This file is the rest: what to read, what stops it, what to do by hand.

## The steps, and what each is for

1. **Read upstream's release notes** (`RELEASE_NOTES_*.md`, `git log --stat OLD..NEW -- firmware/src`). Look for: new
   platform calls in `firmware/src/platform.c` (`git diff OLD NEW -- firmware/src/platform.c`), a bigger settings
   record, new files in `firmware/src/` that `dinghy.c` includes, a new use of the second core, flash it now writes.
2. **`take.py`** copies the files and patches `dinghy.c`. Every patch is `sub(old, new)` with an assert that `old` occurs
   once: when upstream changes the text, the assert names it. Fix the patch to the new text, never loosen the assert. The
   patches turn `app_main` into `app_init` (everything before `for (;;)`) and `app_loop` (one pass, a `do { } while (0)`),
   and the function's local variables into file-level statics (the host's `draw()` calls `app_loop` on every frame).
   A new local in `app_main` is a new `static`; a `continue` in the loop still means "the next pass".
3. **The glue** (`b3_platform.inc` -> `b3/platform.c`). B3's app calls what Dinghy's platform gave it; each name must exist
   here or the build says `undeclared` / `implicit declaration`. Today: panel and time from `hb_fm1.h`, LEDs (sent only
   on change), the screen (`gfx.c` onto the host's `fill` / `blit`), `settings_load/save` (the host's 240-byte record),
   `data_load/save` (the module's bank, up to 1 KiB: the user programs), `ORGAN_C1_RUN/WAIT` and `fm1_c1_on` (the
   host's `c1_run` / `c1_wait`), `midi_in_overflow` (set in `b3_module.c`'s `midi()`), no SysEx (`sysex_get` says none:
   the dock gives a module no SysEx frames, so upstream's web editor and its programs 112 to 127 do not exist here).
4. **The module** (`b3_module.c`): `draw(full)` must reset every "what is drawn" variable of the app, so that the
   screen comes back after the host's module list (`drawn_screen`, `logo_drawn`, `hash[]`). When upstream adds one, add it.
5. **Build**: `tools/hbmod.py modules/b3` (JIELI_TOOLCHAIN, Docker). Module RAM is 272 KiB for everything (B3's `.bss` is
   130 KiB: the delay lines), the flash room is the page's meter: this is a **big** add-on (109 KiB in 0.6.1).
6. **Check in fm1-emu**: `tests/level_test.py --fwsc P build/modules/b3.dock` (chords must not clip) and the run
   `refresh.py --fwsc P` does: it must start ("b3: B3 ORGAN x.y.z"), play a 14-note chord with Leslie, overdrive and
   reverb, stay under 90 % of a block with 0 overruns, and show the second core working. `tests/emu_test.py` has B3 cases too.
7. **The words**: `docs/modules/modules.json` "description", "save", "alt" (what is new, what the dock cannot do);
   `docs/guide.html`'s B3 card (the controls changed in 0.3 and 0.6); README's table row (size, features). The card's
   picture: `refresh.py --shot` (a knob turn first: MIDI alone does not wake the screen from the logo, by upstream's design).
8. **On a real FM-1**, with the room for it: `tools/dock.py info` first, then `deploy build/modules/b3.dock`, play it,
   Leslie and reverb on, a big chord, HOME out and back in (the screen and LEDs must return). A power cycle keeps the
   registration (it is saved when nothing sounds, 2 s after a change).
9. Commit: `modules/b3/` (`b3/` is generated: it is committed so the build needs nothing of upstream), `docs/modules/`,
   `docs/media/screen_b3.png`, the guide and README.

## What stopped a refresh before (v0.2 -> v0.6.1)

- `take.py`'s asserts: `app_main` had grown (settings, the logo, user programs, SysEx); the patches were rewritten.
- `fm1_c1_on`, `midi_in_overflow`, `data_load`, `sysex_*`: new names in upstream's `platform.c`, added to the glue.
- A stale `drawn_db` / `status_key` in `draw()`: the app's variables had changed; `draw()` fails to build then.
- The page tile showed the logo, not the controls: the screenshot run needs a knob turn first.
- "No space" on the unit: the module grew from 77 to 109 KiB; a deploy needs one gap that big (`dock.py info` says the largest).
