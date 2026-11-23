#ifndef IPO_PUBLIC_AUDIO_H
#define IPO_PUBLIC_AUDIO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <driver/audio_core.h>
#include <driver/audio_fp.h>

#define IPO_AUDIO_API_VERSION        0x00010000u

#define IPO_AUDIO_DIR_PLAYBACK       1u
#define IPO_AUDIO_DIR_CAPTURE        2u

#define IPO_AUDIO_CAP_PLAYBACK       (1u << 0)
#define IPO_AUDIO_CAP_CAPTURE        (1u << 1)
#define IPO_AUDIO_CAP_IRQ_DRIVEN     (1u << 2)
#define IPO_AUDIO_CAP_DMA            (1u << 3)
#define IPO_AUDIO_CAP_MIXING         (1u << 4)
#define IPO_AUDIO_CAP_ARB_FP         (1u << 5)

#define IPO_AUDIO_FMT_MASK_U8        (1u << 0)
#define IPO_AUDIO_FMT_MASK_S16       (1u << 1)
#define IPO_AUDIO_FMT_MASK_S24       (1u << 2)
#define IPO_AUDIO_FMT_MASK_S32       (1u << 3)
#define IPO_AUDIO_FMT_MASK_F32       (1u << 4)
#define IPO_AUDIO_FMT_MASK_F64       (1u << 5)

#define IPO_AUDIO_OK                 0
#define IPO_AUDIO_ERR_INVALID        (-1)
#define IPO_AUDIO_ERR_UNSUPPORTED    (-2)
#define IPO_AUDIO_ERR_NO_DEVICE      (-3)
#define IPO_AUDIO_ERR_NOMEM          (-4)
#define IPO_AUDIO_ERR_BUSY           (-5)

typedef uint32_t ipo_audio_handle_t;

typedef struct ipo_audio_device_info {
    uint32_t struct_size;
    uint32_t device_id;
    char    *name;
    uint32_t name_cap;
    uint32_t name_len;
    uint32_t backend;
    uint32_t caps_flags;
    uint32_t pcm_formats_mask;
    uint32_t native_sample_rate;
    uint32_t min_sample_rate;
    uint32_t max_sample_rate;
    uint16_t max_channels;
    uint8_t  fp_arbitrary_supported;
    uint8_t  capture_supported;
    uint32_t ring_frames;
    uint32_t period_frames;
} ipo_audio_device_info_t;

typedef struct ipo_audio_open_params {
    uint32_t struct_size;
    uint32_t device_id;
    uint32_t direction;      /* IPO_AUDIO_DIR_PLAYBACK or CAPTURE */
    int32_t  format;         /* AUDIO_FORMAT_* */
    int32_t  channels;       /* 1 or 2 */
    uint32_t sample_rate;    /* Hz */
    uint32_t buffer_frames;  /* Kernel ring buffer capacity in frames (0 = auto) */
    uint8_t  use_fp_config;
    uint8_t  reserved0;
    uint16_t reserved1;
    audio_fp_config_t fp_config;
} ipo_audio_open_params_t;

typedef struct ipo_audio_clock {
    uint32_t struct_size;
    uint32_t sample_rate;
    uint64_t played_frames;  /* Estimated source frames already played by DAC */
    uint32_t played_ms;
    uint32_t queued_frames;  /* Remaining source frames queued ahead of DAC */
    uint32_t latency_us;
    uint32_t underruns;
    uint32_t overruns;
    uint32_t irq_count;
    uint8_t  running;
    uint8_t  paused;
    uint16_t reserved;
} ipo_audio_clock_t;

uint32_t ipo_audio_api_version(void);
int      ipo_audio_get_device_count(void);
int      ipo_audio_get_device_info(uint32_t device_id, ipo_audio_device_info_t *out_info);

int      ipo_audio_open(const ipo_audio_open_params_t *params, ipo_audio_handle_t *out_handle);
int      ipo_audio_close(ipo_audio_handle_t handle);
int      ipo_audio_write(ipo_audio_handle_t handle, const void *pcm_data, size_t byte_count);
int      ipo_audio_read(ipo_audio_handle_t handle, void *out_buf, size_t max_bytes);
int      ipo_audio_pause(ipo_audio_handle_t handle, bool paused);
int      ipo_audio_set_volume(ipo_audio_handle_t handle, float volume);
int      ipo_audio_get_clock(ipo_audio_handle_t handle, ipo_audio_clock_t *out_clock);
int      ipo_audio_get_stream_clock(audio_stream_t *stream, ipo_audio_clock_t *out_clock);
int      ipo_audio_set_master_volume(uint8_t left_0_255, uint8_t right_0_255);
int      ipo_audio_get_master_volume(uint8_t *out_left, uint8_t *out_right);

#endif /* IPO_PUBLIC_AUDIO_H */
