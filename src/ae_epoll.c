/* Linux epoll(2) based ae.c module
 *
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#include <sys/epoll.h>

typedef struct aeApiState {
    int epfd;
    struct epoll_event *events;
} aeApiState;

static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = zmalloc(sizeof(aeApiState));
    if (state == NULL) goto err;

    state->events = zmalloc(sizeof(struct epoll_event)*eventLoop->setsize);
    if (state->events == NULL) goto err;

    state->epfd = epoll_create(1024); // 1024 is just a hint for the kernel
    if (state->epfd == -1) goto err;

    eventLoop->apidata = state;
    return 0;

err:
    if (state) {
        zfree(state->events);
        zfree(state);
    }
    return -1;
}

static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;
    state->events = zrealloc(state->events, sizeof(struct epoll_event)*setsize);
    return 0;
}

static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    close(state->epfd);
    zfree(state->events);
    zfree(state);
}

static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    //= 如果fd已经关联了其他事件(mask != AE_NONE), 则operation应该是MOD
    int op = eventLoop->events[fd].mask == AE_NONE ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
    mask |= eventLoop->events[fd].mask;  // Merge old events

    struct epoll_event ee = {0};
    ee.events = 0;
    if (mask & AE_READABLE) ee.events |= EPOLLIN;
    if (mask & AE_WRITABLE) ee.events |= EPOLLOUT;
    ee.data.fd = fd;

    //= epoll_ctl()
    return epoll_ctl(eventLoop->apidata->epfd, op, fd, &ee);
}

static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int delmask) {
    int newmask = eventLoop->events[fd].mask & (~delmask);
    int op = newmask == AE_NONE ? EPOLL_CTL_DEL : EPOLL_CTL_MOD;

    struct epoll_event ee = {0};
    ee.events = 0;
    if (newmask & AE_READABLE) ee.events |= EPOLLIN;
    if (newmask & AE_WRITABLE) ee.events |= EPOLLOUT;
    ee.data.fd = fd;

    epoll_ctl(eventLoop->apidata->epfd, op, fd, &ee);
}

static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int timeout = tvp ? (tvp->tv_sec*1000 + tvp->tv_usec/1000) : -1;

    int eventNum = epoll_wait(state->epfd, state->events, eventLoop->setsize, timeout);

    if (eventNum > 0) {
        for (int i = 0; i < eventNum; i++) {
            struct epoll_event *e = state->events + i;
            int mask = 0;

            if (e->events & EPOLLIN)  mask |= AE_READABLE;
            if (e->events & EPOLLOUT) mask |= AE_WRITABLE;
            if (e->events & EPOLLERR) mask |= AE_WRITABLE;
            if (e->events & EPOLLHUP) mask |= AE_WRITABLE;

            eventLoop->fired[j].fd = e->data.fd;
            eventLoop->fired[j].mask = mask;
        }
        return eventNum;
    } else {
        return 0;
    }
}

static char *aeApiName(void) {
    return "epoll";
}
