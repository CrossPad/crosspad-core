// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file SysExProtocol.h
 * @brief CrossPad SysEx protocol constants — single source of truth
 *
 * C-compatible header. Used by ESP-IDF stm32_api component, Arduino,
 * and any other platform that speaks the CrossPad UART protocol.
 */
#pragma once

/* ── SysEx framing ─────────────────────────────────────────────── */
#define CROSSPAD_SYSEX_START        0xF0
#define CROSSPAD_SYSEX_END          0xF7
#define CROSSPAD_SYSEX_MANUFACTURER 0x7D

/* ── Command IDs (byte 2 inside the SysEx frame) ──────────────── */
#define CROSSPAD_CMD_ENCODER_COUNTER    0x10
#define CROSSPAD_CMD_BUTTONS_DIGITAL    0x11
#define CROSSPAD_CMD_BUTTONS_ANALOG     0x12
#define CROSSPAD_CMD_STATUS_REGISTER    0x13
#define CROSSPAD_CMD_ROUTING_MATRIX     0x14
#define CROSSPAD_CMD_POWEROFF           0x15
#define CROSSPAD_CMD_BRIGHTNESS         0x16
#define CROSSPAD_CMD_MIDIOUT            0x17
#define CROSSPAD_CMD_STRING             0x18
#define CROSSPAD_CMD_DFU_REQUEST        0x19
#define CROSSPAD_CMD_KEEP_ALIVE         0x1A
#define CROSSPAD_CMD_USB_MODE           0x1B
#define CROSSPAD_CMD_VERSION_REPLY      0x1C  /* STM->ESP: [pcb_rev, api_ver] */
#define CROSSPAD_CMD_AUDIO_ROUTE        0x1D  /* host->ESP: [sub, args...] audio routing control; ESP replies with same cmd on query */
#define CROSSPAD_CMD_DAW                0x1E  /* host<->ESP: [sub, args...] DAW controller protocol (LED/text/beat/transport); catalog: crosspad-hil knowledge/sysex.yaml `daw` */
#define CROSSPAD_CMD_COMPANION          0x1F  /* host<->ESP: 0x01 <ASCII verb> request; reply chunks 0x02 (more follow) / 0x03 (last); catalog: crosspad-hil knowledge/sysex.yaml `companion` */

#define CROSSPAD_CMD_ADC_VALUES         0x20
#define CROSSPAD_CMD_ADC_BUTTONS        0x21
#define CROSSPAD_CMD_PAD_TOUCH          0x22  /* STM->ESP: [pad, touching] a press window opened (1) or closed without a hit (0); the note-on with its velocity follows a touch that became a hit */

#define CROSSPAD_CMD_VOLTAGES_MV        0x35
#define CROSSPAD_CMD_CHG_STATUS         0x36
#define CROSSPAD_CMD_CHG_FAULT          0x37
