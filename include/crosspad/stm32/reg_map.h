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

/* Wire protocol version (REG_IDENT_PROTO_HI/LO). */
#define STM32_PROTO_VERSION         0x0001

/* Block bases. */
#define REG_IDENTITY_BASE           0x0000
#define REG_INPUTS_BASE             0x0100
#define REG_POWER_BASE              0x0200
#define REG_CONTROL_BASE            0x0300
#define REG_COMMAND_BASE            0x0400
#define REG_LED_BASE                0x0500
#define REG_MIDI_PIPE_BASE          0x0600
#define REG_MAPPING_BASE            0x0700
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

/* Identity magic literals — for handshake validation. */
#define STM32_IDENT_MAGIC_C         0x43
#define STM32_IDENT_MAGIC_P         0x50
#define STM32_IDENT_MAGIC_A         0x41
#define STM32_IDENT_MAGIC_D         0x44

#ifdef __cplusplus
}
#endif
