# Media browser — the real-server gate run (manual)

> **STATUS: PENDING.** Proposal 38 landed all six gates with this runbook
> UNRUN, and says so rather than letting a green suite imply coverage that
> does not exist. `docs/ASIO_WINDOWS_GATE.md` is the precedent: Phase 1
> merged with its gate PENDING and was closed later by a recorded run.
> **Record the result at the end of this file when you run it.**

Proposal 38 (PRs #70, #72, #73, #77, #78, #79, #80, #82) put the media browser
in the tree. Everything that could be gated by CI-less automation was — the
source ABI's async contract, supersession and cancel, the local walk, the
panel's browse and search modes, the drag payload through the REAL drop
handler, the cache's keying and cross-process safety, the WebDAV client's
parsing and error handling, the accounts page, and the secret store's round
trip and failure modes. **What none of it touches is the server.** The other
end of every wire in that suite is `main/testkit`'s `SWebDavStub`: our own
code, answering our own dialect, over plain HTTP, on loopback.

This runbook is the other half. Everything in it needs a real Nextcloud
instance, a real certificate, a real network, or real hardware, and none of it
can be reached from a `.qxa`.

## Before you start

### 1. Build from `main`

```bash
git checkout main && git pull
./build.sh
```

No knob, no SDK, no submodule to fetch: `Qt6::Network` and `Qt6::Concurrent`
ship in qtbase and are already in the root `find_package` lines.

**Run with NO `SMARAGD_*` media knob set.** All three exist to make a headless
run reproducible and every one of them would invalidate a step below:

| Knob | Must be | Why |
|---|---|---|
| `SMARAGD_SECRET_BACKEND` | **unset** | Otherwise you are testing `memory`, not the platform credential store — which is half of what this run is for |
| `SMARAGD_MEDIA_CACHE_DIR` | **unset** | Step 6 measures the real `<configDir>/mediacache` |
| `SMARAGD_MEDIA_TEST_SOURCE` | **unset** | It registers a fake "Delayed local (test)" source |

Check with `env | grep SMARAGD` (Git Bash) or `Get-ChildItem Env:SMARAGD*`.

### 2. A Nextcloud instance, and an APP PASSWORD

Any Nextcloud will do — hosted, self-hosted, or a container you start for the
afternoon. What matters is that it is reached over **HTTPS with a certificate
the OS trusts**; step 7 then deliberately breaks that.

Create the credential in Nextcloud, **not** your login password: *Settings →
Security → Devices & sessions → Create new app password*. Copy it once; it is
never shown again. It is revocable from that same page, which is the whole
reason design §B.7 names it as the credential to use.

The URL the browser wants is the DAV files root:

```
https://<host>/remote.php/dav/files/<username>/
```

### 3. Put material on the server

- at least **2000 entries in ONE directory** (step 5);
- at least one file **≥ 100 MB** (step 6) — any large WAV, or
  `dd if=/dev/urandom of=big.wav bs=1M count=120`; give it an audio suffix or
  the media-type filter will hide it;
- a few ordinary short WAVs, two or three levels deep;
- one file whose name is **legal on WebDAV and illegal on Windows** — say
  `kick:1?.wav`, or a name with a trailing dot — and **two files with the same
  name in two different directories** (step 8);
- one name with a **space and a `#`**, e.g. `Drum Kit/kick#1.wav`.

---

## The procedure

Each step names what to LOOK at and what counts as a FAILURE. Record every
step in the table at the end, pass or fail — a step that was skipped is not a
pass.

### Step 1 — the account round-trips, and the password is where it should be

1. Edit → Options → **Media**. Read the backend line at the top. On Windows it
   must say *"Passwords are stored with Windows DPAPI, protected by your
   login."* (macOS: the login keychain; Linux with libsecret: the Secret
   Service; without it: *"This system has no credential store — the password
   is kept for this session only"*, and the **Remember** checkbox must be
   DISABLED).
2. **Add** an account: an id of your choosing, the DAV URL from above, your
   username, the app password, **Remember this password** ticked. Press
   **Test connection**.
3. The result line must report success ("200 OK (N entries)"). A `401` here
   means the URL or the app password is wrong — fix it before going on;
   nothing below is meaningful against a server you cannot reach.
4. Re-select the account in the list. **The password field must be EMPTY** and
   the status must read `set`. A password that is re-displayed is a FAILURE
   (`main/shell/CONTRACT.md` inv. 39).
5. **Windows, and this is the one people forget:** open
   `%APPDATA%\Smaragd\smaragd.ini` in a text editor and search for your app
   password. It must not be there in any form. The only keys under
   `media/nextcloud/<id>/` may be `url`, `user`, `passwordScheme=dpapi` and a
   base64 `passwordEnc`. **The plaintext appearing anywhere in that file is a
   hard FAILURE.**

### Step 2 — the password survives a restart

Quit the app completely and start it again. Options → Media: the account is
listed and its status is `set`. Open the browser (View → **Media Browser**),
pick the account in the source combo, browse. **It must list without prompting
for anything.** That is the whole of "encrypted at rest, keyed off your login"
working.

### Step 3 — TLS against a real certificate

Browse the server over `https://`. It must simply work — no warning, no
prompt; there is no `ignoreSslErrors()` anywhere in this codebase and there
must never be one (§B.7). Confirm from the app's own log (View → Log) that no
TLS error line appears.

This step is unreachable by the suite in the strongest sense: the stub is
plain HTTP, so **no line of TLS handling has ever been executed by any gate.**

### Step 4 — a real server's PROPFIND dialect

Browse three or four levels down, then check every column against what
Nextcloud's own web UI shows for the same folder:

- **names**, including the one with a space and a `#`. A name that arrives
  percent-encoded in the tree (`Drum%20Kit`) is a FAILURE — gate 4 already
  caught `QUrl::setPath()` double-encoding an already-encoded path, and this
  is where a real server's spelling gets the last word;
- **sizes** in bytes, matching. A directory must show as unknown, not 0;
- **dates.** Nextcloud sends RFC 1123 with a literal `GMT`, and gate 4 caught
  `Qt::RFC2822Date` REJECTING exactly that. A blank or absurd Modified column
  is a FAILURE and is the single most likely thing to break here;
- **directories before files, each sorted by name.**

Then type in the search box: results must STREAM — first hits well before the
walk finishes — and unticking **recurse** must narrow them to the current
directory.

### Step 5 — a large directory

Browse into the ≥ 2000-entry directory.

- The UI must stay responsive throughout. The walk is off the GUI thread and
  so is the parse of the PROPFIND body (§B.7); a megabyte of XML on the main
  thread would show here as a freeze.
- If the listing is bounded, the FOOTER must say so: *"N items shown; at least
  M more were not (truncated)"*. **A silent truncation is a FAILURE** — a
  bound is announced (`main/media/CONTRACT.md` inv. 4).
- Note the wall-clock time to first row and to the footer.

### Step 6 — a large download: progress, memory, cancel, and where it lands

1. Save the project somewhere first (step 9 covers the unsaved case).
2. Drag the ≥ 100 MB file onto a track.
3. The status bar must show *"Fetching &lt;name&gt;…"* and the app must stay
   usable. Watch memory in Task Manager / Activity Monitor: the fetch streams
   into a `QSaveFile`, so RSS must **not** grow by the size of the file. A
   120 MB jump is a FAILURE.
4. When it lands, a clip appears. Check that `<projectdir>/media/<name>` now
   exists and that the clip references THAT, not the cache: save, then grep
   the `.qxp` for the name. The stored spelling must be **relative**. An
   absolute path into `mediacache` is trap T11's failure and means the project
   is not portable.
5. Drag the SAME file onto a second track. It must place **immediately** (a
   cache hit) and produce **no second copy** — no `name (2).wav` in
   `<projectdir>/media/`.
6. Start another large fetch and **cancel it**: close the project, or delete
   the target track while it is in flight. The status bar must say the drop
   was cancelled, no clip may appear, and no partial file may be published
   into the cache.
7. Drag it again. **It restarts from zero** — resumable downloads are not
   implemented (§F). Confirm that is what happens, rather than a stall.

### Step 7 — a self-signed certificate

Point an account at a server with a self-signed or otherwise untrusted
certificate (a local container is easiest).

**The failure must be VISIBLE and NAMED**: an inline banner in the panel
carrying Qt's own TLS error text, with a Retry button. Two things that must
NOT happen: the browse succeeding anyway (that would mean the errors are being
ignored somewhere), and an empty folder with no banner — an unreachable server
and an empty directory must never look the same
(`main/mediabrowser/CONTRACT.md` inv. 6).

### Step 8 — names, collisions, and Windows

- The file whose name is illegal on Windows must place. Its copy in
  `<projectdir>/media/` has the illegal characters replaced with `_`, and the
  clip plays. A silent failure to write, or a crash, is a FAILURE.
- Drag both same-named files, from their two different directories, onto the
  timeline. The second must become **`name (2).ext`**. **It must not
  overwrite the first** — that would be two clips over one file's audio, a
  data-loss bug with no symptom (trap T19).

### Step 9 — the unsaved-project warning

New, unsaved project. Drag a remote file onto a track.

It must place, and the status bar plus the log must WARN that the clip points
into the machine-local cache. Then save the project and confirm the honest
part: the reference is **not** relocated (§F — relocation at save time is not
in the MVP). Knowing that is exactly what the warning is for.

### Step 10 — the dock's geometry, across a restart

The one gate-2 acceptance criterion that was always going to be manual: the
dock rides entirely on its `objectName` through Qt's opaque `ui/windowState`
blob, and no verb can inspect it.

Float the Media Browser, move it, resize it, quit, restart: same place. Dock
it left, quit, restart: docked left. Close it, quit, restart: still closed,
and View → Media Browser brings it back. Tab it with another dock and restart.

### Step 11 — the credential store's failure mode, on real hardware

This is trap T14, and the reason the scheme tag exists at all.

1. Copy your `smaragd.ini` to a **second machine**, or to a second Windows
   user account on the same box, and start the app there.
2. Options → Media: the account must be listed with status **`undecryptable`**,
   and the page must ask you to re-enter the password.
3. Select it in the media browser: the banner must say the credential cannot
   be read. **NOTHING MAY BE SENT TO THE SERVER.** Check the server's own
   access log if you can reach it — a request arriving at all is a FAILURE,
   and a 401 in the app's log means a garbage credential went out.
4. Re-enter the app password: the account works again.

**macOS additionally:** lock the login keychain (Keychain Access → File → Lock
All Keychains) and start the app. It must degrade to session-only with a
message naming the reason, and it must **not hang** waiting on a prompt.

**Linux without libsecret:** the Remember checkbox is disabled, its tooltip
names the reason, the password works for the session, and it is gone after a
restart.

### Step 12 — a slow and a flaky link

Throttle the connection (a proxy, `tc`, Network Link Conditioner, or a phone
hotspot with poor signal) and repeat a browse, a search and a fetch.

Nothing here has a number to hit. What is being looked for is behaviour: no
frozen UI, no infinite spinner, every failure ending in a banner with Retry
rather than in silence, and **no retry storm** — a 401 is reported ONCE and
must not loop, because a retry loop against a rate-limited server is how an
account gets locked (§B.7). Watch the server's access log if you have it.

---

## What this run does NOT cover either

Named so no future PR body can imply otherwise:

- **Redirects and rate limiting.** Nextcloud can be configured to do both; no
  step above provokes either deliberately, and the client has no explicit
  handling for them.
- **`WWW-Authenticate`.** Nothing in this tree reads a challenge; the
  `Authorization` header is always sent up front.
- **Whether DPAPI, Keychain or libsecret actually resist an attacker.** Steps
  1, 2 and 11 gate the plumbing and the failure modes. The cryptography is
  theirs, and this proposal does not audit it.
- **macOS Keychain and Linux libsecret AS CODE.** Both backends are written
  and **have never been compiled** — the development box is Windows/MinGW.
  Step 11's macOS and Linux paragraphs are therefore a first COMPILE as much
  as a first run; expect to fix build errors before you can judge behaviour.
- **Pixels.** No `paintEvent` of the panel is gated anywhere, and this runbook
  does not turn eyeballs into a gate either: it asks whether a control is
  present and behaves, not whether it is pretty.
- **Drag ergonomics** — the ghost pixmap, hover feedback, the arranger's
  auto-scroll during a drag. Synthesised drops in the suite go straight to
  `dropEvent`; steps 6 and 8 here use a real mouse but judge the PLACEMENT,
  not the feel.
- **Resumable downloads.** Not implemented (§F). Step 6.7 confirms the
  behaviour rather than gating a feature.
- **OneDrive / Google Drive / S3, server-side WebDAV `SEARCH`, audition, and
  BPM/key analysis.** All non-goals (§F).

---

## Results

Fill this in when you run it. **Do not mark a step passed that you skipped.**

| Step | What it proves | Result | Notes |
|---|---|---|---|
| 1 | account round-trip; no plaintext in the INI | | |
| 2 | the password survives a restart | | |
| 3 | TLS against a trusted certificate | | |
| 4 | a real server's PROPFIND dialect (names, sizes, DATES) | | |
| 5 | a large directory; truncation announced | | |
| 6 | a large download: progress, memory, project copy, reuse, cancel | | |
| 7 | a self-signed certificate is a NAMED banner, never ignored | | |
| 8 | illegal names sanitised; a collision becomes `(2)`, never an overwrite | | |
| 9 | the unsaved-project warning | | |
| 10 | dock docked / floating / closed across a restart | | |
| 11 | `undecryptable` on a second machine; nothing sent | | |
| 12 | a slow and a flaky link; no retry storm | | |

**Environment** (fill in): OS and version, build commit, Nextcloud version,
whether the certificate was publicly trusted, and the shape of the link.

**Verdict:**

**Anything found that changes the design:** — this is the most valuable part
of the run. The ASIO gate's equivalent section changed three later phases.
