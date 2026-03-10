// SPDX-License-Identifier: GPL-3.0-or-later
#include "editor.h"
#include "filewatcher.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#  include <sys/inotify.h>
#else
#  include <fcntl.h>
#  include <sys/event.h>
#  include <sys/time.h>
#endif

void filewatcher_init(void) {
#ifdef __linux__
    E.file_watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
#else
    E.file_watch_fd = kqueue();
#endif
    /* -1 on failure is fine — watching is best-effort */
}

void filewatcher_add(int buftab_idx) {
    if (E.file_watch_fd < 0) return;
    BufTab *bt = &E.buftabs[buftab_idx];

    /* Only watch regular file buffers. */
    const char *path = (buftab_idx == E.cur_buftab)
                       ? E.buf.filename : bt->buf.filename;
    if (!path || buftab_is_special(bt)) { bt->watch_handle = -1; return; }

#ifdef __linux__
    /* Remove existing watch if any. */
    if (bt->watch_handle >= 0) {
        inotify_rm_watch(E.file_watch_fd, bt->watch_handle);
        bt->watch_handle = -1;
    }

    bt->watch_handle = inotify_add_watch(E.file_watch_fd, path,
                                         IN_MODIFY | IN_DELETE_SELF | IN_MOVE_SELF);
#else
    /* Close existing watch fd if any. */
    if (bt->watch_handle >= 0) {
        close(bt->watch_handle);
        bt->watch_handle = -1;
    }

    int fd = open(path, O_RDONLY | O_EVTONLY | O_CLOEXEC);
    if (fd < 0) return;

    struct kevent kev;
    EV_SET(&kev, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_DELETE | NOTE_RENAME, 0, (void *)(intptr_t)buftab_idx);
    if (kevent(E.file_watch_fd, &kev, 1, NULL, 0, NULL) < 0) {
        close(fd);
        return;
    }
    /* fd is now owned by kqueue; released via filewatcher_remove() */
    bt->watch_handle = fd;
#endif
    bt->file_changed = 0;
}

void filewatcher_remove(int buftab_idx) {
    if (E.file_watch_fd < 0) return;
    BufTab *bt = &E.buftabs[buftab_idx];
    if (bt->watch_handle >= 0) {
#ifdef __linux__
        inotify_rm_watch(E.file_watch_fd, bt->watch_handle);
#else
        struct kevent kev;
        EV_SET(&kev, bt->watch_handle, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
        kevent(E.file_watch_fd, &kev, 1, NULL, 0, NULL);
        close(bt->watch_handle);
#endif
        bt->watch_handle = -1;
    }
    /* Discard stale flag — watch is gone, no further events will arrive. */
    bt->file_changed = 0;
}

void filewatcher_drain(void) {
    if (E.file_watch_fd < 0) return;

#ifdef __linux__
    /* Read all pending inotify events. */
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        ssize_t n = read(E.file_watch_fd, buf, sizeof(buf));
        if (n <= 0) break;

        for (char *ptr = buf; ptr < buf + n; ) {
            struct inotify_event *ev = (struct inotify_event *)ptr;
            ptr += sizeof(*ev) + ev->len;

            /* Find the buftab that owns this watch descriptor. */
            for (int i = 0; i < E.num_buftabs; i++) {
                if (E.buftabs[i].watch_handle == ev->wd) {
                    /* Skip events caused by our own save. */
                    if ((ev->mask & IN_MODIFY) && E.buftabs[i].watch_skip > 0) {
                        E.buftabs[i].watch_skip--;
                        break;
                    }
                    E.buftabs[i].file_changed = 1;

                    /* Deleted/moved: the watch is auto-removed by the kernel. */
                    if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF))
                        E.buftabs[i].watch_handle = -1;
                    break;
                }
            }
        }
    }
#else
    /* Collect all pending kqueue events non-blocking. */
    struct kevent events[32];
    struct timespec ts = { 0, 0 };
    int n = kevent(E.file_watch_fd, NULL, 0, events, 32, &ts);
    for (int j = 0; j < n; j++) {
        int fd = (int)events[j].ident;

        /* Find the buftab that owns this watch fd. */
        for (int i = 0; i < E.num_buftabs; i++) {
            if (E.buftabs[i].watch_handle != fd) continue;

            if ((events[j].fflags & NOTE_WRITE) && E.buftabs[i].watch_skip > 0) {
                E.buftabs[i].watch_skip--;
            } else {
                E.buftabs[i].file_changed = 1;
            }

            if (events[j].fflags & (NOTE_DELETE | NOTE_RENAME)) {
                /* File gone — close our fd; future watches start fresh. */
                close(fd);
                E.buftabs[i].watch_handle = -1;
            }
            break;
        }
    }
#endif

    /* Show reload prompt for the first changed buffer found.
       Only one prompt at a time — remaining changes queued via file_changed. */
    if (E.watch_prompt_buf >= 0) return;  /* already prompting */

    for (int i = 0; i < E.num_buftabs; i++) {
        if (!E.buftabs[i].file_changed) continue;
        E.buftabs[i].file_changed = 0;

        const char *fname = (i == E.cur_buftab) ? E.buf.filename
                                                 : E.buftabs[i].buf.filename;
        if (!fname) continue;
        const char *base = strrchr(fname, '/');
        base = base ? base + 1 : fname;

        E.watch_prompt_buf = i;
        E.statusmsg_is_error = 1;
        snprintf(E.statusmsg, sizeof(E.statusmsg),
                 "W: \"%s\" changed on disk. [O]K, (L)oad File: ", base);

        /* Re-add watch so future changes are also detected. */
        filewatcher_add(i);
        break;  /* handle one at a time */
    }
}
