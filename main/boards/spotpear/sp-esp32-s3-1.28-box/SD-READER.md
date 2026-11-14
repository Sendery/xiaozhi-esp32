# Spotpear 1.28 Box SD reader

This fork adds an optional reader to the existing Spotpear board, without
replacing its display, audio, touch or power management implementation.

## Build

Use ESP-IDF 6.1 and the canonical board builder:

```sh
python scripts/build.py spotpear/sp-esp32-s3-1.28-box \
  --name sp-esp32-s3-1.28-box-sd-reader
```

The stock `sp-esp32-s3-1.28-box` variant leaves the reader disabled. The SD
variant enables `CONFIG_SPOTPEAR_SD_READER` and reports a distinct board **name**.
The physical board **type** remains `sp-esp32-s3-1.28-box`; an OTA server must
also consider the variant name, or use a dedicated update channel, to avoid
replacing the custom variant with a stock build that has the same type.

Compiled images are not automatically flashed or published as OTA updates.
Preserve/provision WiFi and the custom OTA URL before installing a merged image,
whose NVS region is blank. Building does not test the physical card reader.

## Wiring and reused implementations

The manufacturer documents the same wiring in its tutorial and Arduino SD
video example:

| SD signal | GPIO |
|---|---:|
| CLK | 17 |
| CMD | 18 |
| D0 | 21 |
| D3 / SPI CS | 13 |

The reader uses **SDMMC one-bit mode at 8 MHz**, with D3 held high, matching the
manufacturer's example. It does not share the LCD SPI bus. No audio, display,
touch or power pins have been changed. Not all visually similar boards have
the same wiring: verify the PCB revision before installing.

References consulted:

- [Spotpear tutorial, wiring and SD player source](https://spotpear.cn/index/study/detail/id/2002.html).
- [Manufacturer SDcard-Video.zip](https://cdn.static.spotpear.cn/uploads/picture/learn/ESP32/ESP32-AI/SDcard-Video.zip),
  `Arduino/main/main.ino`: confirms one-bit SDMMC and the above pins. Unlike
  that example's automatic-format option, this reader never requests formatting.
- XiaoZhi's existing `espressif/esp32-p4-function-ev-board` SDMMC mount and
  `nologo/xingzhi-abs-2.0` SDSPI mount implementations: same ESP-IDF FAT/VFS API,
  with `format_if_mount_failed = false`. P4-specific power circuitry is not copied.
- [ESPHome SD card component](https://github.com/n-serrette/esphome_sd_card/blob/main/components/sd_mmc_card/sd_mmc_card.cpp):
  alternative integration of the same underlying driver. No ESPHome or Arduino
  framework dependency is added, and no third-party player source is vendored.

## Operation

Mounting starts on a low-priority worker after board initialization. An absent,
RAW, damaged or unsupported card is reported as a mount failure and does not
abort board startup. Existing FAT filesystems supported by the configured
ESP-IDF FatFs build can be read. This is not a RAW data recovery tool.
The SD variant enables long filenames with a heap buffer and UTF-8 filename APIs.

The exposed API only lists and reads files. It does not expose writes, deletion,
formatting or firmware execution from the card. This API restriction is not a
physical write-protect switch: ESP-IDF's generic FAT driver remains in use.

All filesystem I/O runs on the worker. MCP callbacks only enqueue requests,
read cached status or copy a bounded result; they never wait for card I/O on
the application's main task.

## MCP tools

| Tool | Arguments | Result |
|---|---|---|
| `self.sd_card.get_status` | none | Cached mounted state, capacity, last mount error |
| `self.sd_card.mount` | none | Queues mount/retry; never formats |
| `self.sd_card.list` | `path` (default `/`), `offset` (default 0), `limit` (default 16) | Queues a directory page |
| `self.sd_card.read_file` | `path`, `offset` (default 0), `length` (default 4096) | Queues a file chunk |
| `self.sd_card.get_job` | `job_id` | `queued`, `running`, `done` with `result`, or `failed` with `error` |

For example, request `self.sd_card.list` with `{"path":"/","limit":16}`.
The immediate response contains a `job_id`. Query `self.sd_card.get_job` for
that ID until complete; do not treat an accepted job as a successful read.
The result has `entries`, `has_more` and `next_offset`.

`read_file` returns `file_bytes`, `bytes_read`, `next_offset` and `eof`. A chunk
containing valid UTF-8 text has a `text` field; binary or incomplete UTF-8 chunks
have `data_base64` instead. For arbitrary byte-perfect downloads, encode/decode
each returned chunk according to its `encoding`, then concatenate the bytes.

Limits:

- Four outstanding/completed jobs in total; a new job may evict the oldest
  completed result. A missing job is reported as unknown/expired.
- At most 4096 bytes per read, 32 entries per directory page and 16 KiB per result.
- Maximum directory offset 4096, with a one-second scan budget per request.
- Card-relative paths of at most 192 UTF-8 bytes. Dot segments, backslashes,
  colons, control characters and ambiguous trailing dots/spaces are rejected.
- Individual files larger than `INT_MAX` (2 GiB minus one byte) are not supported
  by this first reader. FAT has no symlinks; do not reuse this path policy for a
  host filesystem or another filesystem with symlink support.
- `mounted` describes the last mount, not a continuously polled card-present
  sensor. Stop using the card before removing it; request mount again after
  reinsertion. I/O errors and mount failures are returned to the caller.

## Validation and remaining hardware checks

Validation on 2026-11-14 with ESP-IDF 6.1:

- Stock variant compiled, with the reader disabled: application 2,697,712 bytes.
- SD reader variant compiled with UTF-8 filenames: application 2,772,752 bytes,
  or 67.16% of its 4,128,768-byte partition. The merged image is 9,651,461 bytes;
  fixed partition padding explains why that image does not grow with the program.
- Native C++ reader tests passed with MSVC, and new C++ files passed clang-format.
- Full upstream host suite on Windows: 176 tests, 12 errors, 57 skipped. The 12
  errors match the pre-change baseline: temporary-directory cleanup, Bash-based
  workflow tests and an unavailable POSIX-style C++ compiler command. The new
  reader test passes separately and in that suite. The entire upstream suite
  is not reported as passing.
- No new image has been flashed, and no physical SD card was changed or validated.

The portable C++ reader is exercised on real temporary files by
`scripts/tests/test_spotpear_sd_reader.py`: traversal and FAT aliases, UTF-8,
binary data, chunk boundaries, EOF, missing files/directories and read limits.
The test hashes all fixtures before/after to verify that their contents are
unchanged. Run from an MSVC developer shell or a host GCC with C++23 support:

```sh
python -m unittest discover -s scripts/tests -p test_spotpear_sd_reader.py -v
```

On Windows with Visual Studio C++ Build Tools installed, the helper
`scripts/test_spotpear_sd_reader.ps1` activates the host compiler automatically.

Before installing on hardware, check both the stock and SD variants compile.
Before claiming hardware support, verify boot with no card, an unsupported/RAW
card and a known-good FAT card, directory pagination, a binary download hash,
and speech capture/playback while reading. The developer's current SD has
inconsistent preliminary reads and is not a validated test card. No card has
been formatted or written by this development work.
