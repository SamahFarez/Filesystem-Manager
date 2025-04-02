#ifndef PAGING_H
#define PAGING_H

#include "filesystem.h"


void initialize_paging();
void print_page_table(const char *filename);
void print_page_bitmap();
void free_pages(File *file);
int allocate_pages(int pages_needed, PageTableEntry **page_table);

#endif // PAGING_H