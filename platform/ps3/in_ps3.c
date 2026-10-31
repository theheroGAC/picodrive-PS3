#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <io/pad.h>
#include "../libpicofe/input.h"
#include "../common/emu.h"
#include "in_ps3.h"

#define IN_PS3_PREFIX "ps3:"
#define IN_PS3_NBUTTONS 16

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

static struct in_default_bind *in_ps3_defbinds[4];
static int in_ps3_combo_keys[4];
static int in_ps3_combo_acts[4];
static const char *in_ps3_keys[IN_PS3_NBUTTONS];
static unsigned old_keys[4];
static unsigned pad_state[MAX_PORT_NUM];
static unsigned char pad_valid[MAX_PORT_NUM];

static int lg2(unsigned v)
{
	int r, s;
	r = (v > 0xFFFF) << 4; v >>= r;
	s = (v > 0xFF  ) << 3; v >>= s; r |= s;
	s = (v > 0xF   ) << 2; v >>= s; r |= s;
	s = (v > 0x3   ) << 1; v >>= s; r |= s;
	r |= (v >> 1);
	return r;
}

static unsigned int ps3_pad_read(int pad)
{
	padInfo info;
	padData data;
	unsigned int res = 0;

	if (pad < 0 || pad >= MAX_PORT_NUM)
		return 0;

	if (ioPadGetInfo(&info) != 0 || !info.status[pad]) {
		pad_state[pad] = 0;
		pad_valid[pad] = 0;
		return 0;
	}

	if (ioPadGetData(pad, &data) != 0) {
		pad_state[pad] = 0;
		pad_valid[pad] = 0;
		return 0;
	}

	if (data.len > 0) {
		unsigned int b1 = data.button[2];
		unsigned int b2 = data.button[3];
		if (b1 & PAD_CTRL_UP)       res |= PS3_PAD_UP;
		if (b1 & PAD_CTRL_DOWN)     res |= PS3_PAD_DOWN;
		if (b1 & PAD_CTRL_LEFT)     res |= PS3_PAD_LEFT;
		if (b1 & PAD_CTRL_RIGHT)    res |= PS3_PAD_RIGHT;
		if (b1 & PAD_CTRL_START)    res |= PS3_PAD_START;
		if (b1 & PAD_CTRL_SELECT)   res |= PS3_PAD_SELECT;
		if (b1 & PAD_CTRL_L3)       res |= PS3_PAD_L3;
		if (b1 & PAD_CTRL_R3)       res |= PS3_PAD_R3;
		if (b2 & PAD_CTRL_TRIANGLE) res |= PS3_PAD_TRIANGLE;
		if (b2 & PAD_CTRL_CIRCLE)   res |= PS3_PAD_CIRCLE;
		if (b2 & PAD_CTRL_CROSS)    res |= PS3_PAD_CROSS;
		if (b2 & PAD_CTRL_SQUARE)   res |= PS3_PAD_SQUARE;
		if (b2 & PAD_CTRL_L1)       res |= PS3_PAD_L1;
		if (b2 & PAD_CTRL_R1)       res |= PS3_PAD_R1;
		if (b2 & PAD_CTRL_L2)       res |= PS3_PAD_L2;
		if (b2 & PAD_CTRL_R2)       res |= PS3_PAD_R2;
		if (data.len >= 8) {
			int lx = data.button[6] & 0xff;
			int ly = data.button[7] & 0xff;
			int dz = currentConfig.analog_deadzone;
			int thresh;
			if (dz < 1 || dz > 99)
				dz = 30;
			thresh = (dz * 128) / 100;
			if (thresh < 24)
				thresh = 24;
			if (thresh > 96)
				thresh = 96;
			if (lx < 128 - thresh)
				res |= PS3_PAD_LEFT;
			if (lx > 128 + thresh)
				res |= PS3_PAD_RIGHT;
			if (ly < 128 - thresh)
				res |= PS3_PAD_UP;
			if (ly > 128 + thresh)
				res |= PS3_PAD_DOWN;
		}
		pad_state[pad] = res;
		pad_valid[pad] = 1;
	}

	return pad_valid[pad] ? pad_state[pad] : 0;
}

static unsigned in_ps3_get_bits(int pad)
{
	return ps3_pad_read(pad);
}

static void in_ps3_probe(const in_drv_t *drv)
{
	char name[32];
	int num = (int)(intptr_t)drv->pdata;
	snprintf(name, sizeof(name), "%sPS3 pad %d", IN_PS3_PREFIX, num + 1);
	in_register(name, -1, (void *)(intptr_t)num, IN_PS3_NBUTTONS, in_ps3_keys, 1);
}

static void in_ps3_free(void *drv_data)
{
}

static const char * const *in_ps3_get_key_names(const in_drv_t *drv, int *count)
{
	*count = IN_PS3_NBUTTONS;
	return in_ps3_keys;
}

static int in_ps3_update(void *drv_data, const int *binds, int *result)
{
	int pad = (int)(intptr_t)drv_data & 3;
	int type_start = 0;
	int i, t;
	unsigned keys = in_ps3_get_bits(pad);

	if (keys & in_ps3_combo_keys[pad]) {
		result[IN_BINDTYPE_EMU] = in_combos_do(keys, binds, IN_PS3_NBUTTONS,
					in_ps3_combo_keys[pad], in_ps3_combo_acts[pad]);
		type_start = IN_BINDTYPE_PLAYER12;
	}

	for (i = 0; keys; i++, keys >>= 1) {
		if (!(keys & 1))
			continue;
		for (t = type_start; t < IN_BINDTYPE_COUNT; t++)
			result[t] |= binds[IN_BIND_OFFS(i, t)];
	}
	return 0;
}

int in_ps3_update_keycode(void *data, int *is_down)
{
	int pad = (int)(intptr_t)data & 3;
	unsigned val, diff, i;

	val = in_ps3_get_bits(pad);
	diff = val ^ old_keys[pad];
	if (diff == 0)
		return -1;

	for (i = 0; i < sizeof(diff) * 8; i++) {
		if (diff & (1 << i))
			break;
	}

	old_keys[pad] ^= 1 << i;
	if (is_down)
		*is_down = !!(val & (1 << i));
	return i;
}

static struct {
	unsigned key;
	int pbtn;
} key_pbtn_map[] =
{
	{ PS3_PAD_UP,       PBTN_UP },
	{ PS3_PAD_DOWN,     PBTN_DOWN },
	{ PS3_PAD_LEFT,     PBTN_LEFT },
	{ PS3_PAD_RIGHT,    PBTN_RIGHT },
	{ PS3_PAD_CROSS,    PBTN_MOK },
	{ PS3_PAD_CIRCLE,   PBTN_MBACK },
	{ PS3_PAD_TRIANGLE, PBTN_MA2 },
	{ PS3_PAD_SQUARE,   PBTN_MA3 },
	{ PS3_PAD_L1,       PBTN_L },
	{ PS3_PAD_R1,       PBTN_R },
};

#define KEY_PBTN_MAP_SIZE (sizeof(key_pbtn_map) / sizeof(key_pbtn_map[0]))

static int in_ps3_menu_translate(void *drv_data, int keycode, char *charcode)
{
	int i;
	if (keycode < 0) {
		keycode = -keycode;
		for (i = 0; i < KEY_PBTN_MAP_SIZE; i++) {
			if (key_pbtn_map[i].pbtn == keycode)
				return key_pbtn_map[i].key;
		}
	} else {
		for (i = 0; i < KEY_PBTN_MAP_SIZE; i++) {
			if (key_pbtn_map[i].key == keycode)
				return key_pbtn_map[i].pbtn;
		}
	}
	return 0;
}

static int in_ps3_clean_binds(void *drv_data, int *binds, int *def_binds)
{
	int pad = (int)(intptr_t)drv_data & 3;
	int i, count = 0;

	for (i = 0; i < IN_PS3_NBUTTONS; i++) {
		int t, offs;
		for (t = 0; t < IN_BINDTYPE_COUNT; t++) {
			offs = IN_BIND_OFFS(i, t);
			if (in_ps3_keys[i] == NULL)
				binds[offs] = def_binds[offs] = 0;
			if (binds[offs])
				count++;
		}
	}
	in_combos_find(binds, IN_PS3_NBUTTONS, &in_ps3_combo_keys[pad], &in_ps3_combo_acts[pad]);
	return count;
}

static const in_drv_t in_ps3_drv = {
	.prefix         = IN_PS3_PREFIX,
	.probe          = in_ps3_probe,
	.free           = in_ps3_free,
	.get_key_names  = in_ps3_get_key_names,
	.clean_binds    = in_ps3_clean_binds,
	.update         = in_ps3_update,
	.update_keycode = in_ps3_update_keycode,
	.menu_translate = in_ps3_menu_translate,
};

static struct in_default_bind *copy_defbinds(struct in_default_bind *defbinds)
{
	struct in_default_bind *newbinds;
	int count = 0;
	while (defbinds[count].code | defbinds[count].btype | defbinds[count].bit)
		count++;
	newbinds = malloc((count + 1) * sizeof(*defbinds));
	if (newbinds)
		memcpy(newbinds, defbinds, (count + 1) * sizeof(*defbinds));
	return newbinds;
}

void in_ps3_init(struct in_default_bind *defbinds)
{
	int i, j;

	for (i = 0; defbinds[i].code; i++)
		defbinds[i].code = lg2(defbinds[i].code);
	for (i = 0; i < KEY_PBTN_MAP_SIZE; i++)
		key_pbtn_map[i].key = lg2(key_pbtn_map[i].key);

	memset(in_ps3_combo_keys, 0, sizeof(in_ps3_combo_keys));
	memset(in_ps3_combo_acts, 0, sizeof(in_ps3_combo_acts));

	in_ps3_keys[lg2(PS3_PAD_UP)] = "Up";
	in_ps3_keys[lg2(PS3_PAD_DOWN)] = "Down";
	in_ps3_keys[lg2(PS3_PAD_LEFT)] = "Left";
	in_ps3_keys[lg2(PS3_PAD_RIGHT)] = "Right";
	in_ps3_keys[lg2(PS3_PAD_START)] = "Start";
	in_ps3_keys[lg2(PS3_PAD_SELECT)] = "Select";
	in_ps3_keys[lg2(PS3_PAD_TRIANGLE)] = "Triangle";
	in_ps3_keys[lg2(PS3_PAD_CIRCLE)] = "Circle";
	in_ps3_keys[lg2(PS3_PAD_CROSS)] = "Cross";
	in_ps3_keys[lg2(PS3_PAD_SQUARE)] = "Square";
	in_ps3_keys[lg2(PS3_PAD_L1)] = "L1";
	in_ps3_keys[lg2(PS3_PAD_R1)] = "R1";
	in_ps3_keys[lg2(PS3_PAD_L2)] = "L2";
	in_ps3_keys[lg2(PS3_PAD_R2)] = "R2";
	in_ps3_keys[lg2(PS3_PAD_L3)] = "L3";
	in_ps3_keys[lg2(PS3_PAD_R3)] = "R3";

	for (i = 0; i < 4; i++) {
		in_ps3_defbinds[i] = copy_defbinds(defbinds);
		for (j = 0; in_ps3_defbinds[i]; j++) {
			struct in_default_bind *p = &in_ps3_defbinds[i][j];
			if ((p->code | p->btype | p->bit) == 0)
				break;
			if (p->btype == IN_BINDTYPE_PLAYER12) {
				p->btype += (i >= 2);
				p->bit += (i & 1) * 16;
			}
		}
	}

	in_register_driver(&in_ps3_drv, in_ps3_defbinds[0], NULL, (void *)(intptr_t)0);
	in_register_driver(&in_ps3_drv, in_ps3_defbinds[1], NULL, (void *)(intptr_t)1);
	in_register_driver(&in_ps3_drv, in_ps3_defbinds[2], NULL, (void *)(intptr_t)2);
	in_register_driver(&in_ps3_drv, in_ps3_defbinds[3], NULL, (void *)(intptr_t)3);
}

void in_ps3_deinit(void)
{
	int i;
	for (i = 0; i < 4; i++) {
		if (in_ps3_defbinds[i]) {
			free(in_ps3_defbinds[i]);
			in_ps3_defbinds[i] = NULL;
		}
	}
}
