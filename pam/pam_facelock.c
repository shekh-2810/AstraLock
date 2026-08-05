#define _GNU_SOURCE

/*
 * pam_facelock.c  —  AstraLock v3 PAM module
 *
 * Security fixes vs v2.1:
 *  1. JSON field extraction via pfl_json_get_bool() — no more strstr().
 *     A crafted response containing "\"match\":true" inside another field
 *     can no longer spoof a successful auth.
 *
 *  2. Username sanitization — pfl_json_escape() escapes backslash and
 *     double-quote before embedding the PAM username into the JSON request.
 *     A username containing '"' previously produced malformed JSON and
 *     could confuse the daemon's parser.
 *
 *  3. pam_conversation feedback — sends PAM_TEXT_INFO messages at each
 *     stage so the user sees "Scanning face…", "No face found (1/3)", etc.
 *     instead of a silent frozen prompt.
 *
 *  4. (v3.2) Minimum scan-message display floor — the daemon's cached ONNX
 *     session and already-open camera can finish an auth round-trip faster
 *     than a graphical greeter can actually paint the "scanning face…"
 *     message (greeters relay PAM_TEXT_INFO to their UI process
 *     asynchronously, so pam_conversation() returning does not mean the
 *     message is on screen yet). Without a floor, the result message can
 *     overwrite "scanning" before the user ever sees it, looking like the
 *     scan and the message are racing. PAM_MIN_SCAN_DISPLAY_MS in
 *     facelock.conf (default 600ms, 0 disables) guarantees the scanning
 *     message stays up for at least that long before being replaced.
 */

#include <security/pam_modules.h>
#include <security/pam_ext.h>
#include <security/pam_appl.h>

#include <syslog.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>

#define FACELOCK_SOCK         "/run/facelock/facelock.sock"
#define FACELOCK_CONFIG_PATH  "/etc/facelock/facelock.conf"
#define FACELOCK_TIMEOUT_MS   10000   /* ms to wait for daemon response  */
#define FACELOCK_CONN_TIMEOUT 2000    /* ms to wait for connect          */
#define FACELOCK_MAX_TRIES    3
#define FACELOCK_REQ_MAX      512     /* max request buffer              */
#define FACELOCK_RESP_MAX     1024    /* max response buffer             */
#define FACELOCK_MIN_DISPLAY_DEFAULT_MS 600  /* floor for "scanning" msg */
#define FACELOCK_MIN_DISPLAY_MAX_MS     5000 /* sanity cap on config val */

/* ──────────────────────────────────────────────────────────────────────────
 * pfl_conv — send a PAM_TEXT_INFO message via the conversation function.
 * Silent on any failure (conversation may be unavailable at login screen).
 * ──────────────────────────────────────────────────────────────────────────*/
static void pfl_conv(pam_handle_t *pamh, const char *msg)
{
    struct pam_conv *conv = NULL;
    if (pam_get_item(pamh, PAM_CONV, (const void **)&conv) != PAM_SUCCESS)
        return;
    if (!conv || !conv->conv) return;

    struct pam_message  m   = { PAM_TEXT_INFO, msg };
    struct pam_message *mp  = &m;
    struct pam_response *r  = NULL;

    conv->conv(1, (const struct pam_message **)&mp, &r, conv->appdata_ptr);
    if (r) free(r);
}

/* ──────────────────────────────────────────────────────────────────────────
 * pfl_load_min_display_ms — read PAM_MIN_SCAN_DISPLAY_MS from
 * /etc/facelock/facelock.conf (KEY=VALUE, '#' comments, same format the
 * daemon uses). Falls back to the compiled-in default if the file, key, or
 * value is missing/invalid. This is the only key the PAM module reads, so
 * parsing stays deliberately minimal rather than pulling in the daemon's
 * full config parser.
 * ──────────────────────────────────────────────────────────────────────────*/
static long pfl_load_min_display_ms(void)
{
    static const char *KEY = "PAM_MIN_SCAN_DISPLAY_MS";
    const size_t keylen = strlen(KEY);
    long val = FACELOCK_MIN_DISPLAY_DEFAULT_MS;

    FILE *f = fopen(FACELOCK_CONFIG_PATH, "r");
    if (!f) return val;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';

        char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (strncmp(p, KEY, keylen) != 0) continue;
        p += keylen;

        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '=') continue;
        ++p;
        while (*p == ' ' || *p == '\t') ++p;

        char *endptr = NULL;
        long parsed = strtol(p, &endptr, 10);
        if (endptr != p && parsed >= 0 && parsed <= FACELOCK_MIN_DISPLAY_MAX_MS)
            val = parsed;  /* last valid occurrence wins, same as daemon */
    }
    fclose(f);
    return val;
}

/* ──────────────────────────────────────────────────────────────────────────
 * pfl_wait_min_display — sleeps off the remainder of `min_ms` measured
 * from `start`, if any time is left. Used to guarantee the "scanning
 * face…" message has been visible for a minimum duration before it gets
 * replaced by the result message (see v3.2 note in the file header).
 * ──────────────────────────────────────────────────────────────────────────*/
static void pfl_wait_min_display(const struct timespec *start, long min_ms)
{
    if (min_ms <= 0) return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long elapsed_ms = (now.tv_sec  - start->tv_sec)  * 1000L
                    + (now.tv_nsec - start->tv_nsec) / 1000000L;
    long remaining_ms = min_ms - elapsed_ms;
    if (remaining_ms <= 0) return;

    struct timespec ts;
    ts.tv_sec  = remaining_ms / 1000;
    ts.tv_nsec = (remaining_ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* ──────────────────────────────────────────────────────────────────────────
 * pfl_json_escape — write `src` into `dst` (size `dsz`), escaping
 * backslash and double-quote for safe embedding in a JSON string value.
 * Returns 0 on success, -1 if dst is too small.
 * ──────────────────────────────────────────────────────────────────────────*/
static int pfl_json_escape(const char *src, char *dst, size_t dsz)
{
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0'; ++si) {
        char c = src[si];
        if (c == '\\' || c == '"') {
            if (di + 2 >= dsz) return -1;
            dst[di++] = '\\';
        } else {
            if (di + 1 >= dsz) return -1;
        }
        dst[di++] = c;
    }
    dst[di] = '\0';
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 * pfl_json_get_bool — minimal, correct JSON field extractor.
 *
 * Finds the first occurrence of `"key": true` or `"key": false`
 * (with optional whitespace around the colon) at the top level of the
 * JSON object and returns 1 (true), 0 (false), or -1 (not found / error).
 *
 * This replaces the previous strstr(buf, "\"match\":true") approach which
 * was vulnerable to a crafted value elsewhere in the response spoofing the
 * field result.
 *
 * Approach:
 *  1. Search for the literal key string surrounded by double-quotes.
 *  2. Skip whitespace and colon.
 *  3. Check for the literal tokens "true" or "false".
 *
 * Does NOT handle nested objects — sufficient for the flat response format
 * used by facelockd. Uses no dynamic allocation.
 * ──────────────────────────────────────────────────────────────────────────*/
static int pfl_json_get_bool(const char *json, const char *key)
{
    /* Build search pattern  "key"  */
    char pattern[128];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (plen <= 0 || (size_t)plen >= sizeof(pattern)) return -1;

    const char *p = json;
    while ((p = strstr(p, pattern)) != NULL) {
        p += plen;  /* move past  "key"  */

        /* Skip whitespace */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;

        /* Expect colon */
        if (*p != ':') continue;
        ++p;

        /* Skip whitespace */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;

        if (strncmp(p, "true", 4) == 0) {
            /* Verify the token ends (not "trueXYZ") */
            char next = p[4];
            if (next == ',' || next == '}' || next == ' ' ||
                next == '\t' || next == '\n' || next == '\r' || next == '\0')
                return 1;
        }
        if (strncmp(p, "false", 5) == 0) {
            char next = p[5];
            if (next == ',' || next == '}' || next == ' ' ||
                next == '\t' || next == '\n' || next == '\r' || next == '\0')
                return 0;
        }
        /* Found the key but value is not a boolean — keep searching */
    }
    return -1;  /* not found */
}

/* ──────────────────────────────────────────────────────────────────────────
 * pfl_extract_str — extract a string value for `key` from flat JSON into
 * `out` (size `outsz`).  Returns 0 on success, -1 if not found/truncated.
 * Used to extract the "err" and "hint" fields for syslog.
 * ──────────────────────────────────────────────────────────────────────────*/
static int pfl_extract_str(const char *json, const char *key,
                            char *out, size_t outsz)
{
    char pattern[128];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (plen <= 0 || (size_t)plen >= sizeof(pattern)) return -1;

    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p += plen;

    while (*p == ' ' || *p == ':' || *p == '\t') ++p;
    if (*p != '"') return -1;
    ++p;  /* skip opening quote */

    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outsz) {
        if (*p == '\\' && *(p + 1)) { ++p; }  /* skip escape prefix */
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (*p == '"') ? 0 : -1;
}

/* ──────────────────────────────────────────────────────────────────────────
 * pfl_connect — open and connect a Unix socket.
 * Returns fd on success, -1 on failure.
 * ──────────────────────────────────────────────────────────────────────────*/
static int pfl_connect(pam_handle_t *pamh)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        pam_syslog(pamh, LOG_ERR, "socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FACELOCK_SOCK, sizeof(addr.sun_path) - 1);

    int r = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (r < 0 && errno != EINPROGRESS) {
        /* Daemon not running — fall through to other PAM modules */
        close(fd);
        return -1;
    }

    if (errno == EINPROGRESS) {
        struct pollfd pfd = { fd, POLLOUT, 0 };
        if (poll(&pfd, 1, FACELOCK_CONN_TIMEOUT) <= 0 ||
            !(pfd.revents & POLLOUT)) {
            pam_syslog(pamh, LOG_INFO, "daemon connect timeout");
            close(fd);
            return -1;
        }
        /* Check SO_ERROR to confirm connection succeeded */
        int err = 0; socklen_t len = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err) {
            pam_syslog(pamh, LOG_INFO, "daemon connect failed: %s", strerror(err));
            close(fd);
            return -1;
        }
    }

    return fd;
}

/* ──────────────────────────────────────────────────────────────────────────
 * pfl_send_recv — write `req` to fd, wait up to FACELOCK_TIMEOUT_MS for
 * a newline-terminated response into `resp`.
 * Returns bytes received, or -1 on error/timeout.
 * ──────────────────────────────────────────────────────────────────────────*/
static ssize_t pfl_send_recv(int fd, const char *req,
                              char *resp, size_t resp_sz)
{
    /* Switch socket back to blocking for the send */
    size_t req_len = strlen(req);
    ssize_t written = 0;
    while ((size_t)written < req_len) {
        ssize_t w = write(fd, req + written, req_len - (size_t)written);
        if (w < 0) return -1;
        written += w;
    }

    /* Poll for response */
    struct pollfd pfd = { fd, POLLIN, 0 };
    if (poll(&pfd, 1, FACELOCK_TIMEOUT_MS) <= 0) return -1;

    ssize_t n = read(fd, resp, (ssize_t)resp_sz - 1);
    if (n <= 0) return -1;
    resp[n] = '\0';
    return n;
}

/* ──────────────────────────────────────────────────────────────────────────
 * facelock_single_try — one attempt: connect → send auth → parse response
 * ──────────────────────────────────────────────────────────────────────────*/
static int facelock_single_try(pam_handle_t *pamh, const char *user,
                                int attempt, long min_display_ms)
{
    /* --- Build conversation message for this attempt --- */
    char conv_msg[128];
    snprintf(conv_msg, sizeof(conv_msg),
             "AstraLock: scanning face… (attempt %d/%d)",
             attempt + 1, FACELOCK_MAX_TRIES);

    struct timespec scan_start;
    clock_gettime(CLOCK_MONOTONIC, &scan_start);
    pfl_conv(pamh, conv_msg);

    /* --- Connect --- */
    int fd = pfl_connect(pamh);
    if (fd < 0) {
        pfl_wait_min_display(&scan_start, min_display_ms);
        pfl_conv(pamh, "AstraLock: daemon unavailable — skipping face auth");
        return PAM_IGNORE;
    }

    /* --- Build request with escaped username --- */
    char safe_user[256];
    if (pfl_json_escape(user, safe_user, sizeof(safe_user)) != 0) {
        pam_syslog(pamh, LOG_ERR, "username too long or contains invalid chars");
        close(fd);
        return PAM_IGNORE;
    }

    char req[FACELOCK_REQ_MAX];
    snprintf(req, sizeof(req),
             "{\"v\":3,\"cmd\":\"auth\",\"user\":\"%s\"}\n", safe_user);

    /* --- Send and receive --- */
    char resp[FACELOCK_RESP_MAX] = {0};
    ssize_t n = pfl_send_recv(fd, req, resp, sizeof(resp));
    close(fd);

    /* Every path below shows a result message that replaces "scanning" —
     * hold here first so it was visible for at least min_display_ms. */
    pfl_wait_min_display(&scan_start, min_display_ms);

    if (n <= 0) {
        pam_syslog(pamh, LOG_WARNING, "no response from daemon (user=%s)", user);
        pfl_conv(pamh, "AstraLock: no response from daemon");
        return PAM_IGNORE;
    }

    /* --- Parse response using proper field extractor --- */
    int match = pfl_json_get_bool(resp, "match");
    int ok    = pfl_json_get_bool(resp, "ok");

    if (ok == 0 || match == -1) {
        /* Daemon returned an error — extract hint for syslog */
        char err_val[64]  = "unknown";
        char hint_val[128] = "";
        pfl_extract_str(resp, "err",  err_val,  sizeof(err_val));
        pfl_extract_str(resp, "hint", hint_val, sizeof(hint_val));
        pam_syslog(pamh, LOG_INFO,
                   "auth error for %s: err=%s hint=%s", user, err_val, hint_val);

        if (strcmp(err_val, "not_enrolled") == 0) {
            pfl_conv(pamh, "AstraLock: face not enrolled — using password");
            return PAM_IGNORE;
        }
        if (strcmp(err_val, "liveness_fail") == 0) {
            pfl_conv(pamh, "AstraLock: liveness check failed — present real face");
            return PAM_AUTH_ERR;
        }
        if (strcmp(err_val, "no_face") == 0 ||
            strcmp(err_val, "timeout") == 0) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "AstraLock: no face detected (%d/%d)",
                     attempt + 1, FACELOCK_MAX_TRIES);
            pfl_conv(pamh, msg);
            return PAM_AUTH_ERR;
        }
        return PAM_IGNORE;
    }

    if (match == 1) {
        pfl_conv(pamh, "AstraLock: face recognised ✓");
        pam_syslog(pamh, LOG_INFO, "face auth SUCCESS for %s", user);
        return PAM_SUCCESS;
    }

    /* match == 0 — face found but rejected */
    char msg[128];
    snprintf(msg, sizeof(msg),
             "AstraLock: face not recognised (%d/%d)",
             attempt + 1, FACELOCK_MAX_TRIES);
    pfl_conv(pamh, msg);
    pam_syslog(pamh, LOG_INFO, "face auth FAIL for %s (score above threshold)", user);
    return PAM_AUTH_ERR;
}

/* ──────────────────────────────────────────────────────────────────────────
 * pam_sm_authenticate — PAM entry point
 * ──────────────────────────────────────────────────────────────────────────*/
PAM_EXTERN int pam_sm_authenticate(
    pam_handle_t *pamh, int flags, int argc, const char **argv)
{
    openlog("pam_facelock", LOG_PID, LOG_AUTHPRIV);

    const char *user = NULL;
    if (pam_get_user(pamh, &user, NULL) != PAM_SUCCESS || !user) {
        pam_syslog(pamh, LOG_ERR, "pam_get_user() failed");
        closelog();
        return PAM_IGNORE;
    }

    /* Sanity check: username must be non-empty and sane length */
    size_t ulen = strlen(user);
    if (ulen == 0 || ulen > 256) {
        pam_syslog(pamh, LOG_WARNING, "username invalid length (%zu)", ulen);
        closelog();
        return PAM_IGNORE;
    }

    const long min_display_ms = pfl_load_min_display_ms();

    int result = PAM_IGNORE;
    for (int i = 0; i < FACELOCK_MAX_TRIES; ++i) {
        result = facelock_single_try(pamh, user, i, min_display_ms);
        if (result == PAM_SUCCESS || result == PAM_IGNORE) break;
        if (i < FACELOCK_MAX_TRIES - 1)
            sleep(1);   /* 1s back-off between retries */
    }

    closelog();
    return result;
}

PAM_EXTERN int pam_sm_setcred(
    pam_handle_t *pamh, int flags, int argc, const char **argv)
{
    return PAM_SUCCESS;
}
