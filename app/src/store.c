/*
 * Homebrew Store - on-console client for sw2-homebrew-catalog.
 *
 * The console cannot download anything by itself; the sw2-sdk host can.  The
 * store talks to it through the services mailbox (hbx_svc.h): it fills a
 * request, returns HBX_SVC_CALL, and is re-entered with the reply.  All UI
 * state that must survive those round trips lives in svc->scratch.
 *
 *   D-pad / stick   move          L / R    previous / next page
 *   A               install or update the selected app
 *   X               remove (press twice)
 *   Y               refresh the catalog from the network
 *   B               back to the launcher
 */
#include "hbx_sdk.h"
#include "hbx_app.h"
#include "hbx_ui.h"
#include "hbx_svc.h"

#define STATE_MAGIC   0x52545357u /* "WSTR" */
#define VISIBLE_ROWS  9u
#define ROW_HEIGHT    84.0f
#define LIST_X        96.0f
#define LIST_Y        176.0f
#define LIST_W        1040.0f
#define DETAIL_X      1180.0f

struct store_state {
    uint32_t magic;
    uint32_t last_op;      /* op of the request in flight / just answered */
    uint32_t first;        /* catalog index of rows[0] in the cached page */
    uint32_t total;        /* rows in the whole catalog */
    uint32_t count;        /* rows in the cached page */
    uint32_t selected;     /* absolute catalog index */
    uint32_t scroll;       /* absolute index of the first visible row */
    uint32_t confirm_remove;
    int32_t  status;       /* result of the last action */
    char     message[160];
    char     pending_id[64];
};
_Static_assert(sizeof(struct store_state) <= HBX_SVC_SCRATCH_BYTES, "store state");

static const uint32_t BG = HBX_RGB(246, 247, 249);
static const uint32_t CARD = HBX_RGB(255, 255, 255);
static const uint32_t LINE = HBX_RGB(221, 226, 232);
static const uint32_t INK = HBX_RGB(27, 36, 48);
static const uint32_t MUTED = HBX_RGB(91, 102, 117);
static const uint32_t ACCENT = HBX_RGB(216, 67, 47);
static const uint32_t GOOD = HBX_RGB(31, 138, 76);
static const uint32_t SELECT = HBX_RGB(253, 236, 232);

/* --- tiny string helpers (no libc) ------------------------------------ */

static void copy_text(char *out, uint32_t size, const char *in) {
    uint32_t i = 0;
    while (in && in[i] && i + 1u < size) { out[i] = in[i]; i++; }
    out[i] = 0;
}

static void append_text(char *out, uint32_t size, const char *in) {
    uint32_t i = 0;
    while (i < size && out[i]) i++;
    copy_text(out + i, size - i, in);
}

static const char *u32_text(uint32_t value, char out[12]) {
    char *p = out + 11;
    *p = 0;
    do { *--p = (char)('0' + value % 10u); value /= 10u; } while (value);
    return p;
}

static void size_text(uint32_t bytes, char *out, uint32_t size) {
    char number[12];
    uint32_t kib = (bytes + 1023u) / 1024u;
    out[0] = 0;
    if (kib >= 1024u) {
        append_text(out, size, u32_text(kib / 1024u, number));
        append_text(out, size, ".");
        append_text(out, size, u32_text((kib % 1024u) * 10u / 1024u, number));
        append_text(out, size, " MiB");
    } else {
        append_text(out, size, u32_text(kib, number));
        append_text(out, size, " KiB");
    }
}

/* --- requests ---------------------------------------------------------- */

static int request(struct hbx_svc *svc, struct store_state *st, uint32_t op,
                   uint32_t a0, const char *text) {
    hbx_svc_begin(svc, op, a0, 0);
    if (text) hbx_svc_put_text(svc, text);
    st->last_op = op;
    return HBX_SVC_CALL;
}

static int request_page(struct hbx_svc *svc, struct store_state *st, uint32_t first, int refresh) {
    st->first = first;
    return request(svc, st, refresh ? HBX_SVC_REFRESH : HBX_SVC_CATALOG, first, 0);
}

/* Absolute index -> start of the page that contains it. */
static uint32_t page_of(uint32_t index) {
    return index - index % (uint32_t)HBX_SVC_APPS_PER_PAGE;
}

/* --- drawing ----------------------------------------------------------- */

static void draw_frame(const struct hbx_ctx *ctx, const struct store_state *st,
                       const struct hbx_svc_app *rows, const char *busy) {
    char number[12], line[96];
    uint32_t i;

    hbx_ui_begin(ctx, BG);
    hbx_ui_text(ctx, LIST_X, 64.0f, HBX_FONT_H1, "Homebrew Store", INK);
    line[0] = 0;
    append_text(line, sizeof line, u32_text(st->total, number));
    append_text(line, sizeof line, st->total == 1u ? " app" : " apps");
    hbx_ui_text(ctx, LIST_X, 116.0f, HBX_FONT_SMALL, line, MUTED);

    if (!st->total && !busy) {
        hbx_ui_text(ctx, LIST_X, LIST_Y + 40.0f, HBX_FONT_BODY,
                    "The catalog is empty or could not be loaded. Press Y to retry.", MUTED);
    }

    for (i = 0; i < VISIBLE_ROWS; i++) {
        uint32_t index = st->scroll + i;
        const struct hbx_svc_app *app;
        float y = LIST_Y + (float)i * ROW_HEIGHT;
        uint32_t tag_color;
        const char *tag;
        if (index >= st->total || index < st->first || index >= st->first + st->count) break;
        app = &rows[index - st->first];
        hbx_ui_rect(ctx, LIST_X, y, LIST_W, ROW_HEIGHT - 8.0f, 14.0f,
                    index == st->selected ? SELECT : CARD);
        if (index == st->selected)
            hbx_ui_stroke(ctx, LIST_X, y, LIST_W, ROW_HEIGHT - 8.0f, 14.0f, 3.0f, ACCENT);
        hbx_ui_text(ctx, LIST_X + 24.0f, y + 14.0f, HBX_FONT_H2, app->name, INK);
        line[0] = 0;
        append_text(line, sizeof line, app->author);
        append_text(line, sizeof line, "  -  ");
        append_text(line, sizeof line, app->category);
        append_text(line, sizeof line, (app->flags & HBX_SVC_APP_GPUBREW) ? "  -  GPUBrew" : "  -  native");
        hbx_ui_text(ctx, LIST_X + 24.0f, y + 48.0f, HBX_FONT_SMALL, line, MUTED);

        tag = app->version;
        tag_color = MUTED;
        if (app->flags & HBX_SVC_APP_UPDATE) { tag = "Update"; tag_color = ACCENT; }
        else if (app->flags & HBX_SVC_APP_INSTALLED) { tag = "Installed"; tag_color = GOOD; }
        hbx_ui_text(ctx, LIST_X + LIST_W - 24.0f - hbx_ui_text_width(ctx, HBX_FONT_BODY, tag),
                    y + 26.0f, HBX_FONT_BODY, tag, tag_color);
    }

    /* Detail pane for the selected app. */
    if (st->selected >= st->first && st->selected < st->first + st->count) {
        const struct hbx_svc_app *app = &rows[st->selected - st->first];
        hbx_ui_rect(ctx, DETAIL_X, LIST_Y, 640.0f, 740.0f, 18.0f, CARD);
        hbx_ui_stroke(ctx, DETAIL_X, LIST_Y, 640.0f, 740.0f, 18.0f, 1.0f, LINE);
        hbx_ui_text(ctx, DETAIL_X + 32.0f, LIST_Y + 32.0f, HBX_FONT_H2, app->name, INK);
        hbx_ui_text(ctx, DETAIL_X + 32.0f, LIST_Y + 74.0f, HBX_FONT_SMALL, app->id, MUTED);
        /* The UI has no word wrap; descriptions are short (<= 160 chars) and
           the pane shows them on one scaled line per 40 characters. */
        for (i = 0; i < 4u; i++) {
            char chunk[44];
            uint32_t j, start = i * 40u;
            for (j = 0; j < 40u && app->description[start + j] && start + j < sizeof app->description; j++)
                chunk[j] = app->description[start + j];
            chunk[j] = 0;
            if (!j) break;
            hbx_ui_text(ctx, DETAIL_X + 32.0f, LIST_Y + 130.0f + (float)i * 32.0f,
                        HBX_FONT_BODY, chunk, INK);
        }
        line[0] = 0;
        append_text(line, sizeof line, "Version ");
        append_text(line, sizeof line, app->version);
        hbx_ui_text(ctx, DETAIL_X + 32.0f, LIST_Y + 300.0f, HBX_FONT_BODY, line, MUTED);
        if (app->installed_version[0]) {
            line[0] = 0;
            append_text(line, sizeof line, "Installed ");
            append_text(line, sizeof line, app->installed_version);
            hbx_ui_text(ctx, DETAIL_X + 32.0f, LIST_Y + 336.0f, HBX_FONT_BODY, line,
                        (app->flags & HBX_SVC_APP_UPDATE) ? ACCENT : GOOD);
        }
        size_text(app->size_bytes, line, sizeof line);
        hbx_ui_text(ctx, DETAIL_X + 32.0f, LIST_Y + 372.0f, HBX_FONT_BODY, line, MUTED);
        hbx_ui_text(ctx, DETAIL_X + 32.0f, LIST_Y + 660.0f, HBX_FONT_SMALL,
                    (app->flags & HBX_SVC_APP_UPDATE) ? "A  Update" :
                    (app->flags & HBX_SVC_APP_INSTALLED) ? "A  Reinstall    X  Remove" : "A  Install",
                    ACCENT);
    }

    hbx_ui_text(ctx, LIST_X, 1000.0f, HBX_FONT_SMALL,
                busy ? busy : (st->message[0] ? st->message : "A install   X remove   Y refresh   L/R page   B back"),
                busy ? ACCENT : (st->status < 0 ? ACCENT : MUTED));
    hbx_ui_present(ctx);
}

/* --- entry ------------------------------------------------------------- */

static void move_selection(struct store_state *st, int delta) {
    int64_t next = (int64_t)st->selected + delta;
    if (!st->total) return;
    if (next < 0) next = 0;
    if (next >= (int64_t)st->total) next = (int64_t)st->total - 1;
    st->selected = (uint32_t)next;
    if (st->selected < st->scroll) st->scroll = st->selected;
    if (st->selected >= st->scroll + VISIBLE_ROWS) st->scroll = st->selected - VISIBLE_ROWS + 1u;
    st->confirm_remove = 0;
}

static void no_services(const struct hbx_ctx *ctx) {
    struct hbx_input_state input;
    uint64_t deadline = 0;
    hbx_memset(&input, 0, sizeof input);
    while (!hbx_stop_requested(ctx)) {
        hbx_input_poll(ctx, &input);
        if (input.pressed & (HBX_B | HBX_PLUS)) break;
        hbx_ui_begin(ctx, BG);
        hbx_ui_text(ctx, LIST_X, 120.0f, HBX_FONT_H1, "Homebrew Store", INK);
        hbx_ui_text(ctx, LIST_X, 200.0f, HBX_FONT_BODY,
                    "This launcher's host has no services mailbox.", INK);
        hbx_ui_text(ctx, LIST_X, 240.0f, HBX_FONT_BODY,
                    "Run the launcher from sw2-sdk, or use  sw2 store  on the PC.", MUTED);
        hbx_ui_present(ctx);
        hbx_frame_end(ctx);
        hbx_pace(ctx, &deadline, 60);
    }
}

int hbx_app_main(const struct hbx_ctx *ctx) {
    struct hbx_svc *svc;
    struct store_state *st;
    const struct hbx_svc_app *rows;
    struct hbx_input_state input;
    uint64_t deadline = 0;
    uint32_t held_frames = 0;

    if (hbx_ctx_check(ctx) != HBX_OK) return HBX_ERR_ABI;
    svc = hbx_svc_block(ctx);
    if (!svc) { no_services(ctx); return HBX_OK; }
    st = (struct store_state *)(void *)svc->scratch;
    rows = (const struct hbx_svc_app *)(const void *)svc->data;

    /* First entry: load the catalog. */
    if (st->magic != STATE_MAGIC) {
        hbx_memset(st, 0, sizeof *st);
        st->magic = STATE_MAGIC;
        draw_frame(ctx, st, rows, "Loading catalog...");
        return request_page(svc, st, 0, 0);
    }

    /* Re-entry with a reply. */
    if (hbx_svc_replied(svc) && st->last_op) {
        uint32_t op = st->last_op;
        st->last_op = 0;
        if (op == HBX_SVC_CATALOG || op == HBX_SVC_REFRESH) {
            if (svc->result == HBX_SVC_OK) {
                st->total = svc->r0;
                st->count = svc->r1;
                if (st->selected >= st->total) st->selected = st->total ? st->total - 1u : 0u;
                if (op == HBX_SVC_REFRESH && !st->message[0]) copy_text(st->message, sizeof st->message, "Catalog refreshed.");
            } else {
                st->count = 0;
                st->status = svc->result;
                copy_text(st->message, sizeof st->message, (const char *)svc->data);
            }
        } else {
            /* INSTALL/REMOVE: keep the message, then reload the current page so
               the installed/update flags are current. */
            st->status = svc->result;
            copy_text(st->message, sizeof st->message, (const char *)svc->data);
            draw_frame(ctx, st, rows, st->message);
            return request_page(svc, st, page_of(st->selected), 0);
        }
    }

    hbx_memset(&input, 0, sizeof input);
    while (!hbx_stop_requested(ctx)) {
        int dir;
        hbx_input_poll(ctx, &input);
        if (input.pressed & (HBX_B | HBX_PLUS)) break;

        /* Up/down from the D-pad or the left stick (+y is up), with
           repeat-on-hold for long lists. */
        dir = ((input.buttons & HBX_UP) || input.stick_ly > 55) ? -1 :
              ((input.buttons & HBX_DOWN) || input.stick_ly < -55) ? 1 : 0;
        if (!dir) held_frames = 0;
        else if (held_frames++ == 0u || (held_frames > 18u && held_frames % 4u == 0u))
            move_selection(st, dir);
        if (input.pressed & HBX_L) move_selection(st, -(int)VISIBLE_ROWS);
        if (input.pressed & HBX_R) move_selection(st, (int)VISIBLE_ROWS);

        /* Selection left the cached page: fetch the page that has it. */
        if (st->total && (st->selected < st->first || st->selected >= st->first + st->count)) {
            draw_frame(ctx, st, rows, "Loading...");
            return request_page(svc, st, page_of(st->selected), 0);
        }

        if (input.pressed & HBX_Y) {
            st->message[0] = 0;
            draw_frame(ctx, st, rows, "Refreshing catalog...");
            return request_page(svc, st, page_of(st->selected), 1);
        }
        if (st->total && st->selected >= st->first && st->selected < st->first + st->count) {
            const struct hbx_svc_app *app = &rows[st->selected - st->first];
            if (input.pressed & HBX_A) {
                char busy[128];
                busy[0] = 0;
                append_text(busy, sizeof busy, (app->flags & HBX_SVC_APP_UPDATE) ? "Updating " : "Installing ");
                append_text(busy, sizeof busy, app->name);
                append_text(busy, sizeof busy, "...");
                copy_text(st->pending_id, sizeof st->pending_id, app->id);
                st->confirm_remove = 0;
                draw_frame(ctx, st, rows, busy);
                return request(svc, st, HBX_SVC_INSTALL, 0, st->pending_id);
            }
            if ((input.pressed & HBX_X) && (app->flags & HBX_SVC_APP_INSTALLED)) {
                if (!st->confirm_remove) {
                    st->confirm_remove = 1;
                    copy_text(st->message, sizeof st->message, "Press X again to remove ");
                    append_text(st->message, sizeof st->message, app->name);
                    st->status = 0;
                } else {
                    copy_text(st->pending_id, sizeof st->pending_id, app->id);
                    st->confirm_remove = 0;
                    draw_frame(ctx, st, rows, "Removing...");
                    return request(svc, st, HBX_SVC_REMOVE, 0, st->pending_id);
                }
            }
        }

        draw_frame(ctx, st, rows, 0);
        hbx_frame_end(ctx);
        hbx_pace(ctx, &deadline, 60);
    }
    return HBX_OK;
}
