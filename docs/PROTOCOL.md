# Spancam Direct Stream Protocol (SDSP) v1

The wire protocol between the **phone** (server) and the **OBS plugin** (client).

The phone hardware-encodes H.264/HEVC and streams it; the plugin decodes. Encoding
is dedicated silicon, so the phone stays cool — the expensive half (decode) happens
on the computer. Transport is plain TCP, and all multi-byte integers are
**big-endian**.

## Roles & connect

- **Server:** the phone, bound to `0.0.0.0:8892` so it is reachable over Wi-Fi and,
  later, over a USB port-forward on the same port.
- **Client:** the OBS plugin, which connects to `<phone-ip>:8892`.

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
enterprise APs, anything with client isolation on) drop it entirely. Discovery is
therefore a convenience, never a requirement — typing the host in by hand has to
stay a first-class path.

## Handshake

On connect the client sends one ASCII line, terminated by `\n`:

```
SPANCAM/1 k=<token>
```

`<token>` is the per-session access key shown in the phone app. It gates access on
a shared network — anyone who can reach port 8892 can otherwise watch your camera.
A bad token gets the socket closed with no reply.

If accepted, the server replies with a binary **StreamHeader**, then a continuous
sequence of **Packets** until one side hangs up.

### StreamHeader (24 bytes)

| off | size | field    | notes |
|-----|------|----------|-------|
| 0   | 4    | magic    | `0x53504331` = `"SPC1"` |
| 4   | 1    | codec    | `0` = H.264, `1` = HEVC |
| 5   | 1    | flags    | reserved, 0 |
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
| 0   | 1    | type     | `1` = codecConfig, `2` = videoFrame |
| 1   | 1    | keyframe | videoFrame only: `1` if IDR, else `0` |
| 2   | 2    | reserved | 0 |
| 4   | 8    | ptsUs    | presentation time in microseconds |
| 12  | 4    | size     | payload length in bytes |
| 16  | size | payload  | **Annex-B** bytes |

- **type 1 (codecConfig)** — the encoder's parameter sets in Annex-B: SPS+PPS for
  H.264, VPS+SPS+PPS for HEVC. Sent once at stream start and again on any encoder
  reconfigure. Fed to the decoder ahead of any frame.
- **type 2 (videoFrame)** — one Annex-B access unit.

## Decode (plugin)

1. `avcodec_find_decoder(codec == 1 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264)`, then
   `avcodec_alloc_context3` + `avcodec_open2`.
2. Per packet: an `AVPacket` pointing at the payload, `avcodec_send_packet` /
   `avcodec_receive_frame`.
3. Fill an `obs_source_frame` straight from the `AVFrame` planes — OBS takes planar
   YUV, so there is no swscale in the path.
4. `obs_source_output_video()`. OBS copies the frame, so the `AVFrame` can be
   unreffed immediately after.
