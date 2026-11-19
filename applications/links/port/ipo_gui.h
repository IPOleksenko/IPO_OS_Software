#ifndef LINKS_IPO_GUI_H
#define LINKS_IPO_GUI_H

#include <stdint.h>
#include <stddef.h>

#define IPO_GUI_DEFAULT_W 1280
#define IPO_GUI_DEFAULT_H 720
#define IPO_GUI_MIN_W     640
#define IPO_GUI_MIN_H     360

int ipo_gui_w(void);
int ipo_gui_h(void);
int ipo_tabbar_h(void);
int ipo_toolbar_h(void);
int ipo_bookmarks_h(void);
int ipo_top_chrome(void);
int ipo_statusbar_h(void);
#define IPO_GUI_W (ipo_gui_w())
#define IPO_GUI_H (ipo_gui_h())

#define IPO_TABBAR_H    (ipo_tabbar_h())
#define IPO_TOOLBAR_H   (ipo_toolbar_h())
#define IPO_BOOKMARKS_H (ipo_bookmarks_h())
#define IPO_TOP_CHROME  (ipo_top_chrome())
#define IPO_STATUSBAR_H (ipo_statusbar_h())

struct graphics_device;

/* Lifecycle */
void ipo_gui_init(void);
void ipo_gui_shutdown(void);

/* Virtual tab / graphics_device hooks from ipo_driver.c */
void ipo_gui_on_device_created(struct graphics_device *dev);
void ipo_gui_on_device_destroyed(struct graphics_device *dev);
struct graphics_device *ipo_gui_active_dev(void);
int ipo_gui_is_viewport_live(struct graphics_device *dev);

/* Active web viewport geometry inside the current GUI surface */
int ipo_gui_vp_x(void);
int ipo_gui_vp_y(void);
int ipo_gui_vp_w(void);
int ipo_gui_vp_h(void);

/* 32-bit TrueColor (0x00RRGGBB) master compositor buffer.
 * Bit 24 marks continuous-tone image/video pixels eligible for gentle dithering;
 * all UI chrome, text glyphs, and solid CSS fills keep bit 24 cleared for zero-grain edges. */
#define IPO_PX_DITHER 0x01000000u

uint32_t *ipo_gui_fb32(void);
void ipo_gui_mark_vp_dirty(void);
void ipo_gui_mark_vp_dirty_rows(int y1, int y2);
void ipo_gui_on_scroll(int x1, int y1, int x2, int y2, int scx, int scy);
void ipo_gui_mark_chrome_dirty(void);
void ipo_gui_present_now(void);

/* Hooks called from Links core / select / video_player */
void ipo_gui_set_title(struct graphics_device *dev, const char *title);
void ipo_gui_notify_status(void *ses_ptr);
int  ipo_gui_maybe_intercept_url(void *ses_ptr, const char *url);
void ipo_gui_tick(void);
void ipo_gui_request_initial_video(const char *url);
int  ipo_gui_standalone_video_player(const char *url);

#endif
