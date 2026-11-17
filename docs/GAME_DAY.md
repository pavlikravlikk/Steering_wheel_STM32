# Game day: BeamNG on the Windows laptop, hardware stays at the Linux laptop (variant A)

Status when this was written (2026-11-17): everything up to F7 was done on Linux with the wheel DISMOUNTED (only the rotor housing,
held by hand). Nothing in this file has been run yet. What is NOT verified: how the Windows HID PID driver treats our descriptor, the
direction of game constant forces (convention PID / DirectInput, 90 deg = +X), the OVERSPEED fault on hardware.

Safety rules for the whole session: the housing is held by the hand and the steering wheel stays dismounted; the force is capped at
`ffb.max_amps` 0.5 A (about 0.25 Nm) by the firmware; hand next to the 24 V e-stop; **B1 pressed ONCE disarms** (never hold B1 for
1 s or longer: that re-centres); mains terminals covered; fingers away from the gears.

## 1. Linux laptop: bring the rig to "ready" (every power-up)
**Shortcut (2026-11-17): one command does steps 2-5 below** (close odrivetool first, the STM32 must be running):
`~/odrive-env/bin/python tools/session_start.py`. It reads the errors WITH NAMES, clears only the normal watchdog leftover (0x800) and
stops on anything else (read it, then `--force-clear`), calibrates at 5 A / 6 A after you type `I AM CLEAR` (the motor moves), applies
the profile (RAM only), clears the leftover that enabling the watchdog causes, and prints the pre-arm verdict (`READY`). Tests use a fake
ODrive; it has NOT been run on the real ODrive yet. Do the same after an STM32 restart (the cable move in section 2).

1. 24 V on. STM32: ST-LINK and USER cables in the Linux laptop. ODrive USB in the Linux laptop.
2. `odrv0.axis0.clear_errors()` in odrivetool, then `odrv0.axis0.error` must be 0 and stay 0 (the STM32 feeds the watchdog).
3. Calibrate by hand (the ODrive comes up with the saved 6 A / 5 A values): `odrv0.axis0.clear_errors()`,
   `odrv0.axis0.motor.config.current_lim = 6.0`, `odrv0.axis0.motor.config.calibration_current = 5.0`,
   `odrv0.axis0.requested_state = 7` (the housing turns about 180 deg and back, hands clear), wait ~10 s, then
   `current_state` 1, `error` 0, `encoder.error` 0, `encoder.is_ready` True. Close odrivetool (`Ctrl+D`, `y`).
4. `~/odrive-env/bin/python tools/generated/odrive_setup.py --apply` (profile to RAM: 4 A, vel_limit 2, vel_gain 0.25, watchdog 0.25).
5. `~/odrive-env/bin/python tools/odrive_status.py --live` must end with `FFB precheck would PASS`.
   Keep the housing at the straight position: the centre is the position when the STM32 starts.

## 2. Move only the USER cable to the Windows laptop
1. Disarm if armed: press B1 once (LD6 blinks once a second).
2. Unplug ST-LINK and USER from the Linux laptop. Plug USER into the Windows laptop (it powers the board, as on 2026-11-16).
   The ODrive USB stays on the Linux laptop. 24 V stays on (the calibration lives in RAM).
3. The STM32 restarted, so the ODrive shows `0x800`: on the Linux laptop `odrv0.axis0.clear_errors()`, `odrv0.axis0.error` must be 0.
   (`hid_probe` is not available on the Windows laptop; use the LEDs: LD6 one blink a second = ready, solid = armed, LD4 solid = FAULT,
   LD8 blink count = reason, see `docs/FFB_SAFETY.md` section 6. LD10 blinking is normal: no MCP23017 board.)

## 3. Windows: check the device, then BeamNG
1. `joy.cpl` (Game Controllers): "Steering Wheel Controller" is listed, X moves when the housing turns. Y and Z stay constant (no pedals).
2. BeamNG: steering axis = the wheel X axis; throttle, brake and everything else on the keyboard (as on 2026-11-16). Do NOT bind Y/Z.
3. Force feedback in the controller options: enable it and set the strength to the LOWEST value you can, about 20 % (the option names
   differ between versions; write down what you see). If BeamNG shows no force feedback option for this device, stop and report it.

## 4. Arm and drive
1. Hold the housing with both hands at the rim, away from the gears.
2. Arm with B1: click, click, then press and HOLD about 3 s from the start of that press, until LD4 stops blinking. LD6 solid = armed.
3. Drive slowly. Expected: the wheel pulls back to the centre when you steer (self-aligning), more at speed.
4. **Direction check (the open item):** steer right while moving, the force must push you back to the left. If it pushes you FURTHER to
   the right (or the opposite on the left), release the wheel immediately, press B1, and report it: the fix is `FFB_INVERT_HOST_FORCE`
   (then a rebuild and a re-flash, see `docs/FFB_SAFETY.md` F7). Do not try to "fix" it in the game.
5. Stop at the first surprise: B1 once, or the e-stop.

## 5. Shut down
Press B1 once, quit the game, then switch the 24 V off. The ODrive forgets the calibration (it is in RAM) and the profile: that is
expected, section 1 repeats it next time. Nothing is saved on the ODrive (`--save` was deliberately never used).

## Known limits
- No mechanical rotation stop yet: this is only for the dismounted housing. Mounting the steering wheel (F8) needs the hardware e-stop
  and a mechanical stop, see `docs/FFB_SAFETY.md` section 5.
- The torque tapers to 0 as the speed approaches `vel_limit` (2 turns/s) because of the ODrive velocity limiter (`vel_gain`), so fast
  hand movements feel lighter.
