#ifndef LINKS_MP4_PLAYER_H
#define LINKS_MP4_PLAYER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mp4_player mp4_player_t;

mp4_player_t *mp4_player_create(void);
void          mp4_player_destroy(mp4_player_t *p);

/* Starts resolving (YouTube watch/id) or streaming a progressive MP4 URL asynchronously. */
int           mp4_player_open(mp4_player_t *p, const char *url);
void          mp4_player_stop(mp4_player_t *p);

/* Advances network/demux/H.264/AAC/SB16 state non-blockingly. Safe to call every UI tick. */
void          mp4_player_tick(mp4_player_t *p, uint32_t now_ms, int target_w, int target_h);

void          mp4_player_set_paused(mp4_player_t *p, bool paused);
void          mp4_player_set_muted(mp4_player_t *p, bool muted);
void          mp4_player_seek_ms(mp4_player_t *p, uint32_t pos_ms);

bool          mp4_player_is_paused(const mp4_player_t *p);
bool          mp4_player_is_muted(const mp4_player_t *p);
uint32_t      mp4_player_pos_ms(const mp4_player_t *p);
uint32_t      mp4_player_duration_ms(const mp4_player_t *p);
const char   *mp4_player_title(const mp4_player_t *p);
const char   *mp4_player_status_text(const mp4_player_t *p);
bool          mp4_player_has_frame(const mp4_player_t *p);
const uint32_t *mp4_player_frame_pixels(const mp4_player_t *p, int *out_w, int *out_h,
                                        int *out_src_w, int *out_src_h, uint32_t *out_decoded_cnt);
bool          mp4_player_take_dirty(mp4_player_t *p);
bool          mp4_player_has_audio(const mp4_player_t *p);
int           mp4_player_audio_rate(const mp4_player_t *p);

#ifdef __cplusplus
}
#endif

#endif /* LINKS_MP4_PLAYER_H */
