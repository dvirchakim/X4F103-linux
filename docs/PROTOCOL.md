# X4F103 SPI protocol (as observed on the wire)

Reconstructed from a working Windows HPD Studio 2.4.9 session (USBPcap) and
confirmed against the Linux stack's `libX4Sensor.so` internals.

All transfers are FT4222 SPI master, single IO, mode 1, CLK_DIV_4, CS active.
FT4222 prepends a 2-byte header (`02 00`) to every IN packet; the SPI payload
follows. The first SPI byte of a response is a status byte (`00` = OK).

## Opcodes

| Opcode | Meaning |
|---|---|
| `0x99`, `0x98`, `0x97` | address/control latch (`99 ef`, `98 ff`, `97 00`, `97 01`, `97 02`) |
| `0x93` | firmware chunk write: `93` + 7 data bytes |
| `0x9a` | end of upload |
| `0x14` | poll / acknowledge (`00 02` = busy, `00 06` = done) |
| `0x15` | read 8-byte status block |
| `0x10` | begin register access |
| `0x11` | register read (`11 ff*` returns data; zeros are normal here) |
| `0x8f` | register write prefix: `8f <reg> <payload…>` |
| `0x05` | **frame read** — 385-byte buffer, `0xff` filler; returns the frame |

## Start sequence

```
# reset / probe
99 ef
98 ff
97 00
14 ff                → 00 02
97 02
14 ff                → 00 06
15 ff  (x8)          → status block
15 ff  (x8)          → status block
97 00

# begin upload
99 00
98 00
97 01
93 <7 bytes>         (x511)
14 ff                per chunk → 00 02
9a 00
99 00
98 00
97 02
14 ff                → 00 06
97 00

# verify
10 ff
8f 10
8f 0a 00
11 ff ff ff         → 00 E1 09 xx   (firmware hash check follows)

# LPOSC start
8f 0f 05
   ≈145 ms of silence

# configuration block
8f 10
8f 8b 00 3a 01 00 72 03
14 ff
8f 09 0d ff ff ff ff ff
14 ff
8f ff ff ff ff ff ff ff ff
14 ff
8f ff ff ff ff 09 c8 fe
14 ff
8f 71 3a 46 e5 2d 68 1f
14 ff
8f 4f 16 55 10 45 0c 6b
14 ff
8f 09 5d 07 da 05 b8 04
14 ff
8f da 03 2d 03 a6 02 3a
14 ff
8f 02 04 0c 0a 0c
8f 0e 02 01         # set run mode — the die starts producing frames after this

# ≈33 ms settle, then the steady-state cycle
```

## Steady-state cycle (≈50 ms, 20 Hz)

```
8f 10
   (≈49 ms gap)
10 ff
8f 0c 00
10 ff
11 ff (x8)           # 5 full reads
11 ff (x5)           # short read
8f 92 80 1d 01 ff
05 ff (x384)         # frame read — 384 filler bytes
```

The `0x05` response carries the frame: status byte + 192 int16 little-endian
samples.

## Error codes

| Code | Meaning |
|---|---|
| `0xf4` | chipinterface / IRQ failure |
| `0xf5` | firmware verify failed |
| `0xf6` | verify data or hash mismatch |
| `0xFE` | `chipinterface_wait_for_interrupt` timeout (`-2`) |
| app rc `6` | HPD: "error happened in a call to a chipinterface function" |

## Linux-specific findings

- `FirmwareUpload::upload_firmware_spi` performs the verify read **once** — no
  retry loop. After an `0x11` read the die sometimes returns zeros; a 2 ms
  retry returns `00 E1 09 7D`.
- `Ft4222GpioDriver::RunInterruptThread` polls GPIO port 3 and fires on a **high**
  level (`0x18ebd`), while the die asserts **low**. This is why every Linux
  session aborts with `0xFE` → `0xf4`.
- The same function is not used by this project: the replay reads frames over
  SPI and never waits on the interrupt.
