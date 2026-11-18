#!/usr/bin/env python3
"""Read the wheel's FFB status (state, pre-arm reason, FAULT, current, supply) on WINDOWS (or any OS with hidapi) and keep a log.

Setup (once, on the laptop where the wheel is plugged into USB USER):
    py -m pip install hidapi
    copy tools\\hid_probe.py and tools\\hid_probe_win.py into one folder (this script imports hid_probe for the decoding)

Run:
    py hid_probe_win.py              # live monitor, appends to wheel_log.txt, Ctrl+C stops
    py hid_probe_win.py --once       # read the status once and exit
    py hid_probe_win.py --log C:\\x\\game.txt --interval 0.05

What it writes (only changes, so the file stays small):
  * every change of state / pre-arm reason / fault / flags, with a time stamp;
  * when the state becomes FAULT: the last ~2 s of samples BEFORE it (current, supply voltage, command rate), so you can see
    whether the supply rose (regeneration), the current was at the cap, or the link failed;
  * a line every 10 s with the peak current and the supply min / max;
  * when the wheel disappears (USB unplugged, STM32 reset) and when it comes back.
It only READS (HID feature report 0x40). It never sends anything to the wheel. The fault code is lost when the STM32 is reset,
so start this BEFORE the game and do not reset the STM32 before you have copied the log.
"""
import argparse
import collections
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hid_probe  # noqa: E402  (same folder)

VID, PID = hid_probe.VID, hid_probe.PID
HISTORY_SAMPLES = 40          # about 2 s at 50 ms
SUMMARY_EVERY_S = 10.0


def key_of(r):
    """What counts as a change worth a log line."""
    return (r["state"], r["prearm_reason"], r["fault"], tuple(r["flags"]))


class Monitor:
    """Pure logic (no hardware): feed() takes decoded status dicts and returns the lines to log."""

    def __init__(self, history=HISTORY_SAMPLES):
        self.last_key = None
        self.hist = collections.deque(maxlen=history)
        self.peak_a = 0.0
        self.vmin = None
        self.vmax = None
        self.last_summary = None
        self.present = None

    def _stamp(self, now):
        return time.strftime("%H:%M:%S", time.localtime(now)) + ".%03d" % int((now % 1) * 1000)

    def _sample_line(self, now, r):
        return "%s  %+.3f A  vbus %.1f V  rate %d Hz  loop_max %d us  odrive_timeouts %d" % (
            self._stamp(now), r["torque_cmd_amps"], r["vbus_v"], r["command_rate_hz"], r["loop_time_max_us"],
            r["odrive_timeouts"])

    def feed(self, now, r):
        out = []
        if self.present is not True:
            out.append("%s  wheel connected" % self._stamp(now))
            self.present = True
        self.peak_a = max(self.peak_a, abs(r["torque_cmd_amps"]))
        if r["vbus_v"] > 0:
            self.vmin = r["vbus_v"] if self.vmin is None else min(self.vmin, r["vbus_v"])
            self.vmax = r["vbus_v"] if self.vmax is None else max(self.vmax, r["vbus_v"])
        k = key_of(r)
        if k != self.last_key:
            became_fault = r["state"] == "FAULT" and (self.last_key is None or self.last_key[0] != "FAULT")
            if became_fault and self.hist:
                out.append("%s  --- last samples before the FAULT ---" % self._stamp(now))
                out.extend("    " + line for line in self.hist)
            out.append("%s  %s" % (self._stamp(now), hid_probe.format_report(r)))
            if became_fault:
                out.append("%s  *** FAULT = %s *** (fault codes: docs/FFB_SAFETY.md section 6)" % (self._stamp(now), r["fault"]))
            self.last_key = k
        self.hist.append(self._sample_line(now, r))
        if self.last_summary is None:
            self.last_summary = now
        elif now - self.last_summary >= SUMMARY_EVERY_S:
            out.append("%s  summary: peak %.3f A, vbus %.1f..%.1f V, state %s" % (
                self._stamp(now), self.peak_a, self.vmin or 0.0, self.vmax or 0.0, r["state"]))
            self.peak_a = 0.0
            self.vmin = self.vmax = None
            self.last_summary = now
        return out

    def lost(self, now):
        if self.present is False:
            return []
        self.present = False
        return ["%s  wheel NOT readable (unplugged, STM32 reset, or held by another program)" % self._stamp(now)]


def open_status_device(hid_module):
    """Try every HID interface of the wheel until one answers the status feature report. Returns (device, path) or None."""
    for info in hid_module.enumerate(VID, PID):
        dev = hid_module.device()
        try:
            dev.open_path(info["path"])
            data = bytes(dev.get_feature_report(hid_probe.REPORT_ID_FFB_STATUS, hid_probe.FFB_STATUS_LEN))
            if data and data[0] == hid_probe.REPORT_ID_FFB_STATUS:
                hid_probe.decode_ffb_status(data)
                return dev, info["path"]
            dev.close()
        except (OSError, ValueError):
            try:
                dev.close()
            except Exception:  # noqa: BLE001
                pass
    return None


def read_status(dev):
    data = bytes(dev.get_feature_report(hid_probe.REPORT_ID_FFB_STATUS, hid_probe.FFB_STATUS_LEN))
    return hid_probe.decode_ffb_status(data)


def run(hid_module, log_path, interval, once=False, out=print, sleep=time.sleep, clock=time.time, max_loops=None):
    mon = Monitor()
    log = open(log_path, "a", encoding="utf-8") if log_path else None

    def emit(lines):
        for line in lines:
            out(line)
            if log:
                log.write(line + "\n")
                log.flush()

    if log:
        log.write("=== %s monitor started ===\n" % time.strftime("%Y-%m-%d %H:%M:%S"))
    dev = None
    loops = 0
    try:
        while True:
            if dev is None:
                found = open_status_device(hid_module)
                if found is None:
                    emit(mon.lost(clock()))
                    if once:
                        return 2
                else:
                    dev = found[0]
            if dev is not None:
                try:
                    emit(mon.feed(clock(), read_status(dev)))
                except (OSError, ValueError):
                    try:
                        dev.close()
                    except Exception:  # noqa: BLE001
                        pass
                    dev = None
                    emit(mon.lost(clock()))
                if once:
                    return 0
            loops += 1
            if max_loops is not None and loops >= max_loops:
                return 0
            sleep(interval)
    except KeyboardInterrupt:
        return 0
    finally:
        if log:
            log.close()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--once", action="store_true", help="read the status once and exit")
    ap.add_argument("--log", default="wheel_log.txt", help="log file (appended); '' = no file")
    ap.add_argument("--interval", type=float, default=0.05, help="seconds between reads (default 0.05)")
    a = ap.parse_args(argv)
    try:
        import hid  # noqa: PLC0415
    except ImportError:
        print("The 'hidapi' package is missing: run  py -m pip install hidapi")
        return 2
    print("Monitoring the wheel (VID %04X PID %04X). Ctrl+C stops. Log: %s" % (VID, PID, a.log or "(none)"))
    return run(hid, a.log or None, a.interval, once=a.once)


if __name__ == "__main__":
    sys.exit(main())
