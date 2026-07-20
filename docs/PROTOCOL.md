# Spancam Direct Stream Protocol (SDSP) v1

The wire protocol between the **phone** (server) and the **OBS plugin** (client).

The phone hardware-encodes H.264/HEVC and streams it; the plugin decodes. Encoding
is dedicated silicon, so the phone stays cool — the expensive half (decode) happens
on the computer. Transport is plain TCP, and all multi-byte integers are
**big-endian**.

## Roles & connect

- **Server:** the phone, bound to `0.0.0.0:8892` — the wildcard bind is what makes
  one server answer on both Wi-Fi and USB.
- **Client:** the OBS plugin.
  - Wi-Fi: connect to `<phone-ip>:8892`.
  - USB: `adb forward tcp:8892 tcp:8892`, then connect to loopback.

The phone is the server rather than the client because it is the thing that moves
between networks: OBS sits still, the phone comes and goes.

## Discovery (UDP broadcast)

So that nobody has to read an IP address off a phone screen and type it into OBS:

- The phone runs a UDP responder on **8891**. On receiving the ASCII probe
  `SPANCAM-DISCOVER` it replies, to the sender, with one line:

  ```
  SPANCAM-OBS|<name>|<port>|<token>|<codec>|<width>|<height>
  ```

- The plugin broadcasts `SPANCAM-DISCOVER` to `255.255.255.255:8891`, takes the
  first reply, and uses the reply's **source address** as the host — plus the
  `port` and `token` from the payload.

The host deliberately comes from the packet's source address rather than anything
the phone says about itself. A phone with several interfaces up does not reliably
know which of its addresses the desktop can reach; the datagram that just arrived
is proof.

Broadcast does not cross subnets, and plenty of networks (guest Wi-Fi, most
enterprise APs, anything with client isolation on) drop it entirely.

### Bonjour (mDNS)

iOS can't answer a UDP broadcast without a special entitlement, so the iOS sender
advertises a Bonjour service `_spancam-sdsp._tcp` instead, with a TXT record
carrying `token` (the access key) and `port`. On macOS the plugin also browses for
this service and takes the host + SRV port + token from it — so an iPhone is found
the same "just works" way an Android phone is. The plugin reads the SRV port rather
than guessing it. (Android is found by the UDP broadcast above; the two discovery
methods run side by side.) Discovery is
therefore a convenience, never a requirement — typing the host in by hand has to
stay a first-class path.

## Handshake

On connect the client sends one ASCII line, terminated by `\n`:

```
SPANCAM/1 k=<token>
```

`<token>` is the per-session access key shown in the phone app. Token rules:

- **Wi-Fi (non-loopback):** the token must match. Anyone who can reach port 8892
  could otherwise watch your camera, and 8892 is open on every interface.
- **USB (loopback, via `adb forward`):** the connection arrives on the phone's own
  `127.0.0.1`, which nothing off-device can reach, so the token is skipped. USB is
  meant to be zero-config.

A bad token on Wi-Fi gets the socket closed with no reply.

If accepted, the server replies with a binary **StreamHeader**, then a continuous
sequence of **Packets** until one side hangs up.

### StreamHeader (24 bytes)

| off | size | field    | notes |
|-----|------|----------|-------|
| 0   | 4    | magic    | `0x53504331` = `"SPC1"` |
| 4   | 1    | codec    | `0` = H.264, `1` = HEVC |
| 5   | 1    | flags    | bit0 = mirror seed; other bits reserved, 0 |
| 6   | 2    | reserved | 0 |
| 8   | 4    | width    | pixels |
| 12  | 4    | height   | pixels |
| 16  | 4    | fps      | nominal frame rate |
| 20  | 4    | bitrate  | bits/sec the encoder was configured for |

The magic is checked before anything else — it is the cheapest way to notice you
have connected to something that is not a Spancam phone.

### Packet (16-byte header + payload)

| off | size | field    | notes |
|-----|------|----------|-------|
| 0   | 1    | type     | `1` = codecConfig, `2` = videoFrame, `3` = transform |
| 1   | 1    | keyframe | videoFrame only: `1` if IDR, else `0` |
| 2   | 2    | reserved | 0 |
| 4   | 8    | ptsUs    | presentation time in microseconds |
| 12  | 4    | size     | payload length in bytes |
| 16  | size | payload  | **Annex-B** bytes |

- **type 1 (codecConfig)** — the encoder's parameter sets in Annex-B: SPS+PPS for
  H.264, VPS+SPS+PPS for HEVC. Sent once at stream start and again on any encoder
  reconfigure. Fed to the decoder ahead of any frame.
- **type 2 (videoFrame)** — one Annex-B access unit.
- **type 3 (transform)** — 4-byte payload `[mirror, rotation÷90, 0, 0]`: pixel
  transforms the **plugin** applies to the decoded frame. `payload[0]` is mirror
  (0/1, horizontal flip); `payload[1]` is rotation in 90° steps (0–3). Sent right
  after the StreamHeader, and again whenever the phone's mirror setting or physical
  orientation changes, so OBS follows a phone being turned without the phone ever
  touching a pixel. A client that doesn't know type 3 skips it harmlessly and still
  gets mirror from the StreamHeader flags.

Rotation deliberately lives on this end. Doing it on the phone means a second full
pass over every frame on the device with the battery and the thermal ceiling, to
save work on a desktop that is not busy.

## Control channel (plugin → phone)

The stream socket is bidirectional, so upstream control rides the same connection —
no second port, no second listener. Frames are
`[type:u32][len:u32][payload]`, big-endian like everything else:

| type   | payload | meaning |
|--------|---------|---------|
| `0x30` | `[targetBitrate:u32][resolutionIndex:u8]` | **setBitrate** — re-target the encoder |
| `0x34` | *(empty)* | **requestKeyFrame** — encode an IDR now |

`0x34` is sent immediately after the StreamHeader, and again whenever the decoder
rejects a packet. The phone runs a long GOP to save bandwidth, so a client that
joins mid-stream has nothing decodable until the next scheduled keyframe; asking
for one turns a multi-second black wait into an immediate picture.

Keyframe requests are debounced by the plugin (~500 ms). A corrupt burst is many
consecutive decode failures, and one IDR per failure would flood a link that is
already in trouble.

`0x30` makes the **plugin** the bitrate controller. It is the end that can see
congestion: it knows when each frame arrived and when the PTS said it should have,
and a one-way delay that keeps growing is queuing on the path — congestion visible
well before any loss. The phone cannot see this; it only knows it handed bytes to
a socket.

The rule is the usual asymmetric one: cut hard (×0.85, floor 800 kbps), recover
gently (+10%), never above the StreamHeader's `bitrate`, which is what the encoder
was configured for. Sent at most once a second so the encoder is not being
reconfigured constantly, and the measurement re-anchors after every change so the
next reading reflects the new rate rather than the old backlog.

The phone is expected to clamp anything unreasonable — the plugin is not trusted
to name a bitrate the hardware can actually deliver.

## Decode (plugin)

1. `avcodec_find_decoder(codec == 1 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264)`, then
   `avcodec_alloc_context3` + `avcodec_open2`.
2. Per packet: an `AVPacket` pointing at the payload, `avcodec_send_packet` /
   `avcodec_receive_frame`.
3. Fill an `obs_source_frame` straight from the `AVFrame` planes — OBS takes planar
   YUV, so there is no swscale in the path.
4. `obs_source_output_video()`. OBS copies the frame, so the `AVFrame` can be
   unreffed immediately after.
