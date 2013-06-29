/*
 * libretro core glue for PicoDrive
 * (C) notaz, 2013
 *
 * This work is licensed under the terms of MAME license.
 * See COPYING file in the top-level directory.
 */

#define _GNU_SOURCE 1 // mremap
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <errno.h>
#ifdef __MACH__
#include <libkern/OSCacheControl.h>
#endif

#include <pico/pico_int.h>
#include "common/input_pico.h"
#include "common/version.h"
#include "libretro.h"

static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_environment_t environ_cb;
static retro_audio_sample_batch_t audio_batch_cb;

static FILE *emu_log;

#define VOUT_MAX_WIDTH 320
#define VOUT_MAX_HEIGHT 240
static void *vout_buf;
static int vout_width, vout_height;

static short __attribute__((aligned(4))) sndBuffer[2*44100/50];

// FIXME: these 2 shouldn't be here
static unsigned char PicoDraw2FB_[(8+320) * (8+240+8)];
unsigned char *PicoDraw2FB = PicoDraw2FB_;

static void snd_write(int len);

#ifdef _WIN32
#define SLASH '\\'
#else
#define SLASH '/'
#endif

/* functions called by the core */

void cache_flush_d_inval_i(void *start, void *end)
{
#ifdef __arm__
#if defined(__BLACKBERRY_QNX__)
	msync(start, end - start, MS_SYNC | MS_CACHE_ONLY | MS_INVALIDATE_ICACHE);
#elif defined(__MACH__)
	size_t len = (char *)end - (char *)start;
	sys_dcache_flush(start, len);
	sys_icache_invalidate(start, len);
#else
	__clear_cache(start, end);
#endif
#endif
}

void *plat_mmap(unsigned long addr, size_t size, int need_exec, int is_fixed)
{
	int flags = MAP_PRIVATE | MAP_ANONYMOUS;
	void *req, *ret;

	req = (void *)addr;
	ret = mmap(req, size, PROT_READ | PROT_WRITE, flags, -1, 0);
	if (ret == MAP_FAILED) {
		lprintf("mmap(%08lx, %zd) failed: %d\n", addr, size, errno);
		return NULL;
	}

	if (addr != 0 && ret != (void *)addr) {
		lprintf("warning: wanted to map @%08lx, got %p\n",
			addr, ret);

		if (is_fixed) {
			munmap(ret, size);
			return NULL;
		}
	}

	return ret;
}

void *plat_mremap(void *ptr, size_t oldsize, size_t newsize)
{
	void *ret = mremap(ptr, oldsize, newsize, 0);
	if (ret == MAP_FAILED)
		return NULL;

	return ret;
}

void plat_munmap(void *ptr, size_t size)
{
	if (ptr != NULL)
		munmap(ptr, size);
}

int plat_mem_set_exec(void *ptr, size_t size)
{
	int ret = mprotect(ptr, size, PROT_READ | PROT_WRITE | PROT_EXEC);
	if (ret != 0)
		lprintf("mprotect(%p, %zd) failed: %d\n", ptr, size, errno);

	return ret;
}

void emu_video_mode_change(int start_line, int line_count, int is_32cols)
{
	memset(vout_buf, 0, 320 * 240 * 2);
	vout_width = is_32cols ? 256 : 320;
	PicoDrawSetOutBuf(vout_buf, vout_width * 2);
}

void emu_32x_startup(void)
{
	PicoDrawSetOutFormat(PDF_RGB555, 1);
}

#ifndef ANDROID

void lprintf(const char *fmt, ...)
{
	va_list list;

	va_start(list, fmt);
	fprintf(emu_log, "PicoDrive: ");
	vfprintf(emu_log, fmt, list);
	va_end(list);
	fflush(emu_log);
}

#else

#include <android/log.h>

void lprintf(const char *fmt, ...)
{
	va_list list;

	va_start(list, fmt);
	__android_log_vprint(ANDROID_LOG_INFO, "PicoDrive", fmt, list);
	va_end(list);
}

#endif

/* libretro */
void retro_set_environment(retro_environment_t cb)
{
	static const struct retro_variable vars[] = {
		//{ "region", "Region; Auto|NTSC|PAL" },
		{ NULL, NULL },
	};

	environ_cb = cb;

	cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)vars);
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

unsigned retro_api_version(void)
{
	return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
}

void retro_get_system_info(struct retro_system_info *info)
{
	memset(info, 0, sizeof(*info));
	info->library_name = "PicoDrive";
	info->library_version = VERSION;
	info->valid_extensions = "bin|gen|smd|32x|cue|iso|sms";
	info->need_fullpath = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
	memset(info, 0, sizeof(*info));
	info->timing.fps            = Pico.m.pal ? 50 : 60;
	info->timing.sample_rate    = 44100;
	info->geometry.base_width   = 320;
	info->geometry.base_height  = 240;
	info->geometry.max_width    = VOUT_MAX_WIDTH;
	info->geometry.max_height   = VOUT_MAX_HEIGHT;
	info->geometry.aspect_ratio = 4.0 / 3.0;
}

/* savestates - TODO */
size_t retro_serialize_size(void) 
{ 
       return 0;
}

bool retro_serialize(void *data, size_t size)
{ 
       return false;
}

bool retro_unserialize(const void *data, size_t size)
{
       return false;
}

/* cheats - TODO */
void retro_cheat_reset(void)
{
}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
}

/* multidisk support */
static bool disk_ejected;
static unsigned int disk_current_index;
static unsigned int disk_count;
static struct disks_state {
	char *fname;
} disks[8];

static bool disk_set_eject_state(bool ejected)
{
	// TODO?
	disk_ejected = ejected;
	return true;
}

static bool disk_get_eject_state(void)
{
	return disk_ejected;
}

static unsigned int disk_get_image_index(void)
{
	return disk_current_index;
}

static bool disk_set_image_index(unsigned int index)
{
	cd_img_type cd_type;
	int ret;

	if (index >= sizeof(disks) / sizeof(disks[0]))
		return false;

	if (disks[index].fname == NULL) {
		lprintf("missing disk #%u\n", index);

		// RetroArch specifies "no disk" with index == count,
		// so don't fail here..
		disk_current_index = index;
		return true;
	}

	lprintf("switching to disk %u: \"%s\"\n", index,
		disks[index].fname);

	ret = -1;
	cd_type = PicoCdCheck(disks[index].fname, NULL);
	if (cd_type != CIT_NOT_CD)
		ret = Insert_CD(disks[index].fname, cd_type);
	if (ret != 0) {
		lprintf("Load failed, invalid CD image?\n");
		return 0;
	}

	disk_current_index = index;
	return true;
}

static unsigned int disk_get_num_images(void)
{
	return disk_count;
}

static bool disk_replace_image_index(unsigned index,
	const struct retro_game_info *info)
{
	bool ret = true;

	if (index >= sizeof(disks) / sizeof(disks[0]))
		return false;

	if (disks[index].fname != NULL)
		free(disks[index].fname);
	disks[index].fname = NULL;

	if (info != NULL) {
		disks[index].fname = strdup(info->path);
		if (index == disk_current_index)
			ret = disk_set_image_index(index);
	}

	return ret;
}

static bool disk_add_image_index(void)
{
	if (disk_count >= sizeof(disks) / sizeof(disks[0]))
		return false;

	disk_count++;
	return true;
}

static struct retro_disk_control_callback disk_control = {
	.set_eject_state = disk_set_eject_state,
	.get_eject_state = disk_get_eject_state,
	.get_image_index = disk_get_image_index,
	.set_image_index = disk_set_image_index,
	.get_num_images = disk_get_num_images,
	.replace_image_index = disk_replace_image_index,
	.add_image_index = disk_add_image_index,
};

static void disk_tray_open(void)
{
	lprintf("cd tray open\n");
	disk_ejected = 1;
}

static void disk_tray_close(void)
{
	lprintf("cd tray close\n");
	disk_ejected = 0;
}


static const char * const biosfiles_us[] = {
	"us_scd1_9210", "us_scd2_9306", "SegaCDBIOS9303", "bios_CD_U"
};
static const char * const biosfiles_eu[] = {
	"eu_mcd1_9210", "eu_mcd2_9306", "eu_mcd2_9303", "bios_CD_E"
};
static const char * const biosfiles_jp[] = {
	"jp_mcd1_9112", "jp_mcd1_9111", "bios_CD_J"
};

static void make_system_path(char *buf, size_t buf_size,
	const char *name, const char *ext)
{
	const char *dir = NULL;

	if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir) {
		snprintf(buf, buf_size, "%s%c%s%s", dir, SLASH, name, ext);
	}
	else {
		snprintf(buf, buf_size, "%s%s", name, ext);
	}
}

static const char *find_bios(int *region, const char *cd_fname)
{
	const char * const *files;
	static char path[256];
	int i, count;
	FILE *f = NULL;

	if (*region == 4) { // US
		files = biosfiles_us;
		count = sizeof(biosfiles_us) / sizeof(char *);
	} else if (*region == 8) { // EU
		files = biosfiles_eu;
		count = sizeof(biosfiles_eu) / sizeof(char *);
	} else if (*region == 1 || *region == 2) {
		files = biosfiles_jp;
		count = sizeof(biosfiles_jp) / sizeof(char *);
	} else {
		return NULL;
	}

	for (i = 0; i < count; i++)
	{
		make_system_path(path, sizeof(path), files[i], ".bin");
		f = fopen(path, "rb");
		if (f != NULL)
			break;

		make_system_path(path, sizeof(path), files[i], ".zip");
		f = fopen(path, "rb");
		if (f != NULL)
			break;
	}

	if (f != NULL) {
		lprintf("using bios: %s\n", path);
		fclose(f);
		return path;
	}

	return NULL;
}

bool retro_load_game(const struct retro_game_info *info)
{
	enum media_type_e media_type;
	static char carthw_path[256];
	size_t i;

	enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
	if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
		lprintf("RGB565 suppot required, sorry\n");
		return false;
	}

	if (info == NULL || info->path == NULL) {
		lprintf("info->path required\n");
		return false;
	}

	for (i = 0; i < sizeof(disks) / sizeof(disks[0]); i++) {
		if (disks[i].fname != NULL) {
			free(disks[i].fname);
			disks[i].fname = NULL;
		}
	}

	disk_current_index = 0;
	disk_count = 1;
	disks[0].fname = strdup(info->path);

	make_system_path(carthw_path, sizeof(carthw_path), "carthw", ".cfg");

	media_type = PicoLoadMedia(info->path, carthw_path,
			find_bios, NULL);

	switch (media_type) {
	case PM_BAD_DETECT:
		lprintf("Failed to detect ROM/CD image type.\n");
		return false;
	case PM_BAD_CD:
		lprintf("Invalid CD image\n");
		return false;
	case PM_BAD_CD_NO_BIOS:
		lprintf("Missing BIOS\n");
		return false;
	case PM_ERROR:
		lprintf("Load error\n");
		return false;
	default:
		break;
	}

	PicoLoopPrepare();

	PicoWriteSound = snd_write;
	memset(sndBuffer, 0, sizeof(sndBuffer));
	PsndOut = sndBuffer;
	PsndRerate(1);

	return true;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
	return false;
}

void retro_unload_game(void) 
{
}

unsigned retro_get_region(void)
{
	return Pico.m.pal ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}

void *retro_get_memory_data(unsigned id)
{
	if (id != RETRO_MEMORY_SAVE_RAM)
		return NULL;

	if (PicoAHW & PAHW_MCD)
		return Pico_mcd->bram;
	else
		return SRam.data;
}

size_t retro_get_memory_size(unsigned id)
{
	if (id != RETRO_MEMORY_SAVE_RAM)
		return 0;

	if (PicoAHW & PAHW_MCD)
		// bram
		return 0x2000;
	else
		return SRam.size;
}

void retro_reset(void)
{
	PicoReset();
}

static const unsigned short retro_pico_map[] = {
	[RETRO_DEVICE_ID_JOYPAD_B]	= 1 << GBTN_B,
	[RETRO_DEVICE_ID_JOYPAD_Y]	= 1 << GBTN_A,
	[RETRO_DEVICE_ID_JOYPAD_SELECT]	= 1 << GBTN_MODE,
	[RETRO_DEVICE_ID_JOYPAD_START]	= 1 << GBTN_START,
	[RETRO_DEVICE_ID_JOYPAD_UP]	= 1 << GBTN_UP,
	[RETRO_DEVICE_ID_JOYPAD_DOWN]	= 1 << GBTN_DOWN,
	[RETRO_DEVICE_ID_JOYPAD_LEFT]	= 1 << GBTN_LEFT,
	[RETRO_DEVICE_ID_JOYPAD_RIGHT]	= 1 << GBTN_RIGHT,
	[RETRO_DEVICE_ID_JOYPAD_A]	= 1 << GBTN_C,
	[RETRO_DEVICE_ID_JOYPAD_X]	= 1 << GBTN_Y,
	[RETRO_DEVICE_ID_JOYPAD_L]	= 1 << GBTN_X,
	[RETRO_DEVICE_ID_JOYPAD_R]	= 1 << GBTN_Z,
};
#define RETRO_PICO_MAP_LEN (sizeof(retro_pico_map) / sizeof(retro_pico_map[0]))

static void snd_write(int len)
{
	audio_batch_cb(PsndOut, len / 4);
}

void retro_run(void) 
{
	bool updated = false;
	int pad, i;

	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
		; //update_variables(true);

	input_poll_cb();

	PicoPad[0] = PicoPad[1] = 0;
	for (pad = 0; pad < 2; pad++)
		for (i = 0; i < RETRO_PICO_MAP_LEN; i++)
			if (input_state_cb(pad, RETRO_DEVICE_JOYPAD, 0, i))
				PicoPad[pad] |= retro_pico_map[i];

	PicoFrame();

	video_cb(vout_buf, vout_width, vout_height, vout_width * 2);
}

void retro_init(void)
{
	int level;

#ifdef IOS
	emu_log = fopen("/User/Documents/PicoDrive.log", "w");
	if (emu_log == NULL)
		emu_log = fopen("PicoDrive.log", "w");
	if (emu_log == NULL)
#endif
	emu_log = stdout;

	level = 0;
	environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);

	environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, &disk_control);

	PicoOpt = POPT_EN_STEREO|POPT_EN_FM|POPT_EN_PSG|POPT_EN_Z80
		| POPT_EN_MCD_PCM|POPT_EN_MCD_CDDA|POPT_EN_MCD_GFX
		| POPT_EN_32X|POPT_EN_PWM
		| POPT_ACC_SPRITES|POPT_DIS_32C_BORDER;
#ifdef __arm__
	PicoOpt |= POPT_EN_SVP_DRC;
#endif
	PsndRate = 44100;
	PicoAutoRgnOrder = 0x184; // US, EU, JP
	PicoCDBuffers = 0;

	p32x_msh2_multiplier = MSH2_MULTI_DEFAULT;
	p32x_ssh2_multiplier = SSH2_MULTI_DEFAULT;

	vout_width = 320;
	vout_height = 240;
	vout_buf = malloc(VOUT_MAX_WIDTH * VOUT_MAX_HEIGHT * 2);

	PicoInit();
	PicoDrawSetOutFormat(PDF_RGB555, 1);
	PicoDrawSetOutBuf(vout_buf, vout_width * 2);

	//PicoMessage = plat_status_msg_busy_next;
	PicoMCDopenTray = disk_tray_open;
	PicoMCDcloseTray = disk_tray_close;
}

void retro_deinit(void)
{
	PicoExit();
}