# Memory Allocation Library

A custom implementation of `malloc`, `calloc`, `realloc`, and `free` in C, built from scratch to mimic and extend the behavior of the standard C memory allocator.  
The library uses both **heap-based allocation** via `sbrk` and **mmap-based allocation** for large memory requests, along with memory management features like **best-fit allocation**, **block coalescing**, and **splitting**.

---

## Features

- **Heap and mmap allocation**
  - Uses `sbrk` for small allocations (default preallocation: `128 KB`)
  - Uses `mmap` for large allocations or page-aligned requests
- **Best-fit allocation strategy**
  - Finds the smallest free block that fits the request
- **Block coalescing**
  - Merges adjacent free blocks to reduce fragmentation
- **Block splitting**
  - Creates smaller free blocks from unused space after allocation
- **Thread-safe ready**
  - Code structure designed for easy future integration of mutexes if needed
- **Alignment**
  - Ensures all allocations are 8-byte aligned
- **Custom metadata system**
  - Maintains doubly-linked lists for heap and mmap blocks

---

## File Structure

- `osmem.c` – Core implementation of memory management
- `block_meta.h` – Metadata structure definition
- `osmem.h` – Public API declarations
- Other helper headers/libraries

---

## How It Works

1. **Small allocations (< 128 KB)** are handled on the heap:
   - Uses `sbrk` to increase heap space if no suitable free block is found
   - Preallocates `128 KB` for efficiency
2. **Large allocations (>= 128 KB)** use `mmap`:
   - Allocates private, anonymous memory mappings
   - Freed with `munmap`
3. **Best-fit search** tries to minimize fragmentation
4. **Coalescing** merges continuous free blocks
5. **Splitting** reuses leftover memory after allocations

---

## API

```c
void *os_malloc(size_t size);
void *os_calloc(size_t nmemb, size_t size);
void *os_realloc(void *ptr, size_t size);
void os_free(void *ptr);
```

## 🛠️ Compilation and Running
```bash
# Compile the library
gcc -c osmem.c -o osmem.o
ar rcs libosmem.a osmem.o

# Compile a test program that uses the library
gcc test.c -L. -losmem -o test

# Run the program
./test
