#ifndef __RA_OFFLINE_H__
#define __RA_OFFLINE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "libretro.h"

struct ra_offline_host {
	void *(*get_memory_data)(unsigned id);
	size_t (*get_memory_size)(unsigned id);
	void (*show_toast)(const char *line1, const char *line2, unsigned msec);
	int (*unzip_to_tmp)(const char *zip_path, char *out_tmp, size_t out_len);
};

void ra_offline_init_host(const struct ra_offline_host *host);
void ra_offline_set_memory_maps(const struct retro_memory_map *mmap);
void ra_offline_set_support_achievements(bool supported);

void ra_offline_on_game_load(const char *path, const char *tmpfile,
                             const void *rom_data, size_t rom_size,
                             const char *core_name);
void ra_offline_defer_game_load(const char *path, const char *tmpfile,
                                const void *rom_data, size_t rom_size,
                                const char *core_name);
void ra_offline_commit_deferred_load(void);
void ra_offline_on_game_unload(void);
void ra_offline_do_frame(void);
void ra_offline_idle(void);
void ra_offline_reset(void);

void ra_offline_save_state(const char *state_path);
void ra_offline_load_state(const char *state_path);

bool ra_offline_is_active(void);
const char *ra_offline_get_game_hash(void);
uint32_t ra_offline_get_game_id(void);
void ra_offline_get_status(char *buf, size_t len);
int  ra_offline_get_achievement_count(void);
int  ra_offline_format_achievement(int idx, char *buf, size_t len);

/* Exposed for unit testing against ra_clock.py and custom clocks */
int64_t ra_offline_now_utc_from(int64_t kernel_now);
int64_t ra_offline_now_utc(void);

#endif /* __RA_OFFLINE_H__ */
