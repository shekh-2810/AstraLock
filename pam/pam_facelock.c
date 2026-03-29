#define _GNU_SOURCE

#include <security/pam_modules.h>
#include <security/pam_ext.h>
#include <security/pam_appl.h>

#include <syslog.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>

#include <unistd.h>
#include <string.h>
#include <stdio.h>

#define FACELOCK_SOCK "/run/facelock/facelock.sock"
#define FACELOCK_TIMEOUT_SEC 6
#define FACELOCK_MAX_TRIES 3

static int facelock_single_try(pam_handle_t *pamh, const char *user)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        pam_syslog(pamh, LOG_ERR, "socket() failed");
        return PAM_IGNORE;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FACELOCK_SOCK, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        pam_syslog(pamh, LOG_INFO, "daemon unavailable");
        close(fd);
        return PAM_IGNORE;
    }

    char req[256];
    snprintf(req, sizeof(req),
             "{\"cmd\":\"auth\",\"user\":\"%s\"}\n", user);

    write(fd, req, strlen(req));

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);

    struct timeval tv = { FACELOCK_TIMEOUT_SEC, 0 };

    if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
        close(fd);
        return PAM_IGNORE;
    }

    char buf[512] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0)
        return PAM_IGNORE;

    if (strstr(buf, "\"match\":true"))
        return PAM_SUCCESS;

    if (strstr(buf, "\"match\":false"))
        return PAM_AUTH_ERR;

    return PAM_IGNORE;
}

PAM_EXTERN int pam_sm_authenticate(
    pam_handle_t *pamh, int flags, int argc, const char **argv)
{
    openlog("pam_facelock", LOG_PID, LOG_AUTHPRIV);

    const char *user = NULL;
    if (pam_get_user(pamh, &user, NULL) != PAM_SUCCESS || !user) {
        closelog();
        return PAM_IGNORE;
    }

    for (int i = 0; i < FACELOCK_MAX_TRIES; ++i) {
        int r = facelock_single_try(pamh, user);
        if (r != PAM_AUTH_ERR) {
            closelog();
            return r;
        }
        sleep(1);
    }

    closelog();
    return PAM_IGNORE;
}

PAM_EXTERN int pam_sm_setcred(
    pam_handle_t *pamh, int flags, int argc, const char **argv)
{
    return PAM_SUCCESS;
}
