# ps4torrent
BitTorrent client for jailbroken PS4 (GoldHEN).

A background service (payload) downloads torrents to a USB drive or the internal HDD and is controlled from a web page
on port 8787 or from the on-screen PKG app. Downloads resume after a restart, the console does not sleep while downloading,
finished torrents can be seeded, and direct http:// links can be downloaded as well.

/mnt/usbN/torrents/             Place .torrent files here (only this level is scanned)
/mnt/usbN/torrents/downloads/   Downloads go here
/mnt/usbN/torrents/complete/    .torrent files are moved here upon completion
/mnt/usbN/torrents/.state/      System data: download progress (to persist across restarts)

## Build

Needs [ps4-payload-dev/sdk](https://github.com/ps4-payload-dev/sdk) (with libc++ built via its `libcxx.sh`).

    export PS4_PAYLOAD_SDK=/path/to/sdk
    make

A prebuilt payload is in `release/ps4torrentd.elf`. Ready-made PKG files are on the Releases page.

## PKG app

`pkg/` holds the on-screen PKG app (OpenOrbis, gamepad). It checks the service on launch and, if it is missing or a different version, installs it into GoldHEN autorun and sends it to BinLoader (port 9090). Build: `cd pkg; make full` (needs `OO_PS4_TOOLCHAIN` and `PS4_PAYLOAD_SDK`). The app and the service share one version (`version.h`) and one set of language files (`assets/lang/`, 20 languages: Latin, Cyrillic and Greek scripts). The console does not sleep while a download is running. Upload (seeding) with a speed limit of 24 KB/s ... 1 MB/s is on by default (web settings); in the PKG app Cross on a finished task turns its seeding off/on. The "+ link" button downloads a direct http:// link: a .torrent behind the link is added as a torrent, any other file is downloaded as is (with resume; https is not supported yet).

## Magnet requests

`GET /api/add_magnet?magnet=...` queues a torrent by magnet link: the service gets the metadata from peers found via the HTTP tracker (BEP 9, no DHT), saves a normal `.torrent` and starts it. While there is no network the request waits as `<hash>.magnet` in the torrents folder (`GET /api/magnets` lists waiting requests). See `docs/MAGNET.md`.

## Documentation

* `docs/README_ru.md` - full user guide and API (Russian)
* `docs/HANDOFF.md` - developer notes: architecture, decisions, how to continue (Russian)
* `tests/` - PC test suites (see `tests/README_TESTS.md`)

## License

Copyright (C) 2026 SergioPoverony and Mr.Claude.

Licensed under the GNU General Public License v3.0 (see `LICENSE`). Anyone may use, modify and redistribute it; the copyright notice must be kept, and modified versions must be released under the same license with source code.

Created by SergioPoverony and Mr.Claude.
