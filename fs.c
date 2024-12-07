#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "fs-sim.h"

#define DISK_SIZE 128 * 1024
#define BLOCK_SIZE 1024
#define NUM_BLOCKS 128
#define NUM_INODES 126

Superblock superblock;
char buffer[BLOCK_SIZE];
int current_directory = 127; // Root directory index
int disk_fd = -1;

// Function prototypes
void fs_mount(char *new_disk_name);
void fs_create(char name[5], int size);
void fs_delete(char name[5]);
void fs_read(char name[5], int block_num);
void fs_write(char name[5], int block_num);
void fs_buff(char buff[1024]);
void fs_ls(void);
void fs_resize(char name[5], int new_size);
void fs_defrag(void);
void fs_cd(char name[5]);

// Utility functions
static int find_free_inode();
static int find_free_blocks(int size, int *start_block);
static int find_inode_by_name(char name[5]);
static void zero_block(int block_num);
static void print_error(const char *format, ...);

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <input_file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    FILE *input_file = fopen(argv[1], "r");
    if (!input_file) {
        fprintf(stderr, "Error: Cannot open input file %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    char command[1024];
    while (fgets(command, sizeof(command), input_file)) {
        char cmd, name[5];
        int arg1, arg2;
        int result;

        // Parse command
        result = sscanf(command, "%c %s %d %d", &cmd, name, &arg1, &arg2);

        switch (cmd) {
        case 'M':
            if (result == 2) {
                fs_mount(name);
            } else {
                fprintf(stderr, "Command Error: %s, invalid arguments\n", argv[1]);
            }
            break;
        case 'C':
            if (result == 3) {
                fs_create(name, arg1);
            } else {
                fprintf(stderr, "Command Error: %s, invalid arguments\n", argv[1]);
            }
            break;
        case 'D':
            if (result == 2) {
                fs_delete(name);
            } else {
                fprintf(stderr, "Command Error: %s, invalid arguments\n", argv[1]);
            }
            break;
        case 'R':
            if (result == 3) {
                fs_read(name, arg1);
            } else {
                fprintf(stderr, "Command Error: %s, invalid arguments\n", argv[1]);
            }
            break;
        case 'W':
            if (result == 3) {
                fs_write(name, arg1);
            } else {
                fprintf(stderr, "Command Error: %s, invalid arguments\n", argv[1]);
            }
            break;
        case 'B':
            if (result == 2) {
                fs_buff(name);
            } else {
                fprintf(stderr, "Command Error: %s, invalid arguments\n", argv[1]);
            }
            break;
        case 'L':
            if (result == 1) {
                fs_ls();
            } else {
                fprintf(stderr, "Command Error: %s, invalid arguments\n", argv[1]);
            }
            break;
        case 'E':
            if (result == 3) {
                fs_resize(name, arg1);
            } else {
                fprintf(stderr, "Command Error: %s, invalid arguments\n", argv[1]);
            }
            break;
        case 'O':
            if (result == 1) {
                fs_defrag();
            } else {
                fprintf(stderr, "Command Error: %s, invalid arguments\n", argv[1]);
            }
            break;
        case 'Y':
            if (result == 2) {
                fs_cd(name);
            } else {
                fprintf(stderr, "Command Error: %s, invalid arguments\n", argv[1]);
            }
            break;
        default:
            fprintf(stderr, "Command Error: %s, invalid command\n", argv[1]);
        }
    }

    fclose(input_file);
    return EXIT_SUCCESS;
}

void fs_mount(char *new_disk_name) {
    if (disk_fd != -1) {
        close(disk_fd);
    }

    disk_fd = open(new_disk_name, O_RDWR);
    if (disk_fd == -1) {
        fprintf(stderr, "Error: Cannot find disk %s\n", new_disk_name);
        return;
    }

    // Read the superblock
    lseek(disk_fd, 0, SEEK_SET);
    read(disk_fd, &superblock, sizeof(Superblock));

    // Consistency checks
    for (int i = 0; i < NUM_INODES; ++i) {
        Inode *inode = &superblock.inode[i];
        if (inode->used_size >> 7) { // Inode in use
            // Check block range
            if (inode->start_block < 1 || inode->start_block > 127) {
                fprintf(stderr, "Error: File system in %s is inconsistent (error code: 2)\n", new_disk_name);
                disk_fd = -1;
                return;
            }
            // Ensure name is null-terminated
            if (inode->name[4] != '\0') {
                fprintf(stderr, "Error: File system in %s is inconsistent (error code: 3)\n", new_disk_name);
                disk_fd = -1;
                return;
            }
        }
    }

    current_directory = 127; // Set to root directory
}

void fs_create(char name[5], int size) {
    // Debugging: print the name being created
    //printf("Creating: %s\n", name);

    // Check for duplicate names in the current directory
    for (int i = 0; i < NUM_INODES; i++) {
        Inode *inode = &superblock.inode[i];
        if ((inode->used_size >> 7) && strncmp(inode->name, name, 4) == 0 && inode->dir_parent == current_directory) {
            fprintf(stderr, "Error: File or directory %s already exists\n", name);
            return;
        }
    }

    // Find a free inode
    int inode_idx = find_free_inode();
    if (inode_idx == -1) {
        fprintf(stderr, "Error: Superblock in disk is full, cannot create %s\n", name);
        return;
    }

    // Find free blocks for the file
    int start_block;
    if (find_free_blocks(size, &start_block) == -1) {
        fprintf(stderr, "Error: Cannot allocate %d blocks on disk\n", size);
        return;
    }

    // Initialize the inode
    Inode *inode = &superblock.inode[inode_idx];
    memset(inode, 0, sizeof(Inode)); // Clear the inode
    strncpy(inode->name, name, 4);   // Copy up to 4 characters
    inode->name[4] = '\0';          // Null-terminate the name
    inode->used_size = (1 << 7) | size; // Set in-use flag and size
    inode->start_block = size > 0 ? start_block : 0; // Assign block only for non-zero size
    inode->dir_parent = current_directory;

    // Debugging: print inode details
    //printf("Inode created: %s, size: %d, start_block: %d, dir_parent: %d\n",
           //inode->name, size, start_block, current_directory);

    // Mark blocks as used
    for (int i = 0; i < size; ++i) {
        superblock.free_block_list[start_block + i] = 1;
    }

    // Write updated superblock to disk
    lseek(disk_fd, 0, SEEK_SET);
    write(disk_fd, &superblock, sizeof(Superblock));
}




void fs_delete(char name[5]) {
    int inode_idx = find_inode_by_name(name);
    if (inode_idx == -1) {
        fprintf(stderr, "Error: File or directory %s does not exist\n", name);
        return;
    }

    Inode *inode = &superblock.inode[inode_idx];
    int start_block = inode->start_block;
    int size = inode->used_size & 0x7F;

    // Zero out blocks
    for (int i = 0; i < size; ++i) {
        zero_block(start_block + i);
        superblock.free_block_list[start_block + i] = 0;
    }

    memset(inode, 0, sizeof(Inode)); // Zero out inode

    // Write updated superblock to disk
    lseek(disk_fd, 0, SEEK_SET);
    write(disk_fd, &superblock, sizeof(Superblock));
}

void fs_read(char name[5], int block_num) {
    int inode_idx = find_inode_by_name(name);
    if (inode_idx == -1) {
        print_error("Error: File %s does not exist\n", name);
        return;
    }

    Inode *inode = &superblock.inode[inode_idx];
    if (block_num >= (inode->used_size & 0x7F)) {
        fprintf(stderr, "Error: %s does not have block %d\n", name, block_num);
        return;
    }

    int block = inode->start_block + block_num;
    off_t offset = lseek(disk_fd, block * BLOCK_SIZE, SEEK_SET);
    if (offset == -1) {
        perror("Error seeking to block for reading");
        return;
    }

    ssize_t read_bytes = read(disk_fd, buffer, BLOCK_SIZE);
    if (read_bytes < 0) {
        perror("Error reading from disk");
        return;
    }

    // Null-terminate the buffer for safe string operations
    if (read_bytes < BLOCK_SIZE) {
        buffer[read_bytes] = '\0';
    } else {
        buffer[BLOCK_SIZE - 1] = '\0';
    }

    // Output buffer content to stdout
    printf("%s", buffer);
}

void fs_write(char name[5], int block_num) {
    int inode_idx = find_inode_by_name(name);
    if (inode_idx == -1) {
        print_error("Error: File %s does not exist\n", name);
        return;
    }

    Inode *inode = &superblock.inode[inode_idx];
    if (block_num >= (inode->used_size & 0x7F)) {
        fprintf(stderr, "Error: %s does not have block %d\n", name, block_num);
        return;
    }

    int block = inode->start_block + block_num;
    off_t offset = lseek(disk_fd, block * BLOCK_SIZE, SEEK_SET);
    if (offset == -1) {
        perror("Error seeking to block for writing");
        return;
    }

    ssize_t written = write(disk_fd, buffer, BLOCK_SIZE);
    if (written != BLOCK_SIZE) {
        fprintf(stderr, "Error: Failed to write block %d of file %s (written %zd bytes)\n", block_num, name, written);
        return;
    }
}

void fs_buff(char buff[1024]) {
    memset(buffer, 0, BLOCK_SIZE);        // Clear the buffer completely
    strncpy(buffer, buff, BLOCK_SIZE - 1); // Safely copy data, leaving space for null terminator
    buffer[BLOCK_SIZE - 1] = '\0';        // Ensure null termination
}

void fs_ls(void) {
    printf(". 127\n.. 127\n"); // Root directory pointers
    for (int i = 0; i < NUM_INODES; i++) {
        Inode *inode = &superblock.inode[i];
        if ((inode->used_size >> 7) && inode->dir_parent == current_directory) {
            // Debugging: print inode details
            //printf("Listing Inode: %s, size: %d, dir_parent: %d\n",
                   //inode->name, inode->used_size & 0x7F, inode->dir_parent);
            printf("%-5s %3d KB\n", inode->name, inode->used_size & 0x7F);
        }
    }
}


void fs_resize(char name[5], int new_size) {
    int inode_idx = find_inode_by_name(name);
    if (inode_idx == -1) {
        print_error("Error: File %s does not exist\n", name);
        return;
    }

    Inode *inode = &superblock.inode[inode_idx];
    int current_size = inode->used_size & 0x7F;
    int start_block = inode->start_block;

    if (new_size > current_size) { // Expanding the file
        int additional_blocks = new_size - current_size;
        int next_free_block = start_block + current_size;

        // Check if there are enough contiguous free blocks
        int can_expand = 1;
        for (int i = 0; i < additional_blocks; i++) {
            if (next_free_block + i >= NUM_BLOCKS || superblock.free_block_list[next_free_block + i]) {
                can_expand = 0;
                break;
            }
        }

        if (can_expand) {
            for (int i = 0; i < additional_blocks; i++) {
                superblock.free_block_list[next_free_block + i] = 1;
            }
            inode->used_size = (1 << 7) | new_size;
        } else { // Move the file to a new location
            int new_start_block;
            if (find_free_blocks(new_size, &new_start_block) == -1) {
                print_error("Error: File %s does not exist\n", name);
                return;
            }

            // Copy data to the new blocks
            char temp[BLOCK_SIZE];
            for (int i = 0; i < current_size; i++) {
                lseek(disk_fd, (start_block + i) * BLOCK_SIZE, SEEK_SET);
                read(disk_fd, temp, BLOCK_SIZE);

                lseek(disk_fd, (new_start_block + i) * BLOCK_SIZE, SEEK_SET);
                write(disk_fd, temp, BLOCK_SIZE);
            }

            // Zero out the old blocks
            for (int i = 0; i < current_size; i++) {
                zero_block(start_block + i);
                superblock.free_block_list[start_block + i] = 0;
            }

            // Mark new blocks as used
            for (int i = 0; i < new_size; i++) {
                superblock.free_block_list[new_start_block + i] = 1;
            }

            inode->start_block = new_start_block;
            inode->used_size = (1 << 7) | new_size;
        }
    } else if (new_size < current_size) { // Shrinking the file
        int blocks_to_free = current_size - new_size;

        // Zero out the freed blocks
        for (int i = 0; i < blocks_to_free; i++) {
            zero_block(start_block + new_size + i);
            superblock.free_block_list[start_block + new_size + i] = 0;
        }

        inode->used_size = (1 << 7) | new_size;
    }

    // Write updated superblock to disk
    lseek(disk_fd, 0, SEEK_SET);
    write(disk_fd, &superblock, sizeof(Superblock));
}

void fs_defrag(void) {
    int next_free_block = 1; // Start after the superblock

    for (int i = 0; i < NUM_INODES; i++) {
        Inode *inode = &superblock.inode[i];
        if (inode->used_size >> 7) { // Inode in use
            int current_size = inode->used_size & 0x7F;

            // Skip if already in place
            if (inode->start_block == next_free_block) {
                next_free_block += current_size;
                continue;
            }

            // Move file blocks to the new location
            char temp[BLOCK_SIZE];
            for (int j = 0; j < current_size; j++) {
                lseek(disk_fd, (inode->start_block + j) * BLOCK_SIZE, SEEK_SET);
                read(disk_fd, temp, BLOCK_SIZE);

                lseek(disk_fd, (next_free_block + j) * BLOCK_SIZE, SEEK_SET);
                write(disk_fd, temp, BLOCK_SIZE);
            }

            // Zero out old blocks
            for (int j = 0; j < current_size; j++) {
                zero_block(inode->start_block + j);
                superblock.free_block_list[inode->start_block + j] = 0;
            }

            // Mark new blocks as used
            for (int j = 0; j < current_size; j++) {
                superblock.free_block_list[next_free_block + j] = 1;
            }

            inode->start_block = next_free_block;
            next_free_block += current_size;
        }
    }

    // Write updated superblock to disk
    lseek(disk_fd, 0, SEEK_SET);
    write(disk_fd, &superblock, sizeof(Superblock));
}

void fs_cd(char name[5]) {
    if (strcmp(name, ".") == 0) {
        return; // Stay in the current directory
    } else if (strcmp(name, "..") == 0) {
        for (int i = 0; i < NUM_INODES; i++) {
            Inode *inode = &superblock.inode[i];
            if (i == current_directory && inode->used_size >> 7) {
                current_directory = inode->dir_parent;
                return;
            }
        }
    } else {
        for (int i = 0; i < NUM_INODES; i++) {
            Inode *inode = &superblock.inode[i];
            if ((inode->used_size >> 7) && strcmp(inode->name, name) == 0) {
                if (inode->dir_parent == current_directory && inode->used_size >> 7) {
                    current_directory = i;
                    return;
                }
            }
        }
        fprintf(stderr, "Error: Directory %s does not exist\n", name);
    }
}

// Helper functions
static int find_free_inode() {
    for (int i = 0; i < NUM_INODES; ++i) {
        if (!(superblock.inode[i].used_size >> 7)) {
            return i;
        }
    }
    return -1;
}

static int find_free_blocks(int size, int *start_block) {
    for (int i = 1; i <= NUM_BLOCKS - size; ++i) {
        int free = 1;
        for (int j = 0; j < size; ++j) {
            if (superblock.free_block_list[i + j]) {
                free = 0;
                break;
            }
        }
        if (free) {
            *start_block = i;
            return 0;
        }
    }
    return -1;
}

static int find_inode_by_name(char name[5]) {
    for (int i = 0; i < NUM_INODES; ++i) {
        if ((superblock.inode[i].used_size >> 7) && strncmp(superblock.inode[i].name, name, 5) == 0) {
            return i;
        }
    }
    return -1;
}

static void zero_block(int block_num) {
    char zero_buf[BLOCK_SIZE] = {0};
    lseek(disk_fd, block_num * BLOCK_SIZE, SEEK_SET);
    write(disk_fd, zero_buf, BLOCK_SIZE);
}

#include <stdarg.h>

static void print_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

