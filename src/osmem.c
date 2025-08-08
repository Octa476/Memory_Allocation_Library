// SPDX-License-Identifier: BSD-3-Clause

// Include helping libraries for syscalls and string operations.
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>

#include "osmem.h"
#include "block_meta.h"

// Maximum block size for heap allocation.
#define BRK_LIMIT (128 * 1024)

// Alignment to 8 bytes macro; it returns the smallest multiple of 8 smaller than (size).
#define ALIGNMENT 8
#define SIZE_ALIGN(size)  (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

// Meta_data block size.
#define META_DATA_SIZE SIZE_ALIGN(sizeof(TBlock_meta))

typedef struct block_meta TBlock_meta;


// Global heads for the block_meta lists.
// Sentinel lists are used.
TBlock_meta block_head_brk;
TBlock_meta block_head_mmap;


// HELPFUL FUNCTIONS

// MAP SEGMENT

// Initializes the list used to store map_segment_metadata.
void init_list_mmap(void)
{
	block_head_mmap.status = -1; // status is set for safety
	block_head_mmap.size = 0; // 0 is never a normal size
	block_head_mmap.next = &block_head_mmap;
	block_head_mmap.prev = &block_head_mmap;
}

// Adds a cell into the map_segment_metadata list.
// The function returns the address of the payload.
void *add_meta_cell_mmap(size_t size)
{
	// First initialization of the list.
	if (!block_head_mmap.size)
		init_list_mmap();

	size_t total_size = META_DATA_SIZE + SIZE_ALIGN(size);

	void *addr = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	DIE(addr == MAP_FAILED, "mmap");

	// Initialize the cell.
	TBlock_meta *cell = (TBlock_meta *)addr;

	cell->status = STATUS_MAPPED;
	cell->size = SIZE_ALIGN(size);

	// Insert the cell into the list.
	cell->next = &block_head_mmap;
	cell->prev = block_head_mmap.prev;

	block_head_mmap.prev->next = cell;
	block_head_mmap.prev = cell;

	// Return the address of the payload.
	return (void *)(addr + META_DATA_SIZE);
}

// Deletes the cell from the list and unmap the block.
void delete_meta_cell_mmap(TBlock_meta *cell)
{
	cell->prev->next = cell->next;
	cell->next->prev = cell->prev;
	munmap((void *)cell, META_DATA_SIZE + cell->size);
}

// HEAP

// Initializes the list used to store heap_metadata.
void init_list_brk(void)
{
	block_head_brk.status = -1; // status is set for safety
	block_head_brk.size = 1; // 0 is never a normal size
	block_head_brk.next = &block_head_brk;
	block_head_brk.prev = &block_head_brk;
}

// Add a new cell into the heap_metadata list.
// (size_t size) is the aligned payload size.
void *add_meta_cell_brk(TBlock_meta *last_cell, TBlock_meta *cell, size_t size, int status)
{
	// Initialize cell fields.
	cell->status = status;
	cell->size = SIZE_ALIGN(size);

	// Insert the new cell into the list.
	cell->next = last_cell->next;
	cell->prev = last_cell;

	last_cell->next->prev = cell;
	last_cell->next = cell;

	// Return the payload address.
	return (void *)((void *)cell + META_DATA_SIZE);
}

// Delete the cell from the list without changing
// block status or deleting it.
void delete_meta_cell_brk(TBlock_meta *cell)
{
	cell->prev->next = cell->next;
	cell->next->prev = cell->prev;
}

// Default preallocation of 128kB.
void heap_preallocation(void)
{
	// Initialize heap_metadata list.
	init_list_brk();

	void *heap_start = sbrk(BRK_LIMIT);

	DIE(heap_start == (void *)-1, "sbrk");

	// Add a free zone that takes the whole prealocate space.
	add_meta_cell_brk(&block_head_brk, (TBlock_meta *)heap_start, BRK_LIMIT - META_DATA_SIZE, STATUS_FREE);
}

// Coalesce all the block from curr_cell upwards.
void coalesce_block(TBlock_meta *curr_cell)
{
	// Take every free cell after curr_cell.
	TBlock_meta *free_curr = curr_cell->next;

	while ((free_curr != &block_head_brk) && (free_curr->status == STATUS_FREE)) {
		// If the cell is freed, delete it.
		delete_meta_cell_brk(free_curr);
		free_curr = free_curr->next;
	}
	// Update the connection
	curr_cell->next = free_curr;

	// Change the size of the curr_cell.
	void *start = (void *)curr_cell;
	void *stop = NULL;

	if (free_curr == &block_head_brk) {
		stop = sbrk(0); // extend till heap end
		DIE(stop == (void *)-1, "sbrk");
	} else {
		stop = (void *)free_curr; // extend till the next block
	}
	curr_cell->size = stop - start - META_DATA_SIZE;
}

// Coalesce all the blocks in heap.
void coalesce_blocks(void)
{
	// Begin withthe first block.
	TBlock_meta *curr_cell = block_head_brk.next;

	// Search every continuous zones of free blocks.
	while (curr_cell != &block_head_brk) {
		if (curr_cell->status == STATUS_FREE)
			coalesce_block(curr_cell);
		// After coalesce the conections are already updated.
		curr_cell = curr_cell->next;
	}
}

// Creates free blocks from unused heap space.
void use_unused_space(void *addr, size_t size_used)
{
	TBlock_meta *cell = addr;
	void *start = addr + META_DATA_SIZE + SIZE_ALIGN(size_used);
	void *stop = NULL;

	// Find the limit of the unused space.
	if (cell->next != &block_head_brk) {
		stop = cell->next; // cell bound
	} else {
		stop = sbrk(0); // heap bound
		DIE(stop == (void *)-1, "sbrk");
	}

	// If the unused space is big enough to create a block larger then 0 bytes,
	if ((size_t)(stop - start) > META_DATA_SIZE)
		add_meta_cell_brk(addr, start, stop - start - META_DATA_SIZE, STATUS_FREE);
}

// Search for the smallest free block that can hold (size_t size) bytes.
// If the block doesn't exist, returns NULL.
void *search_best_fit(size_t size)
{
	// Start with the first block.
	TBlock_meta *curr_cell = block_head_brk.next;

	// Search for the smallest free block larger or equal to (size).
	TBlock_meta *best_fit = NULL;

	while (curr_cell != &block_head_brk) {
		if (curr_cell->status == STATUS_FREE) {
			if ((!best_fit) && (curr_cell->size >= size))
				best_fit = curr_cell;
			if ((best_fit) && (curr_cell->size >= size) && (curr_cell->size < best_fit->size))
				best_fit = curr_cell;
		}
		curr_cell = curr_cell->next;
	}

	if (best_fit) {
		// Delete the best_fit old cell.
		delete_meta_cell_brk(best_fit);
		// Truncate the block by creating a new smaller block.
		add_meta_cell_brk(best_fit->prev, best_fit, SIZE_ALIGN(size), STATUS_ALLOC);
		// Use the remaining space to form a free block(if possible).
		use_unused_space((void *)best_fit, best_fit->size);

		// Return the payload address.
		return (void *)((void *)best_fit + META_DATA_SIZE);
	}

	return NULL;
}

// Increases the heap break_point and returns the payload address of
// the new alloced block.
void *increase_heap(size_t size)
{
	// Find the last cell of the heap.
	TBlock_meta *last_cell = block_head_brk.prev;

	// The last cell is freed.
	if (last_cell->status == STATUS_FREE) {
		// Unused space start address.
		void *last_addr = (void *)last_cell + META_DATA_SIZE + last_cell->size;

		void *stop = sbrk(0); // heap bound

		DIE(stop == (void *)-1, "sbrk");

		// Increase heap to the smallest necessary size(use the unused space).
		size_t rem_size = SIZE_ALIGN(size) - last_cell->size - (stop - last_addr);

		void *ret_sbrk = sbrk(rem_size);

		DIE(ret_sbrk == (void *)-1, "sbrk");

		// Delete the last free cell and create a new alloced cell.
		delete_meta_cell_brk(last_cell);
		return add_meta_cell_brk(last_cell->prev, last_cell, SIZE_ALIGN(size), STATUS_ALLOC);

	} else {
		// The last cell is alloced.
		// Ignore the unused space while allocating new heap space.
		size_t total_size = META_DATA_SIZE + SIZE_ALIGN(size);

		void *start = sbrk(total_size);

		DIE(start == (void *)-1, "sbrk");
		return add_meta_cell_brk(last_cell, (TBlock_meta *)start, SIZE_ALIGN(size), STATUS_ALLOC);
	}
}

// OS FUNCTIONS


void *os_malloc(size_t size)
{
	void *return_addr = NULL;

	if (!size)
		return NULL;

	// Malloc on map segment.
	if (size >= BRK_LIMIT) {
		return_addr = add_meta_cell_mmap(size);
	} else {
		// Malloc on heap(use preallocation).
		if (!block_head_brk.size)
			heap_preallocation();

		// Coalesce free blocks for consistency.
		coalesce_blocks();

		// Search for a fitting free block.
		return_addr = search_best_fit(size);

		// If a fitting block isn't found, increase the heap.
		if (!return_addr)
			return_addr = increase_heap(size);
	}
	return return_addr;
}

void os_free(void *ptr)
{
	if (!ptr)
		return;

	TBlock_meta *cell_addr = (TBlock_meta *)(ptr - META_DATA_SIZE);

	if (cell_addr->status == STATUS_ALLOC)
		// Just change the status.
		cell_addr->status = STATUS_FREE;
	if (cell_addr->status == STATUS_MAPPED)
		delete_meta_cell_mmap(cell_addr);
}

// Similar to malloc.
void *os_calloc(size_t nmemb, size_t size)
{
	// Max size for heap calloc allocation.
	size_t page_size = (size_t)getpagesize();

	if ((size_t)getpagesize() > 4080)
		page_size = 4080;

	void *return_addr = NULL;

	size_t total_size = nmemb * size;

	if (!total_size)
		return NULL;

	// Calloc on map segment.
	if (total_size >= page_size) {
		return_addr = add_meta_cell_mmap(total_size);
	} else {
		// Calloc on heap.
		if (!block_head_brk.size)
			heap_preallocation();

		coalesce_blocks();

		return_addr = search_best_fit(total_size);

		if (!return_addr)
			return_addr = increase_heap(total_size);
	}
	// Set the zone to 0.
	memset(return_addr, 0, SIZE_ALIGN(total_size));
	return return_addr;
}

void *os_realloc(void *ptr, size_t size)
{
	// Edge cases.
	if (!ptr) {
		if (size >= BRK_LIMIT)
			return add_meta_cell_mmap(size);
		else
			return os_malloc(size);
	}

	if (!size) {
		os_free(ptr);
		return NULL;
	}

	TBlock_meta *cell_addr = (TBlock_meta *)(ptr - META_DATA_SIZE);

	if (cell_addr->status == STATUS_FREE)
		return NULL;

	void *return_addr = NULL;

	// The cell is on heap.
	if (cell_addr->status == STATUS_ALLOC) {
		// Reallocation on map segment.
		if (size >= BRK_LIMIT) {
			// Mark the cell as freed.
			cell_addr->status = STATUS_FREE;
			return_addr = add_meta_cell_mmap(size);
			// Copy everything.
			memcpy(return_addr, (void *)cell_addr + META_DATA_SIZE, cell_addr->size);
			return return_addr;
		}

		// Truncate case.
		if (SIZE_ALIGN(size) <= cell_addr->size) {
			cell_addr->size = SIZE_ALIGN(size);
			use_unused_space(cell_addr, SIZE_ALIGN(size));
			return_addr = ptr;
		} else {
			// Extend case.
			size_t old_size = cell_addr->size;
			// Try to extend the current block.
			coalesce_block(cell_addr);

			// The block becomes big enough.
			if (cell_addr->size >= SIZE_ALIGN(size)) {
				cell_addr->size = SIZE_ALIGN(size);
				cell_addr->status = STATUS_ALLOC;

				use_unused_space(cell_addr, SIZE_ALIGN(cell_addr->size));

				return_addr = ptr;
			} else {
				// The block is not big enough.
				if ((cell_addr->next == &block_head_brk) && (old_size == cell_addr->size)) {
					// Manual extend of the block by heap increase(when the block is at the end oh heap).
					size_t total_size = SIZE_ALIGN(size) - cell_addr->size;

					void *sbrk_addr = sbrk(total_size);

					DIE(sbrk_addr == (void *)-1, "sbrk");
					cell_addr->size = SIZE_ALIGN(size);
					return_addr = (void *)cell_addr + META_DATA_SIZE;
				} else {
					// The block is not at the end oh heap.
					// Search another good block(or create one) using malloc.
					return_addr = os_malloc(size);
					cell_addr->status = STATUS_FREE;
					// Copy everything.
					memcpy(return_addr, (void *)cell_addr + META_DATA_SIZE, cell_addr->size);
				}
			}
		}
	}

	// The block is on map segment.
	if (cell_addr->status == STATUS_MAPPED) {
		// Delete and reallocate a new block
		if (size >= BRK_LIMIT) {
			return_addr = add_meta_cell_mmap(size);
			memcpy(return_addr, (void *)cell_addr + META_DATA_SIZE, size);
			delete_meta_cell_mmap(cell_addr);
		} else {
			// Search for a heap block.
			return_addr = os_malloc(size);
			memcpy(return_addr, (void *)cell_addr + META_DATA_SIZE, size);
			delete_meta_cell_mmap(cell_addr);
		}
	}
	return return_addr;
}
