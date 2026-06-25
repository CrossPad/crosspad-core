/*
 * STM32 I2C slave register map — portable mirror of
 * CrossPad_STM32_r20/app/inc/reg_map.h.
 *
 * Wire format: 16-bit register address, big-endian (MSB first).
 * Master writes <RegHi><RegLo>[Data...] to set the pointer and optionally
 * write; reads stream from the current pointer.
 *
 * Shared between crosspad-core (portable command builders) and platform
 * adapters (ESP-IDF stm32_hal, Arduino Wire impl).
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 7-bit slave address. */
#define STM32_I2C_ADDR_7B           0x42

/* Wire protocol version (REG_IDENT_PROTO_HI/LO).
 * NOTE: this copy is a thin/older mirror — it tracks the macro names used by
 * crosspad-core command builders and omits registers added in v0x0002-v0x0004
 * that are only consumed by platform-idf / ESP app code. Bumped to 0x0005 to
 * match the STM firmware; full parity lives in stm32_hal/include/stm32_reg_map.h. */
#define STM32_PROTO_VERSION         0x0005

/* Block bases. */
#define REG_IDENTITY_BASE           0x0000
#define REG_INPUTS_BASE             0x0100
#define REG_POWER_BASE              0x0200
#define REG_CONTROL_BASE            0x0300
#define REG_COMMAND_BASE            0x0400
#define REG_LED_BASE                0x0500
#define REG_MIDI_PIPE_BASE          0x0600
#define REG_MAPPING_BASE            0x0700
#define REG_PAD_CFG_BASE            0x0800
#define REG_DEBUG_BASE              0x1000

/* Identity (RO). */
#define REG_IDENT_MAGIC0            (REG_IDENTITY_BASE + 0x00) /* 'C' */
#define REG_IDENT_MAGIC1            (REG_IDENTITY_BASE + 0x01) /* 'P' */
#define REG_IDENT_MAGIC2            (REG_IDENTITY_BASE + 0x02) /* 'A' */
#define REG_IDENT_MAGIC3            (REG_IDENTITY_BASE + 0x03) /* 'D' */
#define REG_IDENT_PROTO_HI          (REG_IDENTITY_BASE + 0x04)
#define REG_IDENT_PROTO_LO          (REG_IDENTITY_BASE + 0x05)
#define REG_IDENT_FW_MAJOR          (REG_IDENTITY_BASE + 0x06)
#define REG_IDENT_FW_MINOR          (REG_IDENTITY_BASE + 0x07)
#define REG_IDENT_PCB_REV           (REG_IDENTITY_BASE + 0x08)
#define REG_IDENT_LEN               0x09

/* Inputs (RO snapshot). */
#define REG_IN_ENCODER              (REG_INPUTS_BASE  + 0x00)  /* low 4 bits = detent index 0..15, bit7 = enc_btn */
#define REG_IN_PADS_HI              (REG_INPUTS_BASE  + 0x01)  /* bits 8..15 of 16-pad bitmap */
#define REG_IN_PADS_LO              (REG_INPUTS_BASE  + 0x02)  /* bits 0..7  of 16-pad bitmap */
#define REG_IN_PAD_SUM_BASE         (REG_INPUTS_BASE  + 0x03)  /* +0..+15 — per-pad pressure 0..127 */
#define REG_IN_PAD_DIFF_BASE        (REG_INPUTS_BASE  + 0x13)  /* +0..+15 — per-pad pan int8_t */
#define REG_IN_JACK_DET             (REG_INPUTS_BASE  + 0x23)
#define REG_IN_COL_PRES_BASE        (REG_INPUTS_BASE  + 0x24)  /* +0..+7 — debug column-min */
#define REG_IN_ENC_PHASE            (REG_INPUTS_BASE  + 0x2E)  /* sub-detent: [1:0]=CNT mod4, [2]=A, [3]=B, [4]=settled gate */
#define REG_INPUTS_LEN              0x2F

/* Power (RO snapshot). */
#define REG_PWR_VBAT_HI             (REG_POWER_BASE   + 0x00)
#define REG_PWR_VBAT_LO             (REG_POWER_BASE   + 0x01)
#define REG_PWR_VBUS_STM_HI         (REG_POWER_BASE   + 0x02)
#define REG_PWR_VBUS_STM_LO         (REG_POWER_BASE   + 0x03)
#define REG_PWR_VBUS_ESP_HI         (REG_POWER_BASE   + 0x04)
#define REG_PWR_VBUS_ESP_LO         (REG_POWER_BASE   + 0x05)
#define REG_PWR_TEMP_C              (REG_POWER_BASE   + 0x06)
#define REG_PWR_CHG_STATUS          (REG_POWER_BASE   + 0x07)
#define REG_PWR_CHG_FAULT           (REG_POWER_BASE   + 0x08)
#define REG_PWR_HBT_MS_3            (REG_POWER_BASE   + 0x09)  /* big-endian uint32 */
#define REG_PWR_HBT_MS_2            (REG_POWER_BASE   + 0x0A)
#define REG_PWR_HBT_MS_1            (REG_POWER_BASE   + 0x0B)
#define REG_PWR_HBT_MS_0            (REG_POWER_BASE   + 0x0C)
#define REG_POWER_LEN               0x0D

/* Control (RW). */
#define REG_CTL_AUDIO_EN            (REG_CONTROL_BASE + 0x00)
#define REG_CTL_AUTO_SLEEP          (REG_CONTROL_BASE + 0x01)
#define REG_CTL_BRIGHTNESS          (REG_CONTROL_BASE + 0x02)
#define REG_CTL_MIDI_CHAN           (REG_CONTROL_BASE + 0x03)
#define REG_CTL_NOTE_OFFSET         (REG_CONTROL_BASE + 0x04)
#define REG_CTL_KEYPAD_DIS          (REG_CONTROL_BASE + 0x05)
#define REG_CTL_ROUTING_0           (REG_CONTROL_BASE + 0x06)
#define REG_CTL_ROUTING_1           (REG_CONTROL_BASE + 0x07)
#define REG_CTL_USB_GATE_S          (REG_CONTROL_BASE + 0x08)
#define REG_CTL_USB_GATE_E          (REG_CONTROL_BASE + 0x09)
#define REG_CTL_EN_3V3_HOLD         (REG_CONTROL_BASE + 0x0A)

/* Analog pads — velocity curve defaults + continuous-stream config (RW).
 * Added in proto 0x0005. */
#define REG_CTL_VEL_CURVE_DEF       (REG_CONTROL_BASE + 0x3E)  /* global default curve */
#define REG_CTL_VEL_FLOOR_DEF       (REG_CONTROL_BASE + 0x3F)  /* global default floor 0..127 */
#define REG_CTL_CC_PRESS_BASE       (REG_CONTROL_BASE + 0x40)  /* pressure→CC base (CC = base+pad) */
#define REG_CTL_PAN_CC_EN           (REG_CONTROL_BASE + 0x41)  /* 0/1 enable pan→CC streaming */
#define REG_CTL_CC_PAN_BASE         (REG_CONTROL_BASE + 0x42)  /* pan→CC base (CC = base+pad) */
#define REG_CTL_ANALOG_MIN_MS       (REG_CONTROL_BASE + 0x43)  /* continuous-stream rate cap ms (~16=60Hz) */
#define REG_CTL_ANALOG_DELTA        (REG_CONTROL_BASE + 0x44)  /* min pressure/pan change to send */
#define REG_CTL_CC_CONFLICT         (REG_CONTROL_BASE + 0x45)  /* RO bitmask (Task 4) */

/* Command (WO trigger — write 1 to fire). */
#define REG_CMD_DFU                 (REG_COMMAND_BASE + 0x00)
#define REG_CMD_POWEROFF            (REG_COMMAND_BASE + 0x01)
#define REG_CMD_KEEPALIVE           (REG_COMMAND_BASE + 0x02)
#define REG_CMD_RESET_ESP           (REG_COMMAND_BASE + 0x03)
#define REG_CMD_SOFT_RESET          (REG_COMMAND_BASE + 0x04)
#define REG_CMD_FIND_PADS           (REG_COMMAND_BASE + 0x05)

/* LED override (RW). */
#define REG_LED1_MODE               (REG_LED_BASE     + 0x00)
#define REG_LED1_BRIGHT             (REG_LED_BASE     + 0x01)
#define REG_LED2_MODE               (REG_LED_BASE     + 0x02)
#define REG_LED2_BRIGHT             (REG_LED_BASE     + 0x03)

/* Pad mapping (RW). 16 pads × 5 bytes (mux_a, mux_b, ch_a, ch_b, swap). */
#define REG_MAPPING_STRIDE          5U
#define REG_MAPPING_LEN             (16U * REG_MAPPING_STRIDE)

/* Per-pad config (RW). 16 pads × 6 bytes per stride. HI stages sensitivity,
 * LO commits. Single-byte fields commit on write. Added proto 0x0004; stride
 * 4→6 in proto 0x0005 (vel_curve, vel_floor). */
#define REG_PAD_CFG_STRIDE          6U
#define REG_PAD_CFG_LEN             (16U * REG_PAD_CFG_STRIDE)
#define REG_PAD_CFG_FIELD_SENS_HI   0
#define REG_PAD_CFG_FIELD_SENS_LO   1
#define REG_PAD_CFG_FIELD_LR_RATIO  2
#define REG_PAD_CFG_FIELD_ANALOG    3
#define REG_PAD_CFG_FIELD_VEL_CURVE 4   /* enum vel_curve, 0xFF = inherit global */
#define REG_PAD_CFG_FIELD_VEL_FLOOR 5   /* 0..127, 0xFF = inherit global */
/* vel_curve enum values (REG_PAD_CFG_FIELD_VEL_CURVE / REG_CTL_VEL_CURVE_DEF). */
#define VEL_LINEAR                  0
#define VEL_EXP_SOFT                1
#define VEL_EXP_HARD                2
#define VEL_CFG_INHERIT             0xFFU  /* per-pad: inherit global default */

/* Identity magic literals — for handshake validation. */
#define STM32_IDENT_MAGIC_C         0x43
#define STM32_IDENT_MAGIC_P         0x50
#define STM32_IDENT_MAGIC_A         0x41
#define STM32_IDENT_MAGIC_D         0x44

#ifdef __cplusplus
}
#endif
