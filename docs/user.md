# avl-wled — User Guide

## What you see

A WLED LED strip that lights up when an AVL waste-collection date is
coming up.

- **Status segment** (leftmost block of LEDs):
  - **Red** — at least one collection is within the next 3 hours.
    Take the bin out now.
  - **Green** — at least one collection is within 24 hours but not yet
    inside the urgent 3-hour window. (Suppressed at night by default.)
- **One additional segment per upcoming collection**, colored by waste
  type (e.g. paper = blue, bio = brown, yellow bag = yellow). The
  mapping is configurable per installation; ask the admin.

If the strip is dark, there is nothing to do.

## Dismissing a reminder

Once you have put the bin out, dismiss the oldest reminder:

```sh
curl http://<server>:8765/ack
```

Common ways to trigger this without typing:

- **Phone home-screen shortcut** (iOS Shortcuts / Android HTTP Shortcuts):
  one tap → done.
- **Physical button** (Shelly, Zigbee button, ESP-based DIY) that fires
  an HTTP GET.
- **Home Assistant** automation, e.g. a button card on the dashboard.
- **NFC tag** stuck on the bin lid that triggers a phone HTTP GET.

Each acknowledge removes the **oldest** active reminder. If multiple
events are showing at once, ack once per event.

## Checking what's active

```sh
curl http://<server>:8765/status
```

Prints how many events the calendar knows about, how many have been
dismissed, and which are currently active with their timestamps.

## Quiet hours

By default, non-urgent warnings (the 24 h window) are suppressed between
22:00 and 07:00 local time so the strip does not glow through the night.
Urgent warnings (within 3 h) always show.
