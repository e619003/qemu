/*
 * QEMU live migration
 *
 * Copyright IBM, Corp. 2008
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#include "qemu-common.h"
#include "qemu_socket.h"
#include "migration.h"
#include "qemu-char.h"
#include "buffered_file.h"
#include "block.h"
#include "sysemu.h"
#include "ft_trans_file.h"
#include "event-tap.h"

//#define DEBUG_MIGRATION_TCP

#ifdef DEBUG_MIGRATION_TCP
#define DPRINTF(fmt, ...) \
    do { printf("migration-tcp: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

static VMChangeStateEntry *vmstate;

static int socket_errno(FdMigrationState *s)
{
    return socket_error();
}

static int socket_write(FdMigrationState *s, const void * buf, size_t size)
{
    return send(s->fd, buf, size, 0);
}

static int socket_read(FdMigrationState *s, const void * buf, size_t size)
{
    ssize_t len;

    do {
        len = recv(s->fd, (void *)buf, size, 0);
    } while (len == -1 && socket_error() == EINTR);
    if (len == -1) {
        len = -socket_error();
    }

    return len;
}

static int tcp_close(FdMigrationState *s)
{
    DPRINTF("tcp_close\n");
    /* FIX ME: accessing ft_mode here isn't clean */
    if (s->fd != -1 && ft_mode != FT_INIT) {
        close(s->fd);
        s->fd = -1;
    }
    return 0;
}


static void tcp_wait_for_connect(void *opaque)
{
    FdMigrationState *s = opaque;
    int val, ret;
    socklen_t valsize = sizeof(val);

    DPRINTF("connect completed\n");
    do {
        ret = getsockopt(s->fd, SOL_SOCKET, SO_ERROR, (void *) &val, &valsize);
    } while (ret == -1 && (s->get_error(s)) == EINTR);

    if (ret < 0) {
        migrate_fd_error(s);
        return;
    }

    qemu_set_fd_handler2(s->fd, NULL, NULL, NULL, NULL);

    if (val == 0)
        migrate_fd_connect(s);
    else {
        DPRINTF("error connecting %d\n", val);
        migrate_fd_error(s);
    }
}

MigrationState *tcp_start_outgoing_migration(Monitor *mon,
                                             const char *host_port,
                                             int64_t bandwidth_limit,
                                             int detach,
					     int blk,
					     int inc)
{
    struct sockaddr_in addr;
    FdMigrationState *s;
    int ret;

    if (parse_host_port(&addr, host_port) < 0)
        return NULL;

    s = qemu_mallocz(sizeof(*s));

    s->get_error = socket_errno;
    s->write = socket_write;
    s->read = socket_read;
    s->close = tcp_close;
    s->mig_state.cancel = migrate_fd_cancel;
    s->mig_state.get_status = migrate_fd_get_status;
    s->mig_state.release = migrate_fd_release;

    s->mig_state.blk = blk;
    s->mig_state.shared = inc;

    s->state = MIG_STATE_ACTIVE;
    s->mon = NULL;
    s->bandwidth_limit = bandwidth_limit;
    s->fd = qemu_socket(PF_INET, SOCK_STREAM, 0);
    if (s->fd == -1) {
        qemu_free(s);
        return NULL;
    }

    socket_set_nonblock(s->fd);

    if (!detach) {
        migrate_fd_monitor_suspend(s, mon);
    }

    do {
        ret = connect(s->fd, (struct sockaddr *)&addr, sizeof(addr));
        if (ret == -1)
            ret = -(s->get_error(s));

        if (ret == -EINPROGRESS || ret == -EWOULDBLOCK)
            qemu_set_fd_handler2(s->fd, NULL, NULL, tcp_wait_for_connect, s);
    } while (ret == -EINTR);

    if (ret < 0 && ret != -EINPROGRESS && ret != -EWOULDBLOCK) {
        DPRINTF("connect failed\n");
        migrate_fd_error(s);
    } else if (ret >= 0)
        migrate_fd_connect(s);

    return &s->mig_state;
}

static void ft_trans_incoming(void *opaque)
{
    QEMUFile *f = opaque;

    qemu_file_get_notify(f);
    if (qemu_file_has_error(f)) {
        ft_mode = FT_ERROR;
        qemu_fclose(f);
    }
}

static void ft_trans_reset(void *opaque, int running, int reason)
{
    QEMUFile *f = opaque;

    if (running) {
        if (ft_mode != FT_ERROR) {
            qemu_fclose(f);
        }
        ft_mode = FT_OFF;
        qemu_del_vm_change_state_handler(vmstate);
    }
}

static void ft_trans_schedule_replay(QEMUFile *f)
{
    event_tap_schedule_replay();
    vmstate = qemu_add_vm_change_state_handler(ft_trans_reset, f);
}

static void tcp_accept_incoming_migration(void *opaque)
{
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    int s = (intptr_t)opaque;
    QEMUFile *f;
    int c;

    do {
        c = qemu_accept(s, (struct sockaddr *)&addr, &addrlen);
    } while (c == -1 && socket_error() == EINTR);

    DPRINTF("accepted migration\n");

    if (c == -1) {
        fprintf(stderr, "could not accept migration connection\n");
        goto out2;
    }

    f = qemu_fopen_socket(c);
    if (f == NULL) {
        fprintf(stderr, "could not qemu_fopen socket\n");
        goto out;
    }

    if (ft_mode == FT_INIT) {
        autostart = 0;
    }

    process_incoming_migration(f);

    if (ft_mode == FT_INIT) {
        int ret;

        socket_set_nodelay(c);

        f = qemu_fopen_ft_trans(s, c);
        if (f == NULL) {
            fprintf(stderr, "could not qemu_fopen_ft_trans\n");
            goto out;
        }

        /* need to wait sender to setup */
        ret = qemu_ft_trans_begin(f);
        if (ret < 0) {
            goto out;
        }

        qemu_set_fd_handler2(c, NULL, ft_trans_incoming, NULL, f);
        ft_trans_schedule_replay(f);
        ft_mode = FT_TRANSACTION_RECV;

        return;
    }

    qemu_fclose(f);

out:
    close(c);
out2:
    qemu_set_fd_handler2(s, NULL, NULL, NULL, NULL);
    close(s);
}

int tcp_start_incoming_migration(const char *host_port)
{
    struct sockaddr_in addr;
    int val;
    int s;

    if (parse_host_port(&addr, host_port) < 0) {
        fprintf(stderr, "invalid host/port combination: %s\n", host_port);
        return -EINVAL;
    }

    s = qemu_socket(PF_INET, SOCK_STREAM, 0);
    if (s == -1)
        return -socket_error();

    val = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&val, sizeof(val));

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) == -1)
        goto err;

    if (listen(s, 1) == -1)
        goto err;

    qemu_set_fd_handler2(s, NULL, tcp_accept_incoming_migration, NULL,
                         (void *)(intptr_t)s);

    return 0;

err:
    close(s);
    return -socket_error();
}
