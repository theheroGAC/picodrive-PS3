#include <stdio.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <stdlib.h>
#include <malloc.h>
#include <errno.h>
#include <io/pad.h>
#include <sysutil/sysutil.h>
#include <ppu-lv2.h>
#include <sys/process.h>
#include "../libpicofe/plat.h"

static int sound_rates[] = { 11025, 22050, 44100, 48000, -1 };
struct plat_target plat_target = { .sound_rates = sound_rates };

int plat_target_init(void)
{
	return 0;
}

void plat_target_finish(void)
{
}

int plat_parse_arg(int argc, char *argv[], int *x)
{
	return 1;
}

void ps3_plog(const char *msg);

void ps3_diag_set(int stage, const char *msg)
{
	char buf[48];

	snprintf(buf, sizeof(buf), "S%d %s", stage, msg);
	ps3_plog(buf);
}

void plat_early_init(void)
{
	void ps3_early_video(void);
	DIR *d;

	ioPadInit(7);
	ps3_diag_set(1, "enter");

	ps3_early_video();

	d = opendir("/dev_hdd0/game/PICO00001/USRDIR");
	if (d) {
		closedir(d);
		chdir("/dev_hdd0/game/PICO00001/USRDIR");
	}
	mkdir("/dev_hdd0/picodrive", 0777);
	mkdir("/dev_hdd0/picodrive/roms", 0777);
	ps3_diag_set(5, "fs_ok");
}

int plat_get_root_dir(char *dst, int len)
{
	DIR *d = opendir("/dev_hdd0/game/PICO00001/USRDIR");
	if (d) {
		closedir(d);
		snprintf(dst, len, "/dev_hdd0/game/PICO00001/USRDIR/");
		return strlen(dst);
	}
	getcwd(dst, len);
	strncat(dst, "/", len - strlen(dst) - 1);
	return strlen(dst);
}

int plat_get_skin_dir(char *dst, int len)
{
	int l = plat_get_root_dir(dst, len);
	strncat(dst, "skin/", len - l - 1);
	return strlen(dst);
}

int plat_get_data_dir(char *dst, int len)
{
	static const char *dirs[] = {
		"/dev_hdd0/picodrive/roms",
		"/dev_hdd0/game/PICO00001/USRDIR/roms",
		"/dev_hdd0/roms",
		"/dev_usb000/roms",
		"/dev_usb000",
		"/dev_usb001/roms",
		"/dev_usb001",
		"/dev_hdd0",
	};
	int i;

	for (i = 0; i < (int)(sizeof(dirs) / sizeof(dirs[0])); i++) {
		DIR *d = opendir(dirs[i]);
		if (d) {
			closedir(d);
			snprintf(dst, len, "%s", dirs[i]);
			return strlen(dst);
		}
	}
	return plat_get_root_dir(dst, len);
}

int plat_is_dir(const char *path)
{
	DIR *dir = opendir(path);
	if (dir) {
		closedir(dir);
		return 1;
	}
	return 0;
}

int scandir(const char *dirp, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **))
{
	DIR *d = opendir(dirp);
	if (!d) return -1;
	struct dirent *entry;
	struct dirent **names = NULL;
	int count = 0, capacity = 0;
	while ((entry = readdir(d)) != NULL) {
		if (filter && !filter(entry)) continue;
		if (count >= capacity) {
			capacity = capacity ? capacity * 2 : 16;
			struct dirent **tmp = realloc(names, capacity * sizeof(struct dirent *));
			if (!tmp) {
				while (count > 0) free(names[--count]);
				free(names);
				closedir(d);
				return -1;
			}
			names = tmp;
		}
		size_t len = sizeof(struct dirent);
		names[count] = malloc(len);
		if (!names[count]) {
			while (count > 0) free(names[--count]);
			free(names);
			closedir(d);
			return -1;
		}
		memcpy(names[count], entry, len);
		count++;
	}
	closedir(d);
	if (compar && count > 1) {
		qsort(names, count, sizeof(struct dirent *), (int (*)(const void *, const void *))compar);
	}
	*namelist = names;
	return count;
}

int alphasort(const struct dirent **a, const struct dirent **b)
{
	return strcoll((*a)->d_name, (*b)->d_name);
}

unsigned int plat_get_ticks_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned int)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

unsigned int plat_get_ticks_us(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned int)(tv.tv_sec * 1000000 + tv.tv_usec);
}

void ps3_check_sysutil(void);
void ps3_menu_idle_flip(int ms);
void ps3_drc_disable(void);

void plat_wait_till_us(unsigned int us_to)
{
	unsigned int ticks = plat_get_ticks_us();
	ps3_check_sysutil();
	if (us_to > ticks)
		usleep(us_to - ticks);
}

void plat_sleep_ms(int ms)
{
	ps3_check_sysutil();
	usleep(ms * 1000);
	ps3_check_sysutil();
}

int plat_wait_event(int *fds_hnds, int count, int timeout_ms)
{
	int ms = 15;
	if (timeout_ms >= 0 && timeout_ms < ms)
		ms = timeout_ms;
	ps3_menu_idle_flip(ms);
	return 0;
}

void *plat_mmap(unsigned long addr, size_t size, int need_exec, int is_fixed)
{
	return malloc(size);
}

void *plat_mremap(void *ptr, size_t oldsize, size_t newsize)
{
	return realloc(ptr, newsize);
}

void plat_munmap(void *ptr, size_t size)
{
	free(ptr);
}

#define SYSCALL8_OPCODE_PS3MAPI 0x7777
#define PS3MAPI_OPCODE_PROC_PAGE_ALLOCATE 0x0033
#define PS3MAPI_OPCODE_PROC_PAGE_FREE 0x0034
#define PAGE_SIZE_AUTO 0

static uint64_t g_drc_page_table[2] = {0, 0};

static int ps3mapi_page_allocate(sys_pid_t pid, uint64_t size, uint64_t page_size, uint64_t flags, uint64_t is_executable, uint64_t *page_table)
{
	lv2syscall8(8, SYSCALL8_OPCODE_PS3MAPI, PS3MAPI_OPCODE_PROC_PAGE_ALLOCATE, (uint64_t)pid, (uint64_t)size, (uint64_t)page_size, (uint64_t)flags, (uint64_t)is_executable, (uint64_t)page_table);
	return_to_user_prog(s32);
}

static int ps3mapi_page_free(sys_pid_t pid, uint64_t flags, uint64_t *page_table)
{
	lv2syscall5(8, SYSCALL8_OPCODE_PS3MAPI, PS3MAPI_OPCODE_PROC_PAGE_FREE, (uint64_t)pid, (uint64_t)flags, (uint64_t)page_table);
	return_to_user_prog(s32);
}

void *plat_mem_get_for_drc(size_t size)
{
	if (g_drc_page_table[0] != 0)
		return (void *)g_drc_page_table[0];

	sys_pid_t pid = sysProcessGetPid();
	int ret = ps3mapi_page_allocate(pid, size, PAGE_SIZE_AUTO, 0x2F, 1, g_drc_page_table);
	if (ret == 0 && g_drc_page_table[0] != 0)
		return (void *)g_drc_page_table[0];

	g_drc_page_table[0] = 0;
	g_drc_page_table[1] = 0;
	return NULL;
}

int plat_mem_set_exec(void *ptr, size_t size)
{
	if (ptr != NULL && g_drc_page_table[0] != 0 && (uint64_t)ptr == g_drc_page_table[0])
		return 0;
	ps3_drc_disable();
	return -1;
}

void plat_mem_free_for_drc(void)
{
	if (g_drc_page_table[0] != 0 && g_drc_page_table[1] != 0) {
		sys_pid_t pid = sysProcessGetPid();
		ps3mapi_page_free(pid, 0x2F, g_drc_page_table);
		g_drc_page_table[0] = 0;
		g_drc_page_table[1] = 0;
	}
}

int plat_drc_supported(void)
{
	return (g_drc_page_table[0] != 0);
}

void lprintf(const char *fmt, ...)
{
	va_list vl;
	va_start(vl, fmt);
	vprintf(fmt, vl);
	va_end(vl);
}

void plat_debug_cat(char *str)
{
}
