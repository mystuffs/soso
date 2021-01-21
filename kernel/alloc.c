#include "common.h"
#include "alloc.h"
#include "vmm.h"
#include "process.h"
#include "debugprint.h"
#include "serial.h"

#define KMALLOC_MINSIZE		16

extern uint32 *g_kernel_page_directory;

static char *g_kernel_heap = NULL;
static uint32 g_kernel_heap_used = 0;


void initialize_kernel_heap()
{
    g_kernel_heap = (char *) KERN_HEAP_BEGIN;

    ksbrk_page(1);
}

void *ksbrk_page(int n)
{
    struct MallocHeader *chunk;
    uint32 p_addr;
    int i;

    if ((g_kernel_heap + (n * PAGESIZE_4K)) > (char *) KERN_HEAP_END) {
        //Screen_PrintF("ERROR: ksbrk(): no virtual memory left for kernel heap !\n");
        return (char *) -1;
    }

    chunk = (struct MallocHeader *) g_kernel_heap;

    for (i = 0; i < n; i++)
    {
        p_addr = vmm_acquire_page_frame_4k();

        if ((int)(p_addr) < 0)
        {
            PANIC("PANIC: ksbrk_page(): no free page frame available !");
            return (char *) -1;
        }

        vmm_add_page_to_pd(g_kernel_heap, p_addr, 0); //add PG_USER to allow user programs to read kernel heap

        g_kernel_heap += PAGESIZE_4K;
    }

    chunk->size = PAGESIZE_4K * n;
    chunk->used = 0;

    return chunk;
}

void *kmalloc(uint32 size)
{
    if (size == 0)
    {
        return 0;
    }

    unsigned long realsize;
    struct MallocHeader *chunk, *other;

    if ((realsize = sizeof(struct MallocHeader) + size) < KMALLOC_MINSIZE)
    {
        realsize = KMALLOC_MINSIZE;
    }

    chunk = (struct MallocHeader *) KERN_HEAP_BEGIN;
    while (chunk->used || chunk->size < realsize)
    {
        if (chunk->size == 0)
        {
            printkf("\nPANIC: kmalloc(): corrupted chunk on %x with null size (heap %x) !\nSystem halted\n", chunk, g_kernel_heap);

            PANIC("kmalloc()");

            return 0;
        }

        chunk = (struct MallocHeader *)((char *)chunk + chunk->size);

        if (chunk == (struct MallocHeader *) g_kernel_heap)
        {
            if ((int)(ksbrk_page((realsize / PAGESIZE_4K) + 1)) < 0)
            {
                PANIC("kmalloc(): no memory left for kernel !\nSystem halted\n");

                return 0;
            }
        }
        else if (chunk > (struct MallocHeader *) g_kernel_heap)
        {
            printkf("\nPANIC: kmalloc(): chunk on %x while heap limit is on %x !\nSystem halted\n", chunk, g_kernel_heap);

            PANIC("kmalloc()");

            return 0;
        }
    }


    if (chunk->size - realsize < KMALLOC_MINSIZE)
    {
        chunk->used = 1;
    }
    else
    {
        other = (struct MallocHeader *)((char *) chunk + realsize);
        other->size = chunk->size - realsize;
        other->used = 0;

        chunk->size = realsize;
        chunk->used = 1;
    }

    g_kernel_heap_used += realsize;

    return (char *) chunk + sizeof(struct MallocHeader);
}

void kfree(void *v_addr)
{
    if (v_addr == (void*)0)
    {
        return;
    }

    struct MallocHeader *chunk, *other;

    chunk = (struct MallocHeader *)((uint32)v_addr - sizeof(struct MallocHeader));
    chunk->used = 0;

    g_kernel_heap_used -= chunk->size;

    //Merge free block with next free block
    while ((other = (struct MallocHeader *)((char *)chunk + chunk->size))
           && other < (struct MallocHeader *)g_kernel_heap
           && other->used == 0)
    {
        chunk->size += other->size;
    }
}

static void sbrk_page(Process* process, int page_count)
{
    if (page_count > 0)
    {
        for (int i = 0; i < page_count; ++i)
        {
            if ((process->brkNextUnallocatedPageBegin + PAGESIZE_4K) > (char*)(MEMORY_END - PAGESIZE_4K))
            {
                return;
            }

            uint32 p_addr = vmm_acquire_page_frame_4k();

            if ((int)(p_addr) < 0)
            {
                //PANIC("sbrk_page(): no free page frame available !");
                return;
            }

            vmm_add_page_to_pd(process->brkNextUnallocatedPageBegin, p_addr, PG_USER | PG_OWNED);

            SET_PAGEFRAME_USED(process->mmappedVirtualMemory, PAGE_INDEX_4K((uint32)process->brkNextUnallocatedPageBegin));

            process->brkNextUnallocatedPageBegin += PAGESIZE_4K;
        }
    }
    else if (page_count < 0)
    {
        page_count *= -1;

        for (int i = 0; i < page_count; ++i)
        {
            if (process->brkNextUnallocatedPageBegin - PAGESIZE_4K >= process->brkBegin)
            {
                process->brkNextUnallocatedPageBegin -= PAGESIZE_4K;

                //This also releases the page frame
                vmm_remove_page_from_pd(process->brkNextUnallocatedPageBegin);

                SET_PAGEFRAME_UNUSED(process->mmappedVirtualMemory, (uint32)process->brkNextUnallocatedPageBegin);
            }
        }
    }
}

void initialize_program_break(Process* process, uint32 size)
{
    process->brkBegin = (char*) USER_OFFSET;
    process->brkEnd = process->brkBegin;
    process->brkNextUnallocatedPageBegin = process->brkBegin;

    //Userland programs (their code, data,..) start from USER_OFFSET
    //Lets allocate some space for them by moving program break.

    sbrk(process, size);
}

void *sbrk(Process* process, int n_bytes)
{
    char* previous_break = process->brkEnd;

    if (n_bytes > 0)
    {
        int remainingInThePage = process->brkNextUnallocatedPageBegin - process->brkEnd;

        if (n_bytes > remainingInThePage)
        {
            int bytesNeededInNewPages = n_bytes - remainingInThePage;
            int neededNewPageCount = ((bytesNeededInNewPages-1) / PAGESIZE_4K) + 1;

            uint32 freePages = vmm_get_free_page_count();
            if ((uint32)neededNewPageCount + 1 > freePages)
            {
                return (void*)-1;
            }

            sbrk_page(process, neededNewPageCount);
        }
    }
    else if (n_bytes < 0)
    {
        char* currentPageBegin = process->brkNextUnallocatedPageBegin - PAGESIZE_4K;

        int remainingInThePage = process->brkEnd - currentPageBegin;

        if (-n_bytes > remainingInThePage)
        {
            int bytesInPreviousPages = -n_bytes - remainingInThePage;
            int neededNewPageCount = ((bytesInPreviousPages-1) / PAGESIZE_4K) + 1;

            sbrk_page(process, -neededNewPageCount);
        }
    }

    process->brkEnd += n_bytes;

    return previous_break;
}

uint32 get_kernel_heap_used()
{
    return g_kernel_heap_used;
}
