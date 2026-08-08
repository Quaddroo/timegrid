# Desktop todo widget — design notes for a future agent

Scope note: this file describes **one specific project** — an ambient todo
widget for dwm. It is not general guidance for this directory (everything else
here is unrelated job research).

Code exists now: `timegrid.c`, built by the `Makefile` into `timegrid.bin`.

## Read this first

The layout is no longer a blank. The user described it, and it is **a
horizontal time table** — see "The layout, as described" below. That section is
settled and verified on screen; treat it as given.

What is still undescribed is what goes *in* the rows. Do not invent that. Ask.

The open questions, as of this writing:

- What red and blue *mean*. They are available and they persist; nothing says
  what they signify. Do not invent a meaning — it may be per-row.
- Whether a coarse cell written over finer ones should replace them or
  coexist. It currently coexists.
- Whether it wants curves and translucency, or stays square and flat.
  User is undecided; we agreed to start square because it costs nothing to
  change later.
- How to keep it from going invisible through familiarity — visual decay, or
  hard scarcity (only N items render). Undecided.

Several earlier open questions are now closed. "Three things for today vs. a
rolling backlog" is moot — the answer was neither; it is a timeline, and the
zoom range decides how much you see. The layout is answered. And cells hold
free text plus an optional colour, at whatever resolution they were written.

## The problem, stated properly

The user runs dwm. There is no desktop. In X11 the background is the root
window, a single drawable that nothing owns; `xsetroot` and `feh` paint it and
exit. It cannot hold widgets and cannot meaningfully be clicked.

So "a clickable desktop background" means: a real window, at the bottom of the
stack, that dwm leaves alone.

The user's usage pattern is the key design input: **they reach the widget by
closing everything on a tag.** It is the residue, not an application. This
matters because it removes the failure mode they were worried about — an app you
must launch is an app you can decline to launch on a lazy day. Nothing about the
design should require them to *start* it.

## The layout, as described

A horizontal table. The first row is **Date** and holds sequential time
buckets. Below it sit freeform rows. It scrolls left/right forever and zooms
from five-minute increments out to years.

A fixed left gutter holds the row names and does not scroll. Everything right
of it does.

The context strip and the Date row hold no cells and cannot be clicked into, so
they take their own colour (`header`) across the full width, gutter included —
otherwise they read as rows you failed to click rather than as a header. The
current-bucket fill deliberately paints over that part of them, which is what
makes "which day is it" answerable at a glance.

Every piece of text in that band — the context label, the bucket labels and the
"Date" gutter label — is drawn in `dim`, not `fg`. Keep them together: they are
one header, and because bold is a property of the palette entry, drawing them
all through `dim` is also what makes a single bold toggle cover the lot.

### Zoom is one number, and this is the important decision

The first version had a discrete ladder — 5m, 15m, 1h, 6h, day, week, month,
year — and jumped between rungs. The user's verdict was immediate: *"I get
totally lost as it immediately snaps to a totally different thing."*

So the viewport is now a single continuous quantity, **pixels per second**
(`pps`), plus the instant at the left edge (`view_left`). Every position on
screen is `gutter + (t - view_left) * pps`. That is the whole model.

The ladder survives only as a choice of **label resolution**: pick the finest
level whose columns are still wide enough to read (`min_col_w`). Zooming
therefore scales the grid smoothly and swaps label tiers in as columns grow.
A full year→5m sweep is ~117 wheel clicks instead of 7 jumps.

Two things fall out of this for free, and both are worth keeping:

- Column widths come from *real* bucket durations, so February is visibly
  narrower than March and a 23-hour DST day is visibly short.
- Panning is exact. A one-pixel drag moves one pixel at every zoom.

**Do not reintroduce a discrete zoom index.** It was tried and rejected.

### Controls

- Wheel zooms about the cursor; the instant under the pointer stays put.
- Button-1 drags the grid, 1:1 with the pointer.
- A **zoom** slider in the top margin, log-scaled over the whole range,
  anchored on the view centre.
- A **scroll** shuttle below it. This is *not* a scrollbar — see below.
- A **size** slider below that. Linear, not log — the range is small enough
  that a log scale would only make a particular size harder to land on. It is
  last of the three deliberately, so it does not come between zoom and scroll,
  which read as one control surface.
- A **colours** control under the row list — see "Colours are live too".
- Keys: `+`/`-` zoom, `h`/`l` pan, `0` returns to now. Escape drops a selection
  and closes the hidden-rows list.
- Shift-drag selects a block of cells — see "Selection" below.
- Ctrl-drag on a row slides its cells along the timeline — see "Shifting a row
  in time" below.
- Button-1 on a **row name in the gutter** drags it up or down to reorder.
  The gutter used to pan the grid along with everything else below the margin;
  the name column is a better use of it.
- Button-3 on a row name **hides** the row — see "Hiding rows" below.

The size slider has one trap worth knowing: its track is measured from the
gutter, and the gutter width scales with the text, so it is the one slider that
moves its own scale as you drag it. Dragging right grows the font, which pushes
`x0` right, which lowers the fraction, which shrinks the font. `DRAG_SIZE`
therefore freezes the track bounds at the grab and drags against those.

### The scroll control is a shuttle, and why

An absolute scrollbar over an endless timeline has to choose a span, and every
choice is wrong: wide enough to reach anything means one pixel is days, and
fine enough to be useful means it cannot reach. The first attempt scaled the
span with zoom and still drew the complaint *"it scrolls too much."*

So displacement from centre is a scroll **rate**, and the knob springs back to
centre on release. Rate is quadratic in displacement — fine control in the
middle, travel at the ends — with a dead zone so it can rest. Full deflection
is 2 screen-widths per second, expressed in screens rather than seconds, so it
feels identical at every zoom.

A rate control has no span to choose. That is the entire point.

### The current period is filled, not marked

The whole current bucket is filled — day, week, month, whatever the resolution
is — because at a glance you want *which day*, not *which instant*. A one-pixel
bright line sits on top for the exact instant. A hairline alone was tried and
was not noticeable enough.

## Architecture

C, Xlib directly, Xft for text. No toolkit. Two files: `timegrid.c`, plus
`picker.c`/`picker.h` for the colour picker.

**On the second file.** `code_style.md` says to avoid splitting code across
files, and that rule earned the single-file design here. The picker is the
exception because it shares *no state at all* with the rest: it never sees
`struct grid`, the plans file, or the time model. It is handed a Display, a
rectangle and a colour, and reports a colour back. It would drop into another
program unchanged. If a future addition needs to reach into `struct grid` to do
its job, it belongs in `timegrid.c` — the split is earned by independence, not
by size or by topic.

One wart that comes with it: the test harness `#include`s `picker.c` rather
than linking it, so the colour-space helpers can be tested without widening
`picker.h` for the harness's benefit.

The original plan was to start from dwm's `drw.c` and `util.c`. **That did not
happen**: dwm is installed to `/usr/local/bin` on this machine but its source
tree is gone, so there was nothing to vendor. The drawing layer is written
directly instead — it is only an `XftFont`, an `XftDraw`, and a double-buffered
Pixmap blitted with `XCopyArea`, which is far less than `drw.c` offers but all
this needs, and it keeps the program to one file.

The cost is real and should be recorded: the widget picks its own font family
rather than inheriting the bar's. Size and palette are no longer part of that
cost — both are live and persisted, so matching dwm's bar by eye is now a
matter of dragging sliders rather than editing source. If the dwm source reappears, aligning those is worth doing — looking
native was the original reason for reusing `drw.c`.

If curves or gradients turn out to be required, swap `cairo-xlib` in for the
rect calls. That touches drawing only, not the architecture. Real translucency
is a separate matter: it needs a running compositor and a 32-bit ARGB visual.
Without one, "transparent" can only mean matching the root color set by
`xsetroot`.

### The event loop is the performance story

There are exactly two sources of change:

- the X connection fd (Expose, ButtonPress, KeyPress, EnterNotify,
  ScreenSaverNotify)
- an inotify fd watching the todo file

Put both in one `poll()` and block. **No timers. No polling.** Idle CPU is
genuinely zero — the process sleeps in a syscall until something happens.
Expect 1–2 MB RSS and millisecond startup, which matters because this gets
launched from `.xinitrc` and forgotten.

```
setup: XOpenDisplay -> create override-redirect window -> XLowerWindow
       select Expose|Button*|Button1Motion|KeyPress|EnterWindow|LeaveWindow
       XScreenSaverSelectInput
       inotify_add_watch on the containing directory
loop:  poll({xfd, inotifyfd}, shuttle_held ? 16ms : block forever)
         timed out       -> advance view by rate * measured elapsed, mark dirty
         xfd ready       -> drain XPending, dispatch by event type
         inotifyfd ready -> reparse file, mark dirty
       if dirty -> redraw, XCopyArea, XFlush
```

If a future change seems to need a periodic timer, that is a strong signal
something is being done wrong. There are exactly two legitimate exceptions,
and both share the same shape: the timer exists only while a specific mode is
active, and idle is still zero wakeups.

1. **Shuttle scrolling.** While the scroll knob is held off centre, `poll()`
   takes a 16ms timeout instead of blocking. Releasing the knob sets the
   displacement to zero and the timeout goes back to `-1`. A velocity control
   cannot be built any other way — the whole idea is that time passes while
   you hold still.
2. **Burn-in drift in saver mode** (below).

The shuttle scrolls by *measured elapsed wall clock*, not a fixed step per
tick, so the speed you asked for is the speed you get even when redraws are
late. It ignores gaps over 0.5s, so a suspend or a long deschedule does not
teleport the view across a decade on resume.

### Staying out of dwm's way

Use **override-redirect**. dwm then ignores the window entirely — no tiling, no
border, no management. Place it yourself and lower it.

`_NET_WM_WINDOW_TYPE_DESKTOP` is the EWMH-correct answer and is worthless here:
dwm ignores window types except `_DIALOG`.

Getting covered by tiled windows is *correct behavior*. It's a background.

### Keyboard focus — sounds broken, isn't

Override-redirect windows never receive focus from the WM. But dwm hands focus
back to the root window when a tag is empty, and an empty tag is precisely when
the user is looking at this thing. So there is no competitor for focus in the
case that matters.

Select `EnterNotify`/`LeaveNotify`, call `XSetInputFocus` on yourself when the
pointer arrives, hand it back on leave. Focus-follows-mouse semantics, no grabs,
no fighting the window manager.

For decoding: `XkbKeycodeToKeysym` for bindings, `Xutf8LookupString` if real
text entry is ever wanted — the latter handles compose and dead keys, which a
naive keysym-to-char mapping will not.

## The file is the source of truth

A plain text file. The widget **watches** it; it is not the primary way to write
it. Adding and editing tasks happens in `$EDITOR` on a dwm keybind — real
terminal, real keyboard, no focus weirdness. Clicks on the widget stay limited
to things that need no keyboard: toggle done, strike through, bump to top.

Consequences, all good: the list is greppable, diffable, syncable, and survives
the widget being a bad idea. Throw away the binary and the todos remain.

**Keep the format nearly trivial.** Every ounce of syntax is paid for twice,
once in the parser and once in the writer. This is the single biggest lever on
total complexity.

### Colours are live too

Every colour the widget draws is editable. The `colours` control below the
hidden-rows list opens a panel: a row of swatches, one per palette entry, with
the selected one ringed; the name and hex of that entry with its bold toggle;
an HSV picker — a saturation/value square you drag in, and a hue strip beside
it; and `save theme as...` with the list of saved themes in a column to the
right of the picker.

The palette is a single table at the top of `timegrid.c` giving each entry a
`key` (what the file calls it, and it must stay stable), a `label` (what the
panel calls it, free to change) and a compiled-in default. **`COL_WRITTEN`,
`COL_RED` and `COL_BLUE` must stay adjacent and in that order** — cell colours
are looked up as `COL_WRITTEN + CELL_PLAIN/RED/BLUE`.

#### The themes list is a column, not a tail

`save theme as...` and the saved names used to run down the full width *below*
the picker, which made the panel one row taller per theme — so the list was the
first thing to run off the bottom of the screen, while the entire area to the
right of the swatches sat empty. It is a column beside the picker now, wrapping
into a further column when it reaches the picker's depth. Two things follow:
`picker_panel_h` no longer mentions `theme_count` at all, so the panel's height
is fixed, and the horizontal space that was already paid for does the work.

`themes_item_rect` is the only place that layout is expressed. The hit test
(`themes_item_at`) **scans** those rects rather than inverting the arithmetic,
which is both fewer lines and impossible to get out of step with the draw pass —
and at a couple of dozen names a linear scan is free, the same reasoning as the
main hit-testing note below. It also means a click past the last theme lands
nowhere instead of on a phantom row, which the old `item - 1 < theme_count`
guard had to say separately.

The one bound worth knowing: `MAX_THEMES` names at the compiled-in column width
is seven columns, which needs a screen wider than about 1700px. There is a test
that a dozen themes fit 1280. If a narrower screen ever has to hold more, wrap on
the window width inside `themes_item_rect` and the hit test follows on its own.

#### Fitting the panel on the screen

Both controls below the grid grow downwards, and with enough rows above them
they grew off the bottom of the monitor — a control you cannot click is a
control you do not have. Two mechanisms, in this order, and both are worth
keeping because they cover different cases. They apply to the **hidden-rows
list** as well as to the colours panel, by the same code: `hidden_row_y` is the
same shape as `colors_row_y` and the two chain, the second measuring from the
first.

1. **The window slides up.** `window_fit` sets `win_top` from the fitted height:
   normally `win_y`, but `screen_h - h` when that would overshoot the bottom
   edge, and never less than zero. Position is a function of height alone, which
   is why the early return on an unchanged height covers both. This is what fixes
   the hidden-rows list too, for free, and it is why the window is created at
   `g.win_top` rather than at `win_y`.
2. **The panel floats.** Once the rows fill the screen there is no upward room
   left, so `colors_row_y` stops being pushed: it clamps to
   `screen_h - row_h - picker_panel_h`, and the panel is drawn over the bottom
   rows instead of below them. It therefore paints an opaque band and a top
   edge — a no-op in the usual case, where there is nothing under it anyway.
   The clamp floors at `margin_top` so the sliders can never be covered.

Because the draw pass, the hit test and the window height all derive from
`colors_row_y`, the clamp is written once and clicks follow the panel wherever
it lands. Rows underneath a floating panel are not clickable while it is open;
the colours branch of the hit test is checked before the grid, which is the
right way round for an overlay.

The hidden-rows list needed the second mechanism too and now has it, in
`hidden_row_y`. Mechanism 1 alone did not cover it: expanding the list with a
screenful of rows above it pushed the names off the bottom edge, and there was
no upward room left for `window_fit` to find. It clamps only while the list is
**open** — a closed header is one row and stays where the rows put it — and the
room it keeps below itself is `row_h * (2 + hidden_count)`, plus
`picker_panel_h` when the panel is open. That last part is what makes the two
floats agree: `colors_row_y` adds `row_h * (1 + hidden_count)` back on, so when
both float they land stacked against the bottom edge rather than on top of each
other. An open list paints its own band and top edge for the same reason the
panel does, and its hit-test branch moved ahead of the rows.

Two supporting rearrangements:

- The picker's **size** is set in `layout_apply` with the rest of the geometry;
  only its **y** is positional. Splitting them is what lets `picker_panel_h`
  report the panel's height before its position has been decided, which the
  clamp needs.
- `picker.y` is now set in the **draw pass**, from the same `y` the panel is
  drawn at, rather than in `window_fit`. It is the one piece of layout that is
  stored instead of recomputed, because `picker_press` reads it too, and setting
  it where the panel is drawn means a click always hits the panel the user is
  looking at.

`screen_h` is read once at setup into `struct grid` rather than called for at
each use, so all of this fitting maths is a function of state and the harness can
exercise it without opening a display.

Two things collapsed into one when this went in, and they are worth keeping
collapsed: an `XftColor` carries a `.pixel` alongside its render colour, so the
same array entry serves both the rectangle calls and the text calls. The old
split between `pixel_alloc` and `XftColorAllocName` is gone.

`theme_apply` frees before it reallocates. That is not tidiness — it is called
on every motion event while the gradient is being dragged, and without the free
it would leak an allocation per pixel of travel.

### Bold, and saved themes

Each palette entry also carries a **bold** flag, toggled from the box beside the
hex on the panel's name row. Bold is a property of the *colour*, not of the call
site: turning it on for "text dim" makes everything drawn in that colour bold.
`draw_text` therefore takes a palette index rather than an `XftColor *`, and
`font_for` picks the face. Only `fg` and `dim` currently reach any text, so the
other twelve toggles are inert — that is the price of doing it uniformly instead
of special-casing two entries, and it means anything that later draws text in a
new colour gets the toggle for free.

Two traps here, both handled:

- `row_h` follows the **taller** of the two faces, or switching a toggle on
  would clip the text it was meant to emphasise.
- The bold face is optional. If the family has none, `XftFontOpenName` fails,
  `font_for` falls back to the regular face, and the toggles quietly do
  nothing rather than the widget losing its text.

**Saved themes** are a named snapshot of the whole appearance — every colour and
every bold flag. `save theme as...` on the panel starts a text entry (`EDIT_THEME`,
the third user of the same input buffer); each name below it loads that theme.
Saving under an existing name overwrites rather than accumulating.

They live in `$HOME/.timegrid_themes`, *not* in the plans file. The plans path is
dated and overridable, so a theme kept beside it would vanish at the turn of the
month, and a theme is not a property of one month's plans. The file uses the same
shape as the plans file — unindented names, indented `color`/`bold` attributes,
a line that does not parse is skipped — because there was no reason to invent a
second grammar.

A stored theme writes **every** slot explicitly, including ones that happened to
match the default when it was saved. Storing only the differences would mean
loading a theme left whatever the current palette had in the untouched slots,
and a theme that is only a partial overwrite is not a theme. There is a test for
this. Note it is read once at startup and has no inotify watch, unlike the plans
file — hand-edits need a restart.

### Where the settings live

Colours are written to the plans file header as `# color <key> rrggbb`, and
**only when they differ from the compiled-in default**, so a file belonging to
someone who never opened the panel carries no colour lines at all. Bold flags go
the same way, as `# bold <key> 1`, written only when on. A malformed value leaves
the default rather than turning some element black.

Inside the picker, HSV is the authoritative representation, kept alongside the
RGB rather than derived from it on demand. A grey has no meaningful hue, so
deriving would make the hue strip snap to red every time value or saturation
touched zero — drag a colour down to black and back up and it would come back
a different colour. There is a test for this.

The gradient lives in a server-side `Pixmap`, rebuilt only when the hue or the
size changes. Pushing a full `XImage` per redraw would put the whole gradient
on the wire for every mouse move; the hue strip, which never changes, is just a
few hundred one-pixel rectangles and needs no cache at all.

### Text size is live, and the file remembers it

Everything geometric — row height, gutter width, padding, knob radius, the
slider rows, the top margin, and the `min_col_w` thresholds that pick the label
tier — is **derived** from one number, `font_size`, by `layout_apply`. Nothing
else is ever assigned; set `font_size`, mark the grid dirty, and the main loop
does the rest in exactly one place. There is no scale factor sprinkled through
the draw code, and no cached geometry that can go stale.

`min_col_w` scaling with the font is not incidental: bigger text needs wider
columns before a label tier becomes legible, so the zoom ladder's thresholds
have to move with the size or labels start colliding when you scale up.

The chosen size is written back into the plans file header as `# font_size N`.
Editing the number by hand works and resizes the widget on save.

Saving happens on **button release**, not on motion — saving per motion event
would rewrite the file once per pixel of drag.

The echo trap does not bite here for the usual reason: we write the value we
already hold, so our own write trips our own watch, we reload, and the reload is
a no-op. An out-of-range number in the file is clamped by `layout_apply` rather
than obeyed, and the clamp is idempotent, so it settles instead of oscillating.

### Where it lives

`$HOME/assets/notebooks/notemaster/202607/plans.q`, set by `plans_path` at the
top of `timegrid.c` and overridable with `argv[1]`. The directory is created
at startup if missing — it has to exist before `inotify_add_watch`.

**The `202607` is hardcoded, not derived from the clock.** That is a decision
waiting to be confirmed rather than a considered one: the user gave that exact
path, and the month in it is the month it was given. Deriving it with
`strftime` is a one-line change, but it would mean every row silently
disappearing at midnight on the 1st, which is a worse default than a stale
path. Ask before changing it.

### The format as it stands

Unindented line: a row name. Indented line: a cell belonging to the row above
it, as

```
  <YYYY-MM-DD> <HH:MM>  <level>  <colour>  <text>
```

where `<level>` is one of `5m 15m 1h 6h day week month year` and `<colour>` is
`-`, `red` or `blue`. Lines starting with `#` are comments, and a row name
prefixed with `~` is hidden. Row order is line order. The file is created
with a header explaining itself to anyone who opens it in an editor:

```
run
  2026-07-22 00:00  day    blue  1.1
  2026-07-23 00:00  day    -     2.4

read
  2026-07-01 00:00  month  red   chapter 3
```

The timestamp is written in full at every level even though the trailing
fields are redundant for a month or a year. Uniform columns keep the parser to
one `sscanf` and the file scannable by eye; that was judged the better trade
than a terser, level-dependent shape.

Indentation carrying meaning is the one sharp edge — it has to be read before
whitespace is trimmed. A cell line that fails to parse is ignored rather than
guessed at.

A row name therefore cannot begin with `#` — it would read back as a comment
and the row would vanish — nor with `~`, which would read back as a hidden row.
`plans_add_row` strips both leading markers rather than losing the row, and
refuses a name that is nothing but markers and spaces. The `#` case was a real
bug found by testing, not a hypothetical, and `~` is the same bug waiting to
happen; both are covered.

This is deliberately the least format that produces rows at all, chosen so that
"rows are freeform, I add them" could be satisfied without inventing todo
semantics that had not been described yet. It is a placeholder holding a place,
not a design. Whatever answers the open questions above — spans vs. points,
what the colored boxes mean — will need syntax here. Add it once, and
grudgingly.

### Adding a row

A "+" row sits below the grid, outside it, with no columns running through it
so it reads as a control rather than as a row with no data. Clicking it starts
text entry in place; Return commits, Escape cancels.

Typing goes through `Xutf8LookupString` with a real `XIC`, so compose and dead
keys work; it falls back to `XLookupString` if `XOpenIM` fails. Backspace steps
over UTF-8 continuation bytes so one press deletes one character rather than
one byte.

While editing, `LeaveNotify` does **not** hand focus back to the root window —
otherwise moving the pointer away mid-word would send the rest of the name to
whatever is underneath. No grab is taken; the focus-follows-mouse handoff is
enough here.

## Cells

Click a cell to type into it; Return commits, Escape cancels. The box is filled
in `edit`, its own palette entry — it used to borrow `bg`, which made a cell
being typed into look like an empty one and gave the user no way to change it.
The edit box is
the wider of the cell and the text in it, outlined so its extent is visible
where it covers the cells to its right, and it slides left rather than running
off the right edge of the window. Display clipping is right for a cell you are
only reading — the words you are typing are the one case where the whole string
has to be legible whatever the zoom. Alt-click cycles
its colour, plain → red → blue → plain, keeping whatever text is there. A cell
with neither text nor colour is not an entry at all, so clearing both deletes
it rather than leaving a husk in the file.

### Selection

Shift-drag over the grid selects a rectangle: rows crossed with a span of time.
It is stored as two anchors in **real instants**, plus the level it was drawn
at — not as column indices — so it stays over the same cells when the view is
panned or zoomed underneath it, and it is drawn as one rectangle rather than
per-cell.

The row axis is held as **drawn positions, not row indices**. A block the user
dragged over is contiguous on screen but its row indices need not be — a hidden
row can sit between two of them — and treating it as an index range would
quietly sweep that hidden row into every bulk edit. There is a test for exactly
this. Any visibility change clears the selection, so the positions cannot go
stale underneath it.

The rule for what a selection action does is: **it is the single-cell action,
repeated over the block.** Nothing new is invented.

- **Delete/Backspace** clears the text of every entry whose `start` falls inside
  the block. Start, not overlap — text is drawn in the cell holding the entry's
  start, so this removes exactly the words you can see in the selected cells and
  can never reach an entry you did not select. A month-long entry beginning
  before the block keeps its text, which is drawn in a cell outside the block
  anyway. Clearing follows the single-cell rule: colour survives, and an entry
  left with neither text nor colour stops being an entry.
- **Alt-click inside the block** steps every cell in it one place around
  plain → red → blue, at the level the selection was drawn at. The whole block
  takes one step on from the *top-left* cell's colour, so a mixed block
  converges on the first click and cycles together from then on.
- Escape drops the selection; any plain or alt click outside it does too.

`selection_clear_text` compacts the entry array in place rather than calling
`entry_set` per cell — `entry_set` re-sorts, which would invalidate the index
being walked.

The overlap-vs-start distinction is the subtle part and it is deliberate. If
delete is ever changed to overlap, it will start silently destroying text that
lives outside the block the user drew.

### Shifting a row in time

Ctrl-drag anywhere in a row's cells picks up the cell under the pointer **and
everything after it** and slides the lot left or right. The gesture answers "all
of this happens later than I thought" without retyping a thing. Only that row
moves; the row name is filled and drawn bright while it happens, the same
affordance the reorder drag uses, because otherwise nothing says which row is
being pushed.

Four decisions worth keeping:

- **The step is the display level, not the entry's own.** Drag at day zoom and a
  5m entry moves by a day, keeping its 5m level. The alternative — each entry
  stepping by its own bucket — would tear a row apart, which is the opposite of
  what the gesture is for.
- **Counted in buckets, not pixels.** Columns are real durations, so February is
  narrower than March; a pixel delta would drift over a long drag. `shift_from`
  travels with the group, and each motion counts bucket steps from it to the
  bucket under the pointer. The loop is bounded by the window width because every
  column is at least `min_col_w` wide.
- **Stepped through `bucket_step`, one bucket at a time**, never by adding
  seconds. That is what makes a month push land on the 1st and a day push land at
  midnight across a DST change. There are tests for both.
- **Saved per whole-cell step, not on release.** A step is coarse, so this is not
  a write per pixel, and it means the widget never holds plans the file does
  not — which is exactly the invariant the inotify echo trap depends on. Deferring
  the save to release would give a mid-drag reload an unsaved edit to clobber.

Two cells can be pushed onto the same instant, when the group closes up on a cell
it did not reach past. They coexist rather than merging: the display already
aggregates several entries in one cell, and dragging back apart separates them
again. There is a test that neither is lost.

### Rows: order, and the visible/hidden split

`rows[]` is the file's order, and the `row` field on every entry is an index
into it. Two consequences that are easy to get wrong:

1. **Reordering must remap entry row indices in the same breath**, or every cell
   lands under the wrong name. `rows_move` slides the rows between source and
   destination by one and fixes the entries as it goes. The file's line order
   *is* the row order, so saving afterwards is the whole of the persistence —
   no new syntax was needed for this.
2. **Hiding does not renumber anything.** `visible[]` maps a screen position to
   a row index, and `visible_count` is what the grid, the window height and all
   hit-testing are laid out from. Anything that turns a y coordinate into a row
   goes through `visible[]`; anything that touches entries uses the row index.
   Keep that split — collapsing it is what makes hidden rows leak into
   operations they should not be part of.

`rows_index` rebuilds the mapping and must be called after anything that adds,
removes, reorders or hides a row. `plans_load` calls it, including on the
file-does-not-exist path.

The reorder drag reorders **live**, so the row moves under the pointer instead
of jumping on release, and the dragged name is filled and drawn bright so it
stays findable while the others slide around it.

### Hiding rows

A hidden row keeps its cells and its place in the file; it only drops out of
`visible[]` and gains a leading `~` on its line:

```
~read
  2026-07-01 00:00  month  red   chapter 3
```

This is the second reserved leading character, after `#`, and it brings the same
sharp edge: a row name cannot begin with one, so `plans_add_row` strips `~` as
well as `#` rather than silently hiding or losing the row. That was the whole
cost of the feature — one character, read in the same place the indent is read.

Hidden rows are reached through a **"hidden rows" control** below the "+" row,
which only takes up space when something is actually hidden. It uses the same
visual language as `+  add row` — `+` opens, `-` closes — and lists the names
indented beneath; clicking one puts that row back where it was. Unhiding the
last one closes the list on its own, because there is nothing left to show.

Right-click is the hide gesture because button 1 on the gutter is the reorder
drag and there is no menu bar to hang it off. It is not especially
discoverable; if that turns out to matter, the fix is a small affordance drawn
in the gutter, not a menu system.

### Keys while editing a cell

| key | does |
| --- | --- |
| Tab / Shift-Tab | commit, move one cell right / left |
| arrows | commit, move by one cell in that direction |
| Alt- or Ctrl-Backspace | delete the trailing word |
| Backspace | delete one character (UTF-8 aware) |
| Return / Escape | commit / cancel |

The arrows are free for navigation because the text entry has no caret to move
within — it only ever appends and backspaces. If a caret is ever added, the
arrows have to be renegotiated; Tab would survive.

Moving **commits first**, or what was just typed would be lost on the way out.
That applies to clicking away as well as to Tab and the arrows — clicking
another cell mid-word used to discard the word silently, which was a real bug,
not a hypothetical.
Row movement clamps at the ends rather than wrapping. If the destination has
scrolled off an edge, the view pans just enough to bring it back.

`cell_commit` returns without touching the disk when the text is unchanged.
That is not an optimisation to trim: without it, holding an arrow key to
navigate would rewrite the whole file on every keystroke.

### One rule, stated twice

The user described zoom-out aggregation and zoom-in expansion as separate
rules. They are the same rule, and implementing them as one is what makes them
round-trip:

> An entry occupies the span `[start, start + one bucket at its level)`.
> **Colour applies to every display cell that span overlaps. Text lands in the
> single display cell containing `start`.**

Zoom out and many entries share a cell, so their texts concatenate in time
order. Zoom in and one entry covers many cells, so the colour repeats and the
text stays in the first. Nothing special-cases direction.

This also handles the case that breaks containment-based schemes: **weeks do
not nest inside months.** A week straddling 31 March colours both March and
April, and the text appears in March because that is where the week starts.
Overlap is the rule, not containment. Do not "simplify" this to a parent/child
tree; it will be wrong at the week/month boundary.

Colour when several entries share a cell: a single chromatic colour wins, red
and blue together cancel to the plain "written" shade. **This reading was
inferred, not confirmed** — the instruction said the aggregate should take the
colour of the written cell "unless literally all the cells are red, blue, or
without writing, in which case it should be the default", which contradicts the
worked example given in the same message (all-blue must aggregate to blue). The
example was treated as authoritative. Worth confirming.

Text fitting has two halves, and the split matters:

- **The first piece always goes in, whole, however long it is.** `draw_text`
  clips it to the cell. Measuring it in `cell_text` and dropping it — which is
  what the code originally did — made a cell's text vanish outright the moment
  the column got narrower than the note, so zooming out lost the writing
  entirely rather than showing the start of it. Do not "fix" this back.
- **Later pieces are only added while the whole lot still fits**, so
  aggregating several entries into one cell never spills past it.

`draw_text` clips by dropping trailing *characters*, stepping over UTF-8
continuation bytes — truncating on a byte boundary would leave half a multi-byte
sequence and render a broken glyph. It costs one measurement per character
dropped; that is fine for labels and for the short notes cells usually hold, but
if a row of long notes ever makes redraws feel slow when zoomed out, that loop is
where the time goes and a proportional first guess would cut it to a couple of
passes.

`cell_gather` deliberately does no text measurement so the whole model is
testable without an X connection — `cell_text` does the fitting. Keep that
split. `cell_text`'s single-piece path does not measure either, so it is covered
by the harness; the multi-piece path needs a font and is not.

Writing into a coarse cell that already contains finer entries does not replace
them; both exist and the display aggregates. Consistent with the model, but it
means a day-level note and an hour-level note can both appear in the same day
cell. Not yet been lived with.

### Write-back, as actually implemented

`plans_save` writes the whole file from memory: leading comment block copied
across verbatim, then every row with its cells beneath it, into a temp file
that is `fsync`ed and renamed over the original. Never a truncate in place.

This replaced an append-only writer that preserved the file byte for byte.
Cells are edited in place, so appending no longer works. **The cost: comments
interleaved further down the file, between rows, are lost on the next write.**
The header survives; anything below it does not. If that turns out to matter,
the fix is to keep unparsed lines attached to the row above and re-emit them —
not to go back to appending.

Every commit does `plans_load` → apply the one change → `plans_save` →
`plans_load`. The leading reload is what keeps a concurrent hand-edit from
being clobbered by a stale in-memory snapshot; the window is now one keystroke
wide rather than the whole session.

The write-back echo trap below still does not bite, and it is worth
understanding why before adding anything that would reintroduce it: the program
holds no state the file does not, so our own write trips our own watch, we
reload, and the reload is a no-op. **The moment the widget gains unsaved
in-memory state, the echo becomes a real clobbering bug** and needs the
write-tracking the trap describes.

### Two traps in file handling

1. **inotify on the file inode will silently die.** Editors write a temp file
   and rename over it. Watch the *directory* for `IN_MOVED_TO` and
   `IN_CLOSE_WRITE`.

2. **Write-back echoes.** Toggling a checkbox rewrites the file (temp file plus
   rename — never truncate in place, or a crash costs the user their todos),
   which trips our own watch, which reloads the file we just wrote. Track our
   own writes and reconcile rather than blindly reloading, or this becomes a
   loop, or worse, clobbers an edit that landed in between.

## Screensaver mode

Same process, same draw code. `XScreenSaverSelectInput` delivers
`ScreenSaverNotify` on the X connection already being polled — no second daemon,
no timer, no `xautolock`.

Two modes rendering from the same list:

- **Ambient** — small, cornered, lowered, quiet.
- **Saver** — same window raised and resized fullscreen, big type, centered.

Idle fires, raise and resize. Any input, shrink and lower. A mode flag and a
geometry change. The payoff: what the user stares at while away from the
keyboard is their own todo list at 60pt, which nags better than a starfield.

Specifics:

- The timeout knob is `xset s <seconds>`, not a constant in the code — the
  extension fires at X's screensaver timeout.
- Set the DPMS timeout longer than the saver timeout (or `xset -dpms`), or the
  monitor powers off mid-display and the user never sees it.
- Grab pointer and keyboard on entry so the dismissing keystroke doesn't leak
  into whatever was focused underneath. Ungrab on exit. **Handle grab failure
  by declining to enter saver mode** rather than half-entering it.
- Burn-in: a static bright list fullscreen for hours is the exact pattern OLED
  panels dislike. Drift the whole composition a few pixels every minute — a
  `poll()` timeout that exists *only* in saver mode, so ambient stays at zero
  wakeups.
- X idle time doesn't know about video playback, so it will blank forty minutes
  into a film. Check for a fullscreen window, or honor player inhibition. Not a
  day-one concern.

### Do not write a lock screen

A screensaver that dismisses on input is low-stakes. A *locker* must survive
grab failures, VT switches, and crashing-into-an-unlocked-desktop; getting it
wrong means the machine is unlocked when the user believes otherwise. If locking
is wanted, chain to `slock` — audited, few hundred lines, same lineage as dwm.
The saver can exec it after a longer second timeout.

## Size expectations

These are rough priors about where effort goes, not targets to measure against.
Do not count lines or report line counts; keep the code short and move on.

- **~600 lines** gets the skeleton on screen and clickable: X setup, event loop,
  inotify, draw pass, hit-testing, mode switch.
- **~1200–1800** is the honest figure for a version still in use after six
  months.
- Past ~1500, suspect scope creep rather than inherent difficulty. dwm is a
  couple thousand lines and it's an entire window manager.

Where the bulk goes, in order:

1. **Layout.** The sleeper. "Text and boxes" is arithmetic until a long task
   needs to wrap instead of ellipsize — then it's measuring substrings with
   Xft, breaking on word boundaries, handling the word longer than the column,
   and reflowing everything below. Routinely 200+ lines and never feels done.
2. **File write-back**, per the traps above.
3. **Config.** Unbounded if allowed to be. Follow dwm: recompile to
   reconfigure. Saves hundreds of lines and a reload path.
4. **Multi-monitor**, if applicable — Xinerama and more geometry.

The parts that sound hard — event loop, focus, screensaver hookup — are
genuinely small and written once. The bulk is boring rather than difficult, and
stays boring only as long as the file format stays simple.

## Hit-testing

Fill a flat array of `{x, y, w, h, item_id}` during the draw pass, linear scan
on click. At the number of items a todo list should have, anything cleverer is
waste.

## Standing constraints

- No toolkit. No GTK, no Qt, no Python.
- No polling timers (two exceptions, both mode-scoped: shuttle scrolling and
  saver-mode drift — see the event loop section).
- **The plans file header carries UI state, deliberately.** This started as
  "no config file parser; recompile to configure", then took `# font_size` as a
  single exception, and then the colour picker made that a fiction. State the
  rule as it actually is now: appearance the user can change from the widget is
  persisted in the header as `# <key> <value>` lines, written only when they
  differ from the compiled-in default, and read by a `sscanf` per line in
  `plans_load`.

  What has *not* changed is the reason the old rule existed. Structure — what
  the rows mean, where the file lives, what the format is — is still compiled
  in, and adding a settings line for any of that is still wrong. The test is
  whether there is a control for it on screen. If the user cannot change it by
  clicking, it does not go in the file.

  Named themes are the exception to the exception: they live in
  `$HOME/.timegrid_themes` because they outlive any one notebook. Per-notebook
  appearance goes in the plans header; reusable appearance goes in the themes
  file.
- No locker.
- Never truncate the todo file in place.
- No discrete zoom index. Zoom is continuous `pps`; the ladder is labels only.
- Ask before designing what goes *in* the rows. The layout is settled; the
  content is not.

## Verifying changes to the time logic

The bucketing, zoom, pan and shuttle maths are covered by a test harness that
`#include`s `timegrid.c` directly (with `main` renamed) so it exercises the
real functions rather than a copy. It needs the X headers to compile but never
opens a display, so it runs anywhere. 51 checks: DST-length days, 28/29-day
Februaries, Monday week starts, month steps that do not drift off the 1st,
zoom anchoring, 1:1 panning, slider round-trips, shuttle rate and its
deschedule guard.

It also covers the plans file and the cell model end to end — rows and cells
round-tripping through save/load, colour-only cells surviving, clearing a cell
deleting it, the header written exactly once, aggregation order, colour voting,
expansion covering every sub-cell with text only in the first, the user's
stated all-blue-plus-"asdf" round-trip, neighbouring periods not bleeding, rows
staying independent, and the week straddling two months. The selection model is
covered too: bounds normalising on both axes and clamping when rows vanish
underneath, delete clearing text while keeping colour, delete removing a plain
cell but sparing a coarse entry that starts outside the block, block recolouring
converging and then cycling together, and the font size round-tripping through
the file exactly once. Rows too: reordering carrying cells with it and being a
no-op when out of range, order round-tripping through the file, hiding keeping
cells and surviving a reload, the `~` marker being stripped from a typed name,
and — the one that would be a silent data-loss bug — a hidden row sitting
between two selected rows staying out of a bulk delete. The theme too: an
untouched palette writing no colour lines, changed ones round-tripping, repeated
saves not accumulating duplicates, a colour set back to its default dropping its
line, malformed hex being rejected without disturbing the value, RGB→HSV→RGB
holding to within a rounding step, and a grey keeping the hue on the strip.
Bold and themes too: bold flags round-tripping and their lines appearing only
when set, `font_for` picking the right face and falling back when there is no
bold one, a theme restoring every slot rather than only the ones that differed,
saving over a name replacing instead of accumulating, and blank or
comment-marker theme names being refused. Cell-edit keys are
covered too: word deletion against a table of awkward inputs, tab/arrow
movement committing as it goes, clamping at the row ends, off-screen cells
scrolling back into view, and the no-change path leaving the file's mtime
alone. The row shift too: the cell left of the grab staying put while the
grabbed cell and its right-hand neighbour move, other rows staying out of it,
colour travelling with the cell, the shift persisting and being its own inverse,
a zero step not writing at all, a 5m entry pushed by a day keeping its level,
month pushes landing on the 1st and day pushes surviving a DST change, and a
collision keeping both entries. And that `edit` round-trips through the file
header like any other colour. Panel fitting too: a short widget staying at
`win_y` with the panel opening in place, a tall one sliding up until its last row
sits on the screen edge, a closed panel never being clamped, an open one floating
over the rows with its themes list still on screen and the sliders still clear,
and the window fitting the screen even with `MAX_ROWS` rows. The hidden-rows
list fits the same way: a short widget opening it in place, a tall one floating
it with every name on screen and the colours control still reachable below,
both floating together stacking rather than overlapping, a closed list never
being clamped, and the sliders staying clear at `MAX_ROWS`. The themes column
too: the panel's height not moving with the theme count, the column clearing the
picker and starting level with its top, items running down it and wrapping into
the next one, every item landing inside the window, the hit test finding the item
that was drawn, and a slot past the last theme not being clickable. Those run against temp paths and never touch the
real notebook.

It is not committed anywhere permanent — rebuild it when needed. Two traps
worth knowing if you write more of it, both of which produced false passes:

- Simulating elapsed time by winding the clock back a full second trips the
  0.5s deschedule guard, so the shuttle correctly does nothing and any
  assertion comparing two zero displacements passes trivially. Use ~100ms.
- `shuttle_advance` reads the clock itself, so simulated `dt` carries real
  microsecond jitter. Compare rates with a relative tolerance, not exactly.

What the harness cannot check is whether any of it *looks* right. Colour
balance, margins and knob sizes have only ever been verified by the user
looking at the screen.
