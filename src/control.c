/*
 * Copyright (c) 2015, Laurent COUSTET <ed@zehome.com>
 *
 * All rights reserved.
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS AND CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "ubond.h"
#include "control.h"
#include "tuntap_generic.h"

extern struct tuntap_s tuntap;
extern char *_progname;
extern struct ubond_status_s ubond_status;
extern double bandwidth;
void ubond_control_write_status(struct ubond_control *ctrl);
extern int ubond_reorder_length();
extern double ubond_total_loss();
extern uint64_t pool_out;
extern LIST_HEAD(rtunhead, ubond_tunnel_s) rtuns;

#define HTTP_HEADERS "HTTP/1.1 200 OK\r\n" \
    "Connection: close\r\n" \
    "Content-type: application/json\r\n" \
    "Access-Control-Allow-Origin: *\r\n" \
    "Server: ubond\r\n" \
    "\r\n"

/* Yeah this is a bit uggly I admit :-) */
#define JSON_STATUS_BASE "{" \
    "\"name\": \"%s\",\n" \
    "\"version\": \"%d.%d\",\n" \
    "\"uptime\": %u,\n" \
    "\"last_reload\": %u,\n" \
    "\"pid\": %d,\n" \
    "\"tuntap\": {\n" \
    "   \"type\": \"%s\",\n" \
    "   \"name\": \"%s\"\n" \
    "},\n" \
    "\"bandwidth_out\": %f,\n" \
    "\"reorder_length\": %d,\n"   \
    "\"total_loss\": %f,\n"     \
    "\"memory_packets\": %lu,\n"       \
    "\"tunnels\": [\n"

#define JSON_STATUS_RTUN "{\n" \
    "   \"name\": \"%s\",\n" \
    "   \"mode\": \"%s\",\n" \
    "   \"bindaddr\": \"%s\",\n" \
    "   \"bindport\": \"%s\",\n" \
    "   \"destaddr\": \"%s\",\n" \
    "   \"destport\": \"%s\",\n" \
    "   \"status\": \"%s\",\n" \
    "   \"sentpackets\": %" PRIu64 ",\n" \
    "   \"recvpackets\": %" PRIu64 ",\n" \
    "   \"sentbytes\": %" PRIu64 ",\n" \
    "   \"recvbytes\": %" PRIu64 ",\n" \
    "   \"bandwidth\": %lu,\n" \
    "   \"srtt\": %.3f,\n" \
    "   \"lossin\": %.3f,\n" \
    "   \"lossout\": %.3f,\n" \
    "   \"reorder_length\": %u,\n"     \
    "   \"permitted\": %u,\n" \
    "   \"disconnects\": %u,\n" \
    "   \"last_packet\": %u,\n" \
    "   \"timeout\": %u,\n" \
    "   \"weight\": %.3f\n" \
    "}%s\n"
#define JSON_STATUS_ERROR_UNKNOWN_COMMAND "{\"error\": 'unknown command'}\n"

static void
ubond_control_client_io_event(struct ev_loop *loop, ev_io *w, int revents)
{
    if (revents & EV_READ) {
        ubond_control_read(w->data);
    }
    if (revents & EV_WRITE) {
        ubond_control_send(w->data);
    }
}

static void
ubond_control_io_event(struct ev_loop *loop, ev_io *w, int revents)
{
    if (revents & EV_READ) {
        struct ubond_control *ctrl = w->data;
        if (w == &ctrl->sock_watcher) {
            ubond_control_accept(ctrl, ctrl->sockfd);
        } else if (w == &ctrl->fifo_watcher) {
            ubond_control_accept(ctrl, ctrl->fifofd);
        }
    }
}

static void
ubond_control_timeout_event(struct ev_loop *loop, ev_timer *w, int revents)
{
    ubond_control_timeout(w->data);
    ev_timer_again(EV_DEFAULT_UC, w);
}

void
ubond_control_close_client(struct ubond_control *ctrl)
{
    ev_io_stop(EV_DEFAULT_UC, &ctrl->client_io_read);
    ev_io_stop(EV_DEFAULT_UC, &ctrl->client_io_write);
    if (ctrl->clientfd >= 0)
        close(ctrl->clientfd);
    ctrl->clientfd = -1;
}

void
ubond_control_init(struct ubond_control *ctrl)
{
    if (ctrl->mode == UBOND_CONTROL_DISABLED)
        return;

    struct sockaddr_un un_addr;
    struct addrinfo hints, *res, *bak;
    int ret;
    int val;

    res = bak = NULL;

    ctrl->fifofd = -1;
    ctrl->sockfd = -1;
    ctrl->clientfd = -1;
    ctrl->wbuflen = 4096;
    ctrl->wbuf = malloc(ctrl->wbuflen);
    ctrl->http = 0;
    ctrl->close_after_write = 0;

    ev_init(&ctrl->fifo_watcher, ubond_control_io_event);
    ev_init(&ctrl->sock_watcher, ubond_control_io_event);
    ev_init(&ctrl->client_io_read, ubond_control_client_io_event);
    ev_init(&ctrl->client_io_write, ubond_control_client_io_event);
    ev_init(&ctrl->client_io_write, ubond_control_client_io_event);
    ev_init(&ctrl->timeout_watcher, ubond_control_timeout_event);
    ctrl->timeout_watcher.repeat = 1.;
    ctrl->fifo_watcher.data = ctrl;
    ctrl->sock_watcher.data = ctrl;
    ctrl->client_io_read.data = ctrl;
    ctrl->client_io_write.data = ctrl;
    ctrl->timeout_watcher.data = ctrl;

    /* UNIX domain socket */
    if (*ctrl->fifo_path)
    {
        ctrl->fifofd = socket(AF_LOCAL, SOCK_STREAM, 0);
        if (ctrl->fifofd < 0)
            log_warn("control", "cannot create unix socket");
        else {
            memset(&un_addr, 0, sizeof(un_addr));
            un_addr.sun_family = AF_UNIX;
            strlcpy(un_addr.sun_path, ctrl->fifo_path, sizeof(un_addr.sun_path));
            /* remove existing sock if exists! (bad stop) */
            /* TODO: handle proper "at_exit" removal of this socket */
            unlink(un_addr.sun_path);
            if (bind(ctrl->fifofd, (struct sockaddr *) &un_addr,
                     sizeof(un_addr)) < 0)
            {
                log_warn("control",
                    "cannot bind socket %s", un_addr.sun_path);
                close(ctrl->fifofd);
                ctrl->fifofd = -1;
            }
        }
    }


    /* INET socket */
    if (*ctrl->bindaddr && *ctrl->bindport)
    {
        ctrl->http = 1;
        memset(&hints, 0, sizeof(hints));
        hints.ai_flags = AI_PASSIVE;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        ret = priv_getaddrinfo(ctrl->bindaddr, ctrl->bindport,
                               &res, &hints);
        bak = res;
        if (ret < 0 || ! res)
        {
            log_warnx("control", "priv_getaddrinfo(%s,%s) failed: %s",
                   ctrl->bindaddr, ctrl->bindport,
                   gai_strerror(ret));
        }

        while(res)
        {
            if ( (ctrl->sockfd = socket(res->ai_family,
                                        res->ai_socktype,
                                        res->ai_protocol)) < 0)
            {
                log_warn("control", "cannot create socket (%s:%s)",
                   ctrl->bindaddr, ctrl->bindport);
            } else {
                val = 1;
                if (setsockopt(ctrl->sockfd, SOL_SOCKET, SO_REUSEADDR,
                        &val, sizeof(int)) < 0) {
                    log_warn("control", "setsockopt SO_REUSEADDR failed");
                    close(ctrl->sockfd);
                    ctrl->sockfd = -1;
                    break;
                }
                if (setsockopt(ctrl->sockfd, IPPROTO_TCP, TCP_NODELAY,
                        &val, sizeof(int)) < 0) {
                    log_warn("control", "setsockopt TCP_NODELAY failed");
                    close(ctrl->sockfd);
                    ctrl->sockfd = -1;
                    break;
                }
                if (bind(ctrl->sockfd, res->ai_addr, res->ai_addrlen) < 0)
                {
                    log_warn("control", "bind failed");
                    close(ctrl->sockfd);
                    ctrl->sockfd = -1;
                }
                break;
            }
            res = res->ai_next;
        }
    }
    if (bak)
        freeaddrinfo(bak);

    /* bind */
    if (ctrl->fifofd >= 0)
    {
        if (ubond_sock_set_nonblocking(ctrl->fifofd) < 0)
        {
            close(ctrl->fifofd);
            ctrl->fifofd = -1;
        } else {
            if (listen(ctrl->fifofd, 1) < 0)
            {
                log_warn("control", "listen error");
                close(ctrl->fifofd);
                ctrl->fifofd = -1;
            }
        }
    }
    if (ctrl->sockfd >= 0)
    {
        if (ubond_sock_set_nonblocking(ctrl->sockfd) < 0)
        {
            close(ctrl->sockfd);
            ctrl->sockfd = -1;
        } else {
            if (listen(ctrl->sockfd, 1) < 0)
            {
                log_warn("control", "listen error");
                close(ctrl->sockfd);
                ctrl->sockfd = -1;
            }
        }
    }
    if (ctrl->sockfd >= 0) {
        ev_io_set(&ctrl->sock_watcher, ctrl->sockfd, EV_READ);
        ev_io_start(EV_DEFAULT_UC, &ctrl->sock_watcher);
    }
    if (ctrl->fifofd >= 0) {
        ev_io_set(&ctrl->fifo_watcher, ctrl->fifofd, EV_READ);
        ev_io_start(EV_DEFAULT_UC, &ctrl->fifo_watcher);
    }
    ev_timer_start(EV_DEFAULT_UC, &ctrl->timeout_watcher);
    return;
}

int
ubond_control_accept(struct ubond_control *ctrl, int fd)
{
    /* Early exit */
    if (fd < 0 || ctrl->mode == UBOND_CONTROL_DISABLED)
        return 0;

    int cfd;
    int accepted = 0;
    struct sockaddr_storage clientaddr;
    socklen_t addrlen = sizeof(clientaddr);

    cfd = accept(fd, (struct sockaddr *)&clientaddr, &addrlen);
    if (cfd < 0)
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            log_warn("control", "accept error");
    } else {
        if (ctrl->clientfd != -1)
        {
            log_debug("control", "already connected on %d",
                ctrl->clientfd);
            send(cfd, "ERR: Already connected.\n", 24, 0);
            close(cfd);
            return 0;
        }
        accepted++;
        if (ubond_sock_set_nonblocking(cfd) < 0)
        {
            log_warnx("control", "cannot set socket to non-blocking");
            ctrl->clientfd = -1;
            close(cfd);
        } else
            ctrl->clientfd = cfd;
        ctrl->rbufpos = 0;
        ctrl->wbufpos = 0;
        ctrl->last_activity = time((time_t *) NULL);
        ev_io_set(&ctrl->client_io_read, ctrl->clientfd, EV_READ);
        ev_io_set(&ctrl->client_io_write, ctrl->clientfd, EV_WRITE);
        ev_io_start(EV_DEFAULT_UC, &ctrl->client_io_read);
    }
    return accepted;
}

int
ubond_control_timeout(struct ubond_control *ctrl)
{
    if (ctrl->mode != UBOND_CONTROL_DISABLED &&
        ctrl->clientfd >= 0)
    {
        if (ctrl->last_activity + UBOND_CTRL_TIMEOUT <=
            time((time_t *)NULL))
        {
            log_info("control", "client %d timeout.", ctrl->clientfd);
            ubond_control_close_client(ctrl);
            return 1;
        }
    }
    return 0;
}

/* Parse a message received from the client.
 * Example messages:
 * STATS
 * UPTIME
 * START tunX
 * STOP tunX
 * RESTART tunX
 */
void
ubond_control_parse(struct ubond_control *ctrl, char *line)
{
    char cline[UBOND_CTRL_BUFSIZ];
    char *cmd = NULL;
    unsigned int i, j;

    /* Cleanup \r */
    for (i = 0, j = 0; i <= strlen(line); i++)
        if (line[i] != '\r')
            cline[j++] = line[i];
    cmd = strtok(cline, " ");
    if (ctrl->http)
        cmd = strtok(NULL, " ");

    if (! cmd)
        return;
    else
        log_debug("control", "command: %s\n", cmd);

    if (ctrl->http)
        ubond_control_write(ctrl, HTTP_HEADERS, strlen(HTTP_HEADERS));

    if (strcasecmp(cmd, "status") == 0 || strcasecmp(cmd, "/status") == 0)
    {
        ubond_control_write_status(ctrl);
    } else if (strcasecmp(cmd, "quit") == 0) {
        ubond_control_write(ctrl, "bye.", 4);
        ubond_control_close_client(ctrl);
    } else {
        ubond_control_write(ctrl, JSON_STATUS_ERROR_UNKNOWN_COMMAND,
            strlen(JSON_STATUS_ERROR_UNKNOWN_COMMAND));
    }

    if (ctrl->http)
        ctrl->close_after_write = 1;
}

extern ubond_pkt_list_t send_buffer;    /* send buffer */

void ubond_control_write_status(struct ubond_control *ctrl)
{
    char buf[1024];
    size_t ret;
    ubond_tunnel_t *t;

    ret = snprintf(buf, 1024, JSON_STATUS_BASE,
        _progname,
        1, 1, /* TODO */
        (uint32_t) ubond_status.start_time,
        (uint32_t) ubond_status.last_reload,
        0,
        tuntap.type == UBOND_TUNTAPMODE_TUN ? "tun" : "tap",
        tuntap.devname,
        bandwidth,
//                   (double) UBOND_TAILQ_LENGTH(&send_buffer),
        1,//ubond_reorder_length(),
        1.0,//ubond_total_loss(),
        pool_out
    );
    ubond_control_write(ctrl, buf, ret);
    LIST_FOREACH(t, &rtuns, entries)
    {
        char *mode = t->server_mode ? "server" : "client";
        char *status;

        if (t->status == UBOND_DISCONNECTED)
            status = "disconnected";
        else if (t->status == UBOND_AUTHSENT)
            status = "waiting peer";
        else if (t->status == UBOND_AUTHOK)
            status = "connected";
        else if (t->status == UBOND_LOSSY)
            status = "lossy link";
        else
            status = "unknown";

        ret = snprintf(buf, 1024, JSON_STATUS_RTUN,
                       t->name,
                       mode,
                       t->bindaddr ? t->bindaddr : "any",
                       t->bindport ? t->bindport : "any",
                       t->destaddr ? t->destaddr : "",
                       t->destport ? t->destport : "",
                       status,
                       t->sentpackets,
                       t->recvpackets,
                       t->sentbytes,
                       t->recvbytes,
                       t->bandwidth_max,
                       t->srtt,
                       (float)(t->loss),
                       (float)(t->sent_loss),
                       (unsigned int)t->reorder_length,//pool_out,
                       pool_out,//(uint32_t)(t->permitted/1000000),
                       t->disconnects,
                       (uint32_t)t->last_activity,
                       (uint32_t)t->timeout,
                       t->bytes_per_sec/128.0,//git it in kbps
                       //t->weight,
                       (LIST_NEXT(t, entries) ? "," : "")
                      );
        ubond_control_write(ctrl, buf, ret);
    }
    ubond_control_write(ctrl, "]}\n", 3);
}

/* Returns 1 if a valid line is found. 0 otherwise. */
int
ubond_control_read_check(struct ubond_control *ctrl)
{
    char line[UBOND_CTRL_BUFSIZ];
    char c;
    int i;
    for (i = 0; i < ctrl->rbufpos; i++)
    {
        c = ctrl->rbuf[i];
        if (c == UBOND_CTRL_EOF)
        {
            log_debug("control", "EOF from client %d", ctrl->clientfd);
            ubond_control_close_client(ctrl);
            break;
        }

        if (c == UBOND_CTRL_TERMINATOR)
        {
            memcpy(line, ctrl->rbuf, i);
            line[i] = '\0';
            /* Shift the actual buffer */
            memmove(ctrl->rbuf, ctrl->rbuf+i,
                    UBOND_CTRL_BUFSIZ - i);
            ctrl->rbufpos -= i+1;
            ubond_control_parse(ctrl, line);
            return 1;
        }
    }
    return 0;
}

/* Read from the socket to rbuf */
int
ubond_control_read(struct ubond_control *ctrl)
{
    ssize_t ret;

    ret = read(ctrl->clientfd, ctrl->rbuf + ctrl->rbufpos,
               UBOND_CTRL_BUFSIZ - ctrl->rbufpos);
    if (ret > 0)
    {
        ctrl->last_activity = time((time_t *)NULL);
        log_debug("control", "received %zd bytes", ret);
        ctrl->rbufpos += ret;
        if (ctrl->rbufpos >= UBOND_CTRL_BUFSIZ)
        {
            log_warnx("control", "overflow on read buffer");
            ubond_control_close_client(ctrl);
            return -1;
        }

        /* Parse the message */
        while (ubond_control_read_check(ctrl) != 0);
    } else if (ret < 0) {
        log_warn("control", "read error on %d", ctrl->clientfd);
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ubond_control_close_client(ctrl);
        }
    } else {
        /* End of file */
        ctrl->clientfd = -1;
    }

    return 0;
}

int
ubond_control_write(struct ubond_control *ctrl, void *buf, size_t len)
{
    if (ctrl->wbuflen - (ctrl->wbufpos+len) <= 0)
    {
        /* Hard realloc */
        ctrl->wbuflen += 1024*32;
        ctrl->wbuf = realloc(ctrl->wbuf, ctrl->wbuflen);
    }

    if (ctrl->wbuflen - (ctrl->wbufpos+len) <= 0)
    {
        log_warnx("control", "send buffer overflow");
        ubond_control_close_client(ctrl);
        return -1;
    }
    memcpy(ctrl->wbuf+ctrl->wbufpos, buf, len);
    ctrl->wbufpos += len;
    ev_io_start(EV_DEFAULT_UC, &ctrl->client_io_write);
    return len;
}

/* Flush the control client wbuf */
int
ubond_control_send(struct ubond_control *ctrl)
{
    ssize_t ret;

    if (ctrl->wbufpos <= 0)
    {
        log_warnx("control", "nothing to write. THIS IS A BUG");
        return -1;
    }
    ret = write(ctrl->clientfd, ctrl->wbuf, ctrl->wbufpos);
    if (ret < 0)
    {
        log_warn("control", "write error on %d",
            ctrl->clientfd);
        ubond_control_close_client(ctrl);
    } else {
        ctrl->wbufpos -= ret;
        if (ctrl->wbufpos > 0) {
            memmove(ctrl->wbuf, ctrl->wbuf+ret, ctrl->wbufpos);
        }
    }

    if (ctrl->close_after_write && ctrl->wbufpos <= 0)
        ubond_control_close_client(ctrl);

    if (ctrl->wbufpos <= 0) {
        ev_io_stop(EV_DEFAULT_UC, &ctrl->client_io_write);
    }

    return ret;
}

