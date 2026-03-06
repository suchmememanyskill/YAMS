# YAMS — Yet Another MJPEG Streamer

A minimal, single-file Linux utility that captures MJPEG frames from a V4L2
camera device and streams them over HTTP as a
[multipart/x-mixed-replace](https://www.iana.org/assignments/media-types/multipart/x-mixed-replace)
MJPEG stream.  Any browser or media player that understands MJPEG-over-HTTP
(e.g. VLC, mpv, most web browsers) can connect and display the live feed.

---

## Features

- Single C source file — easy to read, audit, and modify
- Uses V4L2 memory-mapped I/O for efficient, zero-copy capture
- Non-blocking I/O throughout (`select`-based event loop)
- Serves one client at a time; oldest buffered frames are dropped when the
  client falls behind so the stream stays live
- Graceful shutdown on `SIGINT` / `SIGTERM` with full resource cleanup
- Configurable resolution, frame-rate, port, and device via compile-time
  defines or command-line argument

---

## Requirements

| Dependency | Notes |
|---|---|
| Linux kernel ≥ 3.x | V4L2 subsystem required |
| GCC or Clang | C11-capable compiler |
| GNU Make | For the provided Makefile |
| V4L2-capable camera | Exposed as `/dev/videoN`, must support MJPEG output |

No third-party libraries are needed.

---

## Building

```bash
make
```

This produces the `yams` binary in the current directory.

### Compile-time configuration

The following `#define` constants at the top of `yams.c` can be overridden via
`CFLAGS`:

| Constant | Default | Description |
|---|---|---|
| `PORT`   | `8080`  | TCP port the HTTP server listens on |
| `WIDTH`  | `1280`  | Requested capture width in pixels |
| `HEIGHT` | `720`   | Requested capture height in pixels |
| `FPS`    | `30`    | Requested capture frame rate |

Example — build for 640×480 at 60 fps on port 9090:

```bash
make CFLAGS="-std=c11 -D_GNU_SOURCE -Wall -Wextra -O2 \
             -DPORT=9090 -DWIDTH=640 -DHEIGHT=480 -DFPS=60"
```

> **Note:** The camera driver may silently adjust width, height, and frame rate
> to the nearest supported value.  YAMS prints the actual negotiated resolution
> at startup.

---

## Installation

Install to `/usr/local/bin` (requires write permission or `sudo`):

```bash
sudo make install
```

Use a custom prefix:

```bash
make install PREFIX=/opt/yams
```

Uninstall:

```bash
sudo make uninstall
```

---

## Usage

```
yams [device]
```

| Argument | Default | Description |
|---|---|---|
| `device` | `/dev/video0` | V4L2 device node to capture from |

### Examples

Stream from the default webcam:

```bash
./yams
```

Stream from a second camera:

```bash
./yams /dev/video1
```

### Viewing the stream

Open a browser and navigate to:

```
http://<host-ip>:8080/
```

Or use VLC / mpv:

```bash
vlc http://localhost:8080/
mpv http://localhost:8080/
```

Or embed it in an HTML page:

```html
<img src="http://<host-ip>:8080/" />
```

---

## How it works

1. Opens the V4L2 device and negotiates MJPEG format at the configured
   resolution and frame rate.
2. Allocates four kernel-mapped capture buffers via `VIDIOC_REQBUFS` /
   `mmap`.
3. Starts a non-blocking TCP server that accepts a single client connection.
4. Runs a `select` loop that:
   - Dequeues completed frames from V4L2 as they arrive.
   - Sends the HTTP multipart header on new connections.
   - Streams each MJPEG frame wrapped in a multipart boundary.
   - Drops stale frames if the client cannot keep up.
   - Detects client disconnection and waits for the next connection.
5. On `SIGINT` / `SIGTERM`, stops streaming, unmaps buffers, and closes all
   file descriptors before exiting.

---

## Cleaning up

```bash
make clean
```

---

## License

MIT — see [LICENSE](LICENSE).
