/*
 * STM32 I2C1 slave register map — the single mirror of
 * CrossPad_STM32_r20/app/inc/reg_map.h. Keep in sync with the firmware side.
 *
 * Wire format: 16-bit register address, big-endian (MSB first). Master writes
 * <RegHi><RegLo>[Data...] to set the pointer + optionally write; master reads
 * stream from current pointer.
 *
 * Pure macros, no platform code — shared by crosspad-core (command builders,
 * config sync), crosspad-gui, the ESP-IDF stm32_hal component (which includes
 * this file through stm32_reg_map.h) and the Arduino Wire implementation.
 *
 * Last sync: 2026-10-04 (0x0010 adds REG_IDENT_SERIAL / REG_IDENT_OTP_BLOCK, the PCB serial from OTP).
 * Previous: 2026-08-30 (0x000F adds REG_IDENT_FW_PCB, REG_IDENT_PCB_REV becomes the OTP board rev).
 * Previous: 2026-08-29 (0x000E adds REG_CMD_FW_UPDATE / FW_UPDATE_MAGIC —
 * the ESP-driven STM reflash). Previous: 2026-08-25 (mirror of STM 0x000B — REG_IDENT_BOOT_COUNT,
 * REG_IN_PAD_VELRAW_BASE/REG_IN_PAD_AB_BASE, LED1/LED2 FREQ_HI/LO,
 * REG_CTL_BL_FREQ_HI/LO, REG_CTL_BATT_CAP_MAH_HI/LO, REG_ESP_RST_* block,
 * REG_EXP_* I2C-expander block).
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 7-bit slave address. Mirrors CrossPad_STM32_r20 defines.h I2C1_SLAVE_ADDR_7B. */
#define STM32_I2C_ADDR_7B           0x42

/* Wire protocol version (REG_IDENT_PROTO_HI/LO). Bumped 2026-05-13 from 0x0003
 * to 0x0004 — adds REG_CTL_BTN_* (threshold/hyst/debounce) + REG_PAD_CFG_BASE
 * (per-pad sensitivity/lr_ratio/analog_mode).
 * Bumped 2026-06-25 from 0x0004 to 0x0005 (velocity curve/floor per-pad +
 * analog stream config: cc_press_base, pan_cc_en, cc_pan_base, analog_min_ms,
 * analog_delta; PAD_CFG stride 4→6).
 * Bumped 2026-08-12 from 0x0005 to 0x000A (mirror of STM 0x000A) — adds
 * REG_IDENT_BOOT_COUNT, REG_IN_PAD_VELRAW_BASE/REG_IN_PAD_AB_BASE, LED1/LED2
 * FREQ_HI/LO, REG_CTL_BL_FREQ_HI/LO, REG_CTL_BATT_CAP_MAH_HI/LO, the
 * REG_ESP_RST_* reset-reason counter block, and the REG_EXP_* I2C-expander
 * block. Bumped 2026-08-25 to 0x000B for power-button wake-hold time and
 * active/qualified flags. Bumped 2026-08-28 to 0x000C for the link-watchdog
 * reset telemetry (REG_ESP_RST_WD_*). Keep in sync with CrossPad_STM32_r20
 * app/inc/reg_map.h. */
#define STM32_PROTO_VERSION         0x0010

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
#define REG_ESP_RST_BASE            0x0900
#define REG_DEBUG_BASE              0x1000

/* ESP reset-reason counters (RO snapshot). One big-endian uint16 per reason
 * type (esp_rst_stats_t field order), then total + last raw rst:0x code.
 * Counter index i lives at REG_ESP_RST_BASE + 2*i (HI, LO). */
#define REG_ESP_RST_COUNT_N         20U                                        /* named counters incl. `other` */
#define REG_ESP_RST_TOTAL_HI        (REG_ESP_RST_BASE + 2U * REG_ESP_RST_COUNT_N)       /* 0x0928 */
#define REG_ESP_RST_TOTAL_LO        (REG_ESP_RST_BASE + 2U * REG_ESP_RST_COUNT_N + 1U)  /* 0x0929 */
#define REG_ESP_RST_LAST_CODE       (REG_ESP_RST_BASE + 2U * REG_ESP_RST_COUNT_N + 2U)  /* 0x092A */
/* Link-watchdog telemetry (STM esp_sync): ESP resets the STM itself forced
 * after heartbeat silence, cumulative since STM boot, and whether it gave up. */
#define REG_ESP_RST_WD_TOTAL_HI     (REG_ESP_RST_BASE + 2U * REG_ESP_RST_COUNT_N + 3U)  /* 0x092B */
#define REG_ESP_RST_WD_TOTAL_LO     (REG_ESP_RST_BASE + 2U * REG_ESP_RST_COUNT_N + 4U)  /* 0x092C */
#define REG_ESP_RST_WD_DEAD         (REG_ESP_RST_BASE + 2U * REG_ESP_RST_COUNT_N + 5U)  /* 0x092D */
#define REG_ESP_RST_LEN             (2U * REG_ESP_RST_COUNT_N + 6U)                     /* 0x2E */

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
#define REG_IDENT_BOOT_COUNT        (REG_IDENTITY_BASE + 0x09) /* RO: low 8 bits of TAMP boot counter; ESP detects STM reset via change */
#define REG_IDENT_FW_PCB            (REG_IDENTITY_BASE + 0x0A) /* PCB the running firmware was built for (0xFF on pre-0x000F STM) */
#define REG_IDENT_SERIAL_B3         (REG_IDENTITY_BASE + 0x0B) /* PCB serial NNNN of CPNNNN, u32 BE; 0xFFFFFFFF = not provisioned (and on pre-0x0010 STM) */
#define REG_IDENT_SERIAL_B2         (REG_IDENTITY_BASE + 0x0C)
#define REG_IDENT_SERIAL_B1         (REG_IDENTITY_BASE + 0x0D)
#define REG_IDENT_SERIAL_B0         (REG_IDENTITY_BASE + 0x0E)
#define REG_IDENT_OTP_BLOCK         (REG_IDENTITY_BASE + 0x0F) /* OTP block in use, 1..15; 0 = none */
#define REG_IDENT_LEN               0x10

/* Inputs (RO snapshot). */
#define REG_IN_ENCODER              (REG_INPUTS_BASE  + 0x00)  /* low 4 bits = detent index 0..15, bit7 = enc_btn */
#define REG_IN_PADS_HI              (REG_INPUTS_BASE  + 0x01)  /* bits 8..15 of 16-pad bitmap */
#define REG_IN_PADS_LO              (REG_INPUTS_BASE  + 0x02)  /* bits 0..7  of 16-pad bitmap */
#define REG_IN_PAD_SUM_BASE         (REG_INPUTS_BASE  + 0x03)  /* +0..+15 — per-pad pressure 0..127 */
#define REG_IN_PAD_DIFF_BASE        (REG_INPUTS_BASE  + 0x13)  /* +0..+15 — per-pad pan int8_t */
#define REG_IN_JACK_DET             (REG_INPUTS_BASE  + 0x23)
#define REG_IN_COL_PRES_BASE        (REG_INPUTS_BASE  + 0x24)  /* +0..+7 — debug column-min */
#define REG_IN_BUTTONS              (REG_INPUTS_BASE  + 0x2C)  /* bit0=power_btn (BTN1), bit1=enc_btn */
#define REG_IN_PWR_STATE            (REG_INPUTS_BASE  + 0x2D)  /* PWR_STATE_* — power-transition notification */
#define REG_IN_ENC_PHASE            (REG_INPUTS_BASE  + 0x2E)  /* sub-detent: [1:0]=CNT mod4, [2]=A, [3]=B, [4]=settled gate */
#define REG_IN_PAD_VELRAW_BASE      (REG_INPUTS_BASE  + 0x2F)  /* +0..+15 — last note-on pre-curve linear 0..127 (editor feedback) */
#define REG_IN_PAD_AB_BASE          (REG_INPUTS_BASE  + 0x3F)  /* +0..+47 — per-pad raw A/B 12-bit, 3 bytes/pad (b0=A>>4, b1=(A&F)<<4|B>>8, b2=B&FF) */
#define REG_IN_PWR_HOLD_HI          (REG_INPUTS_BASE  + 0x6F)  /* real BTN1 wake hold time, BE ms */
#define REG_IN_PWR_HOLD_LO          (REG_INPUTS_BASE  + 0x70)
#define REG_IN_PWR_WAKE_FLAGS       (REG_INPUTS_BASE  + 0x71)  /* bit0 active, bit1 qualified */
#define PWR_WAKE_FLAG_ACTIVE        0x01
#define PWR_WAKE_FLAG_QUALIFIED     0x02
#define REG_INPUTS_LEN              0x72

/* REG_IN_PWR_STATE values. The STM sets one and announces it over UART SysEx
 * (F0 7D 50 <state> F7); the ESP writes PWR_ACK_MAGIC to REG_CMD_PWR_ACK when
 * ready, and only then does the STM continue its teardown (3 s timeout). */
#define PWR_STATE_AWAKE             0x00
#define PWR_STATE_SLEEP_PENDING     0x01   /* STOP1 entry */
#define PWR_STATE_SHIP_PENDING      0x02   /* BATFET off (hard power down) */
#define PWR_STATE_WOKE              0x03   /* woke from STOP1, backlight off — the ESP redraws, then sets brightness */
#define PWR_STATE_LIGHT             0x04   /* LIGHT sleep (backlight off, slow scan; ESP zasilony — ściemnij ARGB/display) */
#define PWR_STATE_CHARGE            0x05   /* charger-idle while off (ESP minimally repowered for the charge UI) */
#define PWR_ACK_MAGIC               0x55

/* Power (RO snapshot). Mix of MCU ADC measurements (VBAT, VBUS_STM/ESP, temp)
 * and BQ25895 internal ADC readings (CHG_*). BQ ADC is more accurate but only
 * available while charger is online; MCU ADC is the fallback. */
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
/* BQ25895 telemetry (RO, BE uint16 where 16-bit). Added in proto 0x0003. */
#define REG_PWR_CHG_VBUS_HI         (REG_POWER_BASE   + 0x0D)
#define REG_PWR_CHG_VBUS_LO         (REG_POWER_BASE   + 0x0E)
#define REG_PWR_CHG_VSYS_HI         (REG_POWER_BASE   + 0x0F)
#define REG_PWR_CHG_VSYS_LO         (REG_POWER_BASE   + 0x10)
#define REG_PWR_CHG_VBAT_HI         (REG_POWER_BASE   + 0x11)
#define REG_PWR_CHG_VBAT_LO         (REG_POWER_BASE   + 0x12)
#define REG_PWR_CHG_ICHG_HI         (REG_POWER_BASE   + 0x13)
#define REG_PWR_CHG_ICHG_LO         (REG_POWER_BASE   + 0x14)
#define REG_PWR_CHG_TS_PCT          (REG_POWER_BASE   + 0x15)  /* 21..80 */
#define REG_PWR_CHG_VBUS_ST         (REG_POWER_BASE   + 0x16)  /* enum 0..7 (REG0B >> 5) */
#define REG_PWR_CHG_CHRG_ST         (REG_POWER_BASE   + 0x17)  /* enum 0..3 (REG0B >> 3) */
#define REG_PWR_CHG_IINLIM_HI       (REG_POWER_BASE   + 0x18)
#define REG_PWR_CHG_IINLIM_LO       (REG_POWER_BASE   + 0x19)
#define REG_PWR_CHG_IDPM_HI         (REG_POWER_BASE   + 0x1A)
#define REG_PWR_CHG_IDPM_LO         (REG_POWER_BASE   + 0x1B)
#define REG_PWR_CHG_VINDPM_HI       (REG_POWER_BASE   + 0x1C)
#define REG_PWR_CHG_VINDPM_LO       (REG_POWER_BASE   + 0x1D)
#define REG_PWR_CHG_NTC             (REG_POWER_BASE   + 0x1E)  /* live NTC bits */
#define REG_PWR_CHG_FLAGS           (REG_POWER_BASE   + 0x1F)  /* bit0 PG, 1 VSYS_MIN, 2 THERM, 3 VDPM, 4 IDPM */
#define REG_PWR_CHG_SRC_ACT         (REG_POWER_BASE   + 0x20)  /* enum charger_src_active */
#define REG_PWR_CHG_TGT_LIM_HI      (REG_POWER_BASE   + 0x21)  /* current soft-ICO target mA */
#define REG_PWR_CHG_TGT_LIM_LO      (REG_POWER_BASE   + 0x22)

/* SoC estimator (STM EKF, firmware reg ver >= 0x000A). */
#define REG_PWR_SOC_PCT             (REG_POWER_BASE   + 0x23)  /* 0..100 */
#define REG_PWR_SOC_PMIL_HI         (REG_POWER_BASE   + 0x24)  /* SoC x1000 (0.1%), BE */
#define REG_PWR_SOC_PMIL_LO         (REG_POWER_BASE   + 0x25)
#define REG_PWR_SOC_FLAGS           (REG_POWER_BASE   + 0x26)  /* b0 valid b1 conv b2-3 src(0 rest,1 chg,2 dischg,3 full) b4 cold */
#define REG_PWR_R0_MOHM_HI          (REG_POWER_BASE   + 0x27)  /* learned R0 mOhm, BE */
#define REG_PWR_R0_MOHM_LO          (REG_POWER_BASE   + 0x28)
#define REG_PWR_IBAT_HI             (REG_POWER_BASE   + 0x29)  /* signed mA, + = discharge, BE */
#define REG_PWR_IBAT_LO             (REG_POWER_BASE   + 0x2A)
#define REG_POWER_LEN               0x2B

/* SOC_FLAGS bitmask helpers. */
#define REG_SOC_FLAG_VALID          0x01
#define REG_SOC_FLAG_CONVERGED      0x02
#define REG_SOC_SRC_MASK            0x0C   /* >>2: 0 rest, 1 chg, 2 dischg, 3 full */
#define REG_SOC_FLAG_COLD           0x10
#define REG_SOC_FLAG_NO_BATTERY     0x20   /* charger's no-battery latch; VALID is clear then */
#define REG_SOC_FLAG_LOW_BATTERY    0x40   /* on its own power, VBAT below 3.3 V */

/* CHG_FLAGS bitmask helpers. */
#define REG_CHG_FLAG_PG             0x01
#define REG_CHG_FLAG_VSYS_MIN       0x02
#define REG_CHG_FLAG_THERM          0x04
#define REG_CHG_FLAG_VDPM           0x08
#define REG_CHG_FLAG_IDPM           0x10

/* CHG_STATUS bitmask (REG_PWR_CHG_STATUS). */
#define REG_CHG_STATUS_CHG_ACTIVE   0x01
#define REG_CHG_STATUS_CHG_DONE     0x02
#define REG_CHG_STATUS_BATT_LOW     0x04
#define REG_CHG_STATUS_VBUS_PRESENT 0x08
#define REG_CHG_STATUS_BAT_FAULT    0x10
#define REG_CHG_STATUS_BOOST_FAULT  0x20
#define REG_CHG_STATUS_WDT_FAULT    0x40
#define REG_CHG_STATUS_THERM_REG    0x80

/* charger_src_mode enum (REG_CTL_CHG_SRC_MODE). */
#define CHG_SRC_AUTO                0
#define CHG_SRC_ESP_ONLY            1
#define CHG_SRC_STM_ONLY            2
#define CHG_SRC_DUAL                3
#define CHG_SRC_MANUAL              4

/* charger_src_active enum (REG_PWR_CHG_SRC_ACT). */
#define CHG_ACT_NONE                0
#define CHG_ACT_ESP                 1
#define CHG_ACT_STM                 2
#define CHG_ACT_BOTH                3

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
/* Sleep / power management timeouts (RW, BE u16 for 16-bit, 0 = disabled). */
#define REG_CTL_DIM_TO_S_HI         (REG_CONTROL_BASE + 0x0B)  /* dim backlight after N s */
#define REG_CTL_DIM_TO_S_LO         (REG_CONTROL_BASE + 0x0C)
#define REG_CTL_SLEEP_TO_S_HI       (REG_CONTROL_BASE + 0x0D)  /* STOP1 entry after N s */
#define REG_CTL_SLEEP_TO_S_LO       (REG_CONTROL_BASE + 0x0E)
#define REG_CTL_LONG_PRESS_HI       (REG_CONTROL_BASE + 0x0F)  /* power btn long-press ms */
#define REG_CTL_LONG_PRESS_LO       (REG_CONTROL_BASE + 0x10)
#define REG_CTL_USB_SUSP_S          (REG_CONTROL_BASE + 0x11)  /* USB suspend grace s */
#define REG_CTL_WAKE_BRIGHT         (REG_CONTROL_BASE + 0x12)  /* brightness after wake, 0..100 */
/* Encoder → MIDI CC (RW, opt-in). Default = OFF (ENC drives LVGL GUI only).
 * Mode 0 off, 1 absolute, 2 relative (64±). The master writes mode != 0 to enable. */
#define REG_CTL_ENC_CC_MODE         (REG_CONTROL_BASE + 0x13)
#define REG_CTL_ENC_CC_NUM          (REG_CONTROL_BASE + 0x14)  /* CC# 0..127 */
#define REG_CTL_ENC_CC_CHAN         (REG_CONTROL_BASE + 0x15)  /* 0 = inherit REG_CTL_MIDI_CHAN */
#define REG_CTL_ENC_CC_STEP         (REG_CONTROL_BASE + 0x16)  /* delta per detent (absolute mode) */
#define REG_CTL_ENC_CC_VAL          (REG_CONTROL_BASE + 0x17)  /* absolute current value 0..127 */
/* Charger config knobs (RW, BE u16 for 16-bit); the LO write applies them.
 * Proto 0x0003. */
#define REG_CTL_CHG_ICHG_HI         (REG_CONTROL_BASE + 0x20)  /* fast charge mA */
#define REG_CTL_CHG_ICHG_LO         (REG_CONTROL_BASE + 0x21)
#define REG_CTL_CHG_VREG_HI         (REG_CONTROL_BASE + 0x22)  /* charge voltage mV */
#define REG_CTL_CHG_VREG_LO         (REG_CONTROL_BASE + 0x23)
#define REG_CTL_CHG_IINLIM_HI       (REG_CONTROL_BASE + 0x24)  /* input current limit mA */
#define REG_CTL_CHG_IINLIM_LO       (REG_CONTROL_BASE + 0x25)
#define REG_CTL_CHG_VINDPM_HI       (REG_CONTROL_BASE + 0x26)  /* abs VINDPM mV (0 = relative) */
#define REG_CTL_CHG_VINDPM_LO       (REG_CONTROL_BASE + 0x27)
#define REG_CTL_CHG_ITERM_HI        (REG_CONTROL_BASE + 0x28)  /* term current mA */
#define REG_CTL_CHG_ITERM_LO        (REG_CONTROL_BASE + 0x29)
#define REG_CTL_CHG_IPRE_HI         (REG_CONTROL_BASE + 0x2A)  /* precharge current mA */
#define REG_CTL_CHG_IPRE_LO         (REG_CONTROL_BASE + 0x2B)
#define REG_CTL_CHG_BOOST_HI        (REG_CONTROL_BASE + 0x2C)  /* OTG boost mV */
#define REG_CTL_CHG_BOOST_LO        (REG_CONTROL_BASE + 0x2D)
#define REG_CTL_CHG_SYSMIN_HI       (REG_CONTROL_BASE + 0x2E)  /* SYS_MIN mV */
#define REG_CTL_CHG_SYSMIN_LO       (REG_CONTROL_BASE + 0x2F)
#define REG_CTL_CHG_ENABLE          (REG_CONTROL_BASE + 0x30)  /* 1 = CHG_CONFIG bit */
#define REG_CTL_CHG_OTG             (REG_CONTROL_BASE + 0x31)  /* 1 = OTG_CONFIG bit */
#define REG_CTL_CHG_HIZ             (REG_CONTROL_BASE + 0x32)  /* 1 = EN_HIZ */
#define REG_CTL_CHG_TERMINATE       (REG_CONTROL_BASE + 0x33)  /* 1 = EN_TERM */
#define REG_CTL_CHG_STAT_LED        (REG_CONTROL_BASE + 0x34)  /* 1 = chip STAT LED on */
#define REG_CTL_CHG_SRC_MODE        (REG_CONTROL_BASE + 0x35)  /* enum charger_src_mode */
#define REG_CTL_CHG_AUTO_LIM_HI     (REG_CONTROL_BASE + 0x36)  /* soft-ICO ceiling mA */
#define REG_CTL_CHG_AUTO_LIM_LO     (REG_CONTROL_BASE + 0x37)
/* Buttons globals (RW). BE u16 for 16-bit, committed on the LO write.
 * Thresholds are raw 12-bit ADC counts (presum < thr-hyst → press,
 * > thr+hyst → release); the debounce gates the transition, so the ADC
 * average must stay on one side of the threshold for N ms. Proto 0x0004. */
#define REG_CTL_BTN_THR_HI          (REG_CONTROL_BASE + 0x38)
#define REG_CTL_BTN_THR_LO          (REG_CONTROL_BASE + 0x39)
#define REG_CTL_BTN_HYST_HI         (REG_CONTROL_BASE + 0x3A)
#define REG_CTL_BTN_HYST_LO         (REG_CONTROL_BASE + 0x3B)
#define REG_CTL_BTN_PRESS_DB_MS     (REG_CONTROL_BASE + 0x3C)
#define REG_CTL_BTN_RELEASE_DB_MS   (REG_CONTROL_BASE + 0x3D)

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
#define REG_CTL_BATT_CAP_MAH_HI     (REG_CONTROL_BASE + 0x46)  /* SoC nominal capacity mAh, BE */
#define REG_CTL_BATT_CAP_MAH_LO     (REG_CONTROL_BASE + 0x47)
/* Virtual pad stimulus (STM vpad.c): staged config, REG_CMD_VPAD runs it. */
#define REG_CTL_VPAD_RATE_HI        (REG_CONTROL_BASE + 0x48)  /* hits/s, BE */
#define REG_CTL_VPAD_RATE_LO        (REG_CONTROL_BASE + 0x49)
#define REG_CTL_VPAD_COUNT_HI       (REG_CONTROL_BASE + 0x4A)  /* hits per run, BE */
#define REG_CTL_VPAD_COUNT_LO       (REG_CONTROL_BASE + 0x4B)
#define REG_CTL_VPAD_HOLD_MS        (REG_CONTROL_BASE + 0x4C)
#define REG_CTL_VPAD_MODE           (REG_CONTROL_BASE + 0x4D)  /* bits1:0 pad rr/single/random, bits3:2 vel sweep/random/fixed */

/* Command (WO trigger — write any byte to fire). */
#define REG_CMD_DFU                 (REG_COMMAND_BASE + 0x00)
#define REG_CMD_POWEROFF            (REG_COMMAND_BASE + 0x01)
#define REG_CMD_KEEPALIVE           (REG_COMMAND_BASE + 0x02)
#define REG_CMD_RESET_ESP           (REG_COMMAND_BASE + 0x03)
#define REG_CMD_SOFT_RESET          (REG_COMMAND_BASE + 0x04)
#define REG_CMD_FIND_PADS           (REG_COMMAND_BASE + 0x05)
#define REG_CMD_CHG_RESET           (REG_COMMAND_BASE + 0x06)  /* full chip reset + re-apply defaults */
#define REG_CMD_CHG_FORCE_DPDM      (REG_COMMAND_BASE + 0x07)  /* re-run BC1.2 */
#define REG_CMD_CHG_FORCE_ICO       (REG_COMMAND_BASE + 0x08)
#define REG_CMD_CHG_SHIP            (REG_COMMAND_BASE + 0x09)  /* BATFET off — system loses power */
#define REG_CMD_CHG_PUMPX_UP        (REG_COMMAND_BASE + 0x0A)  /* HVDCP step up */
#define REG_CMD_CHG_PUMPX_DN        (REG_COMMAND_BASE + 0x0B)
#define REG_CMD_PWR_ACK             (REG_COMMAND_BASE + 0x0C)  /* ESP write PWR_ACK_MAGIC */
#define REG_CMD_USER_ACTIVITY       (REG_COMMAND_BASE + 0x0D)  /* ESP-side user input (touch/GUI) — resets STM idle timer */
#define REG_CMD_VPAD                (REG_COMMAND_BASE + 0x0E)  /* 1 = start staged vpad run, 0 = stop */
#define REG_CMD_FW_UPDATE           (REG_COMMAND_BASE + 0x0F)  /* write FW_UPDATE_MAGIC: STM enters its LPUART1 flasher (proto 0x000E) */
#define FW_UPDATE_MAGIC             0xA5
/* REG_DEBUG +0x58..0x78: vpad sent, remaining, short esp, short usb, esp DMA busy, usb busy, esp hwm, usb hwm, pressure sent */
#define REG_DEBUG_VPAD              (REG_DEBUG_BASE + 0x58)

/* SysEx vendor command bytes — wire: F0 7D <CMD> <ARG...> F7. */
#define CROSSPAD_SYSEX_PWR_IRQ      0x50   /* ARG = PWR_STATE_* — IRQ z STM */

/* LED override (RW). */
#define REG_LED1_MODE               (REG_LED_BASE     + 0x00)
#define REG_LED1_BRIGHT             (REG_LED_BASE     + 0x01)
#define REG_LED2_MODE               (REG_LED_BASE     + 0x02)
#define REG_LED2_BRIGHT             (REG_LED_BASE     + 0x03)
/* LED PWM frequency (Hz, big-endian, 100..100000). Alias of the timer group:
 * LED1=TIM3, LED2=TIM1 on r20 (see tim_freq.h). */
#define REG_LED1_FREQ_HI            (REG_LED_BASE     + 0x04)
#define REG_LED1_FREQ_LO            (REG_LED_BASE     + 0x05)
#define REG_LED2_FREQ_HI            (REG_LED_BASE     + 0x06)
#define REG_LED2_FREQ_LO            (REG_LED_BASE     + 0x07)
/* LCD backlight PWM frequency — TIM1 group on r20 (aliases REG_LED2_FREQ). */
#define REG_CTL_BL_FREQ_HI          (REG_CONTROL_BASE + 0x18)
#define REG_CTL_BL_FREQ_LO          (REG_CONTROL_BASE + 0x19)

/* Pad mapping (RW). 16 pads × 5 bytes per stride. Field offsets within stride: */
#define REG_MAPPING_STRIDE          5U
#define REG_MAPPING_LEN             (16U * REG_MAPPING_STRIDE)
#define REG_MAPPING_FIELD_MUX_A     0
#define REG_MAPPING_FIELD_MUX_B     1
#define REG_MAPPING_FIELD_CH_A      2
#define REG_MAPPING_FIELD_CH_B      3
#define REG_MAPPING_FIELD_SWAP      4

/* Per-pad config (RW). 16 pads × 6 bytes per stride. sensitivity = raw ADC
 * 1..4095 (1 = max sensitive, ADC_THRESHOLD = least). lr_ratio 1..99 (50 =
 * symmetric). analog_mode enum: 0 disable, 1 CC, 2 aftertouch, 3 both. HI
 * stages sensitivity, LO commits. Single-byte fields commit on write.
 * Added in proto 0x0004. Stride 4→6 in proto 0x0005 (vel_curve, vel_floor). */
#define REG_PAD_CFG_STRIDE          6U
#define REG_PAD_CFG_LEN             (16U * REG_PAD_CFG_STRIDE)
#define REG_PAD_CFG_FIELD_SENS_HI   0
#define REG_PAD_CFG_FIELD_SENS_LO   1
#define REG_PAD_CFG_FIELD_LR_RATIO  2
#define REG_PAD_CFG_FIELD_ANALOG    3
#define REG_PAD_CFG_FIELD_VEL_CURVE 4   /* enum vel_curve, 0xFF = inherit global */
#define REG_PAD_CFG_FIELD_VEL_FLOOR 5   /* 0..127, 0xFF = inherit global */
/* analog_mode enum values (REG_PAD_CFG_FIELD_ANALOG). */
#define ANALOG_MODE_DISABLE             0
#define ANALOG_MODE_CC                  1
#define ANALOG_MODE_AFTERTOUCH          2
#define ANALOG_MODE_CC_AND_AFTERTOUCH   3
/* vel_curve enum values (REG_PAD_CFG_FIELD_VEL_CURVE / REG_CTL_VEL_CURVE_DEF). */
#define VEL_LINEAR                      0
#define VEL_EXP_SOFT                    1
#define VEL_EXP_HARD                    2
#define VEL_CFG_INHERIT                 0xFFU  /* per-pad: inherit global default */

/* I2C↔SPI/GPIO/PWM expander block (0x0A00..0x0ABF). Six configurable pins;
 * each has a stride-8 register bank. SPI engine sub-block at 0x0A30+. */
#define REG_EXP_BASE                0x0A00
#define REG_EXP_PIN_STRIDE          0x08
#define REG_EXP_PIN_COUNT           6
/* per-pin field offsets */
#define REG_EXP_F_MODE              0x00   /* 0 NC,1 GPIO_OUT,2 GPIO_IN,3 PWM,4 SPI */
#define REG_EXP_F_FLAGS             0x01   /* [1:0] pull, [2] open-drain, [4:3] EXTI edge */
#define REG_EXP_F_VALUE             0x02   /* GPIO level / PWM duty 0..255 */
#define REG_EXP_F_EXTI_LATCH        0x03
#define REG_EXP_F_PWM_FREQ_HI       0x04
#define REG_EXP_F_PWM_FREQ_LO       0x05
/* SPI engine sub-block */
#define REG_EXP_SPI_CFG             0x0A30
#define REG_EXP_SPI_PINS            0x0A31
#define REG_EXP_SPI_LEN             0x0A32
#define REG_EXP_SPI_CTRL            0x0A33
#define REG_EXP_SPI_TX              0x0A40   /* 0x0A40..0x0A7F */
#define REG_EXP_SPI_RX              0x0A80   /* 0x0A80..0x0ABF */
#define REG_EXP_END                 0x0AC0   /* one past block */
/* MODE values */
#define EXP_MODE_NC                 0
#define EXP_MODE_GPIO_OUT           1
#define EXP_MODE_GPIO_IN            2
#define EXP_MODE_PWM                3
#define EXP_MODE_SPI                4
#define EXP_MODE_UART               5   /* reserved stub (PD8/PD9), rejected today */

/* Identity magic literals — for handshake validation. */
#define STM32_IDENT_MAGIC_C         0x43
#define STM32_IDENT_MAGIC_P         0x50
#define STM32_IDENT_MAGIC_A         0x41
#define STM32_IDENT_MAGIC_D         0x44

#ifdef __cplusplus
}
#endif
