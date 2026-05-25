# Device Index

Last refreshed: 2026-05-25.

This file records emulator-facing evidence for real-device stream devices.
It is indexed by guest device name, with the source binary/report evidence and
the API surface that matters for faithful emulation.

Sources used for this pass:

- `D:\INAVI_Emulator\report_serial.txt`
- `D:\INAVI_Emulator\DUMPPLZ\FILES\Windows\SMB380.dll`
- `D:\INAVI_Emulator\DUMPPLZ\FILES\Windows\YAS526B.dll`
- repo `regs.json` and `serial_devices.json`

## Index

| Guest | Driver DLL | Evidence status | Emulator status |
| --- | --- | --- | --- |
| `COM1:` | `VSP.dll` | Real GPS NMEA serial port, 9600 8N1 | Host serial mapping target once profile selection is fixed |
| `SMB1:` | `SMB380.dll` | Bosch SMB380-style accelerometer stream driver over `I2C2:`/optional `SPI1:` | Stub only |
| `MFS1:` | `YAS526B.dll` | Yamaha YAS526B magnetic field/e-compass stream driver over `I2C2:` | Stub only |

## `COM1:` GPS / `VSP.dll`

Real-device report evidence:

- Active stream device: `COM1:`
- Registry key: `HKLM\Drivers\BuiltIn\VSP`
- Driver DLL: `VSP.dll`
- Prefix/index: `COM`, index `1`
- Serial API: yes
- DCB: `BaudRate=9600`, `ByteSize=8`, `Parity=0`, `StopBits=0`, binary on
- Passive RX sample: NMEA-style `$GPRMC`, `$GPVTG`, `$GPGGA`, `$GPGSA`

Emulator context:

- The real hardware GPS path is `COM1:` with NMEA at `9600 8N1`.
- The current SDMMC/profile data can still make the app choose `COM7:`. That is
  a profile/config selection problem, not evidence that the real device GPS is
  `COM7:`.
- While the dump selects `COM7:`, `serial_devices.json` may temporarily map
  `COM7:` to the host feeder for diagnostics. The intended final serial map is
  guest `COM1:` -> host NMEA feeder.

## `SMB1:` Accelerometer / `SMB380.dll`

Real-device report evidence:

- Active stream device: `SMB1:`
- Registry key: `HKLM\Drivers\BuiltIn\SMB380`
- Driver DLL: `SMB380.dll`
- Prefix/index: `SMB`, index `1`
- `CreateFile(SMB1:)`: OK
- Serial API probe returns a dummy-looking zero DCB; treat this as a stream
  device, not a UART.

PE/export evidence:

- Format: Windows CE MIPS R4000 PE DLL
- Exports: `SMB_Init`, `SMB_Deinit`, `SMB_Open`, `SMB_Close`,
  `SMB_Read`, `SMB_Write`, `SMB_Seek`, `SMB_IOControl`,
  `SMB_PowerUp`, `SMB_PowerDown`
- `SMB_Open` and `SMB_Close` both return success.
- `SMB_Read`, `SMB_Write`, and `SMB_Seek` return `0`; the meaningful surface is
  `DeviceIoControl`.

Internal bus/API evidence:

- String evidence names `I2C2:` and `SPI1:`.
- Init tries `CreateFile("I2C2:")`; error string:
  `SMB_InitHW : CreateFile("I2C2:") failed`.
- The driver logs `Invalid bus flag, neither i2c nor spi`, `Open SPI device
  failed!`, and `Open I2C device failed!`.
- The driver imports/uses standard CE APIs consistent with a real stream driver:
  `CreateFileW`, `DeviceIoControl`, `CloseHandle`, `CreateThread`,
  `CreateEventW`, event modification, `WaitForSingleObject`, `Sleep`,
  interrupt helpers, `SetLastError`, and debug/printf-style output.
- It references `giisr.dll` and `ISRHandler`, and has interrupt wait strings,
  so the real driver can support interrupt-driven accelerometer events.

Disassembly notes:

- `SMB_Init` opens `I2C2:` and then reads/writes SMB380 registers.
- It reads register `0x0f`, writes register `0x0f` with `0x6b`, then reads
  registers `0x13` and `0x12` for chip information/version.
- `SMB_IOControl` dispatches a dense IOCTL range around `0x01012ee0` through
  `0x01012f60`. The upper/lower control-code shape is driver-specific, not a
  Win32 serial API.

Named IOCTL families visible in strings:

- init/reset/image: `IOCTL_SMB380_INIT`, `IOCTL_SMB380_SOFT_RESET`,
  `IOCTL_SMB380_GET_IMAGE`, `IOCTL_SMB380_SET_IMAGE`,
  `IOCTL_SMB380_UPDATE_IMAGE`
- raw register access: `IOCTL_SMB380_READ_REG`,
  `IOCTL_SMB380_WRITE_REG`
- sample reads: `IOCTL_SMB380_READ_ACCEL_X`,
  `IOCTL_SMB380_READ_ACCEL_Y`, `IOCTL_SMB380_READ_ACCEL_Z`,
  `IOCTL_SMB380_READ_ACCEL_XYZT`, `IOCTL_SMB380_READ_TEMPERATURE`
- configuration: range, mode, bandwidth, wake-up pause, offsets, EEPROM
- interrupts: interrupt mask/status/reset, wait interrupt, latch interrupt,
  new-data, low-g, high-g, any-motion, alert, advanced interrupt controls

Inferred struct/API shape:

- Guest callers should use `CreateFileW(L"SMB1:", ...)` then
  `DeviceIoControl(handle, ioctl, inBuf, inSize, outBuf, outSize, ...)`.
- Several IOCTL handlers validate input/output pointers and sizes before
  touching registers. Exact per-IOCTL structs still need callsite captures from
  the app before implementing a faithful handler.
- Register IOCTLs are likely small byte-oriented requests, but do not freeze a
  guessed struct into emulator behavior without a guest callsite/log proving
  sizes and field order.

Emulator implications:

- Keep `SMB1:` as an ioctl/stream-device entry, not a serial port.
- A future real handler should probably model an `I2C2:` child bus and then
  expose an SMB380 sensor backend through `SMB1:`.
- Minimal honest behavior before a real handler: open succeeds if configured,
  unsupported IOCTLs fail or return no data consistently, and no fake motion
  values are invented.

## `MFS1:` Magnetic Field Sensor / `YAS526B.dll`

Real-device report evidence:

- Active stream device: `MFS1:`
- Registry key: `HKLM\Drivers\BuiltIn\YAS526B`
- Driver DLL: `YAS526B.dll`
- Prefix/index: `MFS`, index `1`
- `CreateFile(MFS1:)`: OK
- Serial API probe returns a dummy-looking zero DCB; treat this as a stream
  device, not a UART.

PE/export evidence:

- Format: Windows CE MIPS R4000 PE DLL
- Exports: `MFS_Init`, `MFS_Deinit`, `MFS_Open`, `MFS_Close`,
  `MFS_Read`, `MFS_Write`, `MFS_Seek`, `MFS_IOControl`,
  `MFS_PowerUp`, `MFS_PowerDown`
- `MFS_Open` and `MFS_Close` return success.
- `MFS_Read`, `MFS_Write`, and `MFS_Seek` return `0`; the meaningful surface is
  `DeviceIoControl`.

Internal bus/API evidence:

- String evidence names `I2C2:`.
- `MFS_Init` opens `I2C2:` twice and stores two handles:
  one for the YAS526B Z chip and one for the YAS526B XY chip.
- Init strings name `s_hI2C4YAS526B_Z` and `s_hI2C4YAS526B_XY`.
- The driver uses `CreateFileW`, `DeviceIoControl`, `CloseHandle`,
  `SetLastError`, and debug/printf-style output.

Disassembly notes:

- `MFS_Init` calls `CreateFileW("I2C2:", 0, 0, ..., OPEN_EXISTING, 0x80, 0)`
  twice.
- The driver uses I2C bus `DeviceIoControl` control codes `0x80002004` and
  `0x80002005` for write/read style bus transfers.
- Register write builds a 2-byte transfer beginning with register selector
  `0x2e` for one chip and `0x2f` for the other.
- `MFS_IOControl` recognizes five driver-specific codes:
  `0xb0000000`, `0xb0000004`, `0xb0000008`, `0xb000000c`,
  and `0xb0000010`.
- Unsupported control codes log `[MFS] IOCTL_MFS_XXX[0x%X] : Not supported code`
  and set error `0x57` (`ERROR_INVALID_PARAMETER`).

Inferred struct/API shape:

- Guest callers should use `CreateFileW(L"MFS1:", ...)` then
  `DeviceIoControl(handle, ioctl, inBuf, inSize, outBuf, outSize, ...)`.
- For `0xb0000000`, the driver requires a 4-byte input buffer. It reads byte
  fields from offsets `0`, `1`, and `3`, then calls the register-write helper.
- For `0xb0000004`, the driver requires a 4-byte input buffer and a non-null
  output buffer. It reads byte fields from offsets `0` and `2`, then calls the
  register-read helper.
- Codes `0xb0000008`, `0xb000000c`, and `0xb0000010` are recognized by
  `MFS_IOControl`, but the current disassembly pass did not prove a data
  payload for them.
- Do not implement these as guessed compass readings yet. First capture guest
  `DeviceIoControl(MFS1:)` calls from the app to confirm which IOCTLs it uses,
  buffer sizes, and expected return data.

Emulator implications:

- Keep `MFS1:` as an ioctl/stream-device entry, not a serial port.
- A future real handler should probably model enough `I2C2:` register behavior
  to satisfy `YAS526B.dll`-style callers or implement the same stream-device
  IOCTL surface directly.
- For now a stub is acceptable only if callers tolerate missing sensor data.
  If the navigation app blocks on compass availability, implement the narrow
  observed IOCTLs from logs rather than hardcoding app state.

## Shared Notes For Sensor Stubs

- `SMB1:` and `MFS1:` being present in `report_serial.txt` does not mean they
  are serial/GPS devices. Their DLLs expose CE stream-driver names, but their
  internals are I2C sensor clients.
- `serial_devices.json` currently lists them as `ioctl_device` stubs. That is
  the right high-level category until a bus/sensor backend exists.
- Useful next traces:
  - log `CreateFileW` for `SMB1:` and `MFS1:`
  - log `DeviceIoControl` code, input size, output size, and transferred count
  - capture a bounded preview of small input/output buffers for these two
    devices only
- Avoid registry speculation for GPS profile selection. The COM1 proof here is
  from the real active device report and serial RX sample; the emulator's
  current COM7 behavior is still explained by app data/profile flow.
