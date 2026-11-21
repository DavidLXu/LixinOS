/* Native GET fetcher for the NetSurf scheduler. The browser handles redirects,
 * cache policy and MIME dispatch. The guest transport verifies HTTPS peers. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "utils/errors.h"
#include "utils/nsurl.h"
#include "utils/corestrings.h"
#include "content/fetch.h"
#include "content/fetchers.h"
#include "links/load.h"
#include "links/tls.h"
#include "../desktop/browser.h"
#include "../console.h"

/* Keep the first ordinary key for the desktop. Leave later keys in the
 * terminal queue while an HTTP or TLS operation waits for its peer. */
static int pending_key = -1;

int lx_transport_wait(void) {
    if (pending_key >= 0) {
        console_call(43, 8);
        return 0;
    }
    int key = console_call(14, 8);
    if (key == 3) return 1;
    if (key >= 0 && key < 256) pending_key = key;
    return 0;
}

int desk_web_read_key(unsigned timeout_ms) {
    if (pending_key < 0) return console_call(14, timeout_ms);
    int key = pending_key;
    pending_key = -1;
    return key;
}

struct request {
    struct fetch *parent;
    nsurl *url;
    bool aborted, post, only_success;
    struct request *next;
};
static struct request *ready, *last_ready;

static bool init(lwc_string *scheme) { return true; }
static void fini(lwc_string *scheme) {}
static bool accepts(const nsurl *url) { return true; }

static void *setup(struct fetch *parent, nsurl *url, bool only_success, bool downgrade,
                   const char *post, const struct fetch_multipart_data *multipart,
                   const char **headers) {
    struct request *r = calloc(1, sizeof(*r));
    if (r) {
        r->parent = parent;
        r->url = nsurl_ref(url);
        r->post = post != NULL || multipart != NULL;
        r->only_success = only_success;
    }
    return r;
}
static bool start(void *context) {
    struct request *r = context;
    r->next = NULL;
    if (last_ready) last_ready->next = r;
    else ready = r;
    last_ready = r;
    return true;
}
static void abort_request(void *context) { ((struct request *)context)->aborted = true; }
static void release(void *context) {
    struct request *r = context;
    nsurl_unref(r->url);
    free(r);
}
static void receive(struct request *r) {
    struct page_source page = {0};
    char location[ADDRESS_LIMIT] = {0};
    const char *address = nsurl_access(r->url);
    const char *error = r->post ? "This port supports GET requests only" :
        strlen(address) >= ADDRESS_LIMIT ? "URL too long" :
        lx_http_get(address, &page, location, 0);
    fetch_msg msg = {0};
    if (!error) fetch_set_http_code(r->parent, page.status);
    if (!error && *location) {
        nsurl *to = NULL;
        if (nsurl_join(r->url, location, &to) == NSERROR_OK) {
            const char *url = nsurl_access(to);
            if (!strncmp(url, "https://", 8) ||
                (!strncmp(url, "http://", 7) && !strncmp(address, "http://", 7))) {
                msg.type = FETCH_REDIRECT;
                msg.data.redirect = url;
                fetch_send_callback(&msg, r->parent);
            } else error = "Unsupported redirect scheme";
            nsurl_unref(to);
        } else error = "Invalid redirect";
    } else if (!error && r->only_success && (page.status < 200 || page.status >= 300)) {
        error = "HTTP request was unsuccessful";
    } else if (!error) {
        const char *line = page.headers, *end;
        while (!r->aborted && (end = strstr(line, "\r\n")) != NULL) {
            msg.type = FETCH_HEADER;
            msg.data.header_or_data.buf = (const uint8_t *)line;
            msg.data.header_or_data.len = end + 2 - line;
            fetch_send_callback(&msg, r->parent);
            line = end + 2;
        }
        if (!r->aborted) {
            msg.type = FETCH_DATA;
            msg.data.header_or_data.buf = (const uint8_t *)page.body;
            msg.data.header_or_data.len = page.length;
            fetch_send_callback(&msg, r->parent);
        }
        if (!r->aborted) {
            msg.type = FETCH_FINISHED;
            fetch_send_callback(&msg, r->parent);
        }
    }
    if (error && !r->aborted) {
        msg.type = FETCH_ERROR;
        msg.data.error = error;
        fetch_send_callback(&msg, r->parent);
    }
    free(page.bytes);
}
static void poll_requests(lwc_string *scheme) {
    struct request *r = ready;
    if (!r) return;
    ready = r->next;
    if (!ready) last_ready = NULL;
    if (!r->aborted) receive(r);
    fetch_remove_from_queues(r->parent);
    fetch_free(r->parent);
}
nserror lixin_fetch_register(void) {
    const struct fetcher_operation_table ops = {
        .initialise=init, .acceptable=accepts, .setup=setup, .start=start,
        .abort=abort_request, .free=release, .poll=poll_requests, .finalise=fini
    };
    nserror err = fetcher_add(lwc_string_ref(corestring_lwc_http), &ops);
    return err == NSERROR_OK ? fetcher_add(lwc_string_ref(corestring_lwc_https), &ops) : err;
}
