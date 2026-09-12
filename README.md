# NX Uploader

A minimal HTTP file-upload server for the Nintendo Switch. Runs in homebrew
mode, serves a web page on your LAN, and lets any browser on the same
network drop files straight onto the SD card. No client software, no FTP,
no jank.

## Why

My Switch is docked almost all the time. I also emulate a lot on PC, so
there's a constant trickle of ROMs, saves, texture packs, mods, and
random `.nro`s that need to end up on the SD card. The usual options all
sucked in the same way:

- **Pull the SD card**, eject, find a reader, copy, put it back, wait
  for the console to remount everything. Ten minutes of friction for a
  five-second file transfer.
- **Plug in a USB cable**, only works over the right homebrew tools,
  and half the time the driver situation on my machine is a coin flip.
- **FTP**, the server on the Switch is slow, drops connections on
  large files, and wants a dedicated client open on the other end. I
  don't want a client, I want a browser tab.
- **Nintendo's official transfer**, LOL.

The Switch is already on my LAN. It's already running homebrew. Both of
those things are true any time I want to move a file onto it. So the
whole job should be "drag file into browser tab" and nothing else.

So I built that. NX Uploader is ~700 lines of C and ~400 of HTML/CSS/JS,
speaks HTTP/1.1, and streams uploads straight to disk instead of
buffering in RAM, so multi-gigabyte files work fine.

Also, honestly: why not.

## Features

- Drag-and-drop upload from any modern browser
- Streaming multipart parser, no whole-file buffering, works with files
  larger than the Switch's RAM
- Live progress bar in the browser, live bytes-received counter and
  transfer speed on the Switch screen
- Cancel button that actually stops the upload and deletes the partial file
- No dependencies beyond libnx
- Single `.nro`, ~300 KB
- Web UI is a single `web/index.html` embedded into the binary at build
  time — edit the HTML, run `make`, no C required

## Requirements

- A Nintendo Switch with homebrew access (Atmosphère, etc.)
- Wi-Fi or Ethernet connection on the same LAN as your browser machine

## Usage

1. Copy `NXUploader.nro` to `sdmc:/switch/` on your SD card.
2. Launch it from hbmenu.
3. The console shows a URL like `http://192.168.1.10:8080`. Open it in
   any browser on the same network.
4. Drag a file onto the page, click Upload.
5. Files land in `sdmc:/switch/uploads/`.

Press **+** on the Switch to stop the server and exit.

The upload limit is 32 GiB per file. That's arbitrary and can be raised
in `src/server.c` (`MAX_UPLOAD_BYTES`) if you have a reason to.

## Building

Requires devkitPro with the `switch-dev` group installed. If you just
want to run it, grab a release `.nro` and skip this section.

```sh
git clone https://github.com/samedog/NX-Uploader
cd NX-Uploader
make
```

## How it works

- `src/main.c` — applet loop, console rendering, entry point
- `src/server.c` — socket setup, HTTP parsing, `GET` and `POST`
- `src/multipart.c` — streaming multipart parser with a 256-byte lookback
  so boundaries straddling chunk edges aren't missed
- `src/net_info.c` — local IP detection
- `web/index.html` — the browser UI, embedded at build time
- `Makefile` — generates `src/html_data.c` from `web/index.html`

Uploads are written to disk as bytes arrive off the socket. If the
connection drops before the closing boundary, the partial file is
deleted. The whole upload is never held in RAM.

## License

MIT — see [LICENSE](LICENSE).