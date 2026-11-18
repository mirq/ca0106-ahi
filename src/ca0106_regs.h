/*
 * Creative CA0106 (Audigy SE / Audigy LS / SB Live 24-bit) register map.
 *
 * Register names and values are taken from the Linux ALSA driver
 * sound/pci/ca0106/ca0106.h, Copyright (c) 2004 James Courtier-Dutton,
 * licensed under the GNU General Public License version 2 or later.
 * This file is therefore distributed under the GPL as well.
 *
 * All registers are 32-bit little endian on the PCI side.
 */
#ifndef CA0106_REGS_H
#define CA0106_REGS_H

/* ---- Direct I/O registers (offsets into BAR0, 32 bytes) ---- */
#define CA0106_PTR          0x00    /* indexed register pointer: reg<<16 | channel */
#define CA0106_DATA         0x04    /* indexed register data */
#define CA0106_IPR          0x08    /* interrupt pending, write 1 to clear */
#define CA0106_INTE         0x0c    /* interrupt enable */
#define CA0106_HCFG         0x14    /* hardware config */
#define CA0106_GPIO         0x18
#define CA0106_AC97DATA     0x1c    /* 16 bit */
#define CA0106_AC97ADDRESS  0x1e    /* 8 bit */

#define IPR_SPI             0x00000800
#define IPR_AI              0x00000100  /* audio pending: see EXTENDED_INT */
#define IPR_MIDI_RX_A       0x00000004
#define IPR_MIDI_TX_A       0x00000002
#define IPR_PCI             0x00000001

#define INTE_AI             0x00000100
#define INTE_PCI            0x00000001

#define HCFG_PLAYBACK_ATTENUATION 0x00006000
#define HCFG_PLAYBACK_S32_LE      0x00000800
#define HCFG_CAPTURE_S32_LE       0x00000400
#define HCFG_AC97                 0x00000008
#define HCFG_AUDIOENABLE          0x00000001

#define AC97_REC_GAIN       0x1c

/* ---- Indexed registers (via PTR/DATA) ---- */
#define PLAYBACK_LIST_ADDR      0x00
#define PLAYBACK_LIST_SIZE      0x01
#define PLAYBACK_LIST_PTR       0x02
#define PLAYBACK_DMA_ADDR       0x04
#define PLAYBACK_PERIOD_SIZE    0x05
#define PLAYBACK_POINTER        0x06
#define PLAYBACK_PERIOD_END_ADDR 0x07
#define PLAYBACK_FIFO_OFFSET_ADDRESS 0x08
#define CAPTURE_DMA_ADDR        0x10
#define CAPTURE_BUFFER_SIZE     0x11
#define CAPTURE_POINTER         0x12
#define CAPTURE_FIFO_OFFSET_ADDRESS 0x13
#define BASIC_INTERRUPT         0x40    /* start bits + playback rate */
#define SPCS0                   0x41
#define SPCS1                   0x42
#define SPCS2                   0x43
#define SPCS3                   0x44
#define SPDIF_SELECT1           0x45
#define CAPTURE_SOURCE          0x60
#define CAPTURE_VOLUME1         0x61
#define CAPTURE_VOLUME2         0x62
#define PLAYBACK_ROUTING1       0x63
#define PLAYBACK_ROUTING2       0x64
#define PLAYBACK_MUTE           0x65
#define PLAYBACK_VOLUME1        0x66    /* SPDIF/mixer volume, attenuation */
#define CAPTURE_ROUTING1        0x67
#define CAPTURE_ROUTING2        0x68
#define CAPTURE_MUTE            0x69
#define PLAYBACK_VOLUME2        0x6a    /* analog volume, attenuation */
#define SAMPLE_RATE_TRACKER_STATUS 0x70
#define CAPTURE_CONTROL         0x71    /* ch0: rates/routing, ch1: I2S-in -> playback feedback volume */
#define SPDIF_SELECT2           0x72
#define CHIP_VERSION            0x74
#define EXTENDED_INT_MASK       0x75
#define EXTENDED_INT            0x76
#define COUNTER77               0x77    /* 192000 counts/s, 22 bit */
#define COUNTER78               0x78    /* 44100 counts/s, 22 bit */
#define SPI                     0x7a
#define I2C_A                   0x7b
#define I2C_D0                  0x7c
#define I2C_D1                  0x7d

/* SPDIF channel status default (48 kHz, consumer, copyright unasserted) */
#define SPCS_DEFAULT            0x02109204UL

/* EXTENDED_INT bits (channel 0); in single-buffer mode the playback bits
 * signal half and end of the buffer (OSS: AI_PFH / AI_PFF) */
#define EXTINT_PLAY_HALF        0x00000001UL
#define EXTINT_PLAY_FULL        0x00000010UL
#define EXTINT_CAP_HALF_BUFFER  0x00010000UL
#define EXTINT_CAP_FULL_BUFFER  0x00100000UL

/* ---- I2C (WM8775 ADC) ---- */
#define I2C_A_ADC_ADD           0x00000034
#define I2C_A_ADC_START         0x00000100
#define I2C_A_ADC_ABORT         0x00000200
#define I2C_A_ADC_LAST          0x00000400

#define ADC_TIMEOUT             0x07
#define ADC_IFC_CTRL            0x0b
#define ADC_MASTER              0x0c
#define ADC_POWER               0x0d
#define ADC_ATTEN_ADCL          0x0e
#define ADC_ATTEN_ADCR          0x0f
#define ADC_ALC_CTRL1           0x10
#define ADC_ALC_CTRL2           0x11
#define ADC_ALC_CTRL3           0x12
#define ADC_NOISE_CTRL          0x13
#define ADC_LIMIT_CTRL          0x14
#define ADC_MUX                 0x15
#define ADC_RESET               0x17

#define ADC_MUX_PHONE           0x01    /* TAD header */
#define ADC_MUX_MIC             0x02
#define ADC_MUX_LINEIN          0x04
#define ADC_MUX_AUX             0x08

#define ADC_GAIN_0DB            0xcf    /* 0.5 dB steps, 0x01=-103 dB, 0xff=+24 dB */

/* ---- SPI (WM8768 DAC) ---- */
#define SPI_REG_SHIFT           9
#define SPI_REG(r, v)           (((r) << SPI_REG_SHIFT) | (v))
#define SPI_LDA1_REG            0
#define SPI_RDA1_REG            1
#define SPI_PL_REG              2
#define SPI_FMT_REG             3
#define SPI_LDA2_REG            4
#define SPI_RDA2_REG            5
#define SPI_LDA3_REG            6
#define SPI_RDA3_REG            7
#define SPI_MASTDA_REG          8
#define SPI_MS_REG              10
#define SPI_LDA4_REG            13
#define SPI_RDA4_REG            14
#define SPI_DACD4_REG           15

#define SPI_DA_BIT_UPDATE       (1 << 8)
#define SPI_DA_BIT_0dB          0xff
#define SPI_PL_BIT_L_L          (1 << 5)
#define SPI_PL_BIT_R_R          (2 << 7)
#define SPI_IZD_BIT             (0 << 4)
#define SPI_FMT_BIT_I2S         (2 << 0)
#define SPI_IWL_BIT_24          (2 << 4)
#define SPI_DACD0_BIT           (1 << 1)
#define SPI_DACD1_BIT           (1 << 2)
#define SPI_DACD2_BIT           (1 << 3)
#define SPI_DACD4_BIT           (1 << 0)

/* ---- PCI ids ---- */
#define CA0106_VENDOR           0x1102
#define CA0106_DEVICE           0x0007
#define SERIAL_SB0570           0x100a1102UL
#define SERIAL_SB0570A          0x10111102UL

#endif /* CA0106_REGS_H */
