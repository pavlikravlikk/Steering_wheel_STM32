# Bring-up plan: what to do with what is on the table (2026-11-16; limits table updated 2026-11-18)

Order and safety rules come from `docs/HARDWARE_BRINGUP.md` and `docs/FFB_SAFETY.md`; this page only sorts the steps by
**what hardware is needed**. Stop at the first surprise.

## A. Only the STM32F3Discovery + the Linux laptop (no ODrive, motor, 24 V)
| # | Step | Why |
|---|---|---|
| 1 | F1: build + flash the FFB build that cannot arm (`-DWHEEL_FFB=ON -DWHEEL_FFB_ARM_ALLOWED=0`), with `-DWHEEL_FFB_ARM_B1_ONLY=1` for the B1 test | the USB force-feedback descriptor was never enumerated by a real kernel: `dmesg \| grep -i pidff`, `lsusb -v -d 0483:5752` (descriptor length > 255) |
| 2 | `python3 tools/hid_probe.py --status`: DISARMED, reason ARM_NOT_ALLOWED; `evtest` shows the FF capabilities | the safety state machine and the HID PID device are visible |
| 3 | B1 gesture: tap, tap, hold 3 s: LD4 blinks fast during the hold, nothing else happens (cannot arm) | checks the sequence on the real button |
| 4 | Review the gentle ODrive profile with `python3 tools/generated/odrive_setup.py` (dry run, writes nothing) | agree the numbers before the bench day |
| 5 | By hand: mark the wheel side on the housing, mark for the 10-turn count, check the brake resistor sits on a heat sink, insulate unused wires | preparation |
| 6 | Put the default (non-FFB) build back before gaming without FFB | the default build has no write code at all |

## B. Everything powered (ODrive 24 V, motor, encoder, wheel DISMOUNTED)
0. Every session: mains terminals covered, mains switch under the hand, wheel dismounted, nothing near the housing, wiring only with 24 V off.
1. H4/H5: rotate the housing 10 turns by hand, expect about 24000 counts (`tools/measure_counts.py`).
2. H6: read, apply the gentle profile to RAM, check, then `--save`. Decide `dc_max_negative_current` (suggest -1.5 A).
3. H7: motor + encoder offset calibration (the motor moves about half a turn). Hand on the switch.
4. F2-F3: precheck must say PASS except the ARM gate; watchdog while IDLE?
5. F4: the watchdog really stops the motor (odrivetool only).
6. F5: first arm at 0.3 A, wheel dismounted; turn the rotor by hand: it must pull gently TOWARDS the centre. If it pushes away: B1, power off, investigate.
7. F6: every fault on purpose. F7: `fftest`, then a game at 20 % strength. F8: mount the wheel only after F0-F7 and with a hardware e-stop.

## C. Limits (several independent layers)
| Quantity | Limit | Where it is enforced |
|---|---|---|
| Bus voltage | trips below 18 V / above 30 V; FFB refuses/faults outside 20-28 V | profile (ODrive) + firmware precheck |
| Supply current | PSU 13.4 A max; ODrive `dc_max_positive_current` 6 A; DC fuse 10-15 A recommended (not fitted) | profile + hardware |
| Motor current | ODrive `current_lim` 4 A (firmware refuses to arm above the profile); FFB `max_amps` <= 1 A (code cap; now 1.0 A, the first tests used 0.3 A), slew `ffb.slew_amps_per_s` (now 100 A/s) | profile + firmware |
| Speed | `odrive.vel_limit` (now 4 turns/s), ODrive overspeed error, firmware OVERSPEED at `ffb.overspeed_turns_s` (now 5 turns/s); OVERSPEED never tested on hardware | profile + firmware |
| STM32 hang / unplugged wire | ODrive watchdog 0.25 s; firmware FAULT | ODrive + firmware |
| Regeneration | brake resistor 50 W / 2 ohm on AUX; the profile now limits `dc_max_negative_current` to -1.5 A (about 36 W), heat sink still needed | profile + hardware |
| Rotation range | HID clamps at +-lock/2; arming refused when the wheel is outside the range (`WHEEL_OUT_OF_RANGE`); soft wall under FFB; **no mechanical stop yet** (needed before the wheel is mounted) | firmware + (missing) hardware |
| Accidental start | arm gesture (B1 + 2 paddles, or B1 tap-tap-hold with `WHEEL_FFB_ARM_B1_ONLY=1`, wheel dismounted only) + full precheck | firmware |

## D. Not protected (read this)
- There is no hardware emergency stop on the 24 V side (only the mains switch, with a delay of a few seconds); no DC fuse.
- The FFB code never ran on a real motor. The brake resistor heat sink and the gear mesh are checked by hand.
