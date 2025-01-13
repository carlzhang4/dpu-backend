#include <fcntl.h> /* open */
#include <stdint.h> /* uint64_t  */
#include <stdio.h> /* printf */
#include <stdlib.h> /* size_t */
#include <unistd.h> /* pread, sysconf */
#include <errno.h>
#include <string.h>
#include "virt2phys.h"

typedef struct {
    uint64_t pfn : 55;
    unsigned int soft_dirty : 1;
    unsigned int file_page : 1;
    unsigned int swapped : 1;
    unsigned int present : 1;
} PagemapEntry;

/* Parse the pagemap entry for the given virtual address.
 *
 * @param[out] entry      the parsed entry
 * @param[in]  pagemap_fd file descriptor to an open /proc/pid/pagemap file
 * @param[in]  vaddr      virtual address to get entry for
 * @return 0 for success, 1 for failure
 */
int pagemap_get_entry(PagemapEntry *entry, int pagemap_fd, uintptr_t vaddr)
{
    size_t nread;
    ssize_t ret;
    uint64_t data;
    uintptr_t vpn;

    vpn = vaddr / sysconf(_SC_PAGE_SIZE);
    nread = 0;
    while (nread < sizeof(data)) {
        ret = pread(pagemap_fd, ((uint8_t*)&data) + nread, sizeof(data) - nread,
                vpn * sizeof(data) + nread);
        nread += ret;
        if (ret <= 0) {
            return 1;
        }
    }
    entry->pfn = data & (((uint64_t)1 << 55) - 1);
    entry->soft_dirty = (data >> 55) & 1;
    entry->file_page = (data >> 61) & 1;
    entry->swapped = (data >> 62) & 1;
    entry->present = (data >> 63) & 1;
    return 0;
}

int virt_to_phys(uint64_t *paddr, uint64_t vaddr)
{
	pid_t pid = getpid();
    char pagemap_file[BUFSIZ];
    int pagemap_fd;
	snprintf(pagemap_file, sizeof(pagemap_file), "/proc/%ju/pagemap", (uintmax_t)pid);
    pagemap_fd = open(pagemap_file, O_RDONLY);
    if (pagemap_fd < 0) {
		printf("Failed to open %s\n", pagemap_file);
        return 1;
    }
    PagemapEntry entry;
    if (pagemap_get_entry(&entry, pagemap_fd, vaddr)) {
		printf("Failed to get entry for %ju\n", (uintmax_t)vaddr);
        return 1;
    }
    close(pagemap_fd);
    *paddr = (entry.pfn * sysconf(_SC_PAGE_SIZE)) + (vaddr % sysconf(_SC_PAGE_SIZE));
    return 0;
}


void print_pagemap_entry(pid_t pid, unsigned long vaddr_start, unsigned long vaddr_end) {
    char pagemap_path[256];
    snprintf(pagemap_path, sizeof(pagemap_path), "/proc/%d/pagemap", pid);

    int pagemap_fd = open(pagemap_path, O_RDONLY);
    if (pagemap_fd == -1) {
        perror("Failed to open pagemap");
        exit(EXIT_FAILURE);
    }

    for (unsigned long addr = vaddr_start; addr < vaddr_end; addr += _SC_PAGE_SIZE) {
        uint64_t entry;
        unsigned long offset = (addr / _SC_PAGE_SIZE) * sizeof(entry);

        if (lseek(pagemap_fd, offset, SEEK_SET) == -1) {
            fprintf(stderr, "Failed to seek pagemap for 0x%lx: %s\n", addr, strerror(errno));
            continue;
        }

        if (read(pagemap_fd, &entry, sizeof(entry)) != sizeof(entry)) {
            fprintf(stderr, "Failed to read pagemap entry for 0x%lx: %s\n", addr, strerror(errno));
            continue;
        }

        printf("Virtual Address: 0x%lx, Pagemap Entry: 0x%016lx\n", addr, entry);

        if (entry & (1ULL << 63)) {
            unsigned long pfn = entry & ((1ULL << 55) - 1);
            printf("  --> PFN: 0x%lx\n", pfn);
        } else {
            printf("  --> Page not present\n");
        }
    }

    close(pagemap_fd);
}

void read_maps(pid_t pid) {
    char maps_path[256];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);

    FILE *maps_file = fopen(maps_path, "r");
    if (!maps_file) {
        perror("Failed to open maps");
        exit(EXIT_FAILURE);
    }

    char line[256];
    while (fgets(line, sizeof(line), maps_file)) {
        unsigned long start, end;
        if (sscanf(line, "%lx-%lx", &start, &end) != 2) {
            fprintf(stderr, "Failed to parse maps line: %s\n", line);
            continue;
        }
        printf("Processing range: 0x%lx - 0x%lx\n", start, end);
        print_pagemap_entry(pid, start, end);
    }

    fclose(maps_file);
}