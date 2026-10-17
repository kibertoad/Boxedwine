# Local server

`-localServer <port>` starts an HTTP and WebSocket server on 127.0.0.1. Tools and agents use it to
see and hear a running program and to send it keyboard and mouse input, without a host window,
focus, or a platform capture or input API.

For a headless run, pass both `-novideo` and `-nosound`. Without `-nosound`, the guest's audio
also plays on the host's speakers. With it, the host stays silent, and `/audio` and `/ws/audio`
still get every sample the guest writes, paced at the same rate as playback.

```
boxedwine -novideo -nosound -localServer 8086 -zip TinyCore15Wine11.0.zip /bin/wine notepad
```

Port 0 picks a free port. The log has a `Local server listening on http://127.0.0.1:<port>/` line
either way. The server only listens on the loopback address, and answers 403 to a request whose
`Host` or `Origin` header names another host, so a web page open in a browser can't read the
screen through it. `GET /` lists the endpoints.

Each request gets its own connection, and the server closes it after the response.

## Screen

The server keeps a copy of the guest's screen, composited the same way the Boxedwine window shows
it: the Wine desktop, every X window Wine draws (GDI, DirectDraw through GDI), and OpenGL output
(including Direct3D through wined3d) laid over the window it belongs to. The mouse cursor is not
part of it.

The server only keeps the copy up to date while a consumer asked for the screen in the last two
seconds, so a run with nobody watching pays nothing for it. Compositing the desktop costs the UI
thread a few hundred microseconds a present. OpenGL frames come from reading the back buffer before
each swap, at most about 60 times a second, which stalls the game's GL pipeline for about as long.
The first request after an idle spell asks the X server for a full redraw and waits up to 250 ms
for it, and a request while a GL window is presenting waits up to 250 ms for a frame read after
the request, so the answer is never older than the request.

### `GET /screenshot`

The current picture as a PNG (RGB, 8 bits a channel). `x`, `y`, `w` and `h` crop it; a crop
outside the screen is a 400. Before anything has been presented the answer is a 503.

`format=raw` skips the PNG encoding (about 5 ms for 800x600) and returns the pixels as rows of R,
G and B bytes, top-down and unpadded, with `X-Width`, `X-Height` and `X-Pixel-Format: RGB24`
headers.

The `X-Frame-Hash` and `X-Frame-Change-Seq` headers identify the picture, with the same values
`/screen` reports.

```
curl -o screen.png http://127.0.0.1:8086/screenshot
curl -o button.png "http://127.0.0.1:8086/screenshot?x=300&y=200&w=200&h=50"
```

### `GET /screen`

The picture's description without its pixels:

```json
{"width":800,"height":600,"hash":"8f0c2a6d1e4b7c39","change_seq":42,"changed_ms_ago":1250,"presents":3120,"gl_presenting":false}
```

- `hash` is a 64-bit FNV-1a hash of the RGB pixels and the size, in hex.
- `change_seq` counts changes to the picture. A present that leaves every pixel the same does not
  count.
- `changed_ms_ago` is the time since the last change.
- `presents` counts frames presented (desktop redraws plus GL swaps), changed or not.
- `gl_presenting` is true while an OpenGL window swapped in the last second.

`since=<hash>&wait=<ms>` turns it into a long poll: the answer comes when the picture's hash
differs from `since`, or after `wait` ms (at most 60000), with `"changed"` saying which. A script
waits for the screen to move on without transferring pictures:

```
curl "http://127.0.0.1:8086/screen?since=8f0c2a6d1e4b7c39&wait=10000"
```

### `GET /ws/screen` (WebSocket)

Binary messages, each one PNG. `fps` sets the most frames a second (default 5, 0.1 to 60).
`mode=change` (the default) sends a frame only when the picture changed since the previous one;
`mode=all` sends one every tick, for recordings that need a constant frame rate.

## Audio

Every `/dev/dsp` voice the guest writes (Wine's OSS driver opens one per stream) is converted to
44100 Hz stereo and mixed at wall-clock rate, as the host's mixer would play them. MIDI is not
included.

### `GET /audio`

```json
{"voices":1,"peak":12011,"frames_mixed":441000,"sound_frames":220500,"ms_since_sound":35,"sample_rate":44100,"channels":2,"format":"s16le"}
```

- `voices` is the number of open DSP voices.
- `peak` is the loudest sample (0 to 32767) since the previous `/audio` request. 0 means silence.
- `sound_frames` counts mixed frames with any non-zero sample, out of `frames_mixed`.
- `ms_since_sound` is the time since the last non-zero sample, or `null` if there was none.

To check that a game plays sound at all, request `/audio` once, let the game run, and request it
again: a non-zero `peak` means it played something in between.

### `GET /ws/audio` (WebSocket)

A text message with the format, `{"type":"audio_format","sample_rate":44100,"channels":2,"format":"s16le"}`,
then binary messages of interleaved s16le stereo PCM, about every 20 ms. A client that falls more
than a second behind loses the oldest chunks.

## Input

`POST /input/key`, `/input/text`, `/input/mouse`, `/input/wait` and `/input/batch` send keyboard
and mouse input to the guest, so a headless run can get past menus and dialogs. The body is JSON.
Each request answers once Boxedwine's X server has passed every event on to Wine, so a screenshot
taken after the answer can show the game's reaction (the game may still need a frame or two to
draw it; poll `/screen?since=<hash>&wait=<ms>` for that). `?wait=false` answers at once with
`202` and delivers in the background. Requests deliver one at a time, in the order they arrive.

```
curl -s -d '{"key":"enter"}' http://127.0.0.1:8086/input/key
curl -s -d '{"action":"click","x":75,"y":190}' http://127.0.0.1:8086/input/mouse
curl -s -d '{"text":"player one\n"}' http://127.0.0.1:8086/input/text
```

| Want | Body |
| --- | --- |
| Press a key | `/input/key` `{"key":"esc"}`. `hold_ms` sets how long it stays down (default 50). |
| Key combination | `/input/key` `{"key":"x","mods":["alt"]}`. Modifiers are `shift`, `ctrl`, `alt` and `win`. |
| Hold a key | `/input/key` `{"key":"right","action":"down"}`, later `{"key":"right","action":"up"}` |
| Type text | `/input/text` `{"text":"dir\n"}`, with `delay_ms` between characters (default 30) |
| Move the mouse | `/input/mouse` `{"x":320,"y":200}`, or `{"dx":10,"dy":0}` to move by |
| Click | `/input/mouse` `{"action":"click","x":320,"y":200}`, `"button":"right"` for another button |
| Press and release separately | `/input/mouse` `{"action":"down",...}` and `{"action":"up",...}` |
| Scroll | `/input/mouse` `{"action":"wheel","amount":-3}`; positive scrolls up |
| A sequence | `/input/batch` `[{"type":"key","key":"esc"},{"type":"wait","ms":500},{"type":"mouse","action":"click","x":10,"y":10}]` |

- Key names are `a` to `z`, `0` to `9`, `f1` to `f12`, `enter`, `esc`, `space`, `tab`, `backspace`,
  `up`, `down`, `left`, `right`, `home`, `end`, `pageup`, `pagedown`, `insert`, `delete`, the
  modifiers (`shift`, `lctrl`, `ralt`, ...), the keypad (`kp_0` to `kp_9`, `kp_enter`, ...) and
  punctuation by name (`minus`, `slash`, ...) or as the character itself (`";"`). An unknown name
  gets a 400 that lists them all. `{"scancode":N}` sends an SDL scancode.
- `/input/text` types on a US keyboard layout, pressing Shift where a character needs it. A
  character with no key there (`é`) is refused, and nothing is sent.
- Mouse positions are pixels of `/screenshot`. A position outside the screen is moved to its edge.
  `down`, `up`, `click` and `wheel` without `x` and `y` act where the pointer is.
- `dx` and `dy` reach a game that reads raw mouse motion (XInput2, with `-forceRelativeMouse`) as
  motion. Otherwise they move the pointer by that much.
- A field an event doesn't have, such as `modifiers` for `mods`, gets a 400 that names it, and the
  request sends nothing. A batch names the event with the problem (`events[2]: ...`).
- Before the guest has opened its display, input gets a 503 with `"delivered":0`.
- Input from the server moves a pointer of its own: the guest sees it instead of the host mouse
  until the host mouse moves over the Boxedwine window.

## Status

`GET /status` reports what is running:

```json
{"uptime_ms":4333,"display":true,"screen":{"width":800,"height":600},"pointer":{"x":75,"y":190},
 "processes":[{"pid":10,"name":"wine","command_line":"/bin/wine wow.exe","state":"running"}, ...]}
```

- `display` is true once the guest opened its X display, which is when input can be delivered.
- `screen` is the guest's screen size, the space mouse positions are in.
- `pointer` is where server input last put the pointer, or `null` before any.
- `processes` lists the Linux processes in the guest, Wine's own services among them. `state` is
  `running`, `stopped` or `exited` (a process that ended and that its parent hasn't waited for yet).
