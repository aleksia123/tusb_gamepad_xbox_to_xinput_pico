# ReflexX-RP2350 config protocol

The wire contract between the firmware's config mode (`src/config_mode.c`) and the
browser configurator (`tools/configurator/index.html`). **This document is
normative** — both sides are written against it, and the firmware carries
`_Static_assert`s that fail the build if a struct drifts from the offsets below.

## 0. Why a separate mode at all

While the board is acting as an XInput pad, Windows binds its single USB interface
to the in-box `xusb22.sys` driver, which claims the device exclusively at kernel
level. Nothing in a browser can open it, and adding a second interface to
`tusb_gamepad`'s fixed XInput descriptor risks breaking the driverless XInput
enumeration that is the whole point of the dongle. So configuration is a distinct
**boot mode**, entered by grounding a pin, in which the firmware comes up as a
plain USB CDC device with no XInput interface and no USB host stack at all.

**Entering config mode**

1. Short **GP15** to any GND pin.
2. Power-cycle the board, or tap RUN/reset.
3. It enumerates as a USB serial port. Open `index.html` in Chrome/Edge and pick it.
4. Remove the jumper and power-cycle to go back to normal pad operation.

GP15 is free: GP12/GP13 carry PIO-USB D+/D−, and nothing else in the project or in
`board_init()` claims a GPIO. The pin is sampled with the internal pull-up and
debounced (8 samples, 6-of-8 majority), so an unconnected pin always boots normally.

## 1. Transport

* USB CDC (virtual serial). Baud rate is irrelevant — CDC ignores it. The
  configurator opens at 115200 out of habit.
* **Requests**: one line of ASCII, terminated by `\n` (a `\r` is also accepted).
* **Responses**: exactly one line per request, terminated by `\r\n`.
* Strictly request/response, one outstanding request at a time. The device never
  speaks unprompted, so no framing or correlation ID is needed.
* Max request line length is 512 bytes. A longer line is discarded whole and
  answered `ERR toolong` — never truncated, because a truncated `WRITE` would land
  good-looking bytes at the wrong offset.
* Binary payloads are lowercase hex, 2 chars per byte, max **64 bytes (128 chars)**
  per message. Chunking is **client-pulled**: the client asks for each chunk. The
  CDC TX FIFO is 256 bytes, so a server-push stream would need flow control and a
  stall timeout; pull-based transfer is self-pacing and a lost reply is just a retry.

## 2. Commands

| Request | Response | Notes |
|---|---|---|
| `PING` | `PONG` | liveness |
| `HELLO` | `RXCFG <ver> slots=<n> macros=<n> profile_bytes=<n> macro_bytes=<n> seq_steps=<n> script_steps=<n> active=<n>` | handshake + geometry |
| `ACTIVE` | `ACTIVE <n>` | read active slot |
| `ACTIVE <n>` | `OK` | set active slot (RAM only) |
| `READ <slot> <off> <len>` | `DATA <off> <hex>` | `len` ≤ 64, `off+len` ≤ `profile_bytes` |
| `WRITE <slot> <off> <hex>` | `OK <n>` | `n` = bytes stored; RAM only |
| `CRC <slot>` | `CRC <8 hex digits>` | CRC32 over the whole slot as the device holds it |
| `RESET <slot>` | `OK` | slot ← compiled-in defaults, no macros (RAM only) |
| `COMMIT` | `OK committed` / `ERR flash` | the only command that touches flash |

Errors: `ERR cmd` (unknown), `ERR args` (unparseable), `ERR range` (bounds),
`ERR hex` (malformed payload), `ERR toolong`.

### Geometry reported by HELLO (current firmware)

```
ver = 1   slots = 4   macros = 16
profile_bytes = 17632   macro_bytes = 1100
seq_steps = 8   script_steps = 16
```

The client must read these from `HELLO` rather than hardcoding them, so a firmware
built with different caps is *detected* instead of silently mis-parsed.

### Save sequence

`WRITE`s land only in a RAM working copy. The recommended save flow is:

1. `WRITE slot off hex` … for the whole profile (276 chunks of 64 bytes).
2. `CRC slot` → compare against a CRC32 the client computes over its own bytes.
3. `COMMIT` only if they match.

That way a cable yank or a corrupted chunk cannot leave a half-written profile in
flash: the old image stays intact until a verified `COMMIT`. `COMMIT` writes the
whole store (all 4 slots + header + its own CRC32) and verifies by readback.

CRC32 is IEEE 802.3 / zlib: reflected, poly `0xEDB88320`, init `0xFFFFFFFF`, final
one's complement.

## 3. Binary layout

Little-endian. No packing attributes are used — every field sits at its natural
alignment, which is what makes the layout reproducible across compilers. `bool`
is one byte, `0`/`1`. Buttons are a **32-bit mask** (see §3.5); `0` means
"not configured", exactly as `GamepadButton?` null did in the C# source.

### 3.1 `profile_t` — 17632 bytes (the `READ`/`WRITE` unit)

| Off | Size | Field |
|---|---|---|
| 0 | 16 | `name` — ASCII, NUL-padded |
| 16 | 10 | `filters` (`pad_config_t`, §3.2) |
| 26 | 2 | padding |
| 28 | 4 | `macro_count` (i32, 0…16) |
| 32 | 17600 | `macros[16]`, stride **1100** |

### 3.2 `pad_config_t` — 10 bytes

| Off | Type | Field |
|---|---|---|
| 0 | u8 | `left_stick_axial_deadzone` (half-width; int16 units = value×256) |
| 1 | u8 | `right_stick_axial_deadzone` |
| 2 | u8 | `trigger_l_max` (ceiling, 255 = off) |
| 3 | u8 | `trigger_r_max` |
| 4 | u8 | `trigger_l_threshold` (instant-mode activation point) |
| 5 | u8 | `trigger_r_threshold` |
| 6 | bool | `trigger_l_instant` |
| 7 | bool | `trigger_r_instant` |
| 8 | bool | `uncap_radius` |
| 9 | u8 | `right_stick_corner_cap_pct` (0 = off, 100 = hard circle, 101–255 = soft cap) |

### 3.3 `macro_definition_t` — 1100 bytes

| Off | Type | Field |
|---|---|---|
| 0 | char[24] | `name` |
| 24 | u8 | `type` (§3.6) |
| 25 | bool | `enabled` |
| 26 | bool | `toggle_mode` |
| 27 | u8 | `trigger_source` (§3.7) |
| 28 | i32 | `priority` (ascending = runs earlier) |
| 32 | u32 | `activation_button` |
| 36 | i32 | `delay_ms` (reserved; unused by the ported handlers) |
| 40 | i32 | `interval_ms` |
| 44 | i32 | `duration_ms` (reserved) |
| 48 | bool | `loop` |
| 49 | bool | `activate_on_ads` |
| 52 | f32 | `intensity` |
| 56 | f32 | `randomization_factor` (also the AimAssistBuff wiggle jitter, clamped 0…0.5) |
| 60 | u32 | `ping_button` |
| 64 | u32 | `source_button` |
| 68 | u32 | `target_button` |
| 72 | i32 | `recoil_compensation_x` |
| 76 | i32 | `recoil_compensation_y` |
| 80 | i32 | `flick_strength` |
| 84 | i32 | `flick_interval_ms` |
| 88 | u32 | `crouch_button` |
| 92 | u32 | `jump_button` |
| 96 | i32 | `jump_interval_ms` |
| 100 | f32 | `strafe_amplitude` (0…1) |
| 104 | i32 | `strafe_interval_ms` |
| 108 | u32 | `sprint_button` |
| 112 | f32 | `sprint_threshold` (0.05…1) |
| 116 | i32 | `sprint_press_duration_ms` (10…500) |
| 120 | u32 | `breath_button` |
| 124 | u32 | `slide_button` |
| 128 | u32 | `slide_cancel_button` |
| 132 | i32 | `slide_cancel_delay_ms` |
| 136 | i32 | `step_count` (Sequence, 0…8) |
| 140 | 96 | `steps[8]` (`macro_step_t`, stride 12, §3.4a) |
| 236 | 44 | `motion` (`motion_script_t`, §3.4b) |
| 280 | 184 | `head_assist` (§3.4c) |
| 464 | 56 | `progressive_recoil` (§3.4d) |
| 520 | 32 | `tracking_assist` (§3.4e) |
| 552 | 32 | `crowbar` (§3.4f) |
| 584 | 24 | `adaptive_recoil` (§3.4g) |
| 608 | 80 | `aim_assist` (§3.4h) |
| 688 | 412 | `script` (§3.4i) |

### 3.4a `macro_step_t` — 12 bytes
`0` u32 `button_press` · `4` u32 `button_release` · `8` i32 `delay_after_ms`

### 3.4b `motion_script_t` — 44 bytes
`0` u8 `shape` (§3.8) · `1` u8 `target` (0=left,1=right) · `2` u8 `easing` (§3.9) ·
`3` bool `clockwise` · `4` bool `additive` · `8` f32 `radius_x_norm` ·
`12` f32 `radius_y_norm` · `16` f32 `rotation_deg` · `20` f32 `period_ms` ·
`24` f32 `duration_ms` (0 = indefinite, orbital only) · `28` f32 `direction_deg` ·
`32` f32 `amplitude_norm` · `36` f32 `start_phase_deg` · `40` f32 `intensity_mul`

### 3.4c `head_assist_config_t` — 184 bytes
`0` `short_range` · `44` `medium_range` · `88` `long_range` (three
`motion_script_t`) · `132` u8 `distance_source` (§3.10) · `133` bool
`fire_on_press` · `134` bool `fire_once` · `136` u32 `cycle_button` ·
`140` f32 `short_hold_ms_max` · `144` f32 `medium_hold_ms_max` ·
`148` f32 `deflection_short_max` · `152` f32 `deflection_medium_max` ·
`156` f32 `recoil_short_max` · `160` f32 `recoil_medium_max` ·
`164` f32 `weight_trigger` · `168` f32 `weight_deflection` · `172` f32
`weight_recoil` · `176` i32 `refire_cooldown_ms` · `180` i32 `min_trigger_hold_ms`

> `recoil_*` and `weight_recoil` feed the RecoilMagnitude signal, which needs a
> weapon profile from screen OCR. There is no screen here, so that signal returns
> the neutral 0.5 — identical to running ReflexX with weapon detection off. The
> fields are kept for wire compatibility.

### 3.4d `progressive_recoil_config_t` — 56 bytes
`0` i32 `total_ammo` · `4` f32 `full_mag_duration_ms` · `8` i32 `start_comp_x` ·
`12` i32 `start_comp_y` · `16` i32 `mid_comp_x` · `20` i32 `mid_comp_y` ·
`24` i32 `end_comp_x` · `28` i32 `end_comp_y` · `32` bool `second_stage_enabled` ·
`33` u8 `second_stage_trigger_source` · `34` u8 `phase_easing` ·
`36` u32 `second_stage_activation_button` · `40` i32 `second_stage_comp_x` ·
`44` i32 `second_stage_comp_y` · `48` f32 `noise_factor` · `52` f32 `sensitivity_scale`

### 3.4e `tracking_assist_config_t` — 32 bytes
`0` u8 `shape` · `1` u8 `target` · `2` u8 `easing` · `3` bool `clockwise` ·
`4` bool `free_orbit` · `8` f32 `base_radius_norm` · `12` f32 `max_radius_norm` ·
`16` f32 `period_ms` · `20` f32 `deflection_threshold` · `24` f32 `scale_curve` ·
`28` f32 `intensity_mul`

### 3.4f `crowbar_config_t` — 32 bytes
`0` u8 `mode` (0=Rapido, 1=Padrao) · `4` i32 `base_htg_value` ·
`8` i32 `max_compensation` · `12` f32 `assist_factor` ·
`16` f32 `deflection_threshold` · `20` f32 `deflection_curve` ·
`24` f32 `noise_factor` · `28` f32 `htg_scale_padrao`

### 3.4g `adaptive_recoil_config_t` — 24 bytes
`0` i32 `min_compensation_x` · `4` i32 `min_compensation_y` ·
`8` i32 `max_compensation_x` · `12` i32 `max_compensation_y` ·
`16` f32 `intensity` · `20` f32 `randomization_factor`

### 3.4h `aim_assist_config_t` — 80 bytes
`0` i32 `max_fov_pixels` · `4` i32 `max_target_age_ms` · `8` f32 `min_confidence` ·
`12` f32 `max_target_center_y` · `16` f32 `min_target_center_y` ·
`20` f32 `headshot_bias_fraction` · `24` f32 `humanization_sigma` ·
`28` i32 `snap_max_impulse_stick_units` · `32` i32 `snap_impulse_duration_ms` ·
`36` i32 `snap_cooldown_ms` · `40` f32 `magnetic_strength` ·
`44` f32 `magnetic_strength_fire` · `48` i32 `magnetic_max_pull` ·
`52` f32 `magnetic_curve` · `56` bool `magnetic_invert_y` ·
`57` bool `magnetic_sticky_lock` · `58` bool `trigger_release_on_target_lost` ·
`60` f32 `prediction_ms` · `64` f32 `output_smoothing` · `68` f32 `ego_motion_gain` ·
`72` f32 `tracking_feed_forward` · `76` i32 `trigger_burst_ms`

> Carried across in full even though the vision front-end is a stub on this
> hardware — the point is that a future target provider needs no wire change. The
> configurator hides these fields behind a "not available on this hardware yet"
> notice rather than pretending they do something.

### 3.4i `script_definition_t` — 412 bytes
`0` i32 `step_count` (0…16) · `4` u8 `trigger_mode` (§3.11) · `5` bool `auto_loop` ·
`8` f32 `speed_multiplier` · `12` i32 `random_delay_min_ms` ·
`16` i32 `random_delay_max_ms` · `20` i32 `max_repeat_count` (0 = infinite) ·
`24` i32 `stop_after_ms` (0 = no limit) · `28` `steps[16]` (`script_step_t`, stride 24)

`script_step_t` — 24 bytes:
`0` u8 `action` (§3.12) · `1` u8 `axis` (§3.13) · `2` bool `has_axis` ·
`3` bool `disabled` · `4` u32 `button` · `8` i16 `value` (SetAxis −32767…32767,
SetTrigger 0…255) · `12` i32 `duration_ms` · `16` i32 `loop_target_index` ·
`20` i32 `repeat_count` (0 = infinite)

### 3.5 Button mask (bit-identical to `ReflexX.Domain.Enums.GamepadButton`)

| Bit | Button | | Bit | Button |
|---|---|---|---|---|
| `0x00000001` | DPadUp     | | `0x00000200` | RightShoulder |
| `0x00000002` | DPadDown   | | `0x00000400` | Guide |
| `0x00000004` | DPadLeft   | | `0x00001000` | A |
| `0x00000008` | DPadRight  | | `0x00002000` | B |
| `0x00000010` | Start      | | `0x00004000` | X |
| `0x00000020` | Back       | | `0x00008000` | Y |
| `0x00000040` | LeftThumb  | | `0x00100000` | Misc / Share |
| `0x00000080` | RightThumb | | `0x00000000` | None (= unset) |
| `0x00000100` | LeftShoulder | | | |

`Misc` reuses the C# `TouchpadClick` bit. It can arm a macro but is never emitted —
XInput has no wire bit for it.

### 3.6 `macro_type`
`0` NoRecoil · `1` AutoFire · `2` AutoPing · `3` Remap · `4` Sequence · `5` Toggle ·
`6` AimAssistBuff · `7` HeadAssist · `8` ScriptedShape · `9` ProgressiveRecoil ·
`10` TrackingAssist · `11` AutoFireNoRecoil · `12` InstaDropShot · `13` JumpShot ·
`14` StrafeShot · `15` HoldBreath · `16` SlideCancel · `17` FastDrop ·
`18` AutoSprint · `19` CrowBar · `20` Custom · `21` LuaScript · `22` AimSnap ·
`23` AimSmooth · `24` TriggerBot · `25` AdaptiveRecoil

### 3.7 `trigger_source`
`0` None · `1` LeftTrigger · `2` RightTrigger · `3` LeftShoulder · `4` RightShoulder ·
`5` DualTrigger (LT **or** RT) · `6` DualShoulder (LB **or** RB)

### 3.8 `shape_kind`
`0` Flick · `1` Circle · `2` HorizontalOval · `3` VerticalOval · `4` DiagonalOval

### 3.9 `easing_kind`
`0` Linear · `1` EaseOutQuad · `2` EaseOutCubic · `3` EaseInOutSine · `4` EaseOutBack ·
`5` Smoothstep

### 3.10 `distance_source`
`0` TriggerHoldTime · `1` AimStickDeflection · `2` RecoilMagnitude · `3` Manual · `4` Auto

### 3.11 `script_trigger`
`0` WhileHeld · `1` OnPress · `2` Toggle

### 3.12 `script_action`
`0` PressButton · `1` ReleaseButton · `2` SetAxis · `3` SetTrigger · `4` Wait ·
`5` LoopBack · `6` LoopStart

### 3.13 `analog_axis`
`0` LeftStickX · `1` LeftStickY · `2` RightStickX · `3` RightStickY ·
`4` LeftTrigger · `5` RightTrigger

## 4. On-flash image (informational)

Not reachable over the protocol — the device owns it — but useful when debugging
a board with `picotool save`:

```
profile_store_t @ (PICO_FLASH_SIZE_BYTES - 32*4096)
  0   u32 magic       0x314D5852  ("RXM1")
  4   u32 version     1
  8   u32 crc32       IEEE, over bytes 12..end
  12  u32 active_slot 0..3
  16  profile_t[4]    17632 each
```

Blank flash (`0xFF`) fails the magic check; a bit-rotted image fails the CRC.
Either way the firmware silently substitutes the compiled-in `g_pad_config`
defaults and an empty macro list, so a fresh board is never bricked and works
before it has ever been configured.

## 5. On-pad profile switching

During normal (non-config) operation, hold **Back** and tap **DPad-Up** /
**DPad-Down** to cycle the active slot. The combo buttons are consumed so the game
does not also see a menu press. This is **RAM-only** — persisting it would mean a
flash erase from inside the XInput report path, which disables XIP and would fault
core 1. On the next power-cycle the slot marked active in flash (set via `ACTIVE`
+ `COMMIT`) wins.

There is no process-based auto-switching: this hardware has no way to know what
game is running.
