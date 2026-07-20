# Security

## Reporting a vulnerability

Email **support@adewaskar.com** with details and steps to reproduce. Please don't
open a public issue for a security problem until it's been addressed.

## What this plugin does on your machine

- It opens **outbound** TCP connections to your phone. It does not listen for
  inbound connections and does not open any port on the computer.
- For USB it shells out to `adb` (which must already be installed) to run
  `adb forward` and connects over loopback. It does not bundle `adb` or any other
  executable.
- It sends a small UDP broadcast on the local network to discover the phone. No
  data leaves your local network, and nothing is sent to any server operated by
  the author.
- The video stream between phone and computer is **not** encrypted. On Wi-Fi it is
  gated by a per-session access key, but treat it like any other LAN camera feed:
  fine on a network you trust, not something to run across a hostile one.
