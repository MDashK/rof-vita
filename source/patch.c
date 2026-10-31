/*
 * Copyright (C) 2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  patch.c
 * @brief Patching some of the .so internal functions or bridging them to native
 *        for better compatibility.
 */

#include <kubridge.h>
#include <so_util/so_util.h>
#include <stdio.h>
#include <string.h>
#include <vitasdk.h>

#ifdef __cplusplus
extern "C"
{
#endif
	extern so_module so_mod;
#ifdef __cplusplus
};
#endif

#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX (0x0C20D050)

#include "utils/logger.h"
#include "utils/dialog.h"
#include "reimpl/sys.h"
#include <stdbool.h>
#include <math.h>
#include "rof/rof.h"

void __kuser_memory_barrier(void) {
	__sync_synchronize();
}

void kuser_patch(void) {
	SceKernelAllocMemBlockKernelOpt opt;
	memset(&opt, 0, sizeof(SceKernelAllocMemBlockKernelOpt));
	opt.size = sizeof(SceKernelAllocMemBlockKernelOpt);
	opt.attr = 0x1;
	opt.field_C = (SceUInt32)0x9A000000;
	if (kuKernelAllocMemBlock("atomic", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, 0x1000, &opt) < 0)
		fatal_error("Error could not allocate atomic block.");
	kuKernelMemProtect((void *)0x9A000000, (SceSize)0x1000, KU_KERNEL_PROT_EXEC | KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE);

	hook_addr(0x9A000FA0, (uintptr_t)__kuser_memory_barrier);
	hook_addr(0x9A000FC0, (uintptr_t)__atomic_cmpxchg);

	uint32_t patched_addr;
	for (uint32_t addr = so_mod.text_base; addr < so_mod.text_base + so_mod.text_size; addr += 4) {
		uint32_t *a = (uint32_t *)addr;
		if (*a == 0xFFFF0FC0) {
			l_debug("Patching 0x%x -> __kuser_cmpxchg", a);
			patched_addr = 0x9A000FC0;
			kuKernelCpuUnrestrictedMemcpy((void *)(addr), &patched_addr, sizeof(uint32_t));
		}
		else if (*a == 0xFFFF0FA0) {
			l_debug("Patching 0x%x -> __kuser_memory_barrier", a);
			patched_addr = 0x9A000FA0;
			kuKernelCpuUnrestrictedMemcpy((void *)(addr), &patched_addr, sizeof(uint32_t));
		}
	}
}

/*
 * tunes.dat keeps one "chart is up to date" byte per song (AppMain+0xd29d).
 * The game zeroes some of them when SNK published chart fixes, and then
 * AppMain::CheckTuneResData() insists on re-downloading those songs even if
 * the files are present. The servers are gone, so after the save is loaded
 * mark every song as current and let the real file check decide.
 * (Offsets from AppMain::InitTunesData / SC_DownloadInitFromOption.)
 */
#define TUNES_FLAGS_OFFSET 0xd29d
#define TUNES_FLAGS_COUNT  0x100

static so_hook save_tunes_init_hook;

static void SaveTunesDataInit_hook(void *app) {
	SO_CONTINUE(int, save_tunes_init_hook, app);
	uint8_t *flags = (uint8_t *)app + TUNES_FLAGS_OFFSET;
	int fixed = 0;
	for (int i = 0; i < TUNES_FLAGS_COUNT; i++) {
		if (flags[i] != 1) {
			flags[i] = 1;
			fixed++;
		}
	}
	if (fixed)
		l_info("tunes.dat: %i song(s) no longer wait for a chart update.", fixed);
}

/*
 * Font::stringWidth(const char*) asks Java (getFontWidthJava) through JNI on
 * every call - several times per frame for every centred text - and each
 * call allocates a jstring. Measure natively instead: same result,
 * (int)ceil(width of str at the Font's size), no JNI round trip.
 */
static int Font_stringWidth(void *font, const char *str) {
	if (!font || !str)
		return 0;
	return (int)ceilf(text_measure(*(int *)font, str));
}

/*
 * Menu buttons for services that no longer exist. Each button is a 2D task
 * whose function draws it and handles its touch; the button index is at
 * task+0x98. A hidden button's task simply does nothing (it stays alive, so
 * the scene code that polls its "pressed" flag keeps working).
 *   Title: menu 3 = Achievements, 4 = Ranking; icons 6, 7, 8 = Facebook /
 *          SNK "more apps" / Twitter links (top left).
 *   Options: 5 = "Retrieving data" (re-download from SNK servers).
 *   Shop: the "Restore" (purchases) button.
 */
#define TASK_INDEX(t) (*(int *)((char *)(t) + 0x98))

static so_hook title_menu_hook, option_menu_hook;

static int GT_TitleMenu_hook(void *task) {
	int idx = TASK_INDEX(task);
	if (idx == 3 || idx == 4)
		return 0;
	return SO_CONTINUE(int, title_menu_hook, task);
}

static int GT_TitleIcon_hook(void *task) {
	return 0; // all three link icons are hidden
}

static int GT_HiddenTask_hook(void *task) {
	return 0;
}

static int GT_OptionMenu_hook(void *task) {
	if (TASK_INDEX(task) == 5)
		return 0;
	return SO_CONTINUE(int, option_menu_hook, task);
}

static void hide_dead_buttons(void) {
	uintptr_t f;
	if ((f = so_symbol(&so_mod, "_Z12GT_TitleMenuP17GENERAL_TASK_BASE")))
		title_menu_hook = hook_addr(f, (uintptr_t)&GT_TitleMenu_hook);
	if ((f = so_symbol(&so_mod, "_Z12GT_TitleIconP17GENERAL_TASK_BASE")))
		hook_addr(f, (uintptr_t)&GT_TitleIcon_hook);
	if ((f = so_symbol(&so_mod, "_Z13GT_OptionMenuP17GENERAL_TASK_BASE")))
		option_menu_hook = hook_addr(f, (uintptr_t)&GT_OptionMenu_hook);
	if ((f = so_symbol(&so_mod, "_Z14GT_RestoreMenuP17GENERAL_TASK_BASE")))
		hook_addr(f, (uintptr_t)&GT_HiddenTask_hook);
}

/*
 * Options > Credits: put "PSVITA PORT BY / MDASHK" above "STAFF CREDITS".
 * DrawStaffRoll draws everything relative to the scroll value in task+0x98,
 * so the original roll is drawn CREDITS_EXTRA lower and our two lines go on
 * top, in the same letter font (DrawPosition, scale 0.35, same clip). The
 * scroll end is extended by the same amount so the last names still show.
 */
#define CREDITS_EXTRA 160

static so_hook draw_staff_hook, create_staff_hook;
static void (*DrawPosition)(char *, int, int, float);
static void (*GraphicsOpt_setClip)(void *, int, int, int, int);
static void (*GraphicsOpt_clearClip)(void *);
static int (*AppMain_getWidth)(void *);
static void *(*AppMain_getInstance)(void);

static void DrawStaffRoll_hook(void *task) {
	int *scroll = (int *)((char *)task + 0x98);
	int saved = *scroll;
	*scroll = saved + CREDITS_EXTRA;
	SO_CONTINUE(int, draw_staff_hook, task);
	*scroll = saved;

	char *app = AppMain_getInstance();
	char *win = app ? *(char **)(app + 0x3dcc) : NULL;
	if (!win || !win[0x1ec])
		return;
	void *gfx = *(void **)(app + 0x6c);
	GraphicsOpt_setClip(gfx, 0, 0x3c, AppMain_getWidth(app), 0x1c2);
	// x centred on the original title (widths from the letter table)
	DrawPosition((char *)"PSVITA PORT BY", 282, saved + 150, 0.35f);
	DrawPosition((char *)"MDASHK", 384, saved + 210, 0.35f);
	GraphicsOpt_clearClip(gfx);
}

static void CreateStaffRoll_hook(void *app) {
	SO_CONTINUE(int, create_staff_hook, app);
	char *scroller = *(char **)((char *)app + 0xda78);
	if (scroller)
		*(int *)(scroller + 8) += CREDITS_EXTRA; // scroll end (stored positive)
}

static void credits_patch(void) {
	DrawPosition = (void *)so_symbol(&so_mod, "_Z12DrawPositionPciif");
	GraphicsOpt_setClip = (void *)so_symbol(&so_mod, "_ZN11GraphicsOpt7setClipEiiii");
	GraphicsOpt_clearClip = (void *)so_symbol(&so_mod, "_ZN11GraphicsOpt9clearClipEv");
	AppMain_getWidth = (void *)so_symbol(&so_mod, "_ZN7AppMain8getWidthEv");
	AppMain_getInstance = (void *)so_symbol(&so_mod, "_ZN7AppMain11getInstanceEv");
	uintptr_t draw = so_symbol(&so_mod, "_Z13DrawStaffRollP17GENERAL_TASK_BASE");
	uintptr_t create = so_symbol(&so_mod, "_ZN7AppMain15CreateStaffRollEv");
	if (!DrawPosition || !GraphicsOpt_setClip || !GraphicsOpt_clearClip || !AppMain_getWidth ||
	    !AppMain_getInstance || !draw || !create) {
		l_warn("credits: symbols missing, port credit not added");
		return;
	}
	draw_staff_hook = hook_addr(draw, (uintptr_t)&DrawStaffRoll_hook);
	create_staff_hook = hook_addr(create, (uintptr_t)&CreateStaffRoll_hook);
}

/*
 * Tutorial text: mention the Vita controls. The lines live in two .data
 * tables of string pointers (English first, then Japanese); only lines that
 * talk about touching the screen are swapped, keeping every page's line
 * count and similar line lengths. The ×○□△ glyphs come from the Japanese
 * system font, which text.c uses as fallback.
 */
typedef struct { int idx; const char *text; } TextLine;

static const TextLine tutorial_lines[] = {
	// English (0-89)
	{ 6,  "played to the beat with the touch screen or buttons." },
	{ 7,  "You can tap anywhere or press any button," },
	// Japanese (90-179)
	{ 97, "画面のどこをタップしても、ボタンでも操作する事が" },
};

static const TextLine rhythm_lines[] = {
	// English (0-35)
	{ 1,  "Tap the screen or press a button to the beat at the" },
	{ 4,  "Use any button: ×, ○, □, △, D-pad, L or R!" },
	{ 8,  "Flick the screen or push a stick toward the arrow." },
	{ 10, "Both the left and right analog sticks will work." },
	{ 13, "Tap the screen or press a button, then keep holding." },
	{ 14, "Let go at the end to perform these successfully." },
	{ 21, "Instead of letting go at the end, keep holding" },
	{ 22, "and push a stick in the direction of the arrow." },
	// Japanese (36-71)
	{ 38, "音楽に合わせて画面をタップかボタンを押してみましょう。" },
	{ 40, "×○□△・方向キー・L・Rのどのボタンでも大丈夫です！" },
	{ 44, "矢印の方向へフリックかスティックを倒してみましょう。" },
	{ 46, "左右どちらのアナログスティックでも演奏できますよ。" },
	{ 49, "画面かボタンを押した後、そのまま押し続けてください。" },
	{ 50, "終点で離せば演奏成功です。" },
	{ 57, "終点で離すのではなく、押したまま矢印の方向に" },
	{ 58, "スティックを倒してフリックしてみましょう。" },
};

static void replace_lines(const char *table_sym, int table_len, const TextLine *lines, int n) {
	const char **table = (const char **)so_symbol(&so_mod, table_sym);
	if (!table) {
		l_warn("tutorial: %s not found", table_sym);
		return;
	}
	for (int i = 0; i < n; i++)
		if (lines[i].idx >= 0 && lines[i].idx < table_len)
			table[lines[i].idx] = lines[i].text;
}

static void tutorial_text_patch(void) {
	replace_lines("TutorialString", 180, tutorial_lines,
	              sizeof(tutorial_lines) / sizeof(tutorial_lines[0]));
	replace_lines("RhythmGameTutorialString", 72, rhythm_lines,
	              sizeof(rhythm_lines) / sizeof(rhythm_lines[0]));
}

/*
 * res.dat (in the songs folder) records how far the one-time "additional
 * data" download got: AppMain+0xca8c = stage (5 = done), +0xca90 = files
 * downloaded. A new game (no save.dat) runs InitResData, which resets it to
 * stage 0, and SC_LogoLoop then sends the player to the download screen -
 * whose servers are gone. All that data ships with the port, so keep the
 * download marked as finished (same values as a fully downloaded res.dat),
 * both for new games and for a res.dat that was already reset.
 */
#define RES_STAGE_OFFSET 0xca8c
#define RES_FILES_OFFSET 0xca90
#define RES_STAGE_DONE   5
#define RES_FILES_DONE   0x24c

static so_hook init_res_hook, save_res_init_hook;
static void (*WriteResSaveData)(void *);

static void mark_res_complete(void *app) {
	int *stage = (int *)((char *)app + RES_STAGE_OFFSET);
	int *files = (int *)((char *)app + RES_FILES_OFFSET);
	if (*stage >= RES_STAGE_DONE)
		return;
	*stage = RES_STAGE_DONE;
	if (*files < RES_FILES_DONE)
		*files = RES_FILES_DONE;
	WriteResSaveData(app);
	l_info("res.dat: download marked as complete (all data is installed)");
}

static void InitResData_hook(void *app) {
	SO_CONTINUE(int, init_res_hook, app);
	mark_res_complete(app);
}

static void SaveResDataInit_hook(void *app) {
	SO_CONTINUE(int, save_res_init_hook, app);
	mark_res_complete(app);
}

static void res_data_patch(void) {
	WriteResSaveData = (void *)so_symbol(&so_mod, "_ZN7AppMain16WriteResSaveDataEv");
	uintptr_t init = so_symbol(&so_mod, "_ZN7AppMain11InitResDataEv");
	uintptr_t load = so_symbol(&so_mod, "_ZN7AppMain15SaveResDataInitEv");
	if (!WriteResSaveData || !init || !load) {
		l_warn("res.dat patch: symbols missing");
		return;
	}
	init_res_hook = hook_addr(init, (uintptr_t)&InitResData_hook);
	save_res_init_hook = hook_addr(load, (uintptr_t)&SaveResDataInit_hook);
}

void so_patch(void) {
	kuser_patch();
	res_data_patch();
	tutorial_text_patch();
	hide_dead_buttons();
	credits_patch();
	tutorial_patch();
	vorbis_patch();

	uintptr_t string_width = so_symbol(&so_mod, "_ZN4Font11stringWidthEPKc");
	if (string_width)
		hook_addr(string_width, (uintptr_t)&Font_stringWidth);
	else
		l_warn("Font::stringWidth not found, using the JNI path");

	uintptr_t tunes_init = so_symbol(&so_mod, "_ZN7AppMain17SaveTunesDataInitEv");
	if (tunes_init)
		save_tunes_init_hook = hook_addr(tunes_init, (uintptr_t)&SaveTunesDataInit_hook);
	else
		l_warn("SaveTunesDataInit not found, tunes.dat fix disabled");
}