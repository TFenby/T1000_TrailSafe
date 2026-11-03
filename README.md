# T1000 TrailSafe

Meshtastic firmware for the Seeed SenseCAP T1000-E that turns the front button into canned messages with delivery
confirmation. Two presses sends message 1, three presses message 2, and so on, as a direct message to every node you
have marked as a favorite. The buzzer tells you when a favorite has received it. A message that starts with `!` is
resent every two minutes until a favorite acknowledges it.

It is stock Meshtastic plus one drop-in module (`ButtonMessage`) and a one-line patch, rebuilt against upstream
releases by GitHub Actions. Everything is configured from the Meshtastic app or CLI; nothing needs a rebuild.

## Button

| Presses | Action |
|---|---|
| 1 | stock (stops an alert) |
| 2 | send canned message 1 |
| 3 | send canned message 2 |
| 4 | send canned message 3 |
| N | send canned message N-1 |
| hold | stock shutdown |

A press count with no message configured keeps its stock action: 2 = position ping, 3 = GPS on/off, 4 = mute.
With the module disabled, everything is stock.

Every send also broadcasts your position the way the stock double press does, on the lowest-numbered channel that has
position sharing enabled.

### Tones

Morse notation: `.` is one unit, `-` is three. One unit is 80 ms (`BUTTON_MESSAGE_MORSE_UNIT_MS`).

| Tone | Meaning |
|---|---|
| `.` (E) | awake after a button wake, listening |
| `..` (I, "info") | message sent |
| `--` (M, "mayday") | `!` message sent or resent |
| `--.` (G, for "good") | a favorite received it |
| two 7-unit tones, 7 units apart (intentionally invalid) | not delivered: no favorites, no key for the favorite yet, the mesh rejected it, or nobody acked within a minute. For a `!` message the resends continue. |

Tones follow the device's buzzer mode setting. The stock single press, and stock actions for unconfigured press counts,
keep their stock sounds.

## Device setup

1. **Flash** the UF2 (below).
2. **Messages.** In the app, Settings → Module Configuration → Canned Messages: turn it on, optionally turn on
   *send bell* so the receiver's External Notification bell alert fires, and enter the messages separated by `|`,
   for example:

       Check-in OK|!HELP - need assistance|I am OK

   With the CLI instead:

       meshtastic --set canned_message.enabled true --set canned_message.send_bell true
       meshtastic --set-canned-message "Check-in OK|!HELP - need assistance|I am OK"

   The list is at most 200 characters. Changing the list takes effect immediately; changing the toggles reboots the
   device (stock behavior).
3. **Favorites.** While the app is connected to the T1000-E, mark the node(s) that should receive the messages as
   favorites. Messages go to every favorite (up to four); the first one to acknowledge counts.
4. **Receiver.** On the receiving node, enable External Notification with the message and bell alerts you want
   (buzzer, LED, vibration). Its screen and the connected phone show the message as a normal direct message.

### Channels

- **Acknowledgements travel on the primary channel**, so the tracker and the receiver must share it. The default
  LongFast primary is fine.
- **Position precision and privacy come from your channel settings.** The position broadcast goes to the
  lowest-numbered channel with position sharing enabled, at that channel's precision. For a precise position that only
  your group can read: turn position off on the primary channel and add a private secondary channel with position
  precision set to precise, on both devices.
- **Direct messages are encrypted to the favorite's public key.** The tracker needs the favorite's key, which it
  learns when the nodes first see each other. If it is missing you get the not-delivered tone; the tracker then asks for the key,
  so the next attempt works.

### Power saving

For long battery life, use the stock tracker power saving:

    meshtastic --set device.role TRACKER --set power.is_power_saving true --set position.position_broadcast_secs 900

After each position broadcast the tracker sleeps with the radio and GPS off for `position_broadcast_secs`, then
reboots and sends again. With this firmware:

- **A press wakes it.** Pressing the button during the sleep reboots the tracker straight away instead of at the end
  of the interval. Once it is fully up, about 6 s later, a single `.` says it is listening: presses before the tone
  are dropped, and the presses after it count as usual (two for message 1). A timed wake stays silent and, like a
  power-on, ignores presses for its first 10 s.
- **It stays awake until a delivery is resolved.** While a message waits for its acknowledgement it does not sleep:
  up to a minute for a normal message, and for a `!` message until a favorite acks or the resends run out (about two
  hours). If anything holds the sleep off for a whole broadcast interval, the tracker skips that sleep and tries
  again after its next position broadcast.
- **Low battery still wins.** The low-battery shutdown waits at most 30 s for a pending delivery, then shuts down
  anyway.
- **Each wake is a GPS hot start, and the whole wake is about 25 s.** The GNSS backup supply stays on through the
  sleep and the receiver is put into backup mode properly (stock Meshtastic's sleep command was refused at nearly
  every power-down), so it keeps its clock and ephemeris: on the bench the fix came 11–18 s after waking, every
  cycle, indoors. After the fix the receiver stays on for 5 s (stock: 20 s; with none at all the next fix took
  36–236 s, so the receiver needs a moment to keep what it learned), the position goes out, and the tracker is
  asleep about 6 s after the fix (stock: 5 s of waiting plus a 4 s rewrite of all five settings files).

## Flashing

With the T1000-E plugged in by USB, hold the button for about three seconds until a USB drive appears, then copy the
`.uf2` from `dist/` (or from a release or sync-PR artifact) onto it. Settings survive the update.

Updating later through the Meshtastic app or web flasher replaces this firmware with stock. Take new builds from here
instead.

## Building

    ./build.sh

Clones upstream Meshtastic at the tag in `UPSTREAM_REF` into `upstream/`, copies `overlay/` over it, applies
`patches/`, builds env `tracker-t1000-e-trailsafe` with PlatformIO in a local `.venv`, and puts the UF2 in `dist/`.
The first run downloads about a gigabyte of toolchain.

The host-side logic test runs without PlatformIO:

    g++ -std=c++17 -I overlay/src/modules/optional/ButtonMessage test/test_logic.cpp -o /tmp/t && /tmp/t

### What is in the overlay

- `overlay/src/modules/optional/ButtonMessage/`: the module. Upstream's optional-module hook compiles and registers
  anything in that directory, so no upstream file changes for the module itself.
- `overlay/variants/nrf52840/trailsafe/platformio.ini`: the build environment, extending the stock T1000-E one.
- `patches/0001-…`: one `#if` line in `src/main.cpp` so the firmware reports the canned-message settings as
  available, which makes the phone apps show the editor for a screenless device.
- `patches/0002-…`: nRF52 power-saving tracker sleep ends on a button press, and the boot that follows knows whether
  the button or the timer ended it (`trackerWakeCause`, kept in a retained register across the reset).
- `patches/0003-…`: the tracker's post-broadcast sleep waits while the radio or a module (ButtonMessage, awaiting an
  ack) vetoes it, bounded by one broadcast interval; the sleep is armed 1 s after the send instead of 5 s, and only
  the node database and device state are saved on the way down.
- `patches/0004-…`: T1000-E sleep power: the GNSS backup supply stays on through a timed sleep, as Seeed's own
  firmware keeps it, so the receiver hot-starts on wake; the accelerometer rail and the sensor pins are released
  (`variant_shutdown()`).
- `patches/0005-…`: GPS, for a power-saving tracker: a 5 s post-fix hold instead of 20 s (`GPS_SLEEPY_TRACKER_HOLD_MS`);
  the Airoha backup-mode handshake done as Seeed's driver does it (unlock, then the command repeated for up to a
  second: the receiver answers about half a second after the unlock, outside upstream's 400 ms budget); a hold that
  expires is no longer re-armed by a fix sentence in the same pass; on a cached probe the receiver is not
  reconfigured again (upstream's `$PAIR066` reboots it on every boot) and the probe cache is written once, not every
  boot.

0002–0004 are general fixes written to be offered upstream.

Timing defaults (`BUTTON_MESSAGE_*` in `ButtonMessage.h`) can be overridden with `-D` flags in the env ini. Bench
builds can add `-DBUTTON_MESSAGE_FAKE_WAKE` (a timed wake behaves like a button wake) and
`-DBUTTON_MESSAGE_FAKE_PRESSES=2` (that many presses are emulated right after the ready tone), for example through
`PLATFORMIO_BUILD_FLAGS`; neither is set in release builds.

## Keeping up with upstream

The `upstream` workflow runs weekly (and on demand). When Meshtastic publishes a newer release tag (prereleases
included; run it with *stable_only* to ignore them), it bumps `UPSTREAM_REF` on a branch, builds, and opens a pull
request with the UF2 attached to the run. A PR marked **build FAILED** means the overlay or the patch no longer fits
upstream: fix it on that branch, push, and merge. Every push to `master`, including merging a sync PR, publishes a GitHub release named
`<upstream tag>-<overlay commit>` with the UF2 attached, marked pre-release whenever the upstream Meshtastic release is.

## License

GPL-3.0, like Meshtastic.
