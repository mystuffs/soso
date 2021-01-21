#include "process.h"
#include "timer.h"
#include "common.h"
#include "errno.h"
#include "syscall_select.h"

void select_update(Thread* thread)
{
    Process* process = thread->owner;

    int total_ready = 0;
    uint32 count = (uint32)thread->select.nfds;
    count = MIN(count, MAX_OPENED_FILES);

    for (uint32 fd = 0; fd < count; ++fd)
    {
        File* file = process->fd[fd];

        if (file)
        {
            if (FD_ISSET(fd, &thread->select.readSet))
            {
                if (file->node->readTestReady && file->node->readTestReady(file))
                {
                    FD_SET(fd, &thread->select.readSetResult);

                    ++total_ready;
                }
            }

            if (FD_ISSET(fd, &thread->select.writeSet))
            {
                if (file->node->writeTestReady && file->node->writeTestReady(file))
                {
                    FD_SET(fd, &thread->select.writeSetResult);

                    ++total_ready;
                }
            }
        }
    }

    if (total_ready > 0)
    {
        thread->select.result = total_ready;
        thread->select.selectState = SS_FINISHED;
    }
    else if (thread->select.targetTime > 0)
    {
        time_t now = get_uptime_milliseconds64();

        if (now > thread->select.targetTime)
        {
            thread->select.result = 0;
            thread->select.selectState = SS_FINISHED;
        }
    }
}

static int select_finish(Thread* thread, fd_set* rfds, fd_set* wfds)
{
    if (rfds)
    {
        *rfds = thread->select.readSetResult;
    }

    if (wfds)
    {
        *wfds = thread->select.writeSetResult;
    }

    int result = thread->select.result;
    memset((uint8*)&thread->select, 0, sizeof(thread->select));

    return result;
}

int syscall_select(int n, fd_set* rfds, fd_set* wfds, fd_set* efds, struct timeval* tv)
{
    if (!check_user_access(rfds))
    {
        return -EFAULT;
    }

    if (!check_user_access(wfds))
    {
        return -EFAULT;
    }

    if (!check_user_access(efds))
    {
        return -EFAULT;
    }

    if (!check_user_access(tv))
    {
        return -EFAULT;
    }

    Thread* thread = get_current_thread();
    Process* process = thread->owner;

    if (process)
    {
        thread->select.selectState = SS_STARTED;
        thread->select.nfds = n;
        FD_ZERO(&thread->select.readSetResult);
        FD_ZERO(&thread->select.writeSetResult);

        FD_ZERO(&thread->select.readSet);
        if (rfds)
        {
            thread->select.readSet = *rfds;
            FD_ZERO(rfds);
        }

        FD_ZERO(&thread->select.writeSet);
        if (wfds)
        {
            thread->select.writeSet = *wfds;
            FD_ZERO(wfds);
        }

        thread->select.result = -1;

        thread->select.targetTime = 0;
        if (tv)
        {
            thread->select.targetTime = get_uptime_milliseconds64() + tv->tv_sec * 1000 + tv->tv_usec / 1000;
        }

        select_update(thread);

        if (thread->select.selectState == SS_FINISHED)
        {
            int result = select_finish(thread, rfds, wfds);

            return result;
        }

        changeThreadState(thread, TS_SELECT, NULL);
        enableInterrupts();
        halt();

        if (thread->select.selectState == SS_FINISHED)
        {
            int result = select_finish(thread, rfds, wfds);

            return result;
        }

    }

    return -1;
}