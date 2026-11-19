# Documentation index

Start with `STATUS.md` (where the project is) and the root `README.md` (build, HID report, wiring of the fallbacks).

| Group | File | What it is |
|---|---|---|
| **State** | `STATUS.md` | Current state, open items, generated block (`make status`) |
| **Hardware** | `hardware.md` | Facts about the rig (wiring, motor, gears, resistor), what is measured |
| | `odrive_dump.txt`, `odrive_dump_fresh.txt` | Real ODrive property dumps (tests read them; do not rename) |
| **Runbooks** | `HARDWARE_BRINGUP.md` | Bring-up steps H0-H9, F-steps with the bench results |
| | `BRINGUP_PLAN.md` | The same steps sorted by what hardware is on the table; limits per layer |
| | `GAME_DAY.md` | Game session: Linux laptop (ODrive) + Windows laptop (BeamNG) |
| | `BENCH_CHECKLIST.md` | Checklist of the first bench day |
| **Safety** | `FFB_SAFETY.md` | Safety case: what can be written, protections, what is NOT protected |
| **Design** | `ARCHITECTURE.md` | Modules, rules, how to add the pedals |
| | `DECISIONS.md` | Log of decisions with the reason and how to change them |
| | `IOC_TODO.md` | What the CubeMX `.ioc` must mirror |
| **History** (`archive/`) | `STATUS_HISTORY.md` | Dated status notes, older first-session numbers |
| | `AUDIT.md` | Audit of the first firmware (before the ODrive work) |
| | `TEAMMATE_REVIEW.md` | Review of the teammates' zip |
| | `DUMP_RECONCILIATION.md` | First comparison of the assumptions with the real ODrive dump |

Rule: numbers that change (amps, gains, limits) live in `config/hardware_profile.toml`. In the documents, point to the profile instead of copying the value; when a value is quoted, date it.
