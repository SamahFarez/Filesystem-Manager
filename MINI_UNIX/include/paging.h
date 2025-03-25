#ifndef PAGING_H
#define PAGING_H

#include "filesystem.h"

void initialize_paging();
void print_page_table(const char *filename);
void print_page_bitmap();

#endif // PAGING_H