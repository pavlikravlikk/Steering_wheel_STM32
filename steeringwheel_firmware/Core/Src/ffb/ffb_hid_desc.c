/**
 * @file    ffb_hid_desc.c
 * @brief   FFB build report descriptor (joystick + HID PID + vendor status).
 *
 * Layout rules followed (Linux hid-pidff requirements):
 *  - every PID report is a Logical collection named by its report usage;
 *  - Effect Type (0x25), Effect Operation (0x78), Device Control (0x96),
 *    Block Load Status (0x8B) and Direction (0x57) are nested Logical
 *    collections; the arrays inside them have Logical Minimum 1;
 *  - value fields have exactly one usage per element;
 *  - time fields carry Unit = seconds with Unit Exponent -3 (milliseconds);
 *  - the effect-type list is identical (same order) in Set Effect and
 *    Create New Effect.
 * tests/test_ffb_desc.c parses this array and checks all of the above plus
 * every report size against ffb_hid_desc.h.
 */
#include "ffb_hid_desc.h"

#define LO(x)        (uint8_t)((unsigned)(x) & 0xFFU)
#define HI(x)        (uint8_t)(((unsigned)(x) >> 8) & 0xFFU)
#define UP(x)        0x05, (x)                         /* Usage Page            */
#define UP2(x)       0x06, LO(x), HI(x)
#define U(x)         0x09, (x)                         /* Usage                 */
#define UX(page, id) 0x0B, LO(id), HI(id), LO(page), HI(page)  /* extended usage */
#define UMIN(x)      0x19, (x)
#define UMAX(x)      0x29, (x)
#define COLL(x)      0xA1, (x)                         /* 1 Application, 2 Logical */
#define END          0xC0
#define RID(x)       0x85, (x)
#define LMIN1(x)     0x15, (uint8_t)(x)
#define LMIN2(x)     0x16, LO((uint16_t)(int16_t)(x)), HI((uint16_t)(int16_t)(x))
#define LMAX1(x)     0x25, (x)
#define LMAX2(x)     0x26, LO(x), HI(x)
#define LMAX4(x)     0x27, LO(x), HI(x), (uint8_t)(((unsigned long)(x) >> 16) & 0xFFU), \
                           (uint8_t)(((unsigned long)(x) >> 24) & 0xFFU)
#define RSIZE(x)     0x75, (x)
#define RCOUNT(x)    0x95, (x)
#define INPUT(x)     0x81, (x)
#define OUTPUT(x)    0x91, (x)
#define FEATURE(x)   0xB1, (x)
#define UNIT_S       0x66, 0x01, 0x10                  /* SI linear, time^1     */
#define UNIT_DEG     0x65, 0x14                        /* English rotation, deg */
#define UNIT_NONE    0x65, 0x00
#define UEXP(x)      0x55, (x)                         /* 4-bit two's complement */
#define EXP_MS       0x0D                              /* -3                     */
#define EXP_CENTI    0x0E                              /* -2                     */
#define VAR          0x02                              /* Data,Var,Abs           */
#define ARR          0x00                              /* Data,Array,Abs         */
#define PAD          0x03                              /* Cnst,Var,Abs           */

#define BLOCK_INDEX(io) U(0x22), LMIN1(1), LMAX1(FFB_MAX_EFFECTS), RSIZE(8), RCOUNT(1), io(VAR)

#define EFFECT_TYPES \
  U(0x26), U(0x27), U(0x30), U(0x31), U(0x32), U(0x33), U(0x34), \
  U(0x40), U(0x41), U(0x42), U(0x43)

const uint8_t ffb_hid_report_desc[] = {
  UP(0x01), U(0x04), COLL(0x01),                    /* Generic Desktop / Joystick */

  /* ---- 0x01 joystick input: X, Y, Z (uint16) + 16 buttons ---------------- */
  RID(FFB_RID_JOYSTICK),
  U(0x30), U(0x31), U(0x32),
  LMIN1(0), LMAX4(65535UL), RSIZE(16), RCOUNT(3), INPUT(VAR),
  UP(0x09), UMIN(1), UMAX(16), LMIN1(0), LMAX1(1), RSIZE(1), RCOUNT(16), INPUT(VAR),

  UP(0x0F),                                         /* ---- PID page ---------- */

  /* ---- 0x02 PID State (input) ------------------------------------------ */
  U(0x92), COLL(0x02),
    RID(FFB_RID_PID_STATE),
    U(0x9F), U(0xA0), U(0xA4), U(0xA5), U(0xA6),    /* paused, actuators enabled, safety
                                                       switch, override switch, power */
    LMIN1(0), LMAX1(1), RSIZE(1), RCOUNT(5), INPUT(VAR),
    RCOUNT(3), INPUT(PAD),
    U(0x94), RCOUNT(1), INPUT(VAR),                 /* effect playing */
    U(0x22), LMIN1(1), LMAX1(FFB_MAX_EFFECTS), RSIZE(7), RCOUNT(1), INPUT(VAR),
  END,

  /* ---- 0x11 Set Effect (output) ----------------------------------------- */
  U(0x21), COLL(0x02),
    RID(FFB_RID_SET_EFFECT),
    BLOCK_INDEX(OUTPUT),
    U(0x25), COLL(0x02),
      EFFECT_TYPES,
      LMIN1(1), LMAX1(FFB_ET_COUNT), RSIZE(8), RCOUNT(1), OUTPUT(ARR),
    END,
    U(0x50), U(0x54), U(0x51), U(0xA7),             /* duration, trigger repeat,
                                                       sample period, start delay */
    LMIN1(0), LMAX2(32767), UNIT_S, UEXP(EXP_MS), RSIZE(16), RCOUNT(4), OUTPUT(VAR),
    UEXP(0x00), UNIT_NONE,
    U(0x52), LMIN1(0), LMAX2(255), RSIZE(8), RCOUNT(1), OUTPUT(VAR),            /* gain */
    U(0x53), LMIN1(0), LMAX1(16), RSIZE(8), RCOUNT(1), OUTPUT(VAR),             /* trigger button */
    U(0x55), COLL(0x02),                                                         /* axes enable */
      UX(0x0001, 0x0030), LMIN1(0), LMAX1(1), RSIZE(1), RCOUNT(1), OUTPUT(VAR),  /* X */
    END,
    U(0x56), RCOUNT(1), OUTPUT(VAR),                                             /* direction enable */
    RCOUNT(6), OUTPUT(PAD),
    U(0x57), COLL(0x02),                                                         /* direction */
      UX(0x000A, 0x0001), LMIN1(0), LMAX4(FFB_DIR_MAX), UNIT_DEG, UEXP(EXP_CENTI),
      RSIZE(16), RCOUNT(1), OUTPUT(VAR),
      UEXP(0x00), UNIT_NONE,
    END,
  END,

  /* ---- 0x12 Set Envelope (output) --------------------------------------- */
  U(0x5A), COLL(0x02),
    RID(FFB_RID_SET_ENVELOPE),
    BLOCK_INDEX(OUTPUT),
    U(0x5B), U(0x5D), LMIN1(0), LMAX2(10000), RSIZE(16), RCOUNT(2), OUTPUT(VAR),  /* attack, fade level */
    U(0x5C), U(0x5E), LMIN1(0), LMAX2(32767), UNIT_S, UEXP(EXP_MS),
    RSIZE(16), RCOUNT(2), OUTPUT(VAR),                                           /* attack, fade time */
    UEXP(0x00), UNIT_NONE,
  END,

  /* ---- 0x13 Set Condition (output) -------------------------------------- */
  U(0x5F), COLL(0x02),
    RID(FFB_RID_SET_CONDITION),
    BLOCK_INDEX(OUTPUT),
    U(0x23), LMIN1(0), LMAX1(1), RSIZE(4), RCOUNT(1), OUTPUT(VAR),               /* parameter block offset */
    RSIZE(4), RCOUNT(1), OUTPUT(PAD),
    U(0x60), U(0x61), U(0x62), LMIN2(-10000), LMAX2(10000), RSIZE(16), RCOUNT(3), OUTPUT(VAR),
    U(0x63), U(0x64), U(0x65), LMIN1(0), LMAX2(10000), RSIZE(16), RCOUNT(3), OUTPUT(VAR),
  END,

  /* ---- 0x14 Set Periodic (output) --------------------------------------- */
  U(0x6E), COLL(0x02),
    RID(FFB_RID_SET_PERIODIC),
    BLOCK_INDEX(OUTPUT),
    U(0x70), LMIN1(0), LMAX2(10000), RSIZE(16), RCOUNT(1), OUTPUT(VAR),          /* magnitude */
    U(0x6F), LMIN2(-10000), LMAX2(10000), RSIZE(16), RCOUNT(1), OUTPUT(VAR),     /* offset */
    U(0x71), LMIN1(0), LMAX4(FFB_DIR_MAX), UNIT_DEG, UEXP(EXP_CENTI),
    RSIZE(16), RCOUNT(1), OUTPUT(VAR),                                           /* phase */
    U(0x72), LMIN1(0), LMAX2(32767), UNIT_S, UEXP(EXP_MS),
    RSIZE(16), RCOUNT(1), OUTPUT(VAR),                                           /* period */
    UEXP(0x00), UNIT_NONE,
  END,

  /* ---- 0x15 Set Constant Force (output) --------------------------------- */
  U(0x73), COLL(0x02),
    RID(FFB_RID_SET_CONSTANT),
    BLOCK_INDEX(OUTPUT),
    U(0x70), LMIN2(-10000), LMAX2(10000), RSIZE(16), RCOUNT(1), OUTPUT(VAR),
  END,

  /* ---- 0x16 Set Ramp Force (output) ------------------------------------- */
  U(0x74), COLL(0x02),
    RID(FFB_RID_SET_RAMP),
    BLOCK_INDEX(OUTPUT),
    U(0x75), U(0x76), LMIN2(-10000), LMAX2(10000), RSIZE(16), RCOUNT(2), OUTPUT(VAR),
  END,

  /* ---- 0x17 Effect Operation (output) ----------------------------------- */
  U(0x77), COLL(0x02),
    RID(FFB_RID_EFFECT_OP),
    BLOCK_INDEX(OUTPUT),
    U(0x78), COLL(0x02),
      U(0x79), U(0x7A), U(0x7B),                    /* start, start solo, stop */
      LMIN1(1), LMAX1(3), RSIZE(8), RCOUNT(1), OUTPUT(ARR),
    END,
    U(0x7C), LMIN1(0), LMAX2(255), RSIZE(8), RCOUNT(1), OUTPUT(VAR),             /* loop count */
  END,

  /* ---- 0x18 PID Block Free (output) ------------------------------------- */
  U(0x90), COLL(0x02),
    RID(FFB_RID_BLOCK_FREE),
    BLOCK_INDEX(OUTPUT),
  END,

  /* ---- 0x19 PID Device Control (output) --------------------------------- */
  U(0x95), COLL(0x02),
    RID(FFB_RID_DEVICE_CONTROL),
    U(0x96), COLL(0x02),
      U(0x97), U(0x98), U(0x99), U(0x9A), U(0x9B), U(0x9C),
      LMIN1(1), LMAX1(6), RSIZE(8), RCOUNT(1), OUTPUT(ARR),
    END,
  END,

  /* ---- 0x1A Device Gain (output) ---------------------------------------- */
  U(0x7D), COLL(0x02),
    RID(FFB_RID_DEVICE_GAIN),
    U(0x7E), LMIN1(0), LMAX2(255), RSIZE(8), RCOUNT(1), OUTPUT(VAR),
  END,

  /* ---- 0x21 Create New Effect (feature) --------------------------------- */
  U(0xAB), COLL(0x02),
    RID(FFB_RID_CREATE_EFFECT),
    U(0x25), COLL(0x02),
      EFFECT_TYPES,
      LMIN1(1), LMAX1(FFB_ET_COUNT), RSIZE(8), RCOUNT(1), FEATURE(ARR),
    END,
  END,

  /* ---- 0x22 PID Block Load (feature) ------------------------------------ */
  U(0x89), COLL(0x02),
    RID(FFB_RID_BLOCK_LOAD),
    BLOCK_INDEX(FEATURE),
    U(0x8B), COLL(0x02),
      U(0x8C), U(0x8D), U(0x8E),                    /* success, full, error */
      LMIN1(1), LMAX1(3), RSIZE(8), RCOUNT(1), FEATURE(ARR),
    END,
    U(0xAC), LMIN1(0), LMAX4(65535UL), RSIZE(16), RCOUNT(1), FEATURE(VAR),       /* RAM pool available */
  END,

  /* ---- 0x23 PID Pool (feature) ------------------------------------------ */
  U(0x7F), COLL(0x02),
    RID(FFB_RID_POOL),
    U(0x80), LMIN1(0), LMAX4(65535UL), RSIZE(16), RCOUNT(1), FEATURE(VAR),       /* RAM pool size */
    U(0x83), LMIN1(0), LMAX2(255), RSIZE(8), RCOUNT(1), FEATURE(VAR),            /* simultaneous max */
    U(0xA9), U(0xAA), LMIN1(0), LMAX1(1), RSIZE(1), RCOUNT(2), FEATURE(VAR),     /* device managed,
                                                                                    shared blocks */
    RSIZE(6), RCOUNT(1), FEATURE(PAD),
  END,

  /* ---- 0x40 vendor: FFB safety status (feature, tools/hid_probe.py) ----- */
  UP2(0xFF00), U(0x01), COLL(0x02),
    RID(FFB_RID_STATUS),
    U(0x02), LMIN1(0), LMAX2(255), RSIZE(8), RCOUNT(FFB_LEN_STATUS - 1U), FEATURE(VAR),
  END,

  END                                               /* Application */
};

const uint16_t ffb_hid_report_desc_size = (uint16_t)sizeof(ffb_hid_report_desc);
