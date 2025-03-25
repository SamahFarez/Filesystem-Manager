#include "../include/paging.h"


// Initialize paging system
void initialize_paging() {
    memset(page_bitmap, 0, sizeof(page_bitmap));
}

// Allocate pages for a file
int allocate_pages(int pages_needed, PageTableEntry **page_table) {
    *page_table = malloc(pages_needed * sizeof(PageTableEntry));
    if (*page_table == NULL) {
        return -1;
    }

    for (int i = 0; i < pages_needed; i++) {
        int page = -1;
        for (int j = 0; j < TOTAL_PAGES; j++) {
            if (!(page_bitmap[j / 8] & (1 << (j % 8)))) {
                page_bitmap[j / 8] |= (1 << (j % 8));
                (*page_table)[i].physical_page = j;
                (*page_table)[i].is_allocated = 1;
                page = j;
                break;
            }
        }
        if (page == -1) {
            // Cleanup already allocated pages
            for (int k = 0; k < i; k++) {
                page_bitmap[(*page_table)[k].physical_page / 8] &= ~(1 << ((*page_table)[k].physical_page % 8));
            }
            free(*page_table);
            return -1;
        }
    }
    return 0;
}

// Free pages for a file
void free_pages(File *file) {
    for (int i = 0; i < file->page_table_size; i++) {
        int page_num = file->page_table[i].physical_page;
        page_bitmap[page_num / 8] &= ~(1 << (page_num % 8));
    }
    free(file->page_table);
}



// Add this to your file system code
void print_page_table(const char *filename) {
    pthread_mutex_lock(&mutex);
    
    File *file = NULL;
    for (int i = 0; i < fs_state.directories[fs_state.current_directory].file_count; i++) {
        if (strcmp(fs_state.directories[fs_state.current_directory].files[i].filename, filename) == 0) {
            file = &fs_state.directories[fs_state.current_directory].files[i];
            break;
        }
    }

    if (!file) {
        printf("File not found: %s\n", filename);
        pthread_mutex_unlock(&mutex);
        return;
    }

    printf("\nPage Table for %s (Size: %d bytes, Pages: %d):\n",
           filename, file->size, file->page_table_size);
    printf("----------------------------------------\n");
    printf("Page | Physical Page | Status\n");
    printf("-----|---------------|--------\n");

    for (int i = 0; i < file->page_table_size; i++) {
        printf("%4d | %13d | %s\n", 
               i, 
               file->page_table[i].physical_page,
               file->page_table[i].is_allocated ? "Allocated" : "Free");
    }

    pthread_mutex_unlock(&mutex);
}

void print_page_bitmap() {
    pthread_mutex_lock(&mutex);
    
    printf("\nPage Allocation Bitmap:\n");
    printf("----------------------\n");
    
    for (int i = 0; i < TOTAL_PAGES; i++) {
        if (i % 64 == 0) printf("\n%04d: ", i);
        printf("%c", (page_bitmap[i / 8] & (1 << (i % 8))) ? 'X' : '.');
    }
    
    printf("\n\nX = Allocated, . = Free\n");
    pthread_mutex_unlock(&mutex);
}
