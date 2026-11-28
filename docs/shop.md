# The Shop

A place on the OpenFrame for finding and installing apps and screensavers, and taking them off again. Nothing's for sale: "Shop" because it's where you go to get things, and owners know the word. A quiet line in it asks for a contribution towards OpenBeak's running costs, for anyone who'd like to.

This started as the plan, and now says what's built too. Still marked **Decide** where it needs Andy's call.

**Decided 25 November 2026.** It's called the Shop (Library was considered, but on devices that means what you already have). Nothing in it costs anything, its buttons say Get and never Buy, and it says near the top that everything in it is free. We bring apps to the OpenFrame and don't support them ourselves, so each app's page shows where its own community answers questions, as a QR code, since the OpenFrame has no browser.

## What it has to be

- **No accounts, no logins, no store server.** An apt repository is the catalogue, the download and the update path, as it already is for Sprite and the system.
- **Updates come with the system's.** An installed app is updated by `of-update` at the owner's quiet time, like everything else. The Shop itself never needs to be open.
- **An app can't hurt the system.** It installs files and nothing else (no maintainer scripts), and runs in the sandbox it does now.
- **Contributing is a pull request.** Someone who's written an app, or a screensaver, adds a recipe to one repository; once merged, CI builds and signs it and it's in the Shop.
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

CI (one workflow, as `openframe-linux`'s `package.yml`) builds each recipe that changed into `ofapp-<id>_<version>_i386.deb` (or `_all` when it ships no binaries), checks it, signs the repository and publishes it. The checks, so a recipe can't do more than an app should:

- no `preinst`, `postinst`, `prerm`, `postrm` or `triggers`: the package is files only
- files only under `/usr/share/sprite/apps/<id>.app`, `/usr/lib/ofapp-<id>/`, `/usr/share/ofapp-<id>/` and `/usr/share/doc/ofapp-<id>/`; nothing setuid, no devices, no symlinks out of those
- `Depends` only on Debian's own packages (resolved from Debian's mirrors, never ours) and `openframe-sprite (>= the version that understands its manifest)`
- the manifest parses, its `icon=` is one Sprite has, and `exec=` is one command and its arguments, with nothing a shell would read as more (Sprite runs it with `exec`)

A recipe that builds from someone else's source pins a release and its SHA-256, as `openframe-hamclock` does now.

Built: [openframe-apps](https://github.com/birdslikewires/openframe-apps), whose README and `apps/README.md` describe the recipes and their checks. HamClock was the first app released, on 25 November 2026.

### 2. The apt repository: `apps.openbeak.net`

As the others (a `repo` branch on GitHub Pages, keep-latest-only pool, `trixie` and `trixie-testing` suites, promotion by "ok" on a testing issue), but apart from them, as decided on 22 November:

- **Its own signing key,** in its own keyring, `/etc/apt/keyrings/openframe-apps.gpg`, named by `signed-by` in its source. The system's repos never trust it, and it never vouches for them.
- **Origin "OpenFrame Apps",** label "OpenFrame Apps".
- **Pinned to its own names** (openframe-scripts, `/etc/apt/preferences.d/openframe-apps`), so it can only ever supply `ofapp-*`, even if something else turned up in it:

  ```
  Package: *
  Pin: origin "apps.openbeak.net"
  Pin-Priority: -1

  Package: ofapp-*
  Pin: origin "apps.openbeak.net"
  Pin-Priority: 500
  ```

- **Updated nightly** by adding `"origin=OpenFrame Apps,codename=${distro_codename}"` (and `-testing`) to `52openframe-updates`.

`openframe-hamclock` moves there as `ofapp-hamclock` (Sprite's apps are new, so there's nothing to migrate: it goes from linux.openbeak.net at the same time).

### 3. What the Shop knows about an app: the Packages index

No separate catalogue. Each package carries what the Shop shows in its control fields, which `dpkg-scanpackages` copies into the repository's `Packages` index and apt keeps in `/var/lib/apt/lists/`:

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

Sprite reads that file directly (a few hundred lines; no apt library), so the Shop opens instantly and works offline with what it last saw. `X-Sprite-Allow` lets the Shop say what an app may reach ("Uses the network") before it's installed.

On an OpenFrame apt keeps its lists compressed with lz4 (`/var/lib/apt/lists/apps.openbeak.net_dists_trixie_main_binary-i386_Packages.lz4`), so Sprite needs to read lz4 frames (liblz4, which apt already depends on), and should find the file by its suite rather than by that exact name.

Screenshots, later: `X-Sprite-Shot: shots/hamclock-1.png`, served from the same Pages site, fetched when the app's page is opened.

### 4. Installing: `sprite-helper` (built)

Three requests, the only way Sprite touches apt.

- `shop-refresh` fetches apps.openbeak.net's list alone (`-o Dir::Etc::sourcelist=sources.list.d/openframe-apps.list -o Dir::Etc::sourceparts=- -o APT::Get::List-Cleanup=0`, so the other lists are kept), which takes about a second.
- `shop-install <id>` and `shop-remove <id>` take an id the recipes allow (`^[a-z0-9][a-z0-9-]{0,39}$`), and the helper only ever names `ofapp-<id>`. Each starts a job of its own, the `sprite-shop` unit (`systemd-run --collect`), and answers at once. Closing the Shop or restarting Sprite doesn't cut it off, and a second request while it runs is answered "busy".
- The job reports in `/run/sprite-shop/status`, which Sprite reads, as `action`, `id`, `state` (running, done or failed), `percent` of the whole job, and `reason` when it failed, as one of the helper's `helper.shop_*` keys in `en_gb.txt`. The percent comes from apt's `APT::Status-Fd`, and moves through the slow preparation steps too. apt's own output is in `/run/sprite-shop/log`.
- An install refreshes the list first, then refuses anything that would remove an installed package (what an app depends on comes from Debian, and could conflict with the system). If a Debian dependency can't be fetched because the system's lists are behind, it updates them all and tries once more. apt waits up to 5 minutes for the dpkg lock, so it waits for a nightly update rather than failing.
- A removal first works out what would go, and refuses if anything but the app would, leaving it running. Otherwise it stops the app, removes its package (and its off switch, if it's a service), then the packages only it needed (what `autoremove` offers now and didn't before), and the folder it kept, `/var/lib/private/sprite-apps/<id>`.

On .131 an install of HamClock takes about 30 seconds and a removal about 45, most of it apt loading its lists on the Atom.

Sprite watches its apps folders (inotify), and reads the manifests again half a second after they stop changing, so a tile appears as soon as an install unpacks and goes when an app's removed. That covers the nightly updates and apps installed by hand too. Entries in Sprite's table of apps are never moved or freed, as running apps and timers point at them, so a removed app is marked gone and comes back to the same entry if it's installed again. `openframe-sprite` owns `/usr/share/sprite/apps`, so the folder stays when the last app goes. A folder that doesn't exist when Sprite starts is watched from the next rescan.

### 5. The Shop in Sprite (built)

A built-in (`apps/shop.app`, `builtin=shop`) with a tile of its own, which moves like any other, its glyph Lucide's gift. It's laid out as Settings is, a rail and a panel, with a page for each app. `src/shop.c` reads what's offered (apt's lists of apps.openbeak.net through `apt-helper cat-file`, which reads lz4, about 40 ms on the Atom, taking the newest version when a test unit has both suites), what's installed (dpkg's status, which also gives what a package installed by hand says about itself) and the job's status. Versions are compared as dpkg does.

- **Rail:** Apps and Installed. Services and screensavers are in Apps, each under a heading of their own, as "Screensavers" is too long for a tab at Settings' rail width, and making the Shop's rail wider would move its Home button from where Settings has it. Apps opens with "Everything here is free", and who makes the apps.
- **A list row:** the app's icon, as its tile will be, its name, its summary, and one button: Get, Open, Update (when the Shop offers a newer version than the one installed) or, for a screensaver, Choose, which opens Settings at Display.
- **An app's page** (tap the row): its icon, name, summary and who made it, its buttons (with Remove once it's installed, asked first), its description, what it can use, a QR code to its community (its source if it has none) with the address beside it and a line saying OpenBeak doesn't support the apps, then its version (and the one installed, if they differ), size, licence and source.
- **While a job runs:** its button becomes a progress bar, in its row and on its page, read from the status file every 0.4 s. Other Gets and Removes wait. When it's done a toast says so (the tile's on the home screen, or choose a screensaver in Settings), and when it fails a message gives the reason. A job begun before Sprite restarted is picked up when the Shop opens.
- **The list** is fetched again (`shop-refresh`) as the Shop opens if it's more than 30 minutes old. With none ever fetched, it says it's finding out what it has, then, failing that, that it couldn't be reached, with Try again.

Tried on .131: Get, Remove (asked first), progress and the toasts, the Installed tab, Update (with a fake newer version in a list for a moment), and the QR code, which zbar reads as the community's address.

**Keep OpenBeak going:** a short paragraph ("OpenBeak is one person's spare time...") and a QR code to a donations page, as nothing on the OpenFrame can take a payment and it shouldn't try. LVGL has a QR code widget (`LV_USE_QRCODE`), a few KB.

**Decide:** where the QR code goes (Ko-fi, GitHub Sponsors, Buy Me a Coffee, or a page on openbeak.net that lists them, which I'd suggest, so it can change without an update).

### 6. Screensavers as apps

The flip clock is built into Sprite and stays the default. A screensaver from the Shop is an app whose manifest says so:

```
name=Photo Frame
exec=/usr/lib/ofapp-photo-frame/run
kind=screensaver
allow=
```

- It has **no tile** on the home screen; it's listed under Display in Settings instead, where "Screensaver" becomes a choice: Flip clock, each installed one, or None (with "Screensaver after" as now).
- When it's due, Sprite starts it full screen with the home pill hidden, and stops it (`app-stop`) at the first touch or key, which goes no further, exactly as the flip clock.
- If it fails to start or quits by itself, the flip clock shows instead.
- It gets the sandbox like any app; a screensaver showing photos would have `allow=` nothing and read them from a directory the helper binds in read-only (a later `allow=photos`).

This part needs nothing from the repository, so it can be built and tried first with a manifest in `/usr/local/share/sprite/apps`.

## Order of work

1. Screensaver apps in Sprite (6): `kind=screensaver`, the chooser, start and stop. Tried with a hand-made one on .131.
2. `openframe-apps` with CI, the checks, and hamclock as its first recipe; apps.openbeak.net and its key (Andy makes the key and the secret, `APT_SIGNING_KEY_APPS`).
3. openframe-scripts: the source, keyring, pin, and the nightly update origin.
4. The helper's `shop-*` requests (built) and the Shop's pages in Sprite.
5. The donations page and its QR code.

## Open questions

- **Decide:** whether owners can turn on the testing suite for apps separately from the system's (`test.txt`'s `testing=yes` does both, as it stands).
- **Decide:** whether third-party recipes need Andy's review before merging (I'd say yes, always: the checks stop a package harming the system, not an app being unpleasant).
- An app from outside the repository (someone's own `.deb`) still works by hand, as now, and shows in the Shop's Installed list as "Installed by hand", without Remove.
