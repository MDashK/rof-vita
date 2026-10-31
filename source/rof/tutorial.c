/*
 * THE RHYTHM OF FIGHTERS - PS Vita port
 * Button support for the tutorial windows (touch-only on Android).
 *
 *   First part (before the practice song, 3 pages) and second part (15 pages)
 *   are paged windows (GT_TutorialFirstHalf / GT_TutorialAfterHalf): a touch
 *   released on the left/right arrow sets task+0x98 to 3 / 4 (if not on the
 *   first / last page and the pages are not moving), and the window's main
 *   loop then scrolls one page. A page task with +0x2a0 set ends the window:
 *   page 2 of the first part is "PLAY", page 14 of the second part closes it.
 *
 *   During the practice song every tip is a "note window"
 *   (GT_NoteWindowMainTouch) that closes on a touch of pointer id 0.
 *
 * input.c queues a request; the hooks apply it right after the game's own
 * code ran for that frame, exactly where a real tap would have set the same
 * fields - so no screen coordinates are involved.
 */

#include "rof/rof.h"

#include "utils/logger.h"

#define APP_TUT_PAGES   0x41cc // GENERAL_TASK_BASE* pages[15]
#define APP_TUT_SCROLL  0x9c   // TouchManagerScrollXPlus*
#define APP_TUT_MODE    0xda1c // short: 2 while the practice song runs
#define TUT_PAGE_WIDTH  830

static so_hook first_half_hook, after_half_hook;
static void *(*AppMain_getInstance)(void);
static int (*ScrollX_get)(void *);
static int (*ScrollXPlus_moveState)(void *);

static unsigned frame_now = 0;
static unsigned first_seen = 0, after_seen = 0, popup_seen = 0;

static int pending_page = 0;   // -1 previous, +1 next

static void *tut_scroll(void) {
    char *app = AppMain_getInstance ? AppMain_getInstance() : NULL;
    return app ? *(void **)(app + APP_TUT_SCROLL) : NULL;
}

int tutorial_page(void) {
    void *scroll = tut_scroll();
    if (!scroll || !ScrollX_get)
        return -1;
    int page = ScrollX_get(scroll) / TUT_PAGE_WIDTH; // same maths as the game
    return page < 0 ? -page : page;
}

static void apply_page_turn(void *task, int last_page) {
    if (!pending_page)
        return;
    void *scroll = tut_scroll();
    int page = tutorial_page();
    if (scroll && ScrollXPlus_moveState && ScrollXPlus_moveState(scroll) == 0) {
        if (pending_page < 0 && page != 0)
            *(int *)((char *)task + 0x98) = 3;
        else if (pending_page > 0 && page != last_page)
            *(int *)((char *)task + 0x98) = 4;
    }
    pending_page = 0;
}

static int GT_TutorialFirstHalf_hook(void *task) {
    first_seen = frame_now;
    int r = SO_CONTINUE(int, first_half_hook, task);
    apply_page_turn(task, 2);
    return r;
}

static int GT_TutorialAfterHalf_hook(void *task) {
    after_seen = frame_now;
    int r = SO_CONTINUE(int, after_half_hook, task);
    apply_page_turn(task, 14);
    return r;
}

// Tips during the practice song are "note windows": GT_NoteWindowMainTouch
// closes the tip on TouchManager::getTouchTrg of the game's main touch
// manager, which follows pointer id 0 only. input.c makes Cross / Circle
// press with id 0 while a tip is open.
static so_hook note_window_hook;

static long long GT_NoteWindowMainTouch_hook(void *task) {
    popup_seen = frame_now;
    return SO_CONTINUE(long long, note_window_hook, task);
}

void tutorial_frame(void) {
    frame_now++;
}

static int recent(unsigned seen) {
    return seen && frame_now - seen <= 2;
}

int tutorial_window(void) {
    if (recent(first_seen)) return 1;
    if (recent(after_seen)) return 2;
    return 0;
}

int tutorial_popup(void) {
    return recent(popup_seen);
}

void tutorial_turn_page(int dir) {
    pending_page = dir;
}

// Press the button on page `page_id` (task+0x9c): PLAY (2) / close (14).
void tutorial_activate(int page_id, int pages) {
    char *app = AppMain_getInstance ? AppMain_getInstance() : NULL;
    if (!app)
        return;
    char **tasks = (char **)(app + APP_TUT_PAGES);
    for (int i = 0; i < pages; i++) {
        char *t = tasks[i];
        if (t && *(int *)(t + 0x9c) == page_id) {
            *(int *)(t + 0x288) = 1;
            *(int *)(t + 0x2a0) = 1;
            return;
        }
    }
}

void tutorial_patch(void) {
    AppMain_getInstance = (void *)so_symbol(&so_mod, "_ZN7AppMain11getInstanceEv");
    ScrollX_get = (void *)so_symbol(&so_mod, "_ZN19TouchManagerScrollX10getScrollXEv");
    ScrollXPlus_moveState = (void *)so_symbol(&so_mod, "_ZN23TouchManagerScrollXPlus12getMoveStateEv");
    if (!AppMain_getInstance || !ScrollX_get || !ScrollXPlus_moveState) {
        l_warn("tutorial: symbols missing, tutorial buttons disabled");
        return;
    }
    uintptr_t f;
    if ((f = so_symbol(&so_mod, "_Z20GT_TutorialFirstHalfP17GENERAL_TASK_BASE")))
        first_half_hook = hook_addr(f, (uintptr_t)&GT_TutorialFirstHalf_hook);
    if ((f = so_symbol(&so_mod, "_Z20GT_TutorialAfterHalfP17GENERAL_TASK_BASE")))
        after_half_hook = hook_addr(f, (uintptr_t)&GT_TutorialAfterHalf_hook);
    if ((f = so_symbol(&so_mod, "_Z22GT_NoteWindowMainTouchP17GENERAL_TASK_BASE")))
        note_window_hook = hook_addr(f, (uintptr_t)&GT_NoteWindowMainTouch_hook);
}
