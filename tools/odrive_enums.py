"""ODrive 0.5.x enums and error-flag tables (stdlib only, no hardware access).

Source: the official ODrive 0.5.6 API reference (docs.odriverobotics.com,
v/0.5.6/fibre_types/com_odriverobotics_ODrive.html), cross-checked with 0.5.4.
Written from the documentation tables, not copied from code.

CAUTION: the MKS ODrive Mini runs an *unnumbered* dev build (fw 0.0.0,
unreleased). docs/odrive_dump.txt shows an early 0.5.x API (see
firmware_family()). Bit assignments changed between 0.5.1 and 0.5.2 (DC-bus
bits moved from the axis to a device-level ``error``, which this build does
not have). The bits seen in the real dump (axis 0x100, motor 0x10, encoder
0x4) have the same names in both generations. For other bits treat names as a
hint and confirm with odrivetool's dump_errors(odrv0) (TODO(TBD)).
"""

AXIS_ERRORS = {
    0x00000001: "INVALID_STATE",
    0x00000040: "MOTOR_FAILED",
    0x00000080: "SENSORLESS_ESTIMATOR_FAILED",
    0x00000100: "ENCODER_FAILED",
    0x00000200: "CONTROLLER_FAILED",
    0x00000800: "WATCHDOG_TIMER_EXPIRED",
    0x00001000: "MIN_ENDSTOP_PRESSED",
    0x00002000: "MAX_ENDSTOP_PRESSED",
    0x00004000: "ESTOP_REQUESTED",
    0x00020000: "HOMING_WITHOUT_ENDSTOP",
    0x00040000: "OVER_TEMP",
    0x00080000: "UNKNOWN_POSITION",
}

# Older 0.5.x builds (<= 0.5.1) reported these on the axis as well.
AXIS_ERRORS_LEGACY = {
    0x00000002: "DC_BUS_UNDER_VOLTAGE (legacy 0.5.1 bit)",
    0x00000004: "DC_BUS_OVER_VOLTAGE (legacy 0.5.1 bit)",
    0x00000008: "CURRENT_MEASUREMENT_TIMEOUT (legacy 0.5.1 bit)",
    0x00000010: "BRAKE_RESISTOR_DISARMED (legacy 0.5.1 bit)",
    0x00000020: "MOTOR_DISARMED (legacy 0.5.1 bit)",
    0x00000400: "POS_CTRL_DURING_SENSORLESS (legacy 0.5.1 bit)",
}

DEVICE_ERRORS = {
    0x01: "CONTROL_ITERATION_MISSED",
    0x02: "DC_BUS_UNDER_VOLTAGE",
    0x04: "DC_BUS_OVER_VOLTAGE",
    0x08: "DC_BUS_OVER_REGEN_CURRENT",
    0x10: "DC_BUS_OVER_CURRENT",
    0x20: "BRAKE_DEADTIME_VIOLATION",
    0x40: "BRAKE_DUTY_CYCLE_NAN",
    0x80: "INVALID_BRAKE_RESISTANCE",
}

MOTOR_ERRORS = {
    0x1: "PHASE_RESISTANCE_OUT_OF_RANGE",
    0x2: "PHASE_INDUCTANCE_OUT_OF_RANGE",
    0x8: "DRV_FAULT",
    0x10: "CONTROL_DEADLINE_MISSED",
    0x80: "MODULATION_MAGNITUDE",
    0x400: "CURRENT_SENSE_SATURATION",
    0x1000: "CURRENT_LIMIT_VIOLATION",
    0x10000: "MODULATION_IS_NAN",
    0x20000: "MOTOR_THERMISTOR_OVER_TEMP",
    0x40000: "FET_THERMISTOR_OVER_TEMP",
    0x80000: "TIMER_UPDATE_MISSED",
    0x100000: "CURRENT_MEASUREMENT_UNAVAILABLE",
    0x200000: "CONTROLLER_FAILED",
    0x400000: "I_BUS_OUT_OF_RANGE",
    0x800000: "BRAKE_RESISTOR_DISARMED",
    0x1000000: "SYSTEM_LEVEL",
    0x2000000: "BAD_TIMING",
    0x4000000: "UNKNOWN_PHASE_ESTIMATE",
    0x8000000: "UNKNOWN_PHASE_VEL",
    0x10000000: "UNKNOWN_TORQUE",
    0x20000000: "UNKNOWN_CURRENT_COMMAND",
    0x40000000: "UNKNOWN_CURRENT_MEASUREMENT",
    0x80000000: "UNKNOWN_VBUS_VOLTAGE",
    0x100000000: "UNKNOWN_VOLTAGE_COMMAND",
    0x200000000: "UNKNOWN_GAINS",
    0x400000000: "CONTROLLER_INITIALIZING",
    0x800000000: "UNBALANCED_PHASES",
}

# Bits used only by older 0.5.x builds (names from memory of the 0.5.1 enums,
# UNVERIFIED; only shown for bits the 0.5.6 table does not define).
MOTOR_ERRORS_LEGACY = {
    0x4: "ADC_FAILED (0.5.1 name, unverified)",
    0x20: "NOT_IMPLEMENTED_MOTOR_TYPE (0.5.1 name, unverified)",
    0x40: "BRAKE_CURRENT_OUT_OF_RANGE (0.5.1 name, unverified)",
    0x100: "BRAKE_DEADTIME_VIOLATION (0.5.1 name, unverified)",
    0x200: "UNEXPECTED_TIMER_CALLBACK (0.5.1 name, unverified)",
    0x4000: "DC_BUS_OVER_REGEN_CURRENT (0.5.1 name, unverified)",
    0x8000: "DC_BUS_OVER_CURRENT (0.5.1 name, unverified)",
}

ENCODER_ERRORS = {
    0x1: "UNSTABLE_GAIN",
    0x2: "CPR_POLEPAIRS_MISMATCH",
    0x4: "NO_RESPONSE",
    0x8: "UNSUPPORTED_ENCODER_MODE",
    0x10: "ILLEGAL_HALL_STATE",
    0x20: "INDEX_NOT_FOUND_YET",
    0x40: "ABS_SPI_TIMEOUT",
    0x80: "ABS_SPI_COM_FAIL",
    0x100: "ABS_SPI_NOT_READY",
    0x200: "HALL_NOT_CALIBRATED_YET",
}

CONTROLLER_ERRORS = {
    0x1: "OVERSPEED",
    0x2: "INVALID_INPUT_MODE",
    0x4: "UNSTABLE_GAIN",
    0x8: "INVALID_MIRROR_AXIS",
    0x10: "INVALID_LOAD_ENCODER",
    0x20: "INVALID_ESTIMATE",
    0x40: "INVALID_CIRCULAR_RANGE",
    0x80: "SPINOUT_DETECTED",
}

AXIS_STATES = {
    0: "UNDEFINED",
    1: "IDLE",
    2: "STARTUP_SEQUENCE",
    3: "FULL_CALIBRATION_SEQUENCE",
    4: "MOTOR_CALIBRATION",
    6: "ENCODER_INDEX_SEARCH",
    7: "ENCODER_OFFSET_CALIBRATION",
    8: "CLOSED_LOOP_CONTROL",
    9: "LOCKIN_SPIN",
    10: "ENCODER_DIR_FIND",
    11: "HOMING",
    12: "ENCODER_HALL_POLARITY_CALIBRATION",
    13: "ENCODER_HALL_PHASE_CALIBRATION",
}

CONTROL_MODES = {
    0: "VOLTAGE_CONTROL",
    1: "TORQUE_CONTROL",
    2: "VELOCITY_CONTROL",
    3: "POSITION_CONTROL",
}

INPUT_MODES = {
    0: "INACTIVE", 1: "PASSTHROUGH", 2: "VEL_RAMP", 3: "POS_FILTER", 4: "MIX_CHANNELS",
    5: "TRAP_TRAJ", 6: "TORQUE_RAMP", 7: "MIRROR",
}

AXIS_STATE_IDLE = 1
AXIS_STATE_MOTOR_CALIBRATION = 4
AXIS_STATE_ENCODER_OFFSET_CALIBRATION = 7
AXIS_STATE_CLOSED_LOOP_CONTROL = 8
CONTROL_MODE_TORQUE_CONTROL = 1

# Which table decodes which error register (path relative to odrv0).
ERROR_REGISTERS = {
    "error": DEVICE_ERRORS,
    "axis0.error": AXIS_ERRORS,
    "axis0.motor.error": MOTOR_ERRORS,
    "axis0.encoder.error": ENCODER_ERRORS,
    "axis0.controller.error": CONTROLLER_ERRORS,
}


def decode_flags(value, table, legacy=None):
    """Return a list of flag names set in ``value``. Unknown bits are shown as hex."""
    value = int(value)
    names = []
    remaining = value
    for bit in sorted(table):
        if value & bit:
            names.append(table[bit])
            remaining &= ~bit
    if legacy:
        for bit in sorted(legacy):
            if remaining & bit:
                names.append(legacy[bit])
                remaining &= ~bit
    if remaining:
        names.append("UNKNOWN_BITS_0x%X" % remaining)
    return names


def decode_register(path, value):
    """Decode an error register given its path (``axis0.error`` etc.)."""
    key = path[len("odrv0."):] if path.startswith("odrv0.") else path
    key = key.replace("axis1", "axis0")
    table = ERROR_REGISTERS.get(key)
    if table is None:
        return ["0x%X" % int(value)]
    legacy = {"axis0.error": AXIS_ERRORS_LEGACY, "axis0.motor.error": MOTOR_ERRORS_LEGACY}.get(key)
    return decode_flags(value, table, legacy)


def firmware_family(values):
    """Guess the 0.5.x API generation from which properties a dump contains.

    docs/odrive_dump.txt (unnumbered dev build) has config.enable_uart,
    config.max_regen_current, controller.config.enable_current_mode_vel_limit
    and no device-level 'error' -> an early 0.5.x (0.5.1-like) API."""
    early = ("config.enable_uart" in values or "config.max_regen_current" in values
             or "axis0.controller.config.enable_current_mode_vel_limit" in values)
    late = ("error" in values or "config.enable_brake_resistor" in values
            or "axis0.controller.config.enable_torque_mode_vel_limit" in values)
    if early and not late:
        return "early 0.5.x (0.5.1-like)"
    if late and not early:
        return "late 0.5.x (0.5.2+)"
    return "unknown"
