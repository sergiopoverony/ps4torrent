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
