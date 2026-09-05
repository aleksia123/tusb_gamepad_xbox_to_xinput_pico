# Porting the ReflexX macro engine to RP2350 firmware

This firmware now runs the macro engine from **ReflexX** (a Windows C# controller
middleware) directly on the dongle. The physical XInput controller is still hosted
over PIO-USB on core 1 and re-emitted as a USB XInput device on core 0; the macro
engine sits in the middle, as a new **Phase 4** stage in `process_xinput()`.

Nothing else from that project came across: no AI vision pipeline, no licensing,
no WebView2 UI, no process-based profile switching.

## What was ported

| ReflexX (C#) | Here (C) |
|---|---|
| `MacroEngine/MacroProcessor.cs` (2408 lines) | `src/macro_engine.c` / `.h` |
| `MacroEngine/MotionSampler.cs` | `src/motion_sampler.h` |
| `Domain/Entities/*.cs`, `Domain/Enums/*.cs` | `src/macro_types.h` |
| `Domain/MonotonicClock.cs`, `Random.Shared` | `src/mono_clock.h` |
| `Features/AiVisionDebug/AimContext` | `src/aim_context.h` / `.c` (stub) |

Plus the firmware-side infrastructure the port needed:

| New file | Contents |
|---|---|
| `src/profile_store.c` / `.h` | 4 profile slots in the last flash sectors, CRC32-guarded, with a never-brick fallback |
| `src/config_mode.c` / `.h` | Boot-time CDC configuration server |

Changed: `src/hid_app.c` (Phase 4 stage, button-mask bridge, on-pad profile
switch, filters read from the active profile), `src/main.c` (boot-mode fork,
profile-store init, PRNG seed), `CMakeLists.txt`, `.gitignore`.

All **26** macro types are ported, one handler each, with the same activation
gating, the same timing constants and the same arithmetic:

NoRecoil · AdaptiveRecoil · AutoFire · AutoPing · Remap · Sequence · Toggle ·
AimAssistBuff · ScriptedShape · HeadAssist · ProgressiveRecoil · TrackingAssist ·
AutoFireNoRecoil · InstaDropShot · JumpShot · StrafeShot · HoldBreath ·
SlideCancel · FastDrop · AutoSprint · CrowBar · Custom (script sequencer) ·
LuaScript · AimSnap · AimSmooth · TriggerBot

### Non-obvious behaviours preserved deliberately

Each of these looks like something to clean up and is in fact load-bearing:

* **Every macro's own `TriggerSource.None` default.** They genuinely differ:
  NoRecoil and CrowBar fall back to RT, FastDrop to LT, ProgressiveRecoil to
  *not shooting* (so it is inert without a trigger source), TrackingAssist and
  AimAssistBuff to *always active*. Collapsing them would silently change five
  macros' behaviour.
* **`HoldBreath` does not use the shared `IsMacroActive()` gate**, and treats both
  `None` and `RightTrigger` as "use LT (ADS)" — that is the fix for the macro
  appearing dead in earlier ReflexX releases.
* **`AimSmooth`'s identity-based lock retention** (keyed on the tracker's
  `LockId`, not on a release radius or a per-frame position jump), its coast
  guard, its Schmitt-hysteresis sticky lock, its tau-based EMA mapping and its
  round-away-from-zero output. Each replaced an earlier heuristic with a specific,
  documented failure mode.
* **`MotionSampler`'s flick easing on `1 - progress`** — amplitude peaks at t=0
  and decays, like a real stick snap. Inverting it inverts the whole feel.
  `VerticalOval` swaps the radii *and* adds a 90° rotation.
* **`ProgressiveRecoil`'s phase boundaries follow the ammo split**, not equal
  thirds, and it has its own easing table that defaults to smoothstep (not
  linear) on unknown values.
* **The Custom sequencer's 256-iteration safety counter.** On a PC a runaway
  script is a stutter; here it would be a dead USB device.

### Systemic C adaptations

* **`double` → `float`.** The Cortex-M33 has a single-precision FPU; `double` is
  software-emulated and roughly 40× slower, unaffordable in a 1 kHz path.
  Everything lands in int16 stick units or uint8 trigger units, so float's ~7
  significant digits are three orders of magnitude more than the output can
  express — the substitution is not observable.
* **In-place mutation** instead of `state = ProcessX(state, macro)`. Composition
  order is identical: each handler still sees the accumulated effect of the
  higher-priority ones, which is what makes additive stacking (NoRecoil +
  TrackingAssist + AimSmooth all writing RS) behave the same.
* **`GamepadButton?` → a `GP_NONE` (0) sentinel.** Exact: `None` is 0 in the C#
  enum, and every nullable-button call site treated null as "not configured".
* **`Dictionary<string, MacroRuntime>` → a fixed array indexed by macro slot**,
  reset on profile switch so no state bleeds between macros.
* **`MonotonicClock` → `now_ms()` / `now_us()`** wrapping `to_ms_since_boot()`,
  biased by a one-hour epoch. That bias matters: every macro uses 0 as the
  "never fired" sentinel, and an unbiased boot clock would make a zeroed
  timestamp look *recent* during the first seconds after power-on.
* **`Random.Shared` and Box-Muller → xorshift32**, seeded from `get_rand_32()` so
  two boards do not emit an identical humanisation pattern — predictability is
  what that jitter exists to defeat.
* **No `_weaponProfile`.** That object came from OpenCV template-matching the
  weapon name off the game's HUD. There is no screen here, so every
  `if (_weaponProfile != null)` resolves to its else branch — exactly ReflexX's
  behaviour with weapon detection off, i.e. its default. The one visible
  consequence is that HeadAssist's RecoilMagnitude distance signal contributes
  the neutral 0.5.
* Dropped: `ILogger` calls (a printf inside the report callback would cost
  hundreds of microseconds), the `FeatureGate` licence check, and the
  `MacroToggleChanged` event (its only consumer was a WebView2 overlay toast).

### Where Phase 4 sits, and why

`process_xinput()` now runs: locals → axial deadzone → right-stick radial
correction → trigger shaping → **macro engine** → commit.

Macros run *after* the filters for the same reason ReflexX runs
`CompositeInputFilter` before `MacroProcessor`: macros reason about clean, shaped
input ("is the player pulling down past 5% of range?", "is forward past 60%?").
Feeding them raw sensor values would make every threshold behave differently per
controller, and writing before the filters would re-scale a macro's synthesized
deflection through a deadzone rescale it was never meant to pass through.

A profile with no macros costs one NULL/count check, so an unconfigured board's
path is unchanged.

### LuaScript

`MoonSharp` is .NET-only. An embedded Lua would need roughly 100 KB of flash plus
a heap allocator on the 1 kHz input path, and a channel to get script text onto the
device. Not in this pass. The source's fallback for a missing engine is
`if (_scriptEngine is null) return state;` — a silent pass-through — and that is
what `process_lua_script()` does. The dispatch case, the config slot and
`macro_runtime_t.lua_last_tick_ms` are all wired, so adding a VM later is a change
to that one function.

Both stubs are documented at their definitions in the style of `src/pad_config.h`.

### Also out of scope, per the brief

Licensing / Velopack / update mechanics, the WebView2 desktop UI, and
process-based profile auto-switching — the last is impossible here regardless,
since the dongle has no way to know what game is running.

## Config mode

`src/pad_config.h` already documented why there is no *live* config channel:
Windows binds the dongle's single USB interface to its in-box `xusb22.sys` XInput
driver, which claims it exclusively at kernel level (and that exclusivity is what
makes the dongle driverless in the first place), so no browser page can open it —
and grafting a second interface onto `tusb_gamepad`'s fixed XInput descriptor
risks breaking XInput enumeration outright. That reasoning still holds.
Configuration is therefore a **separate boot mode** - and unlike the original
GP15-jumper design, it is now the **unconditional default on every power-on**:
no jumper, no combo, just plug the board in.

**Grace window (`config_mode.c`):** config mode gives up after
`CONFIG_GRACE_MS` (5000 ms) with no host activity, or immediately on an
explicit `REBOOT` command, and warm-reboots into normal XInput operation - so
an unattended board still ends up working as a controller within 5 seconds.
Ground **GP29** (active low, internal pull-up) before powering on to suspend
that timer entirely and stay in config mode indefinitely, for an unhurried
first-time setup. GP12/GP13 carry PIO-USB D+/D− (and are off-header on the
Waveshare RP2350-USB-A anyway); no other GPIO is claimed by this project or by
`board_init()`. GP15 was considered first but is not broken out to a header
pin on the RP2350-USB-A (it only exposes GP0-10 and GP26-29), hence GP29.

**Surviving the reboot without repeating the wait (`src/boot_request.h`/`.c`):**
the warm reboot out of config mode (grace timeout or `REBOOT`) stashes a magic
value in a watchdog scratch register before resetting. `main()` checks that
flag, gated on `watchdog_caused_reboot()`, before deciding whether to even
enter config mode - so a RUN-button reset immediately afterward skips config
mode entirely and comes up as a controller with no delay. A real power-on
reset (unplug or brown-out) clears the always-on scratch domain regardless,
so simply power-cycling the board always lands back in config mode first -
it can never get permanently stuck either way, and the flag is consumed the
moment it's read so a crash loop can't wedge it there.

**Procedure**

1. Plug the board in (or tap RUN/reset). It enumerates as a USB serial
   device — no XInput interface, no USB host stack, core 1 never launched.
2. Open the configurator within 5 seconds (or ground GP29 first for no time
   limit).
3. When finished, send `REBOOT` (the configurator's "Switch to Xbox mode"
   button does this) to warm-reboot straight back to normal operation — no
   unplug needed. A bare power-cycle also returns to normal operation (via
   config mode's grace window, same as any other power-on).

The one-time grace window is the only latency this adds versus the original
jumper design, and it is capped at 5s and skipped entirely on the reboot that
leaves config mode - normal, repeated play pays it exactly once per physical
power cycle, not once per boot.

Config mode reuses `tusb_gamepad`'s existing `INPUT_MODE_USBSERIAL` driver, so no
USB descriptor was authored or modified. It runs only `tud_task()` plus the
protocol handler — deliberately *not* `tusb_gamepad_task()`, which would run the
library's log-pump on the same endpoint with a 10 ms sleep per iteration.

### Flash storage

The last **32 sectors (128 KB)** of flash hold a `profile_store_t`: magic,
version, CRC32, active-slot index, and 4 × `profile_t` (filter config +
`macro_count` + 16 macro definitions; 17632 bytes each, ~70 KB total).

* Blank flash (`0xFF`) fails the magic check; a bit-rotted image fails the CRC.
  Either way the firmware silently substitutes `g_pad_config`'s compiled-in
  defaults and an empty macro list — **a fresh board is never bricked** and works
  before it has ever been configured.
* Writes stage in RAM. Flash is erased and programmed only by an explicit
  `COMMIT`, after the client has verified its own CRC32 against the device's, so a
  cable yank mid-transfer leaves the old image intact. The write is verified by
  readback.
* Flash is only ever written in config mode, where core 1 is never launched —
  `flash_range_erase()` disables XIP and would fault code executing from flash.
* `PICO_BOARD` is `none` here, so `PICO_FLASH_SIZE_BYTES` may be undefined; it
  falls back to 2 MB, the safe floor. On a larger part the very last sectors
  simply go unused.

### On-pad profile switching

During normal operation, hold **Back** and tap **DPad-Up / DPad-Down** to cycle
the active slot. The combo buttons are consumed, so the game does not also see a
menu press. RAM-only: persisting each switch would mean a flash erase from inside
the XInput report path, which is precisely what the point above forbids. On the
next power-cycle the slot marked active in flash wins.

## Opening the configurator

`tools/configurator/index.html` — a single self-contained file: no framework, no
build step, no network access.

1. Plug the board in (or power-cycle it) — it comes up in config mode by
   default. Ground GP29 first if you want more than the 5-second grace
   window to connect.
2. Open `tools/configurator/index.html` in desktop **Chrome or Edge**. Web Serial
   is Chromium-only; Firefox and Safari do not implement it.
3. Click **Connect** and pick the board's serial port (do this within 5s of
   powering on, unless GP29 is grounded).
4. It loads all 4 slots, then lets you edit the filter knobs and the macro list.
5. **Save + Commit** stages every slot, verifies CRCs, then commits to flash.
6. Click **Switch to Xbox mode** (or power-cycle / remove the GP29 jumper) to
   use the new configuration.

`tools/configurator/PROTOCOL.md` is the normative wire format: commands, the
complete binary offset table for every struct, and every enum value. The firmware
carries `_Static_assert`s on all struct sizes, so a layout change fails the build
rather than silently desynchronising the two sides.

> Note: the repo's `.gitignore` excludes `tools/`. It now reads `tools/*` plus
> `!tools/configurator/` — git cannot re-include anything beneath an *excluded
> directory*, so a bare `tools` line would have made the negation a no-op.

## Build

```bash
export PICO_SDK_PATH=~/pico-sdk-right
cd ~/tusbgamepad_back && rm -rf build && mkdir -p build && cd build
cmake .. && make -j$(nproc)
```

Produces `tusb_gamepad_ds4_to_xinput_pico-sdk.uf2` and `.elf`. Clean, zero
warnings.

New CMake dependencies: `hardware_flash` and `hardware_sync` (profile store),
`pico_rand` (PRNG seed), `tinyusb_device` (CDC in config mode).
