# Porting the ReflexX macro engine to RP2350 firmware

This firmware now runs the macro engine from **ReflexX**, a Windows C#
controller-input middleware, directly on the dongle. Nothing else from that
project came across: no AI vision, no licensing, no WebView2 UI, no
process-based profile switching.

## What was ported

Two source files, plus the domain types they need:

| ReflexX (C#) | Here (C) |
|---|---|
| `MacroEngine/MacroProcessor.cs` (~2400 lines) | `src/macro_engine.c` |
| `MacroEngine/MotionSampler.cs` | `src/motion_sampler.h` |
| `Domain/Entities/*.cs`, `Domain/Enums/*.cs` | `src/macro_types.h` |
| `Domain/MonotonicClock.cs`, `Random.Shared` | `src/mono_clock.h` |
| `Features/AiVisionDebug/AimContext` | `src/aim_context.h` / `.c` (stub) |

All **26** macro types are ported, one handler each, with the same activation
gating, the same timing constants and the same arithmetic:

NoRecoil, AdaptiveRecoil, AutoFire, AutoPing, Remap, Sequence, Toggle,
AimAssistBuff, ScriptedShape, HeadAssist, ProgressiveRecoil, TrackingAssist,
AutoFireNoRecoil, InstaDropShot, JumpShot, StrafeShot, HoldBreath, SlideCancel,
FastDrop, AutoSprint, CrowBar, Custom (script sequencer), LuaScript, AimSnap,
AimSmooth, TriggerBot.

The engine runs as a new **Phase 4** stage in `process_xinput()`
(`src/hid_app.c`), immediately after the existing Phase 3 filter pipeline and
before the state is committed to the shared gamepad. That ordering mirrors
ReflexX, where `CompositeInputFilter` runs before `MacroProcessor`: macros reason
about clean, deadzone-corrected input (CrowBar asks "is the player pulling down
past 5% of range?", AutoSprint asks "is forward past 60%?"), so feeding them raw
sensor values with a jittery centre would make every threshold behave
differently per controller.

### Structural changes forced by the MCU

- **No `Dictionary<string, MacroRuntime>`.** Runtime state is a fixed array
  indexed by macro *slot*, parallel to the profile's macro array. No allocator
  runs on the 1 kHz input path.
- **`double` → `float`.** The Cortex-M33 FPU is single-precision; `double` would
  be software-emulated (~40× slower). Output is rounded to int16 anyway, so the
  substitution is not observable. Documented in `mono_clock.h`.
- **`MonotonicClock.NowMs` → `now_ms()`** wrapping `to_ms_since_boot()`, biased
  by one hour so a zeroed "never fired" timestamp can never read as recent —
  the same hazard the C# version solved with a `TickCount64` epoch offset.
- **`Random.Shared` → xorshift32**, seeded from the hardware RNG, for the
  humanisation jitter. Box-Muller gaussian is kept for AimSnap.
- Dropped with no behavioural effect: `FeatureGate` (licence tiers) and the
  `MacroToggleChanged` event (fed an on-screen toast; there is no screen).

## What is intentionally stubbed, and why

These are **the source's own degraded paths**, not invented fallbacks. In C#
each of these handlers opens with `if (_aimContext is null) return state;` or
`if (_scriptEngine is null) return state;` — the null-object pattern used by its
unit tests. The stubs reproduce exactly those branches.

- **AI vision (AimSnap, AimSmooth, TriggerBot).** ReflexX gets targets from a
  thread that captures the screen over DXGI and runs a YOLO ONNX model on the
  GPU. This dongle has no camera, no framebuffer, no GPU, and 520 KB of SRAM
  against ~30 MB of model weights — the pipeline is *unimplementable* here, not
  merely unimplemented. `aim_context_try_acquire()` is a `__attribute__((weak))`
  function that always reports "no target", so these three macros stay silent.
  **Everything downstream of acquisition is fully ported and live**: the FOV-ring
  acquisition gate, identity-based lock retention, coast guard, prediction,
  ego-motion feed-forward, velocity feed-forward with deadband, sticky lock with
  Schmitt hysteresis, the plant-inverted P controller, the legacy open-loop curve,
  the EMA output smoothing, the burst timer. A future vision coprocessor (an
  ESP32-CAM over UART, or a host feeding targets over the config CDC channel)
  only has to provide a strong definition of that one function — no changes to
  `macro_engine.c`, the flash layout, or the configurator's wire format.
- **AdaptiveRecoil is the exception: it works.** With no lock its `distFactor`
  stays 0, which selects the *Min (far)* compensation. That is precisely what the
  desktop app does with AI Vision switched off, so on this hardware the macro
  behaves as a fixed NoRecoil at the Min values — useful, and honest.
- **LuaScript.** No embedded Lua VM in this pass, so the handler is the
  documented no-op the source takes with no engine injected. The dispatch entry,
  config slot and the per-tick delta anchor are wired for a future VM.
- **Weapon profiles.** ReflexX sets `_weaponProfile` from OpenCV template
  matching on the game's weapon HUD — screen capture again. Every use site in the
  source is `_weaponProfile != null ? weapon : macro`, so the port takes the
  fully-specified null branch: the macro's own values. HeadAssist's
  RecoilMagnitude distance estimator returns the source's own no-profile
  constant, 0.5.

## Config mode

There is no live config channel while the dongle is a gamepad: Windows binds its
single XInput interface to the in-box `xusb22.sys`, which claims it exclusively
at kernel level (that exclusivity is what makes the dongle driverless), and
grafting a second interface onto `tusb_gamepad`'s fixed XInput descriptor risks
breaking enumeration. So configuration is a separate **boot mode**.

**Pin: GP15, active low, internal pull-up.** GP12/GP13 carry PIO-USB D+/D−; GP15
is claimed by nothing in this project or in `board_init()`. Unconnected reads
high, so normal boot is the default with no jumper and no external parts.

**Procedure**

1. Short **GP15 to any GND pin**.
2. Power-cycle the board, or tap RUN/reset.
3. It enumerates as a plain USB serial port — no XInput interface, no USB host
   stack, no macro engine.
4. Remove the jumper and power-cycle to return to normal pad operation.

The pin is sampled once, early in `main()`, before any USB init, and requires 6
of 8 agreeing reads so a slow pull-up rise cannot drop a normally-booting board
into config mode. Normal boot takes one branch and is otherwise byte-identical to
the previous firmware — no added latency on the working path.

**Storage.** The last 128 KB of flash holds a header (magic, version, CRC32,
active-slot index) plus **4 profile slots**, each with the Phase-3 filter config
(`pad_config_t`) and up to **16 macros**. Blank flash reads as `0xFF` and fails
both the magic and CRC checks, so a fresh board silently falls back to the
compiled-in `g_pad_config` defaults and an empty macro list — it can never be
bricked, and needs no configurator round-trip to work. Writes land in a RAM
working copy and only reach flash on an explicit, CRC-verified `COMMIT`, so a
cable yank mid-transfer leaves the old image intact. Flash is never written while
the pad is live (erase disables XIP, which would fault core 1).

**On-pad profile switching.** Hold **Back** and tap **D-Pad Up/Down** to cycle
the active slot. RAM-only — it resets to the flash-marked slot on power-cycle,
because persisting each switch would mean a flash erase from inside the XInput
report path. The combo's buttons are stripped from the outgoing report so the
game does not also see them. There is deliberately no process-based
auto-switching: this hardware cannot know what game is running.

## Opening the configurator

`tools/configurator/index.html` — a single self-contained file: no framework, no
build step, no network access. It talks to the board over the **Web Serial API**,
so it is a genuinely separate application, distinct from the firmware and from
anything touching the live XInput interface.

1. Put the board in config mode (above).
2. Open `tools/configurator/index.html` in desktop **Chrome or Edge** (Web Serial
   is Chromium-only — Firefox and Safari will not work).
3. Click **Connect** and pick the board's serial port.
4. It reads slot 0 automatically. Edit filters and macros, then
   **Send to device** (RAM, verified by CRC) and **Commit to flash**.

The page validates its own struct layout against the geometry the firmware
reports in its `HELLO` reply and refuses to write on a mismatch, so a
configurator paired with a differently-built firmware fails loudly instead of
corrupting a profile. Its struct offsets were generated from the firmware headers
with `offsetof()`, not written by hand. Wire format:
`tools/configurator/PROTOCOL.md` (normative).

AimSnap / AimSmooth / TriggerBot / LuaScript appear in the type dropdown but show
a visible "not available on this hardware yet" note instead of their tuning
fields; AdaptiveRecoil is editable with a note explaining its degraded mode.

## Build

```bash
export PICO_SDK_PATH=~/pico-sdk-right
rm -rf build && mkdir build && cd build
cmake .. && make -j$(nproc)
```

Produces `tusb_gamepad_ds4_to_xinput_pico-sdk.uf2` (~237 KB). All project
sources compile warning-free.
