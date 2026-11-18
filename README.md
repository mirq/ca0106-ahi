# ca0106.audio — AHI driver for the Sound Blaster Audigy SE (SB0570)

AHI v4 sub-driver for Creative CA0106 based cards on AmigaOS 3.x / 68k with a
PCI bridge driven by `openpci.library`. Developed and tested on an A4000 with
68060, Prometheus FireStorm, Radeon 9200 (Prometheus.card DMA arena),
OpenPCI 2.1 and AHI 4.18.

Status:

| Feature | State |
| --- | --- |
| Stereo playback, 48 / 96 kHz, 16-bit and HiFi (24-bit out) modes | working, verified by digital loopback |
| Recording (Line / Mic / Aux / CD), 48 / 96 kHz, full duplex | working (data path verified; analog inputs need a cable test) |
| Hardware monitor (input mixed into output, for Paula via Aux) | working on the chip side; Paula→Aux needs a cable test |
| 44.1 kHz | not offered: the CA0106 runs it only toward S/PDIF, the analog DAC path produces garbage |
| 5.1 surround, S/PDIF | not yet |
| new openpci.library (3.x / 40.x, Thomas Richter) | compiled in, selected at run time, **untested** |

## Files

Release archive (`ca0106-<version>.lha`, drawer `CA0106/`):

| File | Install to | Purpose |
| --- | --- | --- |
| `Devs/AHI/ca0106.audio` | `DEVS:AHI/` | the AHI driver |
| `Devs/AudioModes/CA0106` | `DEVS:AudioModes/` | mode file (IDs `$00CA0001`–`$00CA0004`) |
| `C/cactl` | `C:` | mixer control from the shell (input, gain, monitor, output) |
| `Tools/caprobe` | anywhere | hardware go/no-go test |
| `Tools/catone` | anywhere | sine generator with self-verification |
| `Tools/ahitest` | anywhere | end-to-end test through ahi.device |
| `Tools/ahistream` | anywhere | streaming / mode-scan / alloc stress test through ahi.device |
| `Tools/capci` | anywhere | PCI config dump |
| `Install-CA0106` | — | install script |

Audio modes:

| ID | Name |
| --- | --- |
| `$00CA0001` | CA0106: 16 bit stereo++ |
| `$00CA0002` | CA0106: 16 bit stereo |
| `$00CA0003` | CA0106: HiFi 24 bit stereo++ |
| `$00CA0004` | CA0106: HiFi 24 bit stereo |

The `$00CA` hardware ID is provisional; it was not allocated by the AHI
maintainer.

## Install

Unpack the archive, then from the `CA0106` drawer:

```
execute Install-CA0106
```

or by hand:

```
copy Devs/AHI/ca0106.audio DEVS:AHI/
copy Devs/AudioModes/CA0106 DEVS:AudioModes/
copy C/cactl C:
AddAudioModes DEVS:AudioModes/CA0106
```

Then pick a `CA0106:` mode for the units you want in **Prefs/AHI**. 48000 Hz is the
recommended mixing frequency; 96000 Hz works but doubles mixing load.

## Hearing Paula through the card (Aux loopback)

Connect the Amiga audio output to the card's Aux input (the internal 4-pin
header) and add to `S:User-Startup`:

```
cactl INPUT=AUX MONITOR=0
```

This initialises the card at boot and mixes the Aux input into the line output
at 0 dB, with no AHI program running. When an AHI program opens a CA0106
mode, AHI applies the **Monitor volume** and **Input** from Prefs/AHI, so set
those for the CA0106 unit as well (input "Aux (Amiga audio)", monitor > 0),
otherwise the first AHI program mutes the loopback. The monitor stays at the
last setting after the program exits.

`cactl` options:

```
cactl [INIT] [INPUT=LINE|MIC|AUX|CD] [GAIN=dB] [MONITOR=dB|OFF]
      [OUTPUT=dB] [TESTAUX] [STATUS]
```

* `GAIN` is the WM8775 input gain, -103 … +24 dB in 0.5 dB steps.
* `MONITOR` is the input-to-output level, OFF or -51 … +12 dB.
* `OUTPUT` is the front output attenuation, … +12 dB (0 = unity).
* `TESTAUX` plays a 1 kHz tone on all Paula channels and reports whether
  it arrives at the ADC and, with monitoring on, at the output mixer.

`cactl` fails while an AHI program is using the card; use Prefs/AHI then.

## Known issues

* A guru while opening the AHI mode requester was reported once and has not
  been reproduced yet (`ahistream SCAN=20` exercises the same driver calls).
* The Paula→Aux path has not been checked with a real cable yet.

## Tests

```
caprobe                         ; PCI, registers, DMA, codecs, IRQ, DMA in both directions
catone FREQ=1000 VOL=20 VERIFY  ; sine out, recorded back from the chip's mixer
ahitest LOOP                    ; through ahi.device: play + record + compare
ahitest MODE=$00CA0003 LOOP MIXFREQ=96000
```

All of these pass on the reference machine. `catone` without `VERIFY` is the
listening test (`LEFT`, `RIGHT`, `RATE=`, `S32`).

## Building

Requires the bebbo-style `m68k-amigaos-gcc` (tested with GCC 16.2), vasm
(`vasmm68k_mot` in the toolchain's `bin/`), and the AHI 4.18 developer files
(default location `../ahidev_4.18`, override with `AHIINC=`).

```
make            # driver, mode file, tools in build/
make PREFIX=/path/to/toolchain AHIINC=/path/to/AHI/Developer/include/C
make lha        # release archive build/ca0106-<version>.lha
```

The driver is built with `-nostdlib`, no startup code and no C library; the
tools use libnix.

## How it works

* **PCI.** One binary for both openpci generations. Card lookup, ownership,
  config space and interrupts use the calls common to both; registers are
  accessed directly through the I/O BAR with explicit byte swapping.
* **DMA memory.** The Prometheus bridge cannot bus-master into Amiga memory,
  so buffers live on the PCI bus. With OpenPCI 2.x `pci_allocdma_mem(MEM_PCI)`
  lands in the Radeon VRAM arena that Prometheus.card reserves (`DMASIZE`
  tooltype); `prometheus.library` `Prm_AllocDMABuffer` is the fallback. With
  openpci 3+ the driver uses `AllocateDMAMemoryForBoard()` (P96 needs
  `GRANTDIRECTACCESS=Yes`). The driver needs 48 KB.
* **Playback** uses the CA0106 single-buffer mode (as the 4Front OSS driver
  does): the engine loops over one buffer and interrupts at the half and the
  end. The Linux period-list mode does **not** work here: the list entry
  fetched from Radeon VRAM arrives with its two dwords in the wrong order and
  the engine never starts. Each interrupt causes a software interrupt that
  runs the AHI player and mixer hooks, converts to little endian into a fast
  RAM stage, and copies one 10 ms half into VRAM. The player hook runs once
  per mixed block, so `PlayerFreq` timing is exact.
* **Recording** captures into a second VRAM buffer (two 40 ms halves); each
  completed half is swapped into fast RAM and passed to `ahiac_SamplerFunc`.
* **Monitor.** The WM8775 ADC output is fed back into the analog output
  inside the CA0106 (`CAPTURE_CONTROL` channel 1), so Paula is mixed without
  any CPU or DMA load. The codecs keep running after the driver closes.
* **Codec state.** The WM8775 is write-only; input and gain are mirrored in
  an unused CA0106 register so `cactl` and the driver agree.

## Credits and license

GPL-2.0-or-later. Register map and codec initialisation come from the Linux
ALSA `snd-ca0106` driver (James Courtier-Dutton and others); single-buffer
playback, the SB0570 GPIO map and the I2S record source come from the 4Front
OSS `oss_audigyls` driver (GPL-2.0).

Third-party interface headers are included for building:
`include/libraries/openpci.h` (openpci.library SDK) and
`include/inline/ahi.h`, `include/proto/ahi.h` (generated from the AHI SFD);
they belong to their respective authors.
