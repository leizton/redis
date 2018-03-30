/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "ae.h"
#include "zmalloc.h"
#include "config.h"


//== 和eventLoop有关

//= 选择eventLoop的实现
#ifdef HAVE_EVPORT
#include "ae_evport.c"
#else
    #ifdef HAVE_EPOLL
    #include "ae_epoll.c"
    #else
        #ifdef HAVE_KQUEUE
        #include "ae_kqueue.c"
        #else
        #include "ae_select.c"
        #endif
    #endif
#endif

aeEventLoop *aeCreateEventLoop(int setsize) {
    aeEventLoop *eventLoop;
    if ((eventLoop = zmalloc(sizeof(*eventLoop))) == NULL) goto err;
    if ((eventLoop->events = zmalloc(sizeof(aeFileEvent)*setsize)) == NULL) goto err;
    if ((eventLoop->fired = zmalloc(sizeof(aeFiredEvent)*setsize)) == NULL) goto err;
    eventLoop->setsize = setsize;
    eventLoop->lastTime = time(NULL);
    eventLoop->timeEventHead = NULL;
    eventLoop->timeEventNextId = 0;
    eventLoop->stop = 0;
    eventLoop->maxfd = -1;
    eventLoop->beforesleep = NULL;
    eventLoop->aftersleep = NULL;

    //= init eventLoop.apidata
    if (aeApiCreate(eventLoop) == -1) goto err;

    // 把eventLoop->events的每个event.mask置AE_NONE
    for (int i = 0; i < setsize; i++) {
        eventLoop->events[i].mask = AE_NONE;
    }

    return eventLoop;

err:
    if (eventLoop) {
        zfree(eventLoop->events);
        zfree(eventLoop->fired);
        zfree(eventLoop);
    }
    return NULL;
}

int aeGetSetSize(aeEventLoop *eventLoop) {
    return eventLoop->setsize;
}

//= 修改eventLoop的setsize
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize) {
    if (setsize == eventLoop->setsize) return AE_OK;
    if (eventLoop->maxfd >= setsize) return AE_ERR;
    if (aeApiResize(eventLoop,setsize) == -1) return AE_ERR;

    eventLoop->events = zrealloc(eventLoop->events,sizeof(aeFileEvent)*setsize);
    eventLoop->fired = zrealloc(eventLoop->fired,sizeof(aeFiredEvent)*setsize);
    eventLoop->setsize = setsize;

    for (int i = eventLoop->maxfd+1; i < setsize; i++)
        eventLoop->events[i].mask = AE_NONE;
    return AE_OK;
}

void aeDeleteEventLoop(aeEventLoop *eventLoop) {
    aeApiFree(eventLoop);
    zfree(eventLoop->events);
    zfree(eventLoop->fired);
    zfree(eventLoop);
}

void aeStop(aeEventLoop *eventLoop) {
    eventLoop->stop = 1;
}

//= 获取eventLoop的具体实现
char *aeGetApiName(void) {
    return aeApiName();
}


//== 和fileEvent有关的函数

//= 添加FileEvent事件
//= mask: 控制proc是否可用于读或写处理
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask, aeFileProc *proc, void *clientData)
{
    if (fd >= eventLoop->setsize) {
        errno = ERANGE;
        return AE_ERR;
    }

    // ->epoll_ctl()
    if (aeApiAddEvent(eventLoop, fd, mask) == -1) {
        return AE_ERR;
    }

    // 修改eventLoop->events[fd]
    aeFileEvent *fe = &eventLoop->events[fd];
    fe->mask |= mask;
    if (mask & AE_READABLE) fe->rfileProc = proc;
    if (mask & AE_WRITABLE) fe->wfileProc = proc;
    fe->clientData = clientData;
    if (fd > eventLoop->maxfd) eventLoop->maxfd = fd;
    return AE_OK;
}

//= 删除FileEvent事件
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int delmask)
{
    if (fd >= eventLoop->setsize) return;
    aeFileEvent *fe = &eventLoop->events[fd];
    if (fe->mask == AE_NONE) return;

    // ->epoll_ctl()
    aeApiDelEvent(eventLoop, fd, delmask);

    fe->mask = fe->mask & (~delmask);
    if (fe->mask == AE_NONE && fd == eventLoop->maxfd) {
        // update eventLoop->maxfd
        for (int i = eventLoop->maxfd-1; i >= 0; i--) {
            if (eventLoop->events[i].mask != AE_NONE) {
                eventLoop->maxfd = i;
                break;
            }
        }
    }
}

int aeGetFileEvents(aeEventLoop *evloop, int fd) {
    return fd < evloop->setsize ? evloop->events[fd].mask : AE_NONE;
}


//== 和timeEvent有关的函数

//= 在milliseconds毫秒后执行
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
                            aeTimeProc *proc, void *clientData,
                            aeEventFinalizerProc *finalizerProc) {
    aeTimeEvent *te = zmalloc(sizeof(*te));
    if (te == NULL) return AE_ERR;

    te->id = eventLoop->timeEventNextId++;
    te->timeProc = proc;
    te->finalizerProc = finalizerProc;
    te->clientData = clientData;
    aeAddMillisecondsToNow(milliseconds,&te->when_sec,&te->when_ms);

    //= 链接到eventLoop上
    te->next = eventLoop->timeEventHead;
    eventLoop->timeEventHead = te;
    return id;
}

//= 根据id删除timeEvent
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id) {
    aeTimeEvent *te = eventLoop->timeEventHead;
    while(te) {
        if (te->id == id) {
            te->id = AE_DELETED_EVENT_ID;
            return AE_OK;
        }
        te = te->next;
    }
    return AE_ERR;
}

//= 查找接下来最近触发的timeEvent, O(n)时间复杂度
static aeTimeEvent *aeSearchNearestTimer(aeEventLoop *eventLoop) {
    aeTimeEvent *te = eventLoop->timeEventHead;
    aeTimeEvent *nearest = NULL;

    while(te) {
        if (!nearest || te->when_sec < nearest->when_sec ||
                (te->when_sec == nearest->when_sec && te->when_ms < nearest->when_ms))
            nearest = te;
        te = te->next;
    }
    return nearest;
}

//= process time events
static int processTimeEvents(aeEventLoop *eventLoop) {
    aeTimeEvent *te;

    /* If the system clock is moved to the future, and then set back to the
     * right value, time events may be delayed in a random way. Often this
     * means that scheduled operations will not be performed soon enough.
     *
     * Here we try to detect system clock skews, and force all the time
     * events to be processed ASAP when this happens: the idea is that
     * processing events earlier is less dangerous than delaying them
     * indefinitely, and practice suggests it is. */
    time_t now = time(NULL);
    if (now < eventLoop->lastTime) {
        te = eventLoop->timeEventHead;
        while(te) {
            te->when_sec = 0;
            te = te->next;
        }
    }
    eventLoop->lastTime = now;

    int processed = 0;
    long long maxId = eventLoop->timeEventNextId-1;
    aeTimeEvent *prev = NULL;
    te = eventLoop->timeEventHead;
    while(te) {
        if (te->id == AE_DELETED_EVENT_ID) {
            // te was deleted in aeDeleteTimeEvent()
            aeTimeEvent *next = te->next;
            if (prev == NULL) {
                // te is head
                eventLoop->timeEventHead = te->next;
            } else {
                prev->next = te->next;  // remove te direct
            }
            if (te->finalizerProc) {
                te->finalizerProc(eventLoop, te->clientData);
            }
            zfree(te);
            te = next;
            continue;
        }

        /* Make sure we don't process time events created by time events in
         * this iteration. Note that this check is currently useless: we always
         * add new timers on the head, however if we change the implementation
         * detail, this check may be useful again: we keep it here for future
         * defense. */
        if (te->id > maxId) {
            te = te->next;
            continue;
        }

        long now_sec, now_ms;
        aeGetTime(&now_sec, &now_ms);
        if (now_sec > te->when_sec || (now_sec == te->when_sec && now_ms >= te->when_ms)) {
            int retval = te->timeProc(eventLoop, te->id, te->clientData);
            if (retval == AE_NOMORE) {
                te->id = AE_DELETED_EVENT_ID;
            } else {
                // 过了retval毫秒后te再次触发
                aeAddMillisecondsToNow(retval, &te->when_sec, &te->when_ms);
            }
            processed++;
        }

        prev = te;
        te = te->next;
    }
    return processed;
}


//== handle eventLoop

//= return: 事件处理的个数
int aeProcessEvents(aeEventLoop *eventLoop, int flags) {
    if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS)) {
        return 0;
    }

    int processedNum = 0;

    //= process file events
    if (eventLoop->maxfd != -1 || ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT))) {
        timeval tv = {0}, *tvp = NULL;

        //= 最近一个触发的timeEvent
        aeTimeEvent *shortestTimeEv = NULL;
        if ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT)) {
            shortestTimeEv = aeSearchNearestTimer(eventLoop);
        }

        if (shortestTimeEv) {
            long now_sec, now_ms;
            aeGetTime(&now_sec, &now_ms);

            // 到下一个timeEvent触发的时间间隔
            uint64_t ms = (shortestTimeEv->when_sec - now_sec)*1000 + (shortestTimeEv->when_ms - now_ms);
            if (ms > 0) {
                tv.tv_sec = ms/1000;
                tv.tv_usec = (ms % 1000)*1000;
            } else {
                tv.tv_sec = tv.tv_usec = 0;
            }

            // epoll_wait timeout is tv
            tvp = &tv;
        } else if (flags & AE_DONT_WAIT) {
            // epoll_wait should return immediately
            tv.tv_sec = tv.tv_usec = 0;
            tvp = &tv;
        } else {
            // epoll_wait should block indefinitely
            tvp = NULL;
        }

        //= ->epoll_wait()
        int numevents = aeApiPoll(eventLoop, tvp);

        if (eventLoop->aftersleep != NULL && flags & AE_CALL_AFTER_SLEEP)
            eventLoop->aftersleep(eventLoop);

        //= rfileProc() wfileProc()
        for (int i = 0; i < numevents; i++) {
            int mask = eventLoop->fired[i].mask;
            int fd = eventLoop->fired[i].fd;
            aeFileEvent *fe = &eventLoop->events[fd];

            bool readFired = false;
            if (fe->mask & mask & AE_READABLE) {
                readFired = true;
                fe->rfileProc(eventLoop, fd, fe->clientData, mask);
            }
            if (fe->mask & mask & AE_WRITABLE) {
                if (readFired && fe->wfileProc == fe->rfileProc) {
                    // 无需再调用wfileProc()
                } else {
                    fe->wfileProc(eventLoop, fd, fe->clientData, mask);
                }
            }
            processedNum++;
        }
    }

    // process time events
    if (flags & AE_TIME_EVENTS) {
        processedNum += processTimeEvents(eventLoop);
    }

    return processedNum;
}

void aeMain(aeEventLoop *eventLoop) {
    eventLoop->stop = 0;
    while (!eventLoop->stop) {
        if (eventLoop->beforesleep != NULL)
            eventLoop->beforesleep(eventLoop);
        aeProcessEvents(eventLoop, AE_ALL_EVENTS|AE_CALL_AFTER_SLEEP);
    }
}

void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep) {
    eventLoop->beforesleep = beforesleep;
}

void aeSetAfterSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *aftersleep) {
    eventLoop->aftersleep = aftersleep;
}


//== util

//= 等待fd的writable/readable/exception, 直到超时
//= 通过系统调用poll()实现
int aeWait(int fd, int mask, long long milliseconds) {
    struct pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    if (mask & AE_READABLE) pfd.events |= POLLIN;
    if (mask & AE_WRITABLE) pfd.events |= POLLOUT;

    int retval;
    if ((retval = poll(&pfd, 1, milliseconds)) == 1) {
        int retmask = 0;
        if (pfd.revents & POLLIN)  retmask |= AE_READABLE;
        if (pfd.revents & POLLOUT) retmask |= AE_WRITABLE;
	    if (pfd.revents & POLLERR) retmask |= AE_WRITABLE;
        if (pfd.revents & POLLHUP) retmask |= AE_WRITABLE;
        return retmask;
    } else {
        return retval;
    }
}

//= 获取系统当前时间
static void aeGetTime(long *seconds, long *milliseconds)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    *seconds = tv.tv_sec;
    *milliseconds = tv.tv_usec/1000;
}

//= 获取当前时间加上milliseconds的时间, 结果保存到sec和ms上
static void aeAddMillisecondsToNow(long long milliseconds, long *sec, long *ms) {
    long cur_sec, cur_ms;
    aeGetTime(&cur_sec, &cur_ms);

    long when_sec = cur_sec + milliseconds/1000;
    long when_ms = cur_ms + milliseconds%1000;
    if (when_ms >= 1000) {
        when_sec ++;
        when_ms -= 1000;
    }
    *sec = when_sec;
    *ms = when_ms;
}
