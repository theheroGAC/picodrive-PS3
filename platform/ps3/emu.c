#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <io/pad.h>
#include <audio/audio.h>
#include <sys/thread.h>
#include <sys/event_queue.h>
#include <sys/process.h>
#include <sysutil/sysutil.h>
#include <SDL2/SDL.h>
#include "in_ps3.h"
#include "../libpicofe/input.h"
#include "../libpicofe/plat.h"
#include "../libpicofe/menu.h"
#include "../common/input_pico.h"
#include "../common/emu.h"
#include "../common/keyboard.h"
#include <pico/pico_int.h>

void ps3_diag_set(int stage, const char *msg);
void *plat_mem_get_for_drc(size_t size);
void plat_mem_free_for_drc(void);

#define PS3_PAD_UP        (1 << 0)
#define PS3_PAD_DOWN      (1 << 1)
#define PS3_PAD_LEFT      (1 << 2)
#define PS3_PAD_RIGHT     (1 << 3)
#define PS3_PAD_START     (1 << 4)
#define PS3_PAD_SELECT    (1 << 5)
#define PS3_PAD_TRIANGLE  (1 << 6)
#define PS3_PAD_CIRCLE    (1 << 7)
#define PS3_PAD_CROSS     (1 << 8)
#define PS3_PAD_SQUARE    (1 << 9)
#define PS3_PAD_L1        (1 << 10)
#define PS3_PAD_R1        (1 << 11)
#define PS3_PAD_L2        (1 << 12)
#define PS3_PAD_R2        (1 << 13)
#define PS3_PAD_L3        (1 << 14)
#define PS3_PAD_R3        (1 << 15)

static struct in_default_bind in_ps3_defbinds_list[] =
{
	{ PS3_PAD_UP,          IN_BINDTYPE_PLAYER12, GBTN_UP },
	{ PS3_PAD_DOWN,        IN_BINDTYPE_PLAYER12, GBTN_DOWN },
	{ PS3_PAD_LEFT,        IN_BINDTYPE_PLAYER12, GBTN_LEFT },
	{ PS3_PAD_RIGHT,       IN_BINDTYPE_PLAYER12, GBTN_RIGHT },
	{ PS3_PAD_SQUARE,      IN_BINDTYPE_PLAYER12, GBTN_A },
	{ PS3_PAD_CROSS,       IN_BINDTYPE_PLAYER12, GBTN_B },
	{ PS3_PAD_CIRCLE,      IN_BINDTYPE_PLAYER12, GBTN_C },
	{ PS3_PAD_TRIANGLE,    IN_BINDTYPE_PLAYER12, GBTN_Y },
	{ PS3_PAD_L1,          IN_BINDTYPE_PLAYER12, GBTN_X },
	{ PS3_PAD_R1,          IN_BINDTYPE_PLAYER12, GBTN_Z },
	{ PS3_PAD_L2,          IN_BINDTYPE_EMU,      PEVB_STATE_SAVE },
	{ PS3_PAD_R2,          IN_BINDTYPE_EMU,      PEVB_STATE_LOAD },
	{ PS3_PAD_L3,          IN_BINDTYPE_PLAYER12, GBTN_MODE },
	{ PS3_PAD_START,       IN_BINDTYPE_PLAYER12, GBTN_START },
	{ PS3_PAD_SELECT,      IN_BINDTYPE_EMU,      PEVB_MENU },
	{ 0, 0, 0 }
};

const char *renderer_names[] = { "16bit accurate", " 8bit accurate", " 8bit fast", NULL };
const char *renderer_names32x[] = { "accurate", "faster", "fastest", NULL };
enum renderer_types { RT_16BIT, RT_8BIT_ACC, RT_8BIT_FAST, RT_COUNT };
static int is_bg_frame;

static SDL_Window *g_win = NULL;
static SDL_Renderer *g_ren = NULL;
static SDL_Texture *g_screen_tex = NULL;
static void *g_screens[2] = { NULL, NULL };
static int g_screen_index = 0;

static SDL_Texture *g_menu_tex = NULL;
static SDL_Texture *g_bg_tex = NULL;

static int out_x, out_y;
static int out_w = 320, out_h = 224;
static float quad_x, quad_y, quad_w, quad_h;
static int g_out_w = 1280, g_out_h = 720;
static unsigned short active_buf[328 * 256];
static int tex_w = 0, tex_h = 0;

#define PS3_BOOT_DIAG 1

#if PS3_BOOT_DIAG
void ps3_plog(const char *msg)
{
	static int first_call;
	const char *paths[] = {
		"/dev_hdd0/game/PICO00001/USRDIR/boot.log",
		"/dev_hdd0/game/PICO00001/boot.log",
		"/dev_hdd0/tmp/boot.log",
		"/dev_usb000/boot.log",
		"boot.log",
	};
	const char *mode = first_call ? "w" : "a";
	int i;
	first_call = 1;
	for (i = 0; i < 5; i++) {
		FILE *f = fopen(paths[i], mode);
		if (f) {
			fprintf(f, "%s\n", msg);
			fclose(f);
		}
	}
}
#else
void ps3_plog(const char *msg)
{
	(void)msg;
}
#endif

#define PS3_SND_RATE 48000
#define PS3_SND_BUF_SAMPLES 16384
#define PS3_SND_BUF_MASK (PS3_SND_BUF_SAMPLES - 1)
#define PS3_SND_BLOCK_VALUES (AUDIO_BLOCK_SAMPLES * 2)
#define PS3_SND_TARGET 2048

static u32 audio_port = 0;
static audioPortConfig audio_cfg;
static sys_event_queue_t audio_queue;
static sys_ipc_key_t audio_key = 0;
static sys_ppu_thread_t audio_tid;
static volatile int audio_running = 0;
static volatile int sound_stopped = 1;
static volatile int snd_rebuffer = 0;

static short snd_ring[PS3_SND_BUF_SAMPLES];
static volatile u32 snd_write_pos = 0;
static volatile u32 snd_read_pos = 0;

static void sound_deinit(void);

static void audio_fill_block(float *out)
{
	u32 rp, wp, used, n, i;

	if (!out)
		return;
	if (sound_stopped) {
		memset(out, 0, PS3_SND_BLOCK_VALUES * sizeof(float));
		return;
	}

	wp = snd_write_pos;
	rp = snd_read_pos;
	used = wp - rp;

	if (used == 0) {
		memset(out, 0, PS3_SND_BLOCK_VALUES * sizeof(float));
		return;
	}

	n = used < PS3_SND_BLOCK_VALUES ? used : PS3_SND_BLOCK_VALUES;

	if (used > PS3_SND_TARGET + 1536 && n == PS3_SND_BLOCK_VALUES)
		rp += 4;
	else if (used > PS3_SND_TARGET + 512 && n == PS3_SND_BLOCK_VALUES)
		rp += 2;
	else if (used + 1536 < PS3_SND_TARGET && n > 4)
		n -= 4;
	else if (used + 512 < PS3_SND_TARGET && n > 2)
		n -= 2;

	for (i = 0; i < n; i++)
		out[i] = (float)snd_ring[(rp + i) & PS3_SND_BUF_MASK] * (1.0f / 32768.0f);

	if (n < PS3_SND_BLOCK_VALUES) {
		float last = n > 0 ? out[n - 1] : 0.0f;
		for (i = n; i < PS3_SND_BLOCK_VALUES; i++)
			out[i] = last * ((float)(PS3_SND_BLOCK_VALUES - i) / (float)(PS3_SND_BLOCK_VALUES - n + 1));
	}

	__sync_synchronize();
	snd_read_pos = rp + n;
}

static void audio_thread(void *arg)
{
	sys_event_t event;
	float *base = (float *)(u64)audio_cfg.audioDataStart;
	u32 nblocks = (u32)audio_cfg.numBlocks;
	u32 block = 0;

	if (!base || nblocks == 0)
		sysThreadExit(0);

	while (audio_running) {
		if (sysEventQueueReceive(audio_queue, &event, 20000) != 0)
			continue;
		audio_fill_block(base + block * PS3_SND_BLOCK_VALUES);
		block = (block + 1) % nblocks;
	}
	sysThreadExit(0);
}

static void snd_discard(u32 count)
{
	u32 old, nw;

	do {
		old = snd_read_pos;
		nw = old + count;
	} while (!__sync_bool_compare_and_swap(&snd_read_pos, old, nw));
}

static void writeSound(int len)
{
	int samples = len / 2;
	int i;
	short *src = PicoIn.sndOut;
	u32 wp = snd_write_pos;
	u32 used = wp - snd_read_pos;

	if (samples <= 0 || src == NULL)
		return;
	if (used + samples > PS3_SND_BUF_SAMPLES) {
		u32 drop = used + samples - PS3_SND_BUF_SAMPLES;
		snd_discard(drop + (drop & 1));
	}
	for (i = 0; i < samples; i++)
		snd_ring[(wp + i) & PS3_SND_BUF_MASK] = src[i];
	__sync_synchronize();
	snd_write_pos = wp + samples;
}

static void sound_init(void)
{
	audioPortParam aparam;

	if (audioInit() != 0) {
		ps3_plog("audio_init_fail");
		return;
	}
	memset(&aparam, 0, sizeof(aparam));
	aparam.numChannels = AUDIO_PORT_2CH;
	aparam.numBlocks = 8;
	aparam.attrib = AUDIO_PORT_INITLEVEL;
	aparam.level = 1.0f;
	if (audioPortOpen(&aparam, &audio_port) != 0) {
		ps3_plog("audio_port_fail");
		audioQuit();
		return;
	}
	if (audioGetPortConfig(audio_port, &audio_cfg) != 0) {
		ps3_plog("audio_config_fail");
		audioPortClose(audio_port);
		audioQuit();
		return;
	}
	if (audioCreateNotifyEventQueue(&audio_queue, &audio_key) != 0) {
		ps3_plog("audio_queue_fail");
		audioPortClose(audio_port);
		audioQuit();
		return;
	}
	if (audioSetNotifyEventQueue(audio_key) != 0) {
		ps3_plog("audio_setqueue_fail");
		audioRemoveNotifyEventQueue(audio_key);
		audioPortClose(audio_port);
		sysEventQueueDestroy(audio_queue, 0);
		audioQuit();
		return;
	}
	sysEventQueueDrain(audio_queue);
	audio_running = 1;
	if (sysThreadCreate(&audio_tid, audio_thread, NULL, 1000, 32768, THREAD_JOINABLE, "pico_snd") != 0) {
		ps3_plog("audio_thread_fail");
		audio_running = 0;
		audioRemoveNotifyEventQueue(audio_key);
		audioPortClose(audio_port);
		sysEventQueueDestroy(audio_queue, 0);
		audioQuit();
		return;
	}
	if (audioPortStart(audio_port) != 0) {
		ps3_plog("audio_start_fail");
		return;
	}
	ps3_plog("audio_ok");
}

static void __attribute__((unused)) sound_deinit(void)
{
	if (audio_running) {
		audio_running = 0;
		u64 exit_code;
		sysThreadJoin(audio_tid, &exit_code);
		audioPortStop(audio_port);
		audioRemoveNotifyEventQueue(audio_key);
		audioPortClose(audio_port);
		sysEventQueueDestroy(audio_queue, 0);
		audioQuit();
	}
}

void pemu_sound_start(void)
{
	if (!(currentConfig.EmuOpt & EOPT_EN_SOUND))
		return;

	PicoIn.sndRate = PS3_SND_RATE;
	PicoIn.opt |= POPT_EN_STEREO | POPT_EN_MCD_CDDA | POPT_EN_MCD_PCM | POPT_EN_PWM | POPT_PWM_IRQ_OPT;
	emu_sound_start();
	PicoIn.writeSound = writeSound;

	sound_stopped = 1;
	__sync_synchronize();
	memset(snd_ring, 0, sizeof(snd_ring));
	snd_write_pos = 0;
	snd_read_pos = 0;
	snd_rebuffer = 0;
	__sync_synchronize();
	sound_stopped = 0;
}

void pemu_sound_stop(void)
{
	sound_stopped = 1;
}

static void calc_screen_quad(void)
{
	float target_w = 848.0f;
	float target_h = 512.0f;

	if (currentConfig.scaling == EOPT_SCALE_WIDE) {
		quad_x = 0.0f;
		quad_y = 0.0f;
		quad_w = target_w;
		quad_h = target_h;
	} else if (currentConfig.scaling == EOPT_SCALE_NONE) {
		quad_w = (float)out_w * 2.0f;
		quad_h = (float)out_h * 2.0f;
		quad_x = (target_w - quad_w) * 0.5f;
		quad_y = (target_h - quad_h) * 0.5f;
	} else {
		float aspect = 4.0f / 3.0f;
		quad_h = target_h;
		quad_w = target_h * aspect;
		quad_x = (target_w - quad_w) * 0.5f;
		quad_y = 0.0f;
	}
}

static void apply_renderer(void)
{
	PicoIn.opt &= ~(POPT_ALT_RENDERER | POPT_EN_SOFTSCALE);
	PicoDrawSetOutFormat(PDF_RGB555, 0);
}

static volatile int xmb_exit_requested = 0;
static volatile int xmb_menu_open = 0;
static int xmb_exit_done = 0;

static void sysutil_cb(u64 status, u64 param, void *usrdata)
{
	switch (status) {
	case SYSUTIL_EXIT_GAME:
		xmb_exit_requested = 1;
		break;
	case SYSUTIL_MENU_OPEN:
	case SYSUTIL_DRAW_BEGIN:
		xmb_menu_open = 1;
		break;
	case SYSUTIL_MENU_CLOSE:
		xmb_menu_open = 0;
		break;
	default:
		break;
	}
}

void ps3_check_sysutil(void)
{
	sysUtilCheckCallback();
	if (xmb_exit_requested && !xmb_exit_done) {
		xmb_exit_done = 1;
		pemu_sound_stop();
		emu_finish();
		plat_finish();
	}
}

void ps3_drc_disable(void)
{
	currentConfig.s_PicoOpt &= ~POPT_EN_DRC;
	PicoIn.opt &= ~POPT_EN_DRC;
}

void ps3_sdl_render_cp(const char *what)
{
	(void)what;
}

void ps3_sdl_flip_timeout(unsigned int ms)
{
	(void)ms;
}

void ps3_sdl_fifo_blocked(unsigned int ms)
{
	(void)ms;
}

static void video_init(void)
{
	static const Uint32 wflags[] = { 0, SDL_WINDOW_SHOWN };
	static const Uint32 rflags[] = {
		SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE,
		SDL_RENDERER_ACCELERATED,
		SDL_RENDERER_SOFTWARE | SDL_RENDERER_TARGETTEXTURE,
		SDL_RENDERER_SOFTWARE,
		0
	};
	int i;
	int ww = 1280, wh = 720;
	SDL_DisplayMode dm;

	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		ps3_plog("sdl_init_fail");
		sysProcessExit(1);
	}

	if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w >= 640 && dm.h >= 480) {
		ww = dm.w;
		wh = dm.h;
	}
	g_out_w = ww;
	g_out_h = wh;

	for (i = 0; i < 2 && !g_win; i++)
		g_win = SDL_CreateWindow("PicoDrive", 0, 0, ww, wh, wflags[i]);

	if (!g_win)
		g_win = SDL_CreateWindow("PicoDrive", 0, 0, 1280, 720, 0);

	if (g_win) {
		for (i = 0; i < 5 && !g_ren; i++)
			g_ren = SDL_CreateRenderer(g_win, -1, rflags[i]);
	}

	if (!g_win || !g_ren) {
		ps3_plog("sdl_window_fail");
		sysProcessExit(1);
	}

	SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
	SDL_RenderClear(g_ren);
	SDL_RenderPresent(g_ren);

	g_screen_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_BGR555,
		SDL_TEXTUREACCESS_STREAMING, 320, 224);
	tex_w = 320;
	tex_h = 224;
	g_menu_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_BGR555,
		SDL_TEXTUREACCESS_STREAMING, 848, 512);
	g_bg_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888,
		SDL_TEXTUREACCESS_STATIC, 2, 2);
	if (g_bg_tex) {
		Uint32 black = 0xff000000;
		SDL_UpdateTexture(g_bg_tex, NULL, &black, sizeof(black));
	}

	if (g_screen_tex)
		SDL_SetTextureBlendMode(g_screen_tex, SDL_BLENDMODE_NONE);
	if (g_menu_tex)
		SDL_SetTextureBlendMode(g_menu_tex, SDL_BLENDMODE_NONE);
	if (g_bg_tex)
		SDL_SetTextureBlendMode(g_bg_tex, SDL_BLENDMODE_NONE);

	g_screens[0] = malloc(328 * 256 * 2);
	g_screens[1] = malloc(328 * 256 * 2);
	memset(g_screens[0], 0, 328 * 256 * 2);
	memset(g_screens[1], 0, 328 * 256 * 2);

	g_screen_index = 0;
	g_screen_ptr = g_screens[0];
	g_screen_width = 328;
	g_screen_height = 256;
	g_screen_ppitch = 328;

	g_menuscreen_w = 848;
	g_menuscreen_h = 512;
	g_menuscreen_pp = 848;
	g_menuscreen_ptr = malloc(848 * 512 * 2);
	memset(g_menuscreen_ptr, 0, 848 * 512 * 2);

	g_menubg_ptr = malloc(848 * 512 * 2);
	memset(g_menubg_ptr, 0, 848 * 512 * 2);
	g_menubg_src_w = 328;
	g_menubg_src_h = 256;
	g_menubg_src_pp = 328;

	calc_screen_quad();
	sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_cb, NULL);
}

void ps3_early_video(void)
{
	ps3_diag_set(3, "video_pre");
	video_init();
	ps3_diag_set(4, "video_ok");
}

static void __attribute__((unused)) video_deinit(void)
{
	if (g_screen_tex) { SDL_DestroyTexture(g_screen_tex); g_screen_tex = NULL; tex_w = tex_h = 0; }
	if (g_menu_tex) { SDL_DestroyTexture(g_menu_tex); g_menu_tex = NULL; }
	if (g_bg_tex) { SDL_DestroyTexture(g_bg_tex); g_bg_tex = NULL; }
	if (g_ren) { SDL_DestroyRenderer(g_ren); g_ren = NULL; }
	if (g_win) { SDL_DestroyWindow(g_win); g_win = NULL; }
	SDL_Quit();
	if (g_screens[0]) { free(g_screens[0]); g_screens[0] = NULL; }
	if (g_screens[1]) { free(g_screens[1]); g_screens[1] = NULL; }
	if (g_menuscreen_ptr) { free(g_menuscreen_ptr); g_menuscreen_ptr = NULL; }
	if (g_menubg_ptr) { free(g_menubg_ptr); g_menubg_ptr = NULL; }
}

void plat_video_clear_buffers(void)
{
	if (g_screens[0])
		memset(g_screens[0], 0, 328 * 256 * 2);
	if (g_screens[1])
		memset(g_screens[1], 0, 328 * 256 * 2);
	if (g_menubg_ptr)
		memset(g_menubg_ptr, 0, 848 * 512 * 2);
	memset(active_buf, 0, sizeof(active_buf));
}

static void ps3_dst_rect(float x, float y, float w, float h, SDL_Rect *r)
{
	int ww = g_out_w, wh = g_out_h;
	float sx, sy, s;
	int ox, oy;

	if (ww <= 0 || wh <= 0) {
		ww = 1280;
		wh = 720;
	}
	sx = (float)ww / 848.0f;
	sy = (float)wh / 512.0f;
	s = sx < sy ? sx : sy;
	ox = (ww - (int)(848.0f * s)) / 2;
	oy = (wh - (int)(512.0f * s)) / 2;
	r->x = ox + (int)(x * s);
	r->y = oy + (int)(y * s);
	r->w = (int)(w * s);
	r->h = (int)(h * s);
}

static void ps3_game_rect(SDL_Rect *r)
{
	int ww = g_out_w, wh = g_out_h;
	int scale;

	if (ww <= 0 || wh <= 0) {
		ww = 1280;
		wh = 720;
	}
	if (out_w <= 0 || out_h <= 0) {
		r->x = 0;
		r->y = 0;
		r->w = ww;
		r->h = wh;
		return;
	}
	if (currentConfig.scaling == EOPT_SCALE_WIDE) {
		r->x = 0;
		r->y = 0;
		r->w = ww;
		r->h = wh;
		return;
	}
	if (currentConfig.scaling == EOPT_SCALE_NONE) {
		r->w = out_w * 2;
		r->h = out_h * 2;
	} else if (PicoIn.AHW & PAHW_GG) {
		scale = wh / out_h;
		if (scale < 1)
			scale = 1;
		r->w = out_w * scale;
		r->h = out_h * scale;
	} else {
		r->h = wh;
		r->w = (wh * 4) / 3;
	}
	r->x = (ww - r->w) / 2;
	r->y = (wh - r->h) / 2;
}

static void ps3_menu_rect(SDL_Rect *r)
{
	int ww = g_out_w, wh = g_out_h;
	int scale;

	if (ww <= 0 || wh <= 0) {
		ww = 1280;
		wh = 720;
	}
	scale = ww / 848;
	if (wh / 512 < scale)
		scale = wh / 512;
	if (scale >= 2) {
		r->w = 848 * scale;
		r->h = 512 * scale;
		r->x = (ww - r->w) / 2;
		r->y = (wh - r->h) / 2;
	} else {
		ps3_dst_rect(0.0f, 0.0f, 848.0f, 512.0f, r);
	}
}

static void ps3_clear_screen(void)
{
	SDL_Rect r;

	if (!g_ren)
		return;
	if (g_bg_tex) {
		r.x = 0;
		r.y = 0;
		r.w = g_out_w;
		r.h = g_out_h;
		SDL_RenderCopy(g_ren, g_bg_tex, NULL, &r);
		return;
	}
	SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
	SDL_RenderClear(g_ren);
}

static void ps3_clear_borders(const SDL_Rect *dr)
{
	SDL_Rect r;

	if (!g_ren || !g_bg_tex || !dr)
		return;
	if (dr->x > 0) {
		r.x = 0;
		r.y = 0;
		r.w = dr->x;
		r.h = g_out_h;
		SDL_RenderCopy(g_ren, g_bg_tex, NULL, &r);
	}
	if (dr->x + dr->w < g_out_w) {
		r.x = dr->x + dr->w;
		r.y = 0;
		r.w = g_out_w - (dr->x + dr->w);
		r.h = g_out_h;
		SDL_RenderCopy(g_ren, g_bg_tex, NULL, &r);
	}
	if (dr->y > 0) {
		r.x = dr->x;
		r.y = 0;
		r.w = dr->w;
		r.h = dr->y;
		SDL_RenderCopy(g_ren, g_bg_tex, NULL, &r);
	}
	if (dr->y + dr->h < g_out_h) {
		r.x = dr->x;
		r.y = dr->y + dr->h;
		r.w = dr->w;
		r.h = g_out_h - (dr->y + dr->h);
		SDL_RenderCopy(g_ren, g_bg_tex, NULL, &r);
	}
}

void plat_video_flip(void)
{
	unsigned short *src = (unsigned short *)g_screen_ptr;
	SDL_Rect dr;
	int y;

	if (xmb_exit_requested || !src || !g_ren)
		return;

	if (out_w <= 0 || out_h <= 0 || out_w > 328 || out_h > 256) {
		out_w = 320;
		out_h = 224;
		out_x = 0;
		out_y = 0;
	}

	if (!g_screen_tex || tex_w != out_w || tex_h != out_h) {
		if (g_screen_tex)
			SDL_DestroyTexture(g_screen_tex);
		g_screen_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_BGR555,
			SDL_TEXTUREACCESS_STREAMING, out_w, out_h);
		if (g_screen_tex)
			SDL_SetTextureBlendMode(g_screen_tex, SDL_BLENDMODE_NONE);
		tex_w = out_w;
		tex_h = out_h;
	}

	if (!g_screen_tex)
		return;

	for (y = 0; y < out_h; y++)
		memcpy(&active_buf[y * out_w], &src[(out_y + y) * 328 + out_x], out_w * 2);

	SDL_UpdateTexture(g_screen_tex, NULL, active_buf, out_w * 2);
	ps3_game_rect(&dr);

	SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
	ps3_clear_screen();
	SDL_RenderCopy(g_ren, g_screen_tex, NULL, &dr);
	SDL_RenderPresent(g_ren);
	ps3_check_sysutil();

	g_screen_index ^= 1;
	g_screen_ptr = g_screens[g_screen_index];
	plat_video_set_buffer(g_screen_ptr);
}

void plat_video_wait_vsync(void)
{
}

void plat_video_menu_update(void)
{
}

static int in_menu_active = 0;

void plat_video_menu_enter(int is_rom_loaded)
{
	static int diag_once;
	in_menu_active = 1;
	if (!diag_once++)
		ps3_diag_set(9, "menu_enter");
}

void plat_video_menu_begin(void)
{
	in_menu_active = 1;
	if (g_menubg_ptr && g_menuscreen_ptr)
		memcpy(g_menuscreen_ptr, g_menubg_ptr, 848 * 512 * 2);
}

void plat_video_menu_end(void)
{
	static int first_flip;
	SDL_Rect dr;

	if (xmb_exit_requested || !g_menuscreen_ptr || !g_menu_tex || !g_ren)
		return;

	in_menu_active = 1;
	if (!first_flip++)
		ps3_diag_set(10, "first_flip");

	SDL_UpdateTexture(g_menu_tex, NULL, g_menuscreen_ptr, 848 * 2);
	ps3_menu_rect(&dr);

	ps3_clear_borders(&dr);
	SDL_RenderCopy(g_ren, g_menu_tex, NULL, &dr);
	SDL_RenderPresent(g_ren);
	ps3_check_sysutil();
}

void ps3_menu_idle_flip(int ms)
{
	static int was_open = 0;
	SDL_Rect dr;

	ps3_check_sysutil();
	if (!xmb_exit_requested && in_menu_active && (xmb_menu_open || was_open) && g_ren && g_menu_tex && g_menuscreen_ptr) {
		was_open = xmb_menu_open;
		ps3_menu_rect(&dr);
		ps3_clear_borders(&dr);
		SDL_RenderCopy(g_ren, g_menu_tex, NULL, &dr);
		SDL_RenderPresent(g_ren);
	}
	if (ms > 0)
		usleep(ms * 1000);
	ps3_check_sysutil();
}

void plat_video_menu_leave(void)
{
	in_menu_active = 0;
	g_screen_ptr = g_screens[g_screen_index];
	plat_video_set_buffer(g_screen_ptr);
}

void pemu_prep_defconfig(void)
{
	defaultConfig.s_PsndRate = PS3_SND_RATE;
	defaultConfig.s_PicoCDBuffers = 64;
	defaultConfig.filter = EOPT_FILTER_BILINEAR;
	defaultConfig.scaling = EOPT_SCALE_43;
	defaultConfig.vscaling = EOPT_VSCALE_FULL;
	defaultConfig.renderer = RT_16BIT;
	defaultConfig.renderer32x = RT_16BIT;
	defaultConfig.Frameskip = 0;
	defaultConfig.max_skip = 0;
	defaultConfig.EmuOpt |= EOPT_SHOW_RTC | EOPT_EN_SOUND;
	defaultConfig.s_PicoOpt |= POPT_EN_DRC | POPT_EN_MCD_PCM | POPT_EN_MCD_CDDA | POPT_EN_MCD_GFX | POPT_EN_32X | POPT_EN_PWM | POPT_PWM_IRQ_OPT;
	ps3_plog("defconfig_done");
}

void pemu_validate_config(void)
{
	currentConfig.s_PicoOpt |= POPT_EN_DRC | POPT_EN_32X | POPT_EN_PWM | POPT_PWM_IRQ_OPT;
	PicoIn.opt |= POPT_EN_DRC | POPT_EN_32X | POPT_EN_PWM | POPT_PWM_IRQ_OPT;
	currentConfig.Frameskip = 0;
	currentConfig.max_skip = 0;
	if (!(currentConfig.EmuOpt & EOPT_EN_SOUND)) {
		currentConfig.EmuOpt |= EOPT_EN_SOUND;
		ps3_plog("sound_enabled");
	}
	ps3_diag_set(8, "config_ok");
}

void plat_init(void)
{
	sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_cb, NULL);
	in_ps3_init(in_ps3_defbinds_list);
	in_probe();
	ps3_diag_set(6, "input_ok");
	sound_init();
	ps3_diag_set(7, "audio_ok");
	plat_get_data_dir(rom_fname_loaded, sizeof(rom_fname_loaded));
	ps3_diag_set(8, "plat_ok");
}

void plat_finish(void)
{
	pemu_sound_stop();
	sound_deinit();
	plat_mem_free_for_drc();
	ioPadEnd();
	sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
	sysProcessExit(0);
}

void plat_status_msg_busy_first(const char *msg)
{
	pemu_sound_stop();
	plat_status_msg_busy_next(msg);
}

void plat_status_msg_busy_next(const char *msg)
{
	pemu_finalize_frame("", msg);
	plat_video_flip();
}

void plat_status_msg_busy_done(void)
{
	pemu_sound_start();
}

void plat_status_msg_clear(void)
{
}

void plat_update_volume(int has_changed, int is_up)
{
}

void emu_video_mode_change(int start_line, int line_count, int start_col, int col_count)
{
	out_y = start_line;
	out_x = start_col;
	out_h = line_count;
	out_w = col_count;
	if (g_screens[0])
		memset(g_screens[0], 0, 328 * 256 * 2);
	if (g_screens[1])
		memset(g_screens[1], 0, 328 * 256 * 2);
	memset(active_buf, 0, sizeof(active_buf));
	calc_screen_quad();
	Pico.m.dirtyPal = 1;
}

void pemu_forced_frame(int no_scale, int do_emu)
{
	is_bg_frame = 1;
	Pico.m.dirtyPal = 1;
	if (!no_scale)
		no_scale = (currentConfig.scaling == EOPT_SCALE_NONE);
	emu_cmn_forced_frame(no_scale, do_emu, g_screen_ptr);
	g_menubg_src_ptr = g_screen_ptr;
	is_bg_frame = 0;
}

void plat_video_toggle_renderer(int change, int is_menu_call)
{
	if (!change) {
		apply_renderer();
		plat_video_set_buffer(g_screen_ptr);
		calc_screen_quad();
	}
}

void plat_video_set_buffer(void *buf)
{
	PicoDrawSetOutBuf(g_screen_ptr, g_screen_ppitch * 2);
}

void plat_video_loop_prepare(void)
{
	apply_renderer();
	plat_video_set_buffer(g_screen_ptr);
	calc_screen_quad();
}

void plat_show_cursor(int on)
{
}

int plat_grab_cursor(int on)
{
	return 0;
}

int plat_has_wm(void)
{
	return 0;
}

void pemu_loop_prep(void)
{
	plat_video_clear_buffers();
}

void pemu_loop_end(void)
{
	pemu_sound_stop();
	pemu_forced_frame(0, 1);
}

void pemu_finalize_frame(const char *fps, const char *notice)
{
	if (notice && notice[0])
		emu_osd_text16(out_x + 4, out_y + 4, notice);
	if ((currentConfig.EmuOpt & 2) && fps && fps[0] && out_w >= 130 && out_h >= 20)
		emu_osd_text16(out_x + out_w - 100, out_y + 4, fps);
}
