# ps4torrent
Torrent client for PS4 under (jalibrake)

The main component is the torrent payload. It runs in the background and is managed via a web interface on port 8787.

Supports adding torrents, activation, and resuming downloads.
The payload is operational and can be set to launch automatically on startup.
All downloading and processing take place on removable media (USB drive or HDD).

/mnt/usbN/torrents/             Place .torrent files here (only this level is scanned)
/mnt/usbN/torrents/downloads/   Downloads go here
/mnt/usbN/torrents/complete/    .torrent files are moved here upon completion
/mnt/usbN/torrents/.state/      System data: download progress (to persist across restarts)

## Build

Needs [ps4-payload-dev/sdk](https://github.com/ps4-payload-dev/sdk) (with libc++ built via its `libcxx.sh`).

    export PS4_PAYLOAD_SDK=/path/to/sdk
    make
    # send to the console (GoldHEN payload loader, port 9090):
    $PS4_PAYLOAD_SDK/bin/orbis-deploy -h <PS4 IP> -p 9090 ps4torrentd.elf

A prebuilt payload is in `release/ps4torrentd.elf`. After it starts, open `http://<PS4 IP>:8787/`.

## Documentation

* `docs/README_ru.md` - full user guide and API (Russian)
* `docs/HANDOFF.md` - developer notes: architecture, decisions, how to continue (Russian)
* `tests/` - PC test suites (see `tests/README_TESTS.md`)

Created by SergioPoverony and Mr.Claude.
