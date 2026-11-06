# Knock development notes

## Scope

Knock is a compact Windows tray app with a terminal-style interface. Native Win32 popup, no taskbar button or window controls; request inbox with Yes/No/Custom; background command completion and copyable continuation; personal conversation notes with tags and images; a task list and terminal CLI with explicit chat labels.

## Implementation

Version 0.8.0 is a portable Windows x64 application. Keep `KnockTray.exe`,
`knock.exe`, and the `assets` directory together. There is no web runtime,
server account, or network listener.

The [v0.6.0 release](https://github.com/qwoicpp/Knock/releases/tag/v0.6.0) is
published with `Knock-0.6.0-Windows-x64.zip`. The portable archive contains both
ready-built executables, the original image assets, documentation, and dependency
notices. Extract the complete archive and launch `KnockTray.exe`; no build step
is required. Saved data remains under `%LOCALAPPDATA%\Knock`.

- C++20, Win32/GDI/GDI+, per-account/session named pipe, durable JSON store.
- UI: a wide bar centred at the top of the work area, default 1720 x 380 logical
  pixels (Settings: 900-3840 x 300-800, clamped to the work area), sized from the
  scale of the monitor it opens on. A strip along the top holds the status, its
  message, the one line of feedback, the lease and quiet timer, and SETTINGS. Below
  it: the agents list (content window), then tasks and notes (drawn panels, click to
  open), then an arrow strip that collapses the notes and shrinks the bar;
  `window.notes_collapsed` persists it. Side columns narrow before the list does.
  Colours are flat near-black tokens at the top of `ui.cpp`; colour only means state.
  Headings and job names use the bitmap font in `pixel_font.hpp`. The bar uses
  tool-window flags and has no window controls. Opening from the tray hides on
  deactivation and opens on the tray's monitor; the shortcut keeps it topmost and
  open across focus changes and opens on the pointer's monitor. Escape backs out of
  the current view before hiding the bar. Notes, tasks, Settings, note pages and the
  tag picker open in the first column in place of the list.
- Edits sit inside drawn fields: call `place()` during layout, and `draw_fields` draws
  each box and shows which one has the keyboard. Placeholder text is drawn by Knock,
  because the system's is too dark on this background.
- Requests require a chat label. Yes, No, and custom replies are persisted.
  Only trusted UI calls may resolve requests. A custom reply never grants input.
- Custom replies have up to 12 saved presets, managed in Settings → More.
  Choosing a preset only fills the editor; SEND submits the reply.
- Manual presence is in the strip: At PC, Away, Busy, plus an optional
  500-byte UTF-8 message (edit 45). Clicking a state saves it with the message;
  Enter, leaving the box, hiding and a 1.5 s pause after typing save a changed
  message when a state is set. Incoming events never replace a message being
  typed. The initial state is unknown until set. Presence survives restart
  and changes only through trusted GUI calls. It is independent of input
  leases and notification preferences.
- `knock presence` reads availability. Status and request create/result/wait
  responses include it. Ordinary `knock request` passes stop_when_unavailable
  to the condition-variable wait: Away/Busy returns pending, exit 7, with
  wait_reason=presence and the same request ID. This also wakes an existing
  implicit wait. Explicit `result --wait` keeps its chosen waiting budget.
  Resolved decisions take precedence; presence never approves, denies, changes
  expiry, or resumes a chat. Unset/At PC retain the normal wait behavior.
- A persisted quiet deadline suppresses banners for 15/30/60 minutes without
  resolving requests or extending expiry. The app clears the deadline and sends
  one inbox summary when the timer ends. Right-click or the tray menu controls
  the timer; its status is shown in the strip and in the tray tooltip.
  The stored preference remains busy_until for compatibility; the interface
  calls it Quiet to distinguish it from manual Busy presence.
- Agent rows group check-ins, requests, jobs/notices and the finished inbox by chat
  label (`build_cards`). WAITING (pending requests, YES/CUSTOM/NO) sorts first, then
  WORKING (check-in title and status, or latest result or answer; COPY/VIEW LOG or
  RELEASE INPUT), newest start first so updates never reorder the list. A row is DONE
  while its done record is newer than its other activity; DONE rows live in the closed
  Done section (summary, COPY REPORT), newest first. Dismissing a row archives its
  finished items and check-in. The content window's text lists the rows in order, for
  screen readers and the UI tests. Each row's character comes from
  `make_character(chat)` in `character.cpp`.
- `knock start` and `knock update` keep one check-in per chat in the optional `agents`
  list. A state file without it opens unchanged and is not rewritten for it.
- `knock done` stores a plain-text summary (terminal codes stripped), an
  absolute report path, and marks matching tasks done; one record per chat.
  `knock inbox` pages the records. Existing commands' JSON is unchanged.
- Input approvals are exclusive, time-limited cooperative leases. Release,
  timeout, and restart revoke permission. Blocking CLI waits use a condition
  variable, so the model does not need to poll.
- Detached workers run explicit argument arrays, capture output, retain exit
  codes, and persist completion records before notifying the tray. They survive
  tray exit. Completion messages can be copied into the relevant AI chat.
- Notes are personal conversations: Enter sends composer text and pasted images;
  Shift+Enter inserts a newline. EDIT opens title/description metadata; TAGS
  opens the tag selector. Descriptions remain separate from message history.
  Notes retain tag/Untagged filters. Old note bodies migrate once to a first
  message without losing text.
- In the Notes list, `/find-note [query]` opens live, case-insensitive title
  search ranked by exact match, prefix, then substring. `/delete-note [query]`
  opens a dedicated click-to-delete page with Back. Slash-prefix suggestions
  and results support arrow keys and Enter; unknown commands never create a
  note. Commands are not interpreted inside note conversations.
- `/search-notes [query]` searches message text with Unicode case-insensitive
  comparison on Windows, bounded excerpts and result pages. Opening a result
  resolves its message ID again before scrolling and highlighting it. Ctrl+F
  searches the current conversation, including its older history.
- Entry editing uses a required expected revision, preserving message ID,
  creation time and position. Stale edits fail without overwriting current text.
  The original append payload remains private to storage so a retry of the
  original client_id stays idempotent after editing. A separate edit draft
  preserves the normal composer draft and attachments through hide/restart.
- Note deletion moves the complete record into Trash. Undo lasts 10 seconds;
  `/trash` restores records or offers an explicit permanent-delete action.
  Retention defaults to 30 days and is configurable from 1 to 365 in Settings →
  More. Expiry and preference changes are transactional; request approval rules
  are unaffected. Imported images are retained even after purging a note.
- Clearing a title leaves the editor blank through background saves and
  incoming updates. Explicit save/navigation requires a nonblank title and
  keeps focus on the draft when validation fails. Hiding preserves the current
  editor without restoring the previous title; unsaved metadata is not a
  restart-persistent draft.
- Composer drafts survive view changes, hiding, incoming updates, and restart.
  A persisted client_id makes sending and crash recovery idempotent. External
  deletion preserves unsaved work rather than silently dropping it.
- Clipboard images are normalized to PNG under `note-images`, with bounded
  input/decoded dimensions and thumbnails. Removing attachments or deleting
  messages/notes leaves the imported files on disk intentionally; there is no
  automatic attachment garbage collection.
- Ctrl+Alt+K toggles the popup by default. Right-click anywhere in the popup
  for Settings, where CHANGE opens shortcut capture with a disable option.
  The page also has window-size APPLY/RESET and Start with Windows. Shortcut
  and size changes save atomically to the shared settings file while preserving
  unrelated preferences; registration conflicts and failed writes preserve the
  previous working shortcut and size. Invalid settings report an error instead
  of silently replacing the file. The tray menu also opens Settings.
- Inbox/Trash preferences live with transactional state; hotkey/window settings
  remain in settings.json. Only trusted UI calls can change inbox preferences or
  permanently purge Trash. Settings → More contains the additional preferences.
- `knock info` and `knock get-info` print the agent guide without opening the
  tray or reading saved data. CMake embeds docs/AGENT_INSTRUCTIONS.md as the one
  source for the CLI text. `help` and `info` produce plain text; other commands
  keep their JSON contract. The guide explains single waits, detached jobs,
  retaining IDs and bounded output; it makes no automatic CLI-resume claim.
- The drawn task and note lists expose their row titles as window text
  (controls 61 and 62) for screen readers and the UI tests.
- The original GIFs are still bundled and the GIF decoder and its test remain,
  but the bar draws generated characters instead.
- Dark child-control initialization, background erasure, and batched layout
  painting address the white tab-switch flash. The bottom command-labelled
  buttons are removed; Enter creates a task or opens a new note conversation.

## Source map

| Area | Files |
| --- | --- |
| Persistent state and permission rules | `src/store.cpp`, `include/knock/store.hpp` |
| Windows encoding, paths, process arguments | `src/platform_win.cpp` |
| Framed local pipe and access control | `src/ipc_win.cpp` |
| CLI and detached command execution | `src/cli.cpp`, `src/runner_win.cpp` |
| Tray lifecycle and rendering | `src/tray_main.cpp`, `src/ui.cpp`, `include/knock/pixel_font.hpp` |
| Job characters | `src/character.cpp`, `include/knock/character.hpp` |
| COPY REPORT text | `include/knock/report.hpp` |
| GIF decoding, crop, and timing | `src/sprite_win.cpp`, `include/knock/sprite.hpp` |
| Clipboard images and thumbnail rendering | `src/note_images_win.cpp`, `include/knock/note_images.hpp` |
| Global shortcut and capture dialog | `src/hotkey_win.cpp`, `include/knock/hotkey.hpp` |
| Popup dimensions and shared settings persistence | `src/window_settings.cpp`, `include/knock/window_settings.hpp` |
| Correctness and integration checks | `tests/` |
| Running tests beside a live Knock | `tests/isolated_run.cpp`, `tests/test_isolation.hpp` |
| Screenshots of every screen | `tests/ui_gallery.cpp` |

See README.md for build commands, docs/CLI.md for command usage, and
docs/AGENT_INSTRUCTIONS.md for the text to give cooperating agents. In the
portable release, CLI.md and AGENT_INSTRUCTIONS.md are at the top level.

## Contracts

See docs/PROTOCOL.md. Only direct UI calls can resolve permission requests. No
generic AI auto-resume claim. Conversation text is personal content, never an
executable command or an AI request.

Note sends persist a 32-character lowercase-hex client_id with the draft before
appending. Identical replay returns the saved message; differing content with
the same token fails. `note.find_message` lets recovery recognize a committed
send. Keep that boundary intact when changing composer or draft handling.

## Verification and remaining desktop checks

Current verification results and the previous release's baseline are recorded
in docs/VALIDATION.md. Keep native Linux checks, Windows executables exercised
under Wine, and actual Windows desktop checks distinct. A successful Wine run
does not establish how the Windows shell will behave.

Real Windows 11 notification delivery/click behavior and Start-menu/focus behavior for
the pinned window remain desktop acceptance checks; VALIDATION.md says what the 0.8.0
desktop session covered.

Real-input tests go on the main monitor: the owner's agents run on the second one, and
one wrong Esc could stop them, so never send a real Esc key.

Never run a test copy without `KNOCK_DATA_DIR`, and run the GUI tests through
`isolated_run.exe`. A private window station has its own clipboard and no tray or
shell, and `IsWindowVisible` is always false there because its desktop window is never
shown; use `window_shown` instead. Native menus close at once off the real desktop, so
those copies use the `KNOCK_TEST_MENU` stand-in; `KNOCK_TEST_DPI` draws at a chosen
scale without changing the display. Those switches only work with `KNOCK_DATA_DIR` set.
`tests/cli_smoke.ps1` is available for native Windows verification; run it through
`isolated_run.exe`.

Future work should follow desktop feedback and stay within the existing local
tray-app scope. AI providers, automatic chat resumption, cloud services, and
interface expansion are outside that scope.
