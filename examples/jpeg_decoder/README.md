# JPEG decoder on PRU

## Overview

**Tested on hardware.** Every frame result in the Measured results section was measured on TI LaunchPads with the supplied PRU firmware: LP-AM243 (ICSSG, PRU at 250 MHz and 333 MHz) and LP-AM261 (ICSSM, PRU at 225 MHz), in both single-core and dual-core configurations. Every decoded image matched the expected sample-block CRC, which was generated from libjpeg-turbo output, so the PRU output is bit-exact with libjpeg-turbo for every measured image.

This example decodes baseline JPEG entropy scans to planar sample blocks on TI PRU cores. It supports grayscale, 4:2:0 and 4:4:4 sampling, including restart markers. Progressive JPEG and arithmetic coding are unsupported. The R5F host parses headers, builds Huffman and quantization tables, loads the scan, starts the PRU cores, polls completion, and checks the block stream CRC. The R5F libjpeg-turbo comparison below is an earlier independent measurement; that decoder is not included here.

The host rejects frames with no blocks or more than `MAX_BLOCKS=4000`, more than `MAX_SEGMENTS=256` restart segments, output exceeding its MSRAM window, or a scan larger than the selected Shared RAM or MSRAM window. A scan starts in ICSS Shared RAM at `0x16500` if it fits; otherwise the entire scan goes to MSRAM. ICSSM Shared RAM ends at `0x18000`, so its local scan capacity is 6912 bytes. This avoids overflowing the smaller ICSSM Shared RAM; there is no scan refill protocol. These are implementation limits, not a promise that every JPEG geometry below the block limit is supported. Source: [host source](host/mcuplus/jpeg_host.c) and the firmware linker files.

## Architecture

The pipeline is entropy decode, dequantization, inverse DCT, then sample output. [Single core](jpeg_decoder_single_core/README.md) runs the whole pipeline on PRU0. [Dual core](jpeg_decoder_dual_core/README.md) assigns entropy decoding to PRU0 and the remaining stages to PRU1. PRU0 hands coefficient blocks to PRU1 with the scratchpad XFR slot protocol in [ipc_xfr.inc](common/ipc_xfr.inc); PRU1 acknowledges consumed slots. The host starts PRU1 first so its scratchpad initialization finishes before PRU0 publishes a block. Source: firmware and [host source](host/mcuplus/jpeg_host.c).

## Memory map

Offsets in the ICSS column are relative to the ICSS instance base obtained from the PRUICSS driver. Firmware linker scripts reserve PRU local DRAM from `0x100` through `0x1fff` and IRAM through `0x2fff`.

| Region | Both devices |
|---|---|
| PRU0 control and scan descriptor | ICSS + `0x1000` |
| Component descriptors; zigzag table | ICSS + `0x1300`; ICSS + `0x0d40` |
| Dual PRU1 control and quantization tables | ICSS + `0x2000`; ICSS + `0x2100` |
| Single PRU0 output pointer and quantization tables | ICSS + `0x1870`; ICSS + `0x1900` |
| Short scan | ICSS + `0x16500`, ending before device Shared RAM limit |

Source: [host source](host/mcuplus/jpeg_host.c) and the firmware `linker.cmd` files.

| Buffer | LP-AM243, ICSSG0 | LP-AM261, ICSSM0 |
|---|---|---|
| Shared RAM end | `0x20000` | `0x18000` |
| Output blocks | `0x700c0000`, before `0x70100000` | `0x70080000`, before `0x700cc000` |
| Large scan | `0x70120000` to `0x701d0000` | `0x700cc000` to `0x70150000` |
| Metadata base | `0x70100000` | `0x70171000` |
| Block output pointers | metadata + `0x0000` | metadata + `0x6000` |
| Block component and restart tables | metadata + `0x8000`, `0xa000` | metadata + `0x1000`, `0x3000` |
| Restart segments | metadata + `0xc000` | metadata + `0x5000` |

Source: [host source](host/mcuplus/jpeg_host.c). The AM261x host keeps the scan below its R5F shared and log memory windows and uses MSRAM when the scan exceeds local Shared RAM.

## Stage costs

These per-block figures come from a calibrated simulator, not silicon. They illustrate content sensitivity; only the frame totals in the following results tables were measured on boards.

| Stage | Sparse or fast case, cycles/block | Dense or worst observed case, cycles/block |
|---|---:|---:|
| Entropy decode | 109.6 flat gray | 3156.7 max-Huffman gray |
| Dequantization, MAC kernel | 789.00 | 1351.74 |
| Inverse DCT, optimized kernel | 742 | 1996 |
| Sample output, steady state | 364 | 364 |

Sources: calibrated simulation, stage documentation for Huffman, dequantization, inverse DCT and output. These are separate kernel measurements and must not be summed as a measured frame time.

## Measured results

The following frame counts use the PRU `CYCLE` counter with the supplied host application. Nominal clocks are 250 MHz on LP-AM243 and 225 MHz on LP-AM261; clocks measured under that host were 249.9 and 224.9 MHz respectively. R5F figures are from an earlier libjpeg-turbo 2.1.5 C-path run with `-O3`, no SIMD, `JDCT_ISLOW`, `raw_data_out=TRUE`, and caches enabled on the same boards. That R5F decoder is not shipped in this example.

### LP-AM243, ICSSG0

| Image | R5F ms / fps | 1 PRU cycles / ms / fps | 2 PRU cycles (PRU0, PRU1) / ms / fps | Bit-exact |
|---|---|---|---|---|
| flat_gray_400x400 | 6.66 / 150.1 | 5244087 / 20.985 / 47.7 | 4893311, 4899100 / 19.604 / 51.0 | yes, both |
| gradient_gray_400x400 | 8.63 / 115.9 | 6978120 / 27.924 / 35.8 | 6480211, 6487902 / 25.962 / 38.5 | yes, both |
| gradient_420_400x400 | 12.51 / 79.9 | 10441271 / 41.782 / 23.9 | 9615174, 9622401 / 38.505 / 26.0 | yes, both |

Source: measured on LP-AM243 with this host application; CRCs matched the expected sample blocks.

### LP-AM261, ICSSM0

| Image | R5F ms / fps | 1 PRU cycles / ms / fps | 2 PRU cycles (PRU0, PRU1) / ms / fps | Bit-exact |
|---|---|---|---|---|
| flat_gray_400x400 | 10.64 / 94.0 | 5243464 / 23.315 / 42.9 | 4893309, 4899086 / 21.783 / 45.9 | yes, both |
| gradient_gray_400x400 | 14.42 / 69.3 | 6977425 / 31.025 / 32.2 | 6480209, 6487888 / 28.848 / 34.7 | yes, both |
| gradient_420_400x400 | 20.97 / 47.7 | 10440081 / 46.421 / 21.5 | 9615171, 9622387 / 42.785 / 23.4 | yes, both |

Source: measured on LP-AM261 with this host application; CRCs matched the expected sample blocks.

### LP-AM243, ICSSG0 at 333 MHz, 16-image test corpus

The AM243x ICSSG core clock can also be set to 333.33 MHz in SysConfig (`coreClk = 333333333` on the PRU-ICSS instance in the host `example.syscfg`; 200, 225, 250 and 300 MHz are the other options). This run used the same PRU firmware with a test build of the host that embeds 16 test images (not bundled with this example), raises the block limit to 7680 so that a 400x400 4:4:4 frame (7500 blocks) fits, and unstuffs scans that do not fit in Shared RAM in place in MSRAM. The PRU clock measured under the host was 333.2 MHz. The R5F column is the same earlier libjpeg-turbo run at 800 MHz as above. All 16 images were bit-exact in both modes (70 of 70 checks passed).

| Image | Content | R5F ms / fps | 1 PRU cycles / ms / fps | 2 PRU cycles / ms / fps | 2 PRU vs 1 PRU | Bit-exact |
|---|---|---|---|---|---|---|
| flat_gray_400x400 | typical | 6.66 / 150.1 | 5303003 / 15.915 / 62.8 | 4899154 / 14.703 / 68.0 | 1.082x | yes, both |
| gradient_gray_400x400 | typical | 8.63 / 115.9 | 7037171 / 21.120 / 47.3 | 6487958 / 19.472 / 51.4 | 1.085x | yes, both |
| impulse_gray_400x400 | typical | 6.67 / 149.9 | 5334384 / 16.010 / 62.5 | 4928422 / 14.791 / 67.6 | 1.082x | yes, both |
| dc_only_gray_400x400 | typical | 6.77 / 147.8 | 5388728 / 16.173 / 61.8 | 4922920 / 14.775 / 67.7 | 1.095x | yes, both |
| single_ac_gray_400x400 | typical | 9.87 / 101.3 | 9625219 / 28.887 / 34.6 | 6525242 / 19.584 / 51.1 | 1.475x | yes, both |
| gradient_420_400x400 | typical | 12.51 / 79.9 | 10529296 / 31.601 / 31.6 | 9622455 / 28.879 / 34.6 | 1.094x | yes, both |
| gradient_444_400x400 | typical | 23.72 / 42.2 | 19880458 / 59.665 / 16.8 | 18212171 / 54.658 / 18.3 | 1.092x | yes, both |
| restart_gray_400x400 | typical | 8.88 / 112.6 | 7069101 / 21.216 / 47.1 | 6487956 / 19.472 / 51.4 | 1.090x | yes, both |
| restart_420_400x400 | typical | 12.64 / 79.1 | 10545918 / 31.650 / 31.6 | 9622455 / 28.879 / 34.6 | 1.096x | yes, both |
| nonmult16_gray_401x399 | typical | 8.76 / 114.2 | 7144536 / 21.442 / 46.6 | 6586331 / 19.767 / 50.6 | 1.085x | yes, both |
| nonmult16_420_401x399 | typical | 12.84 / 77.9 | 10859874 / 32.593 / 30.7 | 9924391 / 29.785 / 33.6 | 1.094x | yes, both |
| restart_nonmult16_420_401x399 | typical | 12.95 / 77.2 | 10871051 / 32.626 / 30.7 | 9924389 / 29.785 / 33.6 | 1.095x | yes, both |
| dense_ac_gray_400x400 | dense | 24.97 / 40.0 | 22933959 / 68.829 / 14.5 | 13611489 / 40.851 / 24.5 | 1.685x | yes, both |
| dense_ac_420_400x400 | dense | 34.60 / 28.9 | 30135972 / 90.444 / 11.1 | 16160730 / 48.502 / 20.6 | 1.865x | yes, both |
| dense_ac_444_400x400 | dense | 69.67 / 14.4 | 60447634 / 181.415 / 5.5 | 32484308 / 97.492 / 10.3 | 1.861x | yes, both |
| maxhuff_gray_400x400 | worst-case Huffman | 24.79 / 40.3 | 22963991 / 68.920 / 14.5 | 13641891 / 40.942 / 24.4 | 1.683x | yes, both |

Source: measured on LP-AM243 (rev E1); CRCs matched the expected sample blocks. "2 PRU cycles" is the later of the two cores.

| Content | Images | R5F fps (geometric mean) | 1 PRU fps | 2 PRU fps | 2 PRU vs 1 PRU |
|---|---|---|---|---|---|
| typical | 12 | 98.2 | 39.3 | 44.0 | 1.12x (1.08x–1.48x) |
| dense | 3 | 25.5 | 9.6 | 17.3 | 1.80x (1.69x–1.87x) |
| worst-case Huffman | 1 | 40.3 | 14.5 | 24.4 | 1.68x |

At 333 MHz the single-core cycle counts on the typical images are about 1% higher than at 250 MHz (the dual-core counts are unchanged), so throughput scales close to linearly with the clock.

Cycle counts are effectively the same on both devices because they run identical firmware; milliseconds differ with the PRU clock. How much the second PRU helps depends on the content. On typical images it is usually only 7–10% faster, because PRU0 entropy decoding dominates the pipeline (one image with a single AC coefficient per block reached 1.48x). On dense, high-entropy content it is 1.7–1.9x faster. Choose single core when a PRU is scarce and the content is smooth; choose dual core for dense content.

## Build and run

Configure `imports.mak` for the TI PRU compiler and MCU+ SDK, then build firmware before the R5F host so the generated firmware headers are available. The project makefiles expose `all`, `pru`, `host`, and `clean`; the separate host makefile exposes `all`, `host`, `pru`, and `clean`.

```sh
make -C examples/jpeg_decoder/jpeg_decoder_single_core DEVICE=am243x pru
make -C examples/jpeg_decoder/host DEVICE=am243x BUILD_MCUPLUS=y JPEG_MODE=single all
make -C examples/jpeg_decoder/jpeg_decoder_dual_core DEVICE=am261x pru
make -C examples/jpeg_decoder/host DEVICE=am261x BUILD_MCUPLUS=y JPEG_MODE=dual all
```

Use either `DEVICE=am243x` or `DEVICE=am261x` consistently for firmware and host; use `JPEG_MODE=single` or `JPEG_MODE=dual` for the host. CCS users can import each selected firmware `example.projectspec` and the matching host `example.projectspec` under `firmware/<board>/<core>/ti-pru-cgt/` and `host/mcuplus/<board>/r5fss0-0_freertos/ti-arm-clang/`. Source: project and host makefiles and projectspec files.

Load the host application on the R5F and observe its `DebugP_log` UART output. A passing run prints one CRC line per bundled image, for example `flat_gray_400x400: CRC 1bdadcaf expected 1bdadcaf, PRU0 5244087 cycles, 47 fps` on the LP-AM243 single PRU run; dual mode also prints PRU1 cycles. The expected CRCs for the other bundled images are `d5161b37` and `aa881b03`. Source: measured LP-AM243 console results and [host source](host/mcuplus/jpeg_host.c).

## Notes

On ICSSM, enabling the PRU cycle counter only in the start stub can race the host driver's read-modify-write of the PRU CONTROL register. The supplied host clears CYCLE while reset and writes CONTROL with COUNTER_ENABLE set before calling `PRUICSS_enableCore()`. Preserve that sequence when adapting the host, or the counter can stop while decoding continues. Source: [host source](host/mcuplus/jpeg_host.c).

Measured on LP-AM243 (rev E1) and LP-AM261 (rev E2), built with TI PRU CGT 2.3.3, TI Arm Clang 4.0.5.LTS, MCU+ SDK 12.01.00.33 with SysConfig 1.28.0 (AM243x) and MCU+ SDK 11.00.00.29 with SysConfig 1.26.0 (AM261x). The 250 MHz (LP-AM243) and 225 MHz (LP-AM261) tables cover the three bundled images on each board with the supplied host. The 333 MHz table adds 4:4:4, dense and worst-case Huffman content and scans placed in MSRAM, on LP-AM243 only, using the test host build described above.
