#define _POSIX_C_SOURCE 200809L
#include "ra_offline.h"

#include "rc_api_request.h"
#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_error.h"
#include "rc_hash.h"
#include "rc_libretro.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RA_STATE_MAGIC "RAOF"
#define RA_MAX_PATH 1024
#define RA_MAX_JSON (8 * 1024 * 1024)
#define RA_FAST_HASH_MAX_BYTES (4U * 1024U * 1024U)

static struct {
	struct ra_offline_host host;
	bool has_host;
	bool support_achievements;

	rc_client_t *client;
	rc_libretro_memory_regions_t regions;
	bool has_regions;

	struct retro_memory_descriptor *mmap_descs;
	unsigned mmap_count;
	bool has_mmap;

	bool loaded;
	bool active;
	uint32_t console_id;
	uint32_t game_id;
	char game_hash[33];
	char game_title[256];
	char rom_path[RA_MAX_PATH];
	char status_msg[128];

	bool has_deferred_load;
	char deferred_path[RA_MAX_PATH];
	char deferred_tmpfile[RA_MAX_PATH];
	const void *deferred_rom_data;
	size_t deferred_rom_size;
	char deferred_core_name[128];

	char cached_set_hash[33];
	char *cached_set_json;
	size_t cached_set_len;
} g_ra = {
	.support_achievements = true,
	.status_msg = "Idle"
};

static void clear_cached_set_json(void)
{
	if (g_ra.cached_set_json) {
		free(g_ra.cached_set_json);
		g_ra.cached_set_json = NULL;
	}
	g_ra.cached_set_len = 0;
	g_ra.cached_set_hash[0] = '\0';
}

/* ---------- path & file helpers ---------- */

static const char *ra_data_dir(void)
{
	const char *env = getenv("RA_DATA");
	return (env && env[0]) ? env : "/mnt/FunKey/raoffline";
}

static int ensure_dir_recursive(const char *path)
{
	char tmp[RA_MAX_PATH];
	size_t len;
	char *p;

	if (!path || !path[0])
		return -1;
	len = strlen(path);
	if (len >= sizeof(tmp))
		return -1;
	memcpy(tmp, path, len + 1);

	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
				return -1;
			*p = '/';
		}
	}
	if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
		return -1;
	return 0;
}

static char *read_entire_file(const char *path, size_t *out_len)
{
	FILE *f;
	long sz;
	char *buf;
	size_t n;

	if (out_len)
		*out_len = 0;
	f = fopen(path, "rb");
	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	sz = ftell(f);
	if (sz < 0 || (size_t)sz > RA_MAX_JSON) {
		fclose(f);
		return NULL;
	}
	rewind(f);
	buf = (char *)malloc((size_t)sz + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	n = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	buf[n] = '\0';
	if (out_len)
		*out_len = n;
	return buf;
}

static int write_atomic_file(const char *path, const void *data, size_t len)
{
	char tmp_path[RA_MAX_PATH + 16];
	char dir_path[RA_MAX_PATH];
	const char *slash;
	int fd;
	size_t off = 0;

	slash = strrchr(path, '/');
	if (slash && (size_t)(slash - path) < sizeof(dir_path)) {
		memcpy(dir_path, path, (size_t)(slash - path));
		dir_path[slash - path] = '\0';
		ensure_dir_recursive(dir_path);
	}

	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld", path, (long)getpid());
	fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;

	while (off < len) {
		ssize_t w = write(fd, (const uint8_t *)data + off, len - off);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			unlink(tmp_path);
			return -1;
		}
		off += (size_t)w;
	}

	fsync(fd);
	close(fd);
	if (rename(tmp_path, path) != 0) {
		unlink(tmp_path);
		return -1;
	}
	return 0;
}

/* ---------- POSIX TZ & UTC clock normalization (matches ra_clock.py) ---------- */

static int parse_hms_signed(const char **p, int default_west_sign, int *out_sec)
{
	const char *s = *p;
	int sign = default_west_sign;
	long h = 0, m = 0, sec = 0;

	if (*s == '+' || *s == '-') {
		sign = (*s == '-') ? -1 : 1;
		s++;
	}
	if (!isdigit((unsigned char)*s))
		return -1;
	h = strtol(s, (char **)&s, 10);
	if (*s == ':') {
		s++;
		if (!isdigit((unsigned char)*s))
			return -1;
		m = strtol(s, (char **)&s, 10);
		if (*s == ':') {
			s++;
			if (!isdigit((unsigned char)*s))
				return -1;
			sec = strtol(s, (char **)&s, 10);
		}
	}
	*p = s;
	*out_sec = (int)(sign * (h * 3600L + m * 60L + sec));
	return 0;
}

static int parse_tz_abbr(const char **p)
{
	const char *s = *p;
	if (*s == '<') {
		s++;
		while (*s && *s != '>')
			s++;
		if (*s != '>')
			return -1;
		*p = s + 1;
		return 0;
	}
	if (!isalpha((unsigned char)*s))
		return -1;
	while (isalpha((unsigned char)*s))
		s++;
	*p = s;
	return 0;
}

static int is_leap_year(int year)
{
	return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int days_in_month_utc(int year, int month)
{
	static const int dm[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	if (month == 2 && is_leap_year(year))
		return 29;
	if (month < 1 || month > 12)
		return 30;
	return dm[month - 1];
}

/* Howard Hinnant's civil/days algorithms: 1970-01-01 = day 0, Thursday (wday 4, 0=Sun) */
static int64_t days_from_civil(int y, int m, int d)
{
	y -= (m <= 2);
	int era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = (unsigned)(y - era * 400);
	unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static int year_from_epoch(int64_t epoch_sec)
{
	int64_t z = (epoch_sec >= 0) ? (epoch_sec / 86400) : ((epoch_sec - 86399) / 86400);
	z += 719468;
	int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	unsigned doe = (unsigned)(z - era * 146097);
	unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int y = (int)yoe + (int)era * 400;
	unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	unsigned mp = (5 * doy + 2) / 153;
	unsigned m = mp + (mp < 10 ? 3 : -9);
	return y + (m <= 2);
}

static int64_t nth_weekday_epoch(int year, int month, int week, int wday)
{
	int64_t d1 = days_from_civil(year, month, 1);
	/* 1970-01-01 is wday 4 (Thu) in 0=Sun..6=Sat */
	int first_wday = (int)(((d1 + 4) % 7 + 7) % 7);
	int day = 1 + ((wday - first_wday + 7) % 7) + (week - 1) * 7;
	int dim = days_in_month_utc(year, month);
	while (day > dim)
		day -= 7;
	return days_from_civil(year, month, day) * 86400LL;
}

static int parse_rule_epoch(int year, const char *rule, int64_t *out_epoch)
{
	char date_part[32];
	const char *slash = strchr(rule, '/');
	size_t dlen;
	int time_sec = 2 * 3600;

	if (slash) {
		const char *tp = slash + 1;
		dlen = (size_t)(slash - rule);
		if (parse_hms_signed(&tp, 1, &time_sec) != 0 || *tp != '\0')
			return -1;
	} else {
		dlen = strlen(rule);
	}
	if (dlen == 0 || dlen >= sizeof(date_part))
		return -1;
	memcpy(date_part, rule, dlen);
	date_part[dlen] = '\0';

	if (date_part[0] == 'M' || date_part[0] == 'm') {
		int m = 0, w = 0, d = 0;
		if (sscanf(date_part + 1, "%d.%d.%d", &m, &w, &d) != 3)
			return -1;
		if (m < 1 || m > 12 || w < 1 || w > 5 || d < 0 || d > 6)
			return -1;
		*out_epoch = nth_weekday_epoch(year, m, w, d) + time_sec;
		return 0;
	}

	int64_t jan1 = days_from_civil(year, 1, 1) * 86400LL;
	if (date_part[0] == 'J' || date_part[0] == 'j') {
		int jday = atoi(date_part + 1);
		if (jday < 1 || jday > 365)
			return -1;
		int offset_days = jday - 1;
		if (is_leap_year(year) && jday >= 60)
			offset_days += 1;
		*out_epoch = jan1 + (int64_t)offset_days * 86400LL + time_sec;
		return 0;
	}

	if (!isdigit((unsigned char)date_part[0]))
		return -1;
	*out_epoch = jan1 + (int64_t)atoi(date_part) * 86400LL + time_sec;
	return 0;
}

static int posix_tz_local_to_utc(const char *tz, int64_t local_epoch, int64_t *out_utc)
{
	const char *p = tz;
	int std_west_sec = 0;
	int std_east_sec = 0;
	int dst_east_sec = 0;

	if (!tz || !tz[0])
		return -1;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		p++;
	if (*p == ':')
		p++;

	if (parse_tz_abbr(&p) != 0)
		return -1;
	if (parse_hms_signed(&p, 1, &std_west_sec) != 0)
		return -1;
	std_east_sec = -std_west_sec;

	if (*p == '\0' || *p == '\n' || *p == '\r') {
		*out_utc = local_epoch - std_east_sec;
		return 0;
	}

	if (parse_tz_abbr(&p) != 0)
		return -1;

	if (*p != ',' && *p != '\0' && *p != '\n' && *p != '\r') {
		int dst_west_sec = 0;
		if (parse_hms_signed(&p, 1, &dst_west_sec) != 0)
			return -1;
		dst_east_sec = -dst_west_sec;
	} else {
		dst_east_sec = std_east_sec + 3600;
	}

	if (*p != ',') {
		*out_utc = local_epoch - std_east_sec;
		return 0;
	}
	p++;
	const char *comma = strchr(p, ',');
	if (!comma)
		return -1;

	char start_rule[48], end_rule[48];
	size_t slen = (size_t)(comma - p);
	if (slen == 0 || slen >= sizeof(start_rule))
		return -1;
	memcpy(start_rule, p, slen);
	start_rule[slen] = '\0';

	p = comma + 1;
	size_t elen = strcspn(p, " \t\r\n");
	if (elen == 0 || elen >= sizeof(end_rule))
		return -1;
	memcpy(end_rule, p, elen);
	end_rule[elen] = '\0';

	int year = year_from_epoch(local_epoch);
	int64_t s_epoch = 0, e_epoch = 0;
	if (parse_rule_epoch(year, start_rule, &s_epoch) != 0 ||
	    parse_rule_epoch(year, end_rule, &e_epoch) != 0)
		return -1;

	int in_dst;
	if (s_epoch < e_epoch)
		in_dst = (local_epoch >= s_epoch && local_epoch < e_epoch);
	else
		in_dst = (local_epoch >= s_epoch || local_epoch < e_epoch);

	*out_utc = local_epoch - (in_dst ? dst_east_sec : std_east_sec);
	return 0;
}

/* ---------- minimal JSON lookup & escape helpers ---------- */

static const char *skip_ws(const char *p)
{
	while (p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
		p++;
	return p;
}

static void json_escape_append(char **dst, size_t *rem, const char *s);

static const char *find_json_key_val(const char *json, const char *key)
{
	char pat_stack[320];
	char *pat = pat_stack;
	size_t klen;
	size_t cap;
	const char *p = json;
	const char *found = NULL;

	if (!json || !key)
		return NULL;
	klen = strlen(key);
	cap = klen * 2 + 4;
	if (cap > sizeof(pat_stack)) {
		pat = (char *)malloc(cap);
		if (!pat)
			return NULL;
	}

	{
		char *dst = pat;
		size_t rem = cap;
		*dst++ = '"';
		rem--;
		json_escape_append(&dst, &rem, key);
		if (rem < 2) {
			if (pat != pat_stack)
				free(pat);
			return NULL;
		}
		*dst++ = '"';
		*dst = '\0';

		size_t plen = (size_t)(dst - pat);
		while ((p = strstr(p, pat)) != NULL) {
			if (p == json || p[-1] != '\\') {
				const char *after = skip_ws(p + plen);
				if (after && *after == ':') {
					found = skip_ws(after + 1);
					break;
				}
			}
			p += plen;
		}
	}

	if (pat != pat_stack)
		free(pat);
	return found;
}

static int json_get_string(const char *json, const char *key, char *out, size_t out_len)
{
	const char *v = find_json_key_val(json, key);
	size_t i = 0;

	if (!v || *v != '"' || !out || out_len == 0)
		return -1;
	v++;
	while (*v && *v != '"') {
		if (*v == '\\' && v[1]) {
			v++;
			char c = *v++;
			if (c == 'n') c = '\n';
			else if (c == 'r') c = '\r';
			else if (c == 't') c = '\t';
			if (i + 1 < out_len)
				out[i++] = c;
		} else {
			if (i + 1 < out_len)
				out[i++] = *v;
			v++;
		}
	}
	out[i] = '\0';
	return (*v == '"') ? 0 : -1;
}

static int json_get_int64(const char *json, const char *key, int64_t *out)
{
	const char *v = find_json_key_val(json, key);
	char *endp = NULL;
	long long val;

	if (!v)
		return -1;
	if (*v != '-' && !isdigit((unsigned char)*v))
		return -1;
	errno = 0;
	val = strtoll(v, &endp, 10);
	if (errno != 0 || endp == v)
		return -1;
	*out = (int64_t)val;
	return 0;
}

static int json_get_bool(const char *json, const char *key, bool *out)
{
	const char *v = find_json_key_val(json, key);
	if (!v)
		return -1;
	if (strncmp(v, "true", 4) == 0) {
		*out = true;
		return 0;
	}
	if (strncmp(v, "false", 5) == 0) {
		*out = false;
		return 0;
	}
	return -1;
}

static void json_escape_append(char **dst, size_t *rem, const char *s)
{
	if (!s || *rem <= 1)
		return;
	while (*s && *rem > 2) {
		unsigned char c = (unsigned char)*s++;
		if (c == '"' || c == '\\') {
			if (*rem <= 3) break;
			*(*dst)++ = '\\';
			*(*dst)++ = (char)c;
			*rem -= 2;
		} else if (c == '\n') {
			if (*rem <= 3) break;
			*(*dst)++ = '\\';
			*(*dst)++ = 'n';
			*rem -= 2;
		} else if (c < 0x20) {
			/* skip control chars */
		} else {
			*(*dst)++ = (char)c;
			(*rem)--;
		}
	}
	**dst = '\0';
}

/* ---------- public clock API ---------- */

static void load_tz_string(char *tz_out, size_t tz_len, int64_t *utc_offset_out, bool *has_offset_out)
{
	char path[RA_MAX_PATH];
	char *json;

	tz_out[0] = '\0';
	*utc_offset_out = 0;
	*has_offset_out = false;

	snprintf(path, sizeof(path), "%s/clock_sync.json", ra_data_dir());
	json = read_entire_file(path, NULL);
	if (json) {
		json_get_string(json, "posix_tz", tz_out, tz_len);
		if (json_get_int64(json, "utc_offset_sec", utc_offset_out) == 0)
			*has_offset_out = true;
		free(json);
	}

	if (!tz_out[0]) {
		const char *candidates[2] = {"/mnt/FunKey/.tz", "/etc/TZ"};
		for (int i = 0; i < 2; i++) {
			char *buf = read_entire_file(candidates[i], NULL);
			if (buf) {
				size_t n = strcspn(buf, "\r\n");
				if (n > 0 && n < tz_len) {
					memcpy(tz_out, buf, n);
					tz_out[n] = '\0';
					free(buf);
					break;
				}
				free(buf);
			}
		}
	}
}

int64_t ra_offline_now_utc_from(int64_t kernel_now)
{
	char tz[128];
	int64_t offset_sec = 0;
	bool has_offset = false;
	int64_t utc = 0;

	load_tz_string(tz, sizeof(tz), &offset_sec, &has_offset);
	if (tz[0] && posix_tz_local_to_utc(tz, kernel_now, &utc) == 0)
		return utc;
	if (has_offset)
		return kernel_now - offset_sec;
	return kernel_now;
}

int64_t ra_offline_now_utc(void)
{
	return ra_offline_now_utc_from((int64_t)time(NULL));
}

/* ---------- ROM console inference & hash lookup ---------- */

static bool ends_with_ci(const char *s, const char *sfx)
{
	size_t sl = strlen(s), xl = strlen(sfx);
	if (sl < xl)
		return false;
	return strcasecmp(s + sl - xl, sfx) == 0;
}

static uint32_t infer_console_id(const char *path, const char *core_name)
{
	static const struct {
		const char *folder;
		uint32_t id;
	} folder_map[] = {
		{"Game Boy Advance", RC_CONSOLE_GAMEBOY_ADVANCE},
		{"Game Boy Color", RC_CONSOLE_GAMEBOY_COLOR},
		{"Game Boy", RC_CONSOLE_GAMEBOY},
		{"NES", RC_CONSOLE_NINTENDO},
		{"Famicom", RC_CONSOLE_NINTENDO},
		{"SNES", RC_CONSOLE_SUPER_NINTENDO},
		{"Super Famicom", RC_CONSOLE_SUPER_NINTENDO},
		{"Mega Drive", RC_CONSOLE_MEGA_DRIVE},
		{"Genesis", RC_CONSOLE_MEGA_DRIVE},
		{"Master System", RC_CONSOLE_MASTER_SYSTEM},
		{"Game Gear", RC_CONSOLE_GAME_GEAR},
		{"PC Engine", RC_CONSOLE_PC_ENGINE},
		{"TurboGrafx", RC_CONSOLE_PC_ENGINE},
		{"PS1", RC_CONSOLE_PLAYSTATION},
		{"PlayStation", RC_CONSOLE_PLAYSTATION},
		{"Neo Geo Pocket Color", RC_CONSOLE_NEOGEO_POCKET},
		{"Neo Geo Pocket", RC_CONSOLE_NEOGEO_POCKET},
		{"WonderSwan Color", RC_CONSOLE_WONDERSWAN},
		{"WonderSwan", RC_CONSOLE_WONDERSWAN},
		{"Lynx", RC_CONSOLE_ATARI_LYNX},
		{"Atari 2600", RC_CONSOLE_ATARI_2600},
		{"Atari 7800", RC_CONSOLE_ATARI_7800},
		{"ColecoVision", RC_CONSOLE_COLECOVISION},
		{"Pokemon Mini", RC_CONSOLE_POKEMON_MINI},
		{"Vectrex", RC_CONSOLE_VECTREX},
		{"Arcade", RC_CONSOLE_ARCADE},
		{"FBA", RC_CONSOLE_ARCADE},
		{"MAME", RC_CONSOLE_ARCADE},
		{"Neo Geo", RC_CONSOLE_ARCADE},
		{"Virtual Boy", RC_CONSOLE_VIRTUAL_BOY},
		{"Sega 32X", RC_CONSOLE_SEGA_32X},
		{"Sega CD", RC_CONSOLE_SEGA_CD},
	};

	if (path) {
		/* extract parent directory name */
		const char *last_slash = strrchr(path, '/');
		if (last_slash && last_slash > path) {
			const char *prev_slash = last_slash - 1;
			while (prev_slash > path && *prev_slash != '/')
				prev_slash--;
			if (*prev_slash == '/')
				prev_slash++;
			size_t dlen = (size_t)(last_slash - prev_slash);
			for (size_t i = 0; i < sizeof(folder_map) / sizeof(folder_map[0]); i++) {
				if (strlen(folder_map[i].folder) == dlen &&
				    strncasecmp(prev_slash, folder_map[i].folder, dlen) == 0)
					return folder_map[i].id;
			}
		}

		if (ends_with_ci(path, ".gba")) return RC_CONSOLE_GAMEBOY_ADVANCE;
		if (ends_with_ci(path, ".gbc")) return RC_CONSOLE_GAMEBOY_COLOR;
		if (ends_with_ci(path, ".gb")) return RC_CONSOLE_GAMEBOY;
		if (ends_with_ci(path, ".nes") || ends_with_ci(path, ".fds")) return RC_CONSOLE_NINTENDO;
		if (ends_with_ci(path, ".sfc") || ends_with_ci(path, ".smc")) return RC_CONSOLE_SUPER_NINTENDO;
		if (ends_with_ci(path, ".md") || ends_with_ci(path, ".gen") || ends_with_ci(path, ".smd")) return RC_CONSOLE_MEGA_DRIVE;
		if (ends_with_ci(path, ".sms")) return RC_CONSOLE_MASTER_SYSTEM;
		if (ends_with_ci(path, ".gg")) return RC_CONSOLE_GAME_GEAR;
		if (ends_with_ci(path, ".pce")) return RC_CONSOLE_PC_ENGINE;
		if (ends_with_ci(path, ".ngp") || ends_with_ci(path, ".ngc")) return RC_CONSOLE_NEOGEO_POCKET;
		if (ends_with_ci(path, ".ws") || ends_with_ci(path, ".wsc")) return RC_CONSOLE_WONDERSWAN;
		if (ends_with_ci(path, ".lnx")) return RC_CONSOLE_ATARI_LYNX;
		if (ends_with_ci(path, ".a26")) return RC_CONSOLE_ATARI_2600;
		if (ends_with_ci(path, ".a78")) return RC_CONSOLE_ATARI_7800;
		if (ends_with_ci(path, ".min")) return RC_CONSOLE_POKEMON_MINI;
		if (ends_with_ci(path, ".vec")) return RC_CONSOLE_VECTREX;
		if (ends_with_ci(path, ".vb") || ends_with_ci(path, ".vboy")) return RC_CONSOLE_VIRTUAL_BOY;
		if (ends_with_ci(path, ".32x")) return RC_CONSOLE_SEGA_32X;
	}

	if (core_name) {
		if (strstr(core_name, "gpsp") || strstr(core_name, "mgba")) return RC_CONSOLE_GAMEBOY_ADVANCE;
		if (strstr(core_name, "gambatte") || strstr(core_name, "tgb")) return RC_CONSOLE_GAMEBOY_COLOR;
		if (strstr(core_name, "fceumm") || strstr(core_name, "nestopia") || strstr(core_name, "quicknes")) return RC_CONSOLE_NINTENDO;
		if (strstr(core_name, "snes9x") || strstr(core_name, "pocketsnes")) return RC_CONSOLE_SUPER_NINTENDO;
		if (strstr(core_name, "picodrive") || strstr(core_name, "genesis")) return RC_CONSOLE_MEGA_DRIVE;
		if (strstr(core_name, "pce")) return RC_CONSOLE_PC_ENGINE;
		if (strstr(core_name, "pcsx")) return RC_CONSOLE_PLAYSTATION;
		if (strstr(core_name, "ngp")) return RC_CONSOLE_NEOGEO_POCKET;
		if (strstr(core_name, "wswan")) return RC_CONSOLE_WONDERSWAN;
		if (strstr(core_name, "handy")) return RC_CONSOLE_ATARI_LYNX;
		if (strstr(core_name, "stella")) return RC_CONSOLE_ATARI_2600;
		if (strstr(core_name, "prosystem")) return RC_CONSOLE_ATARI_7800;
		if (strstr(core_name, "pokemini")) return RC_CONSOLE_POKEMON_MINI;
		if (strstr(core_name, "vecx")) return RC_CONSOLE_VECTREX;
		if (strstr(core_name, "fba") || strstr(core_name, "fbneo") || strstr(core_name, "mame")) return RC_CONSOLE_ARCADE;
	}
	return 0;
}

static bool lookup_rom_hash_cache(const char *rom_path, char *hash_out, uint32_t *console_out,
                                  bool *index_present_out)
{
	char idx_path[RA_MAX_PATH];
	char *json;
	const char *entry;
	char h[40] = {0};
	int64_t cid = 0;

	if (index_present_out)
		*index_present_out = false;
	if (!rom_path || !rom_path[0])
		return false;
	snprintf(idx_path, sizeof(idx_path), "%s/rom_hashes.json", ra_data_dir());
	json = read_entire_file(idx_path, NULL);
	if (!json)
		return false;
	if (index_present_out)
		*index_present_out = true;

	entry = find_json_key_val(json, rom_path);
	if (!entry || *entry != '{') {
		free(json);
		return false;
	}
	/* Limit scan to this object up to '}' */
	const char *end = strchr(entry, '}');
	bool ok = false;
	if (end) {
		size_t obj_len = (size_t)(end - entry + 1);
		char *obj = (char *)malloc(obj_len + 1);
		if (obj) {
			memcpy(obj, entry, obj_len);
			obj[obj_len] = '\0';
			if (json_get_string(obj, "hash", h, sizeof(h)) == 0 && strlen(h) == 32) {
				memcpy(hash_out, h, 33);
				if (json_get_int64(obj, "console_id", &cid) == 0 && cid > 0)
					*console_out = (uint32_t)cid;
				ok = true;
			}
			free(obj);
		}
	}
	free(json);
	return ok;
}

static bool is_disc_console(uint32_t console_id)
{
	return console_id == RC_CONSOLE_PLAYSTATION ||
	       console_id == RC_CONSOLE_PC_ENGINE_CD ||
	       console_id == RC_CONSOLE_SEGA_CD ||
	       console_id == RC_CONSOLE_SATURN ||
	       console_id == RC_CONSOLE_DREAMCAST ||
	       console_id == RC_CONSOLE_3DO;
}

static bool is_small_regular_file(const char *path, size_t max_bytes)
{
	struct stat st;
	if (!path || !path[0] || stat(path, &st) != 0 || !S_ISREG(st.st_mode))
		return false;
	return st.st_size > 0 && (size_t)st.st_size <= max_bytes;
}

static bool compute_rom_hash(const char *path, const char *tmpfile,
                             const void *rom_data, size_t rom_size,
                             uint32_t console_id, bool has_hash_index, char hash_out[33])
{
	hash_out[0] = '\0';
	if (console_id == 0 || is_disc_console(console_id))
		return false;

	if (rom_data && rom_size > 0 && rom_size <= RA_FAST_HASH_MAX_BYTES) {
		if (rc_hash_generate_from_buffer(hash_out, console_id, (const uint8_t *)rom_data, rom_size))
			return true;
	}

	/* If Connect already built rom_hashes.json, avoid slow synchronous SD reads/unzips
	 * for unindexed large ROMs on launch; rely on in-RAM hashes or Sync ROMs. */
	if (has_hash_index)
		return false;

	if (console_id == RC_CONSOLE_ARCADE && path) {
		return rc_hash_generate_from_file(hash_out, console_id, path) != 0;
	}

	if (tmpfile && tmpfile[0] && is_small_regular_file(tmpfile, RA_FAST_HASH_MAX_BYTES)) {
		if (rc_hash_generate_from_file(hash_out, console_id, tmpfile))
			return true;
	}

	if (path && path[0] && is_small_regular_file(path, RA_FAST_HASH_MAX_BYTES)) {
		if (ends_with_ci(path, ".zip") && g_ra.has_host && g_ra.host.unzip_to_tmp) {
			char unzipped[RA_MAX_PATH];
			if (g_ra.host.unzip_to_tmp(path, unzipped, sizeof(unzipped)) == 0) {
				bool ok = rc_hash_generate_from_file(hash_out, console_id, unzipped) != 0;
				unlink(unzipped);
				if (ok)
					return true;
			}
		}
		if (rc_hash_generate_from_file(hash_out, console_id, path))
			return true;
	}
	return false;
}

static bool peek_cached_set_metadata(const char *hash, uint32_t *console_out, uint32_t *game_id_out)
{
	char path[RA_MAX_PATH];
	char *json;
	size_t blen = 0;
	int64_t cid = 0, gid = 0;

	clear_cached_set_json();
	snprintf(path, sizeof(path), "%s/cache/sets/m_%s.json", ra_data_dir(), hash);
	json = read_entire_file(path, &blen);
	if (!json)
		return false;

	if (json_get_int64(json, "ConsoleId", &cid) != 0)
		json_get_int64(json, "ConsoleID", &cid);
	if (json_get_int64(json, "GameId", &gid) != 0)
		json_get_int64(json, "GameID", &gid);

	/* Keep JSON cached for immediate reuse by rc_client's achievementsets call */
	snprintf(g_ra.cached_set_hash, sizeof(g_ra.cached_set_hash), "%s", hash);
	g_ra.cached_set_json = json;
	g_ra.cached_set_len = blen;

	if (cid > 0)
		*console_out = (uint32_t)cid;
	if (gid > 0)
		*game_id_out = (uint32_t)gid;
	return true;
}

/* ---------- query parameter parser ---------- */

static void url_decode_inplace(char *s)
{
	char *r = s, *w = s;
	while (*r) {
		if (*r == '+') {
			*w++ = ' ';
			r++;
		} else if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
			char hex[3] = {r[1], r[2], '\0'};
			*w++ = (char)strtoul(hex, NULL, 16);
			r += 3;
		} else {
			*w++ = *r++;
		}
	}
	*w = '\0';
}

static bool get_form_param(const char *data, const char *key, char *out, size_t out_len)
{
	size_t klen;
	const char *p = data;

	if (!data || !key || !out || out_len == 0)
		return false;
	klen = strlen(key);
	while (p && *p) {
		const char *amp = strchr(p, '&');
		size_t plen = amp ? (size_t)(amp - p) : strlen(p);
		if (plen > klen + 1 && strncmp(p, key, klen) == 0 && p[klen] == '=') {
			size_t vlen = plen - (klen + 1);
			if (vlen >= out_len)
				vlen = out_len - 1;
			memcpy(out, p + klen + 1, vlen);
			out[vlen] = '\0';
			url_decode_inplace(out);
			return true;
		}
		p = amp ? amp + 1 : NULL;
	}
	return false;
}

/* ---------- libretro memory & client callbacks ---------- */

static void RC_CCONV ra_get_core_memory_info(uint32_t id, rc_libretro_core_memory_info_t *info)
{
	memset(info, 0, sizeof(*info));
	if (g_ra.has_host && g_ra.host.get_memory_data && g_ra.host.get_memory_size) {
		info->data = (uint8_t *)g_ra.host.get_memory_data(id);
		info->size = g_ra.host.get_memory_size(id);
	}
}

static uint32_t RC_CCONV ra_read_memory(uint32_t address, uint8_t *buffer, uint32_t num_bytes, rc_client_t *client)
{
	(void)client;
	if (!g_ra.has_regions)
		return 0;
	return rc_libretro_memory_read(&g_ra.regions, address, buffer, num_bytes);
}

static rc_clock_t RC_CCONV ra_get_time_millisecs(const rc_client_t *client)
{
	struct timespec ts;
	(void)client;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
		return (rc_clock_t)ts.tv_sec * 1000ULL + (rc_clock_t)(ts.tv_nsec / 1000000L);
	return (rc_clock_t)time(NULL) * 1000ULL;
}

static void send_json_response(rc_client_server_callback_t callback, void *callback_data,
                               int http_code, const char *body, size_t len)
{
	rc_api_server_response_t resp;
	memset(&resp, 0, sizeof(resp));
	resp.http_status_code = http_code;
	resp.body = body;
	resp.body_length = len ? len : (body ? strlen(body) : 0);
	callback(&resp, callback_data);
}

/* Parse comma-separated uints from JSON array after '"UserUnlocks": [' */
static void append_cached_unlocks(uint32_t gid, char **buf, size_t *len, size_t *cap, int *count)
{
	char path[RA_MAX_PATH];
	char *json;
	const char *arr;

	snprintf(path, sizeof(path), "%s/cache/unlocks/g_%u.json", ra_data_dir(), gid);
	json = read_entire_file(path, NULL);
	if (!json)
		return;

	arr = find_json_key_val(json, "UserUnlocks");
	if (arr && *arr == '[') {
		const char *p = arr + 1;
		while (*p && *p != ']') {
			if (isdigit((unsigned char)*p)) {
				char *endp = NULL;
				unsigned long aid = strtoul(p, &endp, 10);
				if (endp > p && aid > 0) {
					char item[64];
					int n = snprintf(item, sizeof(item), "%s{\"ID\":%lu,\"When\":1700000000}",
					                 (*count > 0) ? "," : "", aid);
					if (*len + (size_t)n + 128 < *cap) {
						memcpy(*buf + *len, item, (size_t)n);
						*len += (size_t)n;
						(*count)++;
					}
					p = endp;
					continue;
				}
			}
			p++;
		}
	}
	free(json);
}

static void append_queued_unlocks(uint32_t gid, const char *hash, char **buf, size_t *len, size_t *cap, int *count)
{
	char qdir[RA_MAX_PATH];
	DIR *dir;
	struct dirent *de;

	snprintf(qdir, sizeof(qdir), "%s/queue", ra_data_dir());
	dir = opendir(qdir);
	if (!dir)
		return;

	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.' || !ends_with_ci(de->d_name, ".json"))
			continue;
		char fpath[RA_MAX_PATH + 260];
		snprintf(fpath, sizeof(fpath), "%s/%s", qdir, de->d_name);
		char *qjson = read_entire_file(fpath, NULL);
		if (!qjson)
			continue;

		int64_t qgid = 0, qaid = 0, qts = 0;
		char qhash[40] = {0};
		bool match = false;
		json_get_int64(qjson, "game_id", &qgid);
		json_get_string(qjson, "game_hash", qhash, sizeof(qhash));
		if ((gid > 0 && qgid == (int64_t)gid) ||
		    (hash && hash[0] && strcasecmp(qhash, hash) == 0))
			match = true;

		if (match && json_get_int64(qjson, "achievement_id", &qaid) == 0 && qaid > 0) {
			json_get_int64(qjson, "unlocked_at_utc", &qts);
			if (qts <= 0)
				qts = 1700000000LL;
			char item[64];
			int n = snprintf(item, sizeof(item), "%s{\"ID\":%lld,\"When\":%lld}",
			                 (*count > 0) ? "," : "", (long long)qaid, (long long)qts);
			if (*len + (size_t)n + 128 >= *cap) {
				size_t ncap = *cap * 2;
				char *nbuf = (char *)realloc(*buf, ncap);
				if (nbuf) {
					*buf = nbuf;
					*cap = ncap;
				}
			}
			if (*len + (size_t)n + 128 < *cap) {
				memcpy(*buf + *len, item, (size_t)n);
				*len += (size_t)n;
				(*count)++;
			}
		}
		free(qjson);
	}
	closedir(dir);
}

static void handle_award_achievement(const char *post_data, rc_client_server_callback_t callback, void *callback_data)
{
	char aid_s[32] = {0}, gid_s[32] = {0}, hash_s[40] = {0};
	uint32_t aid = 0, gid = g_ra.game_id;
	int64_t now_utc = ra_offline_now_utc();
	const rc_client_achievement_t *ach = NULL;
	const char *title = "";
	char qpath[RA_MAX_PATH];
	char payload[1024];
	char *p = payload;
	size_t rem = sizeof(payload);
	int n;

	if (get_form_param(post_data, "a", aid_s, sizeof(aid_s)))
		aid = (uint32_t)strtoul(aid_s, NULL, 10);
	if (get_form_param(post_data, "g", gid_s, sizeof(gid_s)) && gid_s[0]) {
		uint32_t pgid = (uint32_t)strtoul(gid_s, NULL, 10);
		if (pgid > 0)
			gid = pgid;
	}
	if (!get_form_param(post_data, "m", hash_s, sizeof(hash_s)) || !hash_s[0])
		snprintf(hash_s, sizeof(hash_s), "%s", g_ra.game_hash);

	if (aid == 0) {
		send_json_response(callback, callback_data, 400,
		                   "{\"Success\":false,\"Error\":\"Missing achievement ID\"}", 0);
		return;
	}

	if (g_ra.client)
		ach = rc_client_get_achievement_info(g_ra.client, aid);
	if (ach && ach->title)
		title = ach->title;

	n = snprintf(p, rem,
	             "{\"achievement_id\":%u,\"game_id\":%u,\"game_hash\":\"%s\","
	             "\"hardcore\":0,\"unlocked_at_utc\":%lld,\"title\":\"",
	             aid, gid, hash_s, (long long)now_utc);
	if (n > 0 && (size_t)n < rem) {
		p += n;
		rem -= (size_t)n;
		json_escape_append(&p, &rem, title);
		if (rem > 2) {
			*p++ = '"';
			*p++ = '}';
			*p = '\0';
		}
	}

	snprintf(qpath, sizeof(qpath), "%s/queue/%lld_%u_%ld.json",
	         ra_data_dir(), (long long)now_utc, aid, (long)getpid());
	if (write_atomic_file(qpath, payload, strlen(payload)) != 0) {
		send_json_response(callback, callback_data, 500,
		                   "{\"Success\":false,\"Error\":\"Failed to write queue\"}", 0);
		return;
	}

	char resp[96];
	snprintf(resp, sizeof(resp), "{\"Success\":true,\"AchievementID\":%u}", aid);
	send_json_response(callback, callback_data, 200, resp, 0);
}

static void RC_CCONV ra_server_call(const rc_api_request_t *request,
                                    rc_client_server_callback_t callback,
                                    void *callback_data, rc_client_t *client)
{
	char action[64] = {0};
	const char *params = (request && request->post_data) ? request->post_data : "";
	(void)client;

	if (!get_form_param(params, "r", action, sizeof(action)) && request && request->url) {
		const char *q = strchr(request->url, '?');
		if (q)
			get_form_param(q + 1, "r", action, sizeof(action));
	}

	if (strcmp(action, "login2") == 0) {
		char path[RA_MAX_PATH];
		size_t blen = 0;
		snprintf(path, sizeof(path), "%s/auth.json", ra_data_dir());
		char *body = read_entire_file(path, &blen);
		if (body) {
			send_json_response(callback, callback_data, 200, body, blen);
			free(body);
		} else {
			send_json_response(callback, callback_data, 401,
			                   "{\"Success\":false,\"Error\":\"Not signed in\"}", 0);
		}
		return;
	}

	if (strcmp(action, "achievementsets") == 0 || strcmp(action, "patch") == 0) {
		char hash[40] = {0};
		char path[RA_MAX_PATH];
		size_t blen = 0;
		if (!get_form_param(params, "m", hash, sizeof(hash)) || !hash[0])
			snprintf(hash, sizeof(hash), "%s", g_ra.game_hash);
		if (g_ra.cached_set_json && strcmp(hash, g_ra.cached_set_hash) == 0) {
			send_json_response(callback, callback_data, 200,
			                   g_ra.cached_set_json, g_ra.cached_set_len);
			clear_cached_set_json();
			return;
		}
		snprintf(path, sizeof(path), "%s/cache/sets/m_%s.json", ra_data_dir(), hash);
		char *body = read_entire_file(path, &blen);
		if (body) {
			send_json_response(callback, callback_data, 200, body, blen);
			free(body);
		} else {
			send_json_response(callback, callback_data, 404,
			                   "{\"Success\":false,\"Error\":\"Game not cached offline\"}", 0);
		}
		return;
	}

	if (strcmp(action, "startsession") == 0) {
		char gid_s[32] = {0}, hash[40] = {0};
		uint32_t gid = g_ra.game_id;
		if (get_form_param(params, "g", gid_s, sizeof(gid_s)) && gid_s[0])
			gid = (uint32_t)strtoul(gid_s, NULL, 10);
		if (!get_form_param(params, "m", hash, sizeof(hash)) || !hash[0])
			snprintf(hash, sizeof(hash), "%s", g_ra.game_hash);

		size_t cap = 4096;
		char *buf = (char *)malloc(cap);
		if (!buf) {
			send_json_response(callback, callback_data, 500, "{\"Success\":false}", 0);
			return;
		}
		int cnt = 0;
		size_t len = (size_t)snprintf(buf, cap, "{\"Success\":true,\"Unlocks\":[");
		append_cached_unlocks(gid, &buf, &len, &cap, &cnt);
		append_queued_unlocks(gid, hash, &buf, &len, &cap, &cnt);
		int tail = snprintf(buf + len, cap - len,
		                    "],\"HardcoreUnlocks\":[],\"ServerNow\":%lld}",
		                    (long long)ra_offline_now_utc());
		if (tail > 0)
			len += (size_t)tail;
		send_json_response(callback, callback_data, 200, buf, len);
		free(buf);
		return;
	}

	if (strcmp(action, "awardachievement") == 0) {
		handle_award_achievement(params, callback, callback_data);
		return;
	}

	if (strcmp(action, "ping") == 0) {
		send_json_response(callback, callback_data, 200, "{\"Success\":true}", 0);
		return;
	}

	/* Any unsupported online-only API (leaderboards, etc.) */
	send_json_response(callback, callback_data, 200, "{\"Success\":true}", 0);
}

static void RC_CCONV ra_event_handler(const rc_client_event_t *event, rc_client_t *client)
{
	(void)client;
	if (!event)
		return;

	switch (event->type) {
	case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
		if (event->achievement && g_ra.has_host && g_ra.host.show_toast) {
			const char *t = event->achievement->title ? event->achievement->title : "Unlocked";
			char hdr[64];
			if (event->achievement->points > 0)
				snprintf(hdr, sizeof(hdr), "Unlocked! +%u pts", event->achievement->points);
			else
				snprintf(hdr, sizeof(hdr), "Achievement Unlocked!");
			g_ra.host.show_toast(hdr, t, 4000);
		}
		break;
	case RC_CLIENT_EVENT_GAME_COMPLETED:
		if (g_ra.has_host && g_ra.host.show_toast) {
			g_ra.host.show_toast("Game Mastered!", g_ra.game_title[0] ? g_ra.game_title : "All Unlocked", 4500);
		}
		break;
	default:
		break;
	}
}

static void RC_CCONV ra_login_cb(int result, const char *error_message, rc_client_t *client, void *userdata)
{
	(void)client;
	(void)userdata;
	if (result != RC_OK) {
		snprintf(g_ra.status_msg, sizeof(g_ra.status_msg), "Auth failed: %s",
		         error_message ? error_message : "err");
	}
}

static void RC_CCONV ra_load_cb(int result, const char *error_message, rc_client_t *client, void *userdata)
{
	(void)userdata;
	if (result != RC_OK) {
		snprintf(g_ra.status_msg, sizeof(g_ra.status_msg), "Load failed: %s",
		         error_message ? error_message : "err");
		g_ra.active = false;
		return;
	}

	const rc_client_game_t *game = rc_client_get_game_info(client);
	if (!game || game->id == 0 || !rc_client_has_achievements(client)) {
		snprintf(g_ra.status_msg, sizeof(g_ra.status_msg), "No achievements for game");
		g_ra.active = false;
		return;
	}

	rc_client_user_game_summary_t summary;
	memset(&summary, 0, sizeof(summary));
	rc_client_get_user_game_summary(client, &summary);

	g_ra.loaded = true;
	g_ra.active = true;
	g_ra.game_id = game->id;
	snprintf(g_ra.game_title, sizeof(g_ra.game_title), "%s", game->title ? game->title : "");
	snprintf(g_ra.status_msg, sizeof(g_ra.status_msg), "%u/%u unlocked (Softcore)",
	         summary.num_unlocked_achievements, summary.num_promoted_achievements);

	if (g_ra.has_host && g_ra.host.show_toast) {
		char line1[64];
		snprintf(line1, sizeof(line1), "RetroAchievements %u/%u",
		         summary.num_unlocked_achievements,
		         summary.num_promoted_achievements);
		g_ra.host.show_toast(line1, g_ra.game_title[0] ? g_ra.game_title : "Achievements Active", 3000);
	}
}

/* ---------- public lifecycle API ---------- */

void ra_offline_init_host(const struct ra_offline_host *host)
{
	if (host) {
		g_ra.host = *host;
		g_ra.has_host = true;
	} else {
		memset(&g_ra.host, 0, sizeof(g_ra.host));
		g_ra.has_host = false;
	}
}

static void free_mmap_copy(void)
{
	if (g_ra.mmap_descs) {
		for (unsigned i = 0; i < g_ra.mmap_count; i++)
			free((void *)g_ra.mmap_descs[i].addrspace);
		free(g_ra.mmap_descs);
	}
	g_ra.mmap_descs = NULL;
	g_ra.mmap_count = 0;
	g_ra.has_mmap = false;
}

void ra_offline_set_memory_maps(const struct retro_memory_map *mmap)
{
	free_mmap_copy();
	if (!mmap || !mmap->descriptors || mmap->num_descriptors == 0)
		return;

	g_ra.mmap_descs = (struct retro_memory_descriptor *)calloc(
		mmap->num_descriptors, sizeof(struct retro_memory_descriptor));
	if (!g_ra.mmap_descs)
		return;

	for (unsigned i = 0; i < mmap->num_descriptors; i++) {
		g_ra.mmap_descs[i] = mmap->descriptors[i];
		g_ra.mmap_descs[i].addrspace =
			mmap->descriptors[i].addrspace ? strdup(mmap->descriptors[i].addrspace) : NULL;
	}
	g_ra.mmap_count = mmap->num_descriptors;
	g_ra.has_mmap = true;
}

void ra_offline_set_support_achievements(bool supported)
{
	g_ra.support_achievements = supported;
}

void ra_offline_on_game_unload(void)
{
	g_ra.has_deferred_load = false;
	g_ra.deferred_path[0] = '\0';
	g_ra.deferred_tmpfile[0] = '\0';
	g_ra.deferred_rom_data = NULL;
	g_ra.deferred_rom_size = 0;
	g_ra.deferred_core_name[0] = '\0';
	clear_cached_set_json();

	if (g_ra.client && rc_client_is_game_loaded(g_ra.client))
		rc_client_unload_game(g_ra.client);
	if (g_ra.client) {
		rc_client_destroy(g_ra.client);
		g_ra.client = NULL;
	}
	if (g_ra.has_regions) {
		rc_libretro_memory_destroy(&g_ra.regions);
		g_ra.has_regions = false;
	}
	free_mmap_copy();
	g_ra.loaded = false;
	g_ra.active = false;
	g_ra.game_id = 0;
	g_ra.console_id = 0;
	g_ra.game_hash[0] = '\0';
	g_ra.game_title[0] = '\0';
	g_ra.rom_path[0] = '\0';
	g_ra.support_achievements = true;
	snprintf(g_ra.status_msg, sizeof(g_ra.status_msg), "Idle");
}

void ra_offline_defer_game_load(const char *path, const char *tmpfile,
                                const void *rom_data, size_t rom_size,
                                const char *core_name)
{
	clear_cached_set_json();
	if (g_ra.client) {
		if (rc_client_is_game_loaded(g_ra.client))
			rc_client_unload_game(g_ra.client);
		rc_client_destroy(g_ra.client);
		g_ra.client = NULL;
	}
	if (g_ra.has_regions) {
		rc_libretro_memory_destroy(&g_ra.regions);
		g_ra.has_regions = false;
	}
	g_ra.loaded = false;
	g_ra.active = false;
	g_ra.game_id = 0;
	g_ra.game_title[0] = '\0';
	g_ra.game_hash[0] = '\0';
	snprintf(g_ra.rom_path, sizeof(g_ra.rom_path), "%s", path ? path : "");
	snprintf(g_ra.status_msg, sizeof(g_ra.status_msg), "Pending load");

	snprintf(g_ra.deferred_path, sizeof(g_ra.deferred_path), "%s", path ? path : "");
	snprintf(g_ra.deferred_tmpfile, sizeof(g_ra.deferred_tmpfile), "%s", tmpfile ? tmpfile : "");
	g_ra.deferred_rom_data = rom_data;
	g_ra.deferred_rom_size = rom_size;
	snprintf(g_ra.deferred_core_name, sizeof(g_ra.deferred_core_name), "%s", core_name ? core_name : "");
	g_ra.has_deferred_load = true;
}

void ra_offline_commit_deferred_load(void)
{
	if (!g_ra.has_deferred_load)
		return;
	g_ra.has_deferred_load = false;
	ra_offline_on_game_load(g_ra.deferred_path, g_ra.deferred_tmpfile,
	                        g_ra.deferred_rom_data, g_ra.deferred_rom_size,
	                        g_ra.deferred_core_name);
}

void ra_offline_on_game_load(const char *path, const char *tmpfile,
                             const void *rom_data, size_t rom_size,
                             const char *core_name)
{
	char auth_path[RA_MAX_PATH];
	char *auth_json = NULL;
	char user[64] = {0}, token[128] = {0};
	bool success = false;
	bool has_hash_index = false;
	uint32_t console_id = 0;
	char hash[33] = {0};

	g_ra.has_deferred_load = false;
	clear_cached_set_json();

	/* Reset game-specific state while keeping mmap descriptors set during retro_load_game */
	if (g_ra.client) {
		if (rc_client_is_game_loaded(g_ra.client))
			rc_client_unload_game(g_ra.client);
		rc_client_destroy(g_ra.client);
		g_ra.client = NULL;
	}
	if (g_ra.has_regions) {
		rc_libretro_memory_destroy(&g_ra.regions);
		g_ra.has_regions = false;
	}
	g_ra.loaded = false;
	g_ra.active = false;
	g_ra.game_id = 0;
	g_ra.game_title[0] = '\0';
	snprintf(g_ra.rom_path, sizeof(g_ra.rom_path), "%s", path ? path : "");

	if (!g_ra.support_achievements) {
		snprintf(g_ra.status_msg, sizeof(g_ra.status_msg), "Core disabled achievements");
		return;
	}

	snprintf(auth_path, sizeof(auth_path), "%s/auth.json", ra_data_dir());
	auth_json = read_entire_file(auth_path, NULL);
	if (!auth_json ||
	    json_get_bool(auth_json, "Success", &success) != 0 || !success ||
	    json_get_string(auth_json, "User", user, sizeof(user)) != 0 ||
	    json_get_string(auth_json, "Token", token, sizeof(token)) != 0 ||
	    !user[0] || !token[0]) {
		free(auth_json);
		snprintf(g_ra.status_msg, sizeof(g_ra.status_msg), "Not signed in (Connect app)");
		return;
	}
	free(auth_json);

	if (!lookup_rom_hash_cache(path, hash, &console_id, &has_hash_index)) {
		console_id = infer_console_id(path, core_name);
		if (!compute_rom_hash(path, tmpfile, rom_data, rom_size, console_id,
		                      has_hash_index, hash)) {
			snprintf(g_ra.status_msg, sizeof(g_ra.status_msg),
			         has_hash_index ? "ROM not indexed (sync in Connect)" : "ROM hash unavailable");
			return;
		}
	}
	if (console_id == 0)
		console_id = infer_console_id(path, core_name);

	snprintf(g_ra.game_hash, sizeof(g_ra.game_hash), "%s", hash);
	if (!peek_cached_set_metadata(hash, &console_id, &g_ra.game_id)) {
		snprintf(g_ra.status_msg, sizeof(g_ra.status_msg), "Game not cached (sync in Connect)");
		return;
	}
	g_ra.console_id = console_id;

	struct retro_memory_map mmap = { g_ra.mmap_descs, g_ra.mmap_count };
	if (!rc_libretro_memory_init(&g_ra.regions, g_ra.has_mmap ? &mmap : NULL,
	                             ra_get_core_memory_info, console_id)) {
		clear_cached_set_json();
		snprintf(g_ra.status_msg, sizeof(g_ra.status_msg), "No core memory mapped");
		return;
	}
	g_ra.has_regions = true;

	g_ra.client = rc_client_create(ra_read_memory, ra_server_call);
	if (!g_ra.client) {
		clear_cached_set_json();
		snprintf(g_ra.status_msg, sizeof(g_ra.status_msg), "Out of memory");
		return;
	}

	rc_client_set_hardcore_enabled(g_ra.client, 0);
	rc_client_set_unpromoted_enabled(g_ra.client, 0);
	rc_client_set_encore_mode_enabled(g_ra.client, 0);
	rc_client_set_event_handler(g_ra.client, ra_event_handler);
	rc_client_set_get_time_millisecs_function(g_ra.client, ra_get_time_millisecs);

	rc_client_begin_login_with_token(g_ra.client, user, token, ra_login_cb, NULL);
	if (!rc_client_get_user_info(g_ra.client)) {
		clear_cached_set_json();
		return;
	}

	rc_client_begin_load_game(g_ra.client, hash, ra_load_cb, NULL);
	clear_cached_set_json();
}

void ra_offline_do_frame(void)
{
	if (g_ra.client && g_ra.active)
		rc_client_do_frame(g_ra.client);
}

void ra_offline_idle(void)
{
	if (g_ra.client && g_ra.active)
		rc_client_idle(g_ra.client);
}

void ra_offline_reset(void)
{
	if (g_ra.client && g_ra.active)
		rc_client_reset(g_ra.client);
}

/* ---------- save-state sidecar (.ra) ---------- */

void ra_offline_save_state(const char *state_path)
{
	char sidecar[RA_MAX_PATH];
	size_t prog_sz;
	uint8_t *buf;

	if (!state_path || !state_path[0] || !g_ra.client || !g_ra.active)
		return;

	snprintf(sidecar, sizeof(sidecar), "%s.ra", state_path);
	prog_sz = rc_client_progress_size(g_ra.client);
	if (prog_sz == 0)
		return;

	buf = (uint8_t *)malloc(12 + prog_sz);
	if (!buf)
		return;

	memcpy(buf, RA_STATE_MAGIC, 4);
	uint32_t gid = g_ra.game_id;
	uint32_t psz = (uint32_t)prog_sz;
	memcpy(buf + 4, &gid, 4);
	memcpy(buf + 8, &psz, 4);

	if (rc_client_serialize_progress_sized(g_ra.client, buf + 12, prog_sz) == RC_OK)
		write_atomic_file(sidecar, buf, 12 + prog_sz);

	free(buf);
}

void ra_offline_load_state(const char *state_path)
{
	char sidecar[RA_MAX_PATH];
	size_t flen = 0;
	char *raw;

	if (!g_ra.client || !g_ra.active)
		return;

	if (!state_path || !state_path[0]) {
		rc_client_reset(g_ra.client);
		return;
	}

	snprintf(sidecar, sizeof(sidecar), "%s.ra", state_path);
	raw = read_entire_file(sidecar, &flen);
	if (!raw || flen < 12 || memcmp(raw, RA_STATE_MAGIC, 4) != 0) {
		free(raw);
		rc_client_reset(g_ra.client);
		return;
	}

	uint32_t gid = 0, psz = 0;
	memcpy(&gid, raw + 4, 4);
	memcpy(&psz, raw + 8, 4);
	if (gid != g_ra.game_id || flen != (size_t)(12 + psz) ||
	    rc_client_deserialize_progress_sized(g_ra.client, (const uint8_t *)(raw + 12), psz) != RC_OK) {
		rc_client_reset(g_ra.client);
	}
	free(raw);
}

/* ---------- menu & inspection helpers ---------- */

bool ra_offline_is_active(void)
{
	return g_ra.active;
}

const char *ra_offline_get_game_hash(void)
{
	return g_ra.game_hash;
}

uint32_t ra_offline_get_game_id(void)
{
	return g_ra.game_id;
}

void ra_offline_get_status(char *buf, size_t len)
{
	if (!buf || len == 0)
		return;
	if (g_ra.active && g_ra.client) {
		rc_client_user_game_summary_t s;
		memset(&s, 0, sizeof(s));
		rc_client_get_user_game_summary(g_ra.client, &s);
		snprintf(buf, len, "%s: %u/%u (Softcore)",
		         g_ra.game_title[0] ? g_ra.game_title : "RA",
		         s.num_unlocked_achievements, s.num_promoted_achievements);
	} else {
		snprintf(buf, len, "%s", g_ra.status_msg);
	}
}

int ra_offline_get_achievement_count(void)
{
	int count = 0;
	rc_client_achievement_list_t *list;

	if (!g_ra.active || !g_ra.client)
		return 0;
	list = rc_client_create_achievement_list(g_ra.client,
	                                         RC_CLIENT_ACHIEVEMENT_CATEGORY_PROMOTED,
	                                         RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
	if (!list)
		return 0;
	for (uint32_t b = 0; b < list->num_buckets; b++)
		count += (int)list->buckets[b].num_achievements;
	rc_client_destroy_achievement_list(list);
	return count;
}

int ra_offline_format_achievement(int idx, char *buf, size_t len)
{
	int cur = 0;
	rc_client_achievement_list_t *list;

	if (!buf || len == 0)
		return -1;
	buf[0] = '\0';
	if (!g_ra.active || !g_ra.client || idx < 0)
		return -1;

	list = rc_client_create_achievement_list(g_ra.client,
	                                         RC_CLIENT_ACHIEVEMENT_CATEGORY_PROMOTED,
	                                         RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
	if (!list)
		return -1;

	int found = -1;
	for (uint32_t b = 0; b < list->num_buckets && found < 0; b++) {
		const rc_client_achievement_bucket_t *bk = &list->buckets[b];
		for (uint32_t i = 0; i < bk->num_achievements; i++) {
			if (cur == idx) {
				const rc_client_achievement_t *a = bk->achievements[i];
				const char *mark = (a->unlocked & RC_CLIENT_ACHIEVEMENT_UNLOCKED_BOTH) ? "[x]" : "[ ]";
				snprintf(buf, len, "%s %s (%upt)",
				         mark, a->title ? a->title : "?", a->points);
				found = 0;
				break;
			}
			cur++;
		}
	}
	rc_client_destroy_achievement_list(list);
	return found;
}
