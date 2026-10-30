/*
 * Host test for the store's mailbox state machine.  Compiles app/src/store.c
 * for the workstation with stubbed runtime calls (no UI table, scripted
 * input) and plays the host's side of each service round trip.
 *
 *   sh tests/run.sh
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hbx_app.h"
#include "hbx_svc.h"

int hbx_app_main(const struct hbx_ctx *ctx);

static int failures, checks;
#define EXPECT(c) do { checks++; if (!(c)) { failures++; \
    fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

/* Scripted input: buttons pressed on a given frame of the current entry. */
static uint32_t script[64];
static uint32_t script_frames, frame;

hbx_status hbx_ctx_check(const struct hbx_ctx *ctx) { (void)ctx; return HBX_OK; }
void hbx_log(const struct hbx_ctx *ctx, const char *line) { (void)ctx; (void)line; }
void hbx_logf(const struct hbx_ctx *ctx, const char *format, ...) { (void)ctx; (void)format; }
uint64_t hbx_pace(const struct hbx_ctx *ctx, uint64_t *deadline, uint32_t hz) {
    (void)deadline; (void)hz;
    if (++frame >= script_frames) ((struct hbx_ctx *)ctx)->exit = HBX_EXIT_REQUEST;
    return 0;
}
void hbx_input_poll(const struct hbx_ctx *ctx, struct hbx_input_state *state) {
    (void)ctx;
    uint32_t buttons = frame < script_frames ? script[frame] : 0;
    state->pressed = buttons & ~state->previous;
    state->buttons = buttons;
    state->previous = buttons;
}

static struct hbx_svc svc;
static struct hbx_ctx ctx;

static int enter(const uint32_t *buttons, uint32_t count) {
    memset(script, 0, sizeof script);
    memcpy(script, buttons, count * sizeof *buttons);
    script_frames = count;
    frame = 0;
    memset(&ctx, 0, sizeof ctx);
    ctx.args[5] = (uint64_t)(uintptr_t)&svc;
    return hbx_app_main(&ctx);
}

static void reply_catalog(uint32_t total, uint32_t installed_mask) {
    struct hbx_svc_app *rows = (struct hbx_svc_app *)(void *)svc.data;
    uint32_t i;
    memset(svc.data, 0, sizeof svc.data);
    for (i = 0; i < total; i++) {
        snprintf(rows[i].id, sizeof rows[i].id, "app-%u", i);
        snprintf(rows[i].name, sizeof rows[i].name, "App %u", i);
        snprintf(rows[i].version, sizeof rows[i].version, "1.0.%u", i);
        if (installed_mask & (1u << i)) {
            rows[i].flags |= HBX_SVC_APP_INSTALLED;
            snprintf(rows[i].installed_version, sizeof rows[i].installed_version, "1.0.%u", i);
        }
    }
    svc.result = HBX_SVC_OK;
    svc.r0 = total;
    svc.r1 = total;
    svc.op = 0;
    svc.completed = svc.sequence;
}

static void reply_text(int32_t result, const char *text) {
    memset(svc.data, 0, sizeof svc.data);
    snprintf((char *)svc.data, sizeof svc.data, "%s", text);
    svc.result = result;
    svc.op = 0;
    svc.completed = svc.sequence;
}

int main(void) {
    int status;
    memset(&svc, 0, sizeof svc);
    svc.magic = HBX_SVC_MAGIC;
    svc.version = HBX_SVC_VERSION;

    /* 1. First entry asks for the first catalog page. */
    status = enter(NULL, 0);
    EXPECT(status == HBX_SVC_CALL);
    EXPECT(svc.op == HBX_SVC_CATALOG && svc.a0 == 0);

    /* 2. Three apps; move down once and install. */
    reply_catalog(3, 0);
    {
        uint32_t keys[] = {0, HBX_DOWN, 0, HBX_A};
        status = enter(keys, 4);
    }
    EXPECT(status == HBX_SVC_CALL);
    EXPECT(svc.op == HBX_SVC_INSTALL);
    EXPECT(strcmp((const char *)svc.data, "app-1") == 0);

    /* 3. Install reply: the store reloads the page to refresh flags. */
    reply_text(HBX_SVC_OK, "installed App 1 1.0.1");
    status = enter(NULL, 0);
    EXPECT(status == HBX_SVC_CALL);
    EXPECT(svc.op == HBX_SVC_CATALOG && svc.a0 == 0);

    /* 4. App 1 now installed: X once arms, X again removes. */
    reply_catalog(3, 1u << 1);
    {
        uint32_t keys[] = {0, HBX_X, 0, HBX_X};
        status = enter(keys, 4);
    }
    EXPECT(status == HBX_SVC_CALL);
    EXPECT(svc.op == HBX_SVC_REMOVE);
    EXPECT(strcmp((const char *)svc.data, "app-1") == 0);

    /* 5. A failed call surfaces the message and reloads; Y refreshes. */
    reply_text(HBX_SVC_E_NETWORK, "download failed");
    status = enter(NULL, 0);
    EXPECT(status == HBX_SVC_CALL && svc.op == HBX_SVC_CATALOG);
    reply_catalog(3, 0);
    {
        uint32_t keys[] = {0, HBX_Y};
        status = enter(keys, 2);
    }
    EXPECT(status == HBX_SVC_CALL && svc.op == HBX_SVC_REFRESH);

    /* 6. B leaves with HBX_OK; X on an app that is not installed does nothing. */
    reply_catalog(3, 0);
    {
        uint32_t keys[] = {0, HBX_X, 0, HBX_X, 0, HBX_B};
        status = enter(keys, 6);
    }
    EXPECT(status == HBX_OK);

    /* 7. Without a mailbox the store shows help and exits normally. */
    memset(&ctx, 0, sizeof ctx);
    script_frames = 1; frame = 0;
    EXPECT(hbx_app_main(&ctx) == HBX_OK);

    printf("%d/%d store checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
