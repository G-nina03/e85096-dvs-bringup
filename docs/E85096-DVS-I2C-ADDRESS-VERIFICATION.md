# E850-96 × S5KRC1S: I²C Address & Verification

## Confirmed on E850-96 (2026-10-07)

| Item | Value | Evidence |
|---|---|---|
| 7-bit I²C device address | `0x10` | DVS driver README, Linux I²C client `0-0010` |
| 8-bit write/read representation | `0x20 / 0x21` | `0x10 << 1`, with R/W bit |
| Device Tree node | `dvs@10` | DVS driver README |
| Device Tree compatible | `nrv,rc1s` | DVS driver README |
| Control transaction register | `0x303B` | Driver bring-up test |
| Read-back | `0x55` | 2026-10-07 DVS ready log |
| Sensor CHIP_ID | Not verified | I²C register read-back is not the same as chip identity |
| MIPI CSI-2 / DMA | Not verified | Requires receiver host / PHY bring-up |

The Linux device instance `0-0010` means adapter 0, 7-bit address `0x10` **in that validation session**. This does not imply every E850 operating system maps HSI2C2 to adapter 0.

## Before scanning

```sh
i2cdetect -l
# Find the Linux bus number associated with E850 CN4 / HSI2C2.
sudo i2cdetect -y <actual-bus>
```

The `<actual-bus>` placeholder must be replaced by the device's current adapter number. A kernel-bound device may appear as `UU`; do not force probing while a driver owns the sensor.

## Hardware facts to validate

E850 CN4 carries I²C control on pins 13 (SCL) and 14 (SDA). RESET control through E850 CN4 pin 12 requires that the SoC pin be configured as GPIO instead of MCLK, and that the cable, RESET voltage, and default levels are verified.

Do not confuse the sensor's silicon PAD numbering with the DVS module's 22-pin FPC connector numbering.

## Scope and source handling

The user also supplied a summary attributed to a Samsung S5KRC1SY preliminary datasheet (Rev0.01, Table 8), including alternate address-selection states, die PAD numbers and a user-defined address register. Those details are **not reproduced here** pending authorization for publication in this public repository, consistent with the repository README's ban on non-public register information. The original datasheet was not uploaded.

## Links

- [Public E850 DVS driver evidence](../modules/dvs/README.md)
- [Editable 16:9 connection and porting slides](E850_DVS_22to15_Connection_Porting_Guide_Editable.html)
- [Live slides](https://philipdekim-ond01.github.io/obsidian-blog/dvs-ondevice-ai/e850-dvs-connection-porting.html)
