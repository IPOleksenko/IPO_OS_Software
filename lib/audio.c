/*
 * Userspace wrappers for kernel audio stream management syscalls.
 */

#include <driver/audio_core.h>
#include <driver/sound.h>
#include <ipo/audio.h>
#include <syscall.h>
#include <stdlib.h>
#include <string.h>

bool audio_init(void) {
    return ipo_syscall(IPO_SYSCALL_AUDIO_INIT, 0u, NULL) != 0;
}

int audio_get_active_backend(void) {
    ipo_audio_device_info_t info;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    if (ipo_audio_get_device_count() > 0 && ipo_audio_get_device_info(0u, &info) == IPO_AUDIO_OK) {
        return (int)info.backend;
    }
    return AUDIO_BACKEND_NONE;
}

const char *audio_get_backend_name(void) {
    static char *s_name = NULL;
    static size_t s_name_cap = 0u;
    ipo_audio_device_info_t info;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    if (ipo_audio_get_device_count() > 0 && ipo_audio_get_device_info(0u, &info) == IPO_AUDIO_OK &&
        info.name && info.name[0]) {
        size_t len = strlen(info.name);
        if (len + 1u > s_name_cap) {
            size_t ncap = len + 1u;
            char *nb = (char *)realloc(s_name, ncap);
            if (!nb) return "Unknown";
            s_name = nb;
            s_name_cap = ncap;
        }
        memcpy(s_name, info.name, len + 1u);
        return s_name;
    }
    return "None";
}

audio_stream_t *audio_stream_create(int format, int channels, uint32_t sample_rate, const void *data, size_t size) {
    uint32_t args[5];
    args[0] = (uint32_t)format;
    args[1] = (uint32_t)channels;
    args[2] = sample_rate;
    args[3] = (uint32_t)(uintptr_t)data;
    args[4] = (uint32_t)size;
    return (audio_stream_t *)(uintptr_t)ipo_syscall(IPO_SYSCALL_AUDIO_STREAM_CREATE, 5u, args);
}

audio_stream_t *audio_stream_create_ring(int format, int channels, uint32_t sample_rate,
                                         const void *ring_buf, size_t ring_size_samples,
                                         size_t total_expected_samples) {
    uint32_t args[6];
    args[0] = (uint32_t)format;
    args[1] = (uint32_t)channels;
    args[2] = sample_rate;
    args[3] = (uint32_t)(uintptr_t)ring_buf;
    args[4] = (uint32_t)ring_size_samples;
    args[5] = (uint32_t)total_expected_samples;
    return (audio_stream_t *)(uintptr_t)ipo_syscall(IPO_SYSCALL_AUDIO_STREAM_CREATE_RING, 6u, args);
}

void audio_stream_update_buffered(audio_stream_t *stream, size_t buffered_samples, bool is_eof) {
    uint32_t args[3];
    args[0] = (uint32_t)(uintptr_t)stream;
    args[1] = (uint32_t)buffered_samples;
    args[2] = is_eof ? 1u : 0u;
    ipo_syscall(IPO_SYSCALL_AUDIO_STREAM_UPDATE, 3u, args);
}

void audio_stream_destroy(audio_stream_t *stream) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)stream;
    ipo_syscall(IPO_SYSCALL_AUDIO_STREAM_DESTROY, 1u, args);
}

void audio_stream_play(audio_stream_t *stream) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)stream;
    ipo_syscall(IPO_SYSCALL_AUDIO_STREAM_PLAY, 1u, args);
}

void audio_stream_pause(audio_stream_t *stream) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)stream;
    ipo_syscall(IPO_SYSCALL_AUDIO_STREAM_PAUSE, 1u, args);
}

void audio_stream_stop(audio_stream_t *stream) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)stream;
    ipo_syscall(IPO_SYSCALL_AUDIO_STREAM_STOP, 1u, args);
}

void audio_stream_set_volume(audio_stream_t *stream, float volume) {
    uint32_t args[2];
    args[0] = (uint32_t)(uintptr_t)stream;
    args[1] = (uint32_t)(uintptr_t)&volume;
    ipo_syscall(IPO_SYSCALL_AUDIO_STREAM_SET_VOL, 2u, args);
}

void audio_stream_seek(audio_stream_t *stream, double progress_fraction) {
    uint32_t args[2];
    args[0] = (uint32_t)(uintptr_t)stream;
    args[1] = (uint32_t)(uintptr_t)&progress_fraction;
    ipo_syscall(IPO_SYSCALL_AUDIO_STREAM_SEEK, 2u, args);
}

double audio_stream_get_progress(audio_stream_t *stream) {
    double prog = 0.0;
    uint32_t args[2];
    args[0] = (uint32_t)(uintptr_t)stream;
    args[1] = (uint32_t)(uintptr_t)&prog;
    ipo_syscall(IPO_SYSCALL_AUDIO_STREAM_PROGRESS, 2u, args);
    return prog;
}

void audio_tick(void) {
    ipo_syscall(IPO_SYSCALL_AUDIO_TICK, 0u, NULL);
}

bool audio_set_fp_config(const audio_fp_config_t *cfg) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)cfg;
    return ipo_syscall(IPO_SYSCALL_AUDIO_SET_FP_CONFIG, 1u, args) != 0;
}

bool audio_get_fp_config(audio_fp_config_t *out_cfg) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)out_cfg;
    return ipo_syscall(IPO_SYSCALL_AUDIO_GET_FP_CONFIG, 1u, args) != 0;
}

bool audio_get_stats(audio_stats_t *out_stats) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)out_stats;
    return ipo_syscall(IPO_SYSCALL_AUDIO_GET_STATS, 1u, args) != 0;
}

bool audio_fp_format_normalize(const audio_fp_format_t *in, audio_fp_format_t *out) {
    if (!in) return false;
    uint8_t sign = in->is_signed ? 1u : 0u;
    uint32_t sum = (uint32_t)in->int_bits + (uint32_t)in->frac_bits + (uint32_t)sign;
    if (sum < 1u || sum > 64u) return false;
    uint8_t total = (in->total_bits == 0u) ? (uint8_t)sum : in->total_bits;
    if (total < sum || total > 64u) return false;
    if (in->round_mode > AUDIO_FP_ROUND_HALF_EVEN) return false;
    if (in->overflow_mode > AUDIO_FP_OVERFLOW_WRAP) return false;
    if (out) {
        *out = *in;
        out->total_bits = total;
        out->is_signed = sign;
        out->reserved = 0u;
    }
    return true;
}

bool audio_fp_format_is_valid(const audio_fp_format_t *fmt) {
    return audio_fp_format_normalize(fmt, NULL);
}

audio_fp_format_t audio_fp_format_make(uint8_t int_bits, uint8_t frac_bits, bool is_signed,
                                       audio_fp_round_t round_mode,
                                       audio_fp_overflow_t overflow_mode) {
    audio_fp_format_t f;
    f.int_bits = int_bits;
    f.frac_bits = frac_bits;
    uint32_t total = (uint32_t)int_bits + (uint32_t)frac_bits + (is_signed ? 1u : 0u);
    f.total_bits = (total <= 64u) ? (uint8_t)total : 0u;
    f.is_signed = is_signed ? 1u : 0u;
    f.round_mode = (uint8_t)round_mode;
    f.overflow_mode = (uint8_t)overflow_mode;
    f.reserved = 0u;
    return f;
}

audio_fp_format_t audio_fp_format_make_ex(uint8_t total_bits, uint8_t int_bits, uint8_t frac_bits,
                                          bool is_signed, audio_fp_round_t round_mode,
                                          audio_fp_overflow_t overflow_mode) {
    audio_fp_format_t f;
    f.int_bits = int_bits;
    f.frac_bits = frac_bits;
    f.total_bits = total_bits;
    f.is_signed = is_signed ? 1u : 0u;
    f.round_mode = (uint8_t)round_mode;
    f.overflow_mode = (uint8_t)overflow_mode;
    f.reserved = 0u;
    return f;
}

void audio_fp_default_config(audio_fp_config_t *cfg) {
    if (!cfg) return;
    cfg->pos_format = audio_fp_format_make_ex(
        64u, 39u, 24u, true, AUDIO_FP_ROUND_HALF_EVEN, AUDIO_FP_OVERFLOW_SATURATE);
    cfg->mix_format = audio_fp_format_make_ex(
        32u, 7u, 24u, true, AUDIO_FP_ROUND_HALF_EVEN, AUDIO_FP_OVERFLOW_SATURATE);
}

uint32_t ipo_audio_api_version(void) {
    return (uint32_t)ipo_syscall(IPO_SYSCALL_AUDIO_API_VERSION, 0u, NULL);
}

int ipo_audio_get_device_count(void) {
    return (int)ipo_syscall(IPO_SYSCALL_AUDIO_GET_DEV_COUNT, 0u, NULL);
}

int ipo_audio_get_device_info(uint32_t device_id, ipo_audio_device_info_t *out_info) {
    if (!out_info) return IPO_AUDIO_ERR_INVALID;
    static char *s_dev_name = NULL;
    static uint32_t s_dev_name_cap = 0u;
    bool use_internal = (out_info->name == NULL || out_info->name_cap == 0u);
    if (use_internal) {
        if (!s_dev_name || s_dev_name_cap < 64u) {
            char *nb = (char *)realloc(s_dev_name, 64u);
            if (nb) {
                s_dev_name = nb;
                s_dev_name_cap = 64u;
            }
        }
        out_info->name = s_dev_name;
        out_info->name_cap = s_dev_name_cap;
    }
    uint32_t args[2] = { device_id, (uint32_t)(uintptr_t)out_info };
    int rc = (int)ipo_syscall(IPO_SYSCALL_AUDIO_GET_DEV_INFO, 2u, args);
    if (rc == IPO_AUDIO_OK && use_internal && out_info->name_len + 1u > s_dev_name_cap) {
        uint32_t ncap = out_info->name_len + 1u;
        char *nb = (char *)realloc(s_dev_name, ncap);
        if (nb) {
            s_dev_name = nb;
            s_dev_name_cap = ncap;
            out_info->name = s_dev_name;
            out_info->name_cap = s_dev_name_cap;
            rc = (int)ipo_syscall(IPO_SYSCALL_AUDIO_GET_DEV_INFO, 2u, args);
        }
    }
    return rc;
}

int ipo_audio_open(const ipo_audio_open_params_t *params, ipo_audio_handle_t *out_handle) {
    uint32_t args[2] = { (uint32_t)(uintptr_t)params, (uint32_t)(uintptr_t)out_handle };
    return (int)ipo_syscall(IPO_SYSCALL_AUDIO_OPEN, 2u, args);
}

int ipo_audio_close(ipo_audio_handle_t handle) {
    uint32_t args[1] = { handle };
    return (int)ipo_syscall(IPO_SYSCALL_AUDIO_CLOSE, 1u, args);
}

int ipo_audio_write(ipo_audio_handle_t handle, const void *pcm_data, size_t byte_count) {
    uint32_t args[3] = { handle, (uint32_t)(uintptr_t)pcm_data, (uint32_t)byte_count };
    return (int)ipo_syscall(IPO_SYSCALL_AUDIO_WRITE, 3u, args);
}

int ipo_audio_read(ipo_audio_handle_t handle, void *out_buf, size_t max_bytes) {
    uint32_t args[3] = { handle, (uint32_t)(uintptr_t)out_buf, (uint32_t)max_bytes };
    return (int)ipo_syscall(IPO_SYSCALL_AUDIO_READ, 3u, args);
}

int ipo_audio_pause(ipo_audio_handle_t handle, bool paused) {
    uint32_t args[2] = { handle, paused ? 1u : 0u };
    return (int)ipo_syscall(IPO_SYSCALL_AUDIO_HANDLE_PAUSE, 2u, args);
}

int ipo_audio_set_volume(ipo_audio_handle_t handle, float volume) {
    uint32_t args[2] = { handle, (uint32_t)(uintptr_t)&volume };
    return (int)ipo_syscall(IPO_SYSCALL_AUDIO_HANDLE_SET_VOL, 2u, args);
}

int ipo_audio_get_clock(ipo_audio_handle_t handle, ipo_audio_clock_t *out_clock) {
    uint32_t args[2] = { handle, (uint32_t)(uintptr_t)out_clock };
    return (int)ipo_syscall(IPO_SYSCALL_AUDIO_HANDLE_CLOCK, 2u, args);
}

int ipo_audio_get_stream_clock(audio_stream_t *stream, ipo_audio_clock_t *out_clock) {
    uint32_t args[2] = { (uint32_t)(uintptr_t)stream, (uint32_t)(uintptr_t)out_clock };
    return (int)ipo_syscall(IPO_SYSCALL_AUDIO_STREAM_CLOCK, 2u, args);
}

int ipo_audio_set_master_volume(uint8_t left_0_255, uint8_t right_0_255) {
    uint32_t args[2] = { left_0_255, right_0_255 };
    return (int)ipo_syscall(IPO_SYSCALL_AUDIO_SET_MASTER_VOL, 2u, args);
}

int ipo_audio_get_master_volume(uint8_t *out_left, uint8_t *out_right) {
    uint32_t args[2] = { (uint32_t)(uintptr_t)out_left, (uint32_t)(uintptr_t)out_right };
    return (int)ipo_syscall(IPO_SYSCALL_AUDIO_GET_MASTER_VOL, 2u, args);
}

void sb16_stop(void) {
    ipo_syscall(IPO_SYSCALL_AUDIO_STOP_ALL, 0u, NULL);
}

void audio_stop_all(void) {
    ipo_syscall(IPO_SYSCALL_AUDIO_STOP_ALL, 0u, NULL);
}

void sound_init(void) {}
void sound_play(uint32_t frequency) { (void)frequency; }
void sound_stop(void) {}
void sound_beep(uint32_t frequency, uint32_t duration) { (void)frequency; (void)duration; }

