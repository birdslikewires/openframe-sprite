# The Shop

A place on the OpenFrame for finding and installing apps and screensavers, and taking them
off again. Nothing's for sale: "Shop" because it's where you go to get things, and owners
know the word. A quiet line in it asks for a contribution towards OpenBeak's running costs,
for anyone who'd like to.

This is the plan, for Andy to pick through. Marked **Decide** where it needs his call, with
what I'd suggest.

## What it has to be

- **No accounts, no logins, no store server.** An apt repository is the catalogue, the
  download and the update path, as it already is for Sprite and the system.
- **Updates come with the system's.** An installed app is updated by `of-update` at the
  owner's quiet time, like everything else. The Shop itself never needs to be open.
- **An app can't hurt the system.** It installs files and nothing else (no maintainer
  scripts), and runs in the sandbox it does now.
- **Contributing is a pull request.** Someone who's written an app, or a screensaver, adds a
  recipe to one repository; once merged, CI builds and signs it and it's in the Shop.
- **It works on both OpenFrames,** on the OF1's CPU, with the screen as it is.

## The pieces

### 1. The repository of recipes: `openframe-apps` (new, public)

```
apps/
  hamclock/
    app.txt         the manifest Sprite reads (name, exec, icon, colour, allow, ...)
    shop.txt        what the Shop shows: summary, description, kind, author, licence, source
    build.sh        fetches and builds, installing into $DESTDIR; runs in i386 Trixie
    depends.txt     Debian packages it needs (optional)
  flip-photos/
    ...
```

CI (one workflow, as `openframe-linux`'s `package.yml`) builds each recipe that changed into
`ofapp-<id>_<version>_i386.deb` (or `_all` when it ships no binaries), checks it, signs
the repository and publishes it. The checks, so a recipe can't do more than an app should:

- no `preinst`, `postinst`, `prerm`, `postrm` or `triggers`: the package is files only
- files only under `/usr/share/sprite/apps/<id>.app`, `/usr/lib/ofapp-<id>/`,
  `/usr/share/ofapp-<id>/` and `/usr/share/doc/ofapp-<id>/`; nothing setuid, no
  devices, no symlinks out of those
- `Depends` only on Debian's own packages (resolved from Debian's mirrors, never ours) and
  `openframe-sprite (>= the version that understands its manifest)`
- the manifest parses, its `icon=` is one Sprite has, and `exec=` points into the package
  or at a Debian binary it depends on

A recipe that builds from someone else's source pins a release and its SHA-256, as
`openframe-hamclock` does now.

**Decide:** the name. `openframe-apps` fits the others; the packages `ofapp-*` as agreed.

### 2. The apt repository: `apps.openbeak.net`

As the others (a `repo` branch on GitHub Pages, keep-latest-only pool, `trixie` and
`trixie-testing` suites, promotion by "ok" on a testing issue), but apart from them, as
decided on 22 November:

- **Its own signing key,** in its own keyring, `/etc/apt/keyrings/openframe-apps.gpg`,
  named by `signed-by` in its source. The system's repos never trust it, and it never
  vouches for them.
- **Origin "OpenFrame Apps",** label "OpenFrame Apps".
- **Pinned to its own names** (openframe-scripts, `/etc/apt/preferences.d/openframe-apps`),
  so it can only ever supply `ofapp-*`, even if something else turned up in it:

  ```
  Package: *
  Pin: origin "apps.openbeak.net"
  Pin-Priority: -1

  Package: ofapp-*
  Pin: origin "apps.openbeak.net"
  Pin-Priority: 500
  ```

- **Updated nightly** by adding `"origin=OpenFrame Apps,codename=${distro_codename}"`
  (and `-testing`) to `52openframe-updates`.

`openframe-hamclock` moves there as `ofapp-hamclock` (Sprite's apps are new, so there's
nothing to migrate: it goes from linux.openbeak.net at the same time).

### 3. What the Shop knows about an app: the Packages index

No separate catalogue. Each package carries what the Shop shows in its control fields,
which `dpkg-scanpackages` copies into the repository's `Packages` index and apt keeps in
`/var/lib/apt/lists/`:

```
Package: ofapp-hamclock
Version: 4.33-1
Installed-Size: 2140
Description: Ham radio clock and propagation maps
 HamClock, by Elwood Downey (WB0OEW), with maps of the sun, the grey line, ...
X-Sprite-Name: HamClock
X-Sprite-Kind: app
X-Sprite-Icon: radio
X-Sprite-Color: b8860b
X-Sprite-Author: Elwood Downey, packaged by OpenBeak
X-Sprite-Licence: MIT
X-Sprite-Allow: network
```

Sprite reads that file directly (a few hundred lines; no apt library), so the Shop opens
instantly and works offline with what it last saw. `X-Sprite-Allow` lets the Shop say what
an app may reach ("Uses the network") before it's installed.

Screenshots, later: `X-Sprite-Shot: shots/hamclock-1.png`, served from the same Pages
site, fetched when the app's page is opened.

### 4. Installing: `sprite-helper`

Three new requests, the only way Sprite touches apt:

- `shop-refresh`: `apt-get update` for the apps source only
  (`-o Dir::Etc::sourcelist=sources.list.d/openframe-apps.list -o Dir::Etc::sourceparts=-`),
  so it's a second or two, not the whole system's lists.
- `shop-install <id>` and `shop-remove <id>`: `<id>` must match `^[a-z0-9][a-z0-9-]{0,40}$`,
  and the helper only ever names `ofapp-<id>`. It runs apt as a transient unit
  (`systemd-run --unit=sprite-shop`) so closing the Shop or a restart of Sprite doesn't
  cut it off, with `APT::Status-Fd` written to `/run/sprite-shop/status` for the progress
  bar, and `DPkg::Lock::Timeout=300` so it waits for a nightly update rather than failing.
  Removal is `apt-get purge` then `autoremove` of what only it needed; its
  `/var/lib/sprite-apps/<id>` goes too (asked first: "Remove HamClock and what it's kept?").

Sprite notices a new manifest by watching the apps directories (inotify), so a tile
appears the moment the install finishes.

### 5. The Shop in Sprite

A built-in like Settings (`builtin=shop`), with its own tile, laid out the same way: the
rail on the left, the list on the right.

- **Rail:** Apps, Screensavers, Installed, and at the foot "Keep OpenBeak going".
- **A list row:** the app's tile (its icon on its colour, as on the home screen), its name,
  its one-line summary, and one button: Get, Open, or Update (when a newer version's waiting
  and the owner would rather not wait for the night).
- **An app's page** (tap the row): the description, what it may reach, its size, author,
  licence, a link to its source as a QR code, and Get or Remove.
- **While installing:** the button becomes a progress bar (downloading, then installing),
  and the rest of the Shop stays usable; one install at a time.
- **No network, or the list never fetched:** a single message and a Try again button.

**Keep OpenBeak going:** a short paragraph ("OpenBeak is one person's spare time...") and
a QR code to a donations page, as nothing on the OpenFrame can take a payment and it
shouldn't try. LVGL has a QR code widget (`LV_USE_QRCODE`), a few KB.

**Decide:** where the QR code goes (Ko-fi, GitHub Sponsors, Buy Me a Coffee, or a page on
openbeak.net that lists them, which I'd suggest, so it can change without an update).

### 6. Screensavers as apps

The flip clock is built into Sprite and stays the default. A screensaver from the Shop is
an app whose manifest says so:

```
name=Photo Frame
exec=/usr/lib/ofapp-photo-frame/run
kind=screensaver
allow=
```

- It has **no tile** on the home screen; it's listed under Display in Settings instead,
  where "Screensaver" becomes a choice: Flip clock, each installed one, or None (with
  "Screensaver after" as now).
- When it's due, Sprite starts it full screen with the home pill hidden, and stops it
  (`app-stop`) at the first touch or key, which goes no further, exactly as the flip clock.
- If it fails to start or quits by itself, the flip clock shows instead.
- It gets the sandbox like any app; a screensaver showing photos would have
  `allow=` nothing and read them from a directory the helper binds in read-only (a later
  `allow=photos`).

This part needs nothing from the repository, so it can be built and tried first with a
manifest in `/usr/local/share/sprite/apps`.

## Order of work

1. Screensaver apps in Sprite (6): `kind=screensaver`, the chooser, start and stop. Tried with a
   hand-made one on .131.
2. `openframe-apps` with CI, the checks, and hamclock as its first recipe; apps.openbeak.net
   and its key (Andy makes the key and the secret, `APT_SIGNING_KEY_APPS`).
3. openframe-scripts: the source, keyring, pin, and the nightly update origin.
4. The helper's `shop-*` requests and the Shop's pages in Sprite.
5. The donations page and its QR code.

## Open questions

- **Decide:** whether owners can turn on the testing suite for apps separately from the
  system's (`test.txt`'s `testing=yes` does both, as it stands).
- **Decide:** whether third-party recipes need Andy's review before merging (I'd say yes,
  always: the checks stop a package harming the system, not an app being unpleasant).
- An app from outside the repository (someone's own `.deb`) still works by hand, as now,
  and shows in the Shop's Installed list as "Installed by hand", without Remove.
