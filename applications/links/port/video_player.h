#ifndef LINKS_VIDEO_PLAYER_H
#define LINKS_VIDEO_PLAYER_H

#ifdef __cplusplus
extern "C" {
#endif

int links_is_youtube_url(const char *url);
int links_play_youtube_video(const char *url);
int links_download_file(const char *url, const char *outfile);

#ifdef __cplusplus
}
#endif

#endif /* LINKS_VIDEO_PLAYER_H */
