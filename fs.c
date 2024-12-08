#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include "fs-sim.h" 
#include <ctype.h>
#include <libgen.h>
#include <string.h>

static Superblock superblock; // In-memory superblock
static char buffer[1024];     // File system buffer
static int current_working_dir = 127; // Start at root (special case)
static FILE *disk_file = NULL; // Pointer to the virtual disk

char *returnBinary(char *free_block_list) {
    char *binary_map = malloc(129); // 128 blocks + null terminator
    binary_map[128] = '\0';
    for (int i = 0; i < 128; i++) {
        binary_map[i] = (free_block_list[i / 8] & (1 << (7 - (i % 8)))) ? '1' : '0';
    }
    return binary_map;
}

void setBitInRange(char *free_block_list, int start, int end, int value) {
    for (int i = start; i <= end; i++) {
        if (value) {
            free_block_list[i / 8] |= (1 << (7 - (i % 8))); // Set bit
        } else {
            free_block_list[i / 8] &= ~(1 << (7 - (i % 8))); // Clear bit
        }
    }
}

void fs_mount(char *new_disk_name) {
    FILE *disk_file_temp = fopen(new_disk_name, "rb+");
    if (!disk_file_temp) {
        fprintf(stderr, "Error: Cannot find disk %s\n", new_disk_name);
        return;
    }

    Superblock temp_superblock;
    if (fread(&temp_superblock, sizeof(Superblock), 1, disk_file_temp) != 1) {
        fprintf(stderr, "Error: Failed to read superblock from %s\n", new_disk_name);
        fclose(disk_file_temp);
        return;
    }

    // Consistency Check 1: If an inode is free, all its fields must be zero
    for (int i = 0; i < 126; i++) {
        Inode *inode = &temp_superblock.inode[i];
        if ((inode->used_size & 0x80) == 0) { // Free inode
            if (inode->used_size != 0 || inode->start_block != 0 || inode->dir_parent != 0) {
                fprintf(stderr, "Error: File system in %s is inconsistent (error code: 1)\n", new_disk_name);
                fclose(disk_file_temp);
                return;
            }
        }
    }

    // Consistency Check 2: Valid range for start block and size for in-use files
    for (int i = 0; i < 126; i++) {
        Inode *inode = &temp_superblock.inode[i];
        if ((inode->used_size & 0x80) && ((inode->used_size & 0x7F) > 0)) { // In-use file inode (size > 0 indicates a file)
            int size = inode->used_size & 0x7F; // Extract size
            if (inode->start_block < 1 || inode->start_block > 127 ||
                (inode->start_block + size - 1) > 127) {
                fprintf(stderr, "Error: File system in %s is inconsistent (error code: 2)\n", new_disk_name);
                fclose(disk_file_temp);
                return;
            }
        }
    }

    // Consistency Check 3: Directories must have size and start_block = 0
    for (int i = 0; i < 126; i++) {
        Inode *inode = &temp_superblock.inode[i];
        if ((inode->used_size & 0x80) && (inode->dir_parent & 0x80)) { // In-use directory inode
            if ((inode->used_size & 0x7F) != 0 || inode->start_block != 0) {
                fprintf(stderr, "Error: File system in %s is inconsistent (error code: 3)\n", new_disk_name);
                fclose(disk_file_temp);
                return;
            }
        }
    }

    // Consistency Check 4: Parent inode index validity
    for (int i = 0; i < 126; i++) {
        Inode *inode = &temp_superblock.inode[i];
        if (inode->used_size & 0x80) { // In-use inode
            int parent_index = inode->dir_parent & 0x7F; // Extract parent index
            if (parent_index == 126) {
                fprintf(stderr, "Error: File system in %s is inconsistent (error code: 4)\n", new_disk_name);
                fclose(disk_file_temp);
                return;
            }
            if (parent_index >= 0 && parent_index <= 125) {
                Inode *parent_inode = &temp_superblock.inode[parent_index];
                if (!(parent_inode->used_size & 0x80) || !(parent_inode->dir_parent & 0x80)) { // Parent must be in use and a directory
                    fprintf(stderr, "Error: File system in %s is inconsistent (error code: 4)\n", new_disk_name);
                    fclose(disk_file_temp);
                    return;
                }
            }
        }
    }

    // Consistency Check 5: Unique names within each directory
    for (int i = 0; i < 126; i++) {
        Inode *inode1 = &temp_superblock.inode[i];
        if (inode1->used_size & 0x80) { // In-use inode
            for (int j = i + 1; j < 126; j++) {
                Inode *inode2 = &temp_superblock.inode[j];
                if (inode2->used_size & 0x80 && inode1->dir_parent == inode2->dir_parent &&
                    strncmp(inode1->name, inode2->name, 5) == 0) {
                    fprintf(stderr, "Error: File system in %s is inconsistent (error code: 5)\n", new_disk_name);
                    fclose(disk_file_temp);
                    return;
                }
            }
        }
    }

    // Consistency Check 6: Block allocation in free-space list
    char *block_map = returnBinary(temp_superblock.free_block_list);
    for (int block = 1; block < 128; block++) { // Exclude superblock (block 0)
        int block_in_use = 0;
        for (int i = 0; i < 126; i++) {
            Inode *inode = &temp_superblock.inode[i];
            if (inode->used_size & 0x80) { // In-use inode
                int start = inode->start_block;
                int size = inode->used_size & 0x7F;
                if (block >= start && block < start + size) {
                    block_in_use++;
                }
            }
        }
        if ((block_map[block] == '1' && block_in_use != 1) ||
            (block_map[block] == '0' && block_in_use != 0)) {
            fprintf(stderr, "Error: File system in %s is inconsistent (error code: 6)\n", new_disk_name);
            free(block_map);
            fclose(disk_file_temp);
            return;
        }
    }
    free(block_map);

    // If all checks pass, mount the file system
    if (disk_file) fclose(disk_file);
    disk_file = disk_file_temp;
    superblock = temp_superblock;
    current_working_dir = 127; // Root directory
}

void fs_create(char name[5], int size) {
    // Check if filesystem is mounted
    if (!disk_file) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    // Find a free inode
    int free_inode_index = -1;
    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) == 0) { // MSB not set means free
            free_inode_index = i;
            break;
        }
    }
    if (free_inode_index == -1) {
        fprintf(stderr, "Error: No free inode available to create '%.*s'.\n", 5, name);
        return;
    }

    // Validate name and check for duplicates in the current directory
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        fprintf(stderr, "Error: File or directory '%s' already exists\n", name);
        return;
    }

    for (int i = 0; i < 126; i++) {
        Inode *inode = &superblock.inode[i];
        if ((inode->used_size & 0x80) && inode->dir_parent == current_working_dir &&
            strncmp(inode->name, name, 5) == 0) {
            fprintf(stderr, "Error: File or directory '%.*s' already exists.\n", 5, name);
            return;
        }
    }

    Inode *new_inode = &superblock.inode[free_inode_index];
    memset(new_inode, 0, sizeof(Inode));
    strncpy(new_inode->name, name, 5);

    // If creating a directory
    if (size == 0) {
        // Set MSB of used_size to indicate in-use, size=0 means directory
        new_inode->used_size = 0x80;
        new_inode->dir_parent = current_working_dir;

        // Write the updated superblock to disk
        fseek(disk_file, 0, SEEK_SET);
        if (fwrite(&superblock, sizeof(Superblock), 1, disk_file) != 1) {
            perror("fwrite failed");
        }
        fflush(disk_file);
        return;
    }

    // Convert the free_block_list to a binary string for easy scanning
    char *strInBinary = returnBinary(superblock.free_block_list);

    int first = 1;        // We start checking from block 1 because block 0 is reserved
    int last = first+size;
    int isThereBlocksAvailable = 0;

    while (last <= 128) {
        int firstRef = first;
        while (firstRef < last) {
            if (strInBinary[firstRef] != '0') break;
            firstRef++;
        }
        if (firstRef == last) {
            isThereBlocksAvailable = 1;
            break;
        } else {
            first++;
            last++;
        }
    }

    free(strInBinary);

    if (!isThereBlocksAvailable) {
        fprintf(stderr, "Error: Cannot allocate %d blocks on disk.\n", size);
        return;
    }

    // Mark the found range as used
    setBitInRange(superblock.free_block_list, first, last-1, 1);

    // Initialize the inode for the file
    new_inode->start_block = first;
    new_inode->dir_parent = current_working_dir;
    // Set MSB of used_size to indicate in-use, and the lower 7 bits to file size
    new_inode->used_size = 0x80 | (size & 0x7F);

    // Save changes to disk
    fseek(disk_file, 0, SEEK_SET);
    if (fwrite(&superblock, sizeof(Superblock), 1, disk_file) != 1) {
        perror("fwrite failed");
    }
    fflush(disk_file);
}

void fs_delete(char name[5]) {
    if (!disk_file) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    // Locate the inode for the file in the current directory
    for (int i = 0; i < 126; i++) {
        Inode *inode = &superblock.inode[i];
        if ((inode->used_size & 0x80) && inode->dir_parent == current_working_dir) {
            if (memcmp(inode->name, name, 5) == 0) {
                // File found, proceed with deletion
                int start_block = inode->start_block;
                int size = inode->used_size & 0x7F; // Get size in blocks

                // Overwrite data in the blocks
                char empty_block[1024] = {0}; // Empty block data
                for (int j = 0; j < size; j++) {
                    int block_to_clear = start_block + j;

                    // Overwrite block data with zeros
                    fseek(disk_file, block_to_clear * 1024, SEEK_SET);
                    fwrite(empty_block, 1, 1024, disk_file);

                    // Mark block as free
                    superblock.free_block_list[block_to_clear / 8] &=
                        ~(1 << (7 - (block_to_clear % 8)));
                }

                // Clear the inode
                memset(inode, 0, sizeof(Inode));

                // Save updated superblock to disk
                fseek(disk_file, 0, SEEK_SET);
                if (fwrite(&superblock, sizeof(Superblock), 1, disk_file) != 1) {
                    perror("fwrite failed");
                }
                fflush(disk_file);

                return;
            }
        }
    }

    // If no matching file is found, print an error
    fprintf(stderr, "Error: File or directory '%.*s' does not exist\n", 5, name);
}


void fs_read(char name[5], int block_num) {
    // Check if a file system is mounted
    if (!disk_file) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    // Locate the inode for the specified file
    Inode *target_inode = NULL;
    for (int i = 0; i < 126; i++) {
        Inode *inode = &superblock.inode[i];
        if ((inode->used_size & 0x80) && strncmp(inode->name, name, 5) == 0) {
            target_inode = inode;
            break;
        }
    }

    if (!target_inode) {
        fprintf(stderr, "Error: File %s does not exist.\n", name);
        return;
    }

    // Validate block number
    int file_size = target_inode->used_size & 0x7F; // Extract size from used_size
    if (block_num >= file_size) {
        fprintf(stderr, "Error: Block number %d exceeds file size (%d blocks).\n", block_num, file_size);
        return;
    }

    // Calculate the disk block to read from
    int start_block = target_inode->start_block;
    int disk_block = start_block + block_num;

    // Read data from the specified block
    char block_data[1024] = {0};
    fseek(disk_file, disk_block * 1024, SEEK_SET);
    if (fread(block_data, 1024, 1, disk_file) != 1) {
        fprintf(stderr, "Error: Failed to read block %d of file %s.\n", block_num, name);
        return;
    }
}

void fs_write(char name[5], int block_num) {
    if (!disk_file) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    Inode *target_inode = NULL;
    for (int i = 0; i < 126; i++) {
        Inode *inode = &superblock.inode[i];
        if ((inode->used_size & 0x80) && memcmp(inode->name, name, 5) == 0) {
            target_inode = inode;
            break;
        }
    }

    if (!target_inode) {
        fprintf(stderr, "Error: File '%.*s' not found.\n", 5, name);
        return;
    }

    int file_size = target_inode->used_size & 0x7F;
    if (block_num >= file_size) {
        fprintf(stderr, "Error: Block number %d exceeds file size (%d blocks).\n", block_num, file_size);
        return;
    }

    int disk_block = target_inode->start_block + block_num;

    fseek(disk_file, disk_block * 1024, SEEK_SET);
    if (fwrite(buffer, 1024, 1, disk_file) != 1) {
        fprintf(stderr, "Error: Failed to write to block %d.\n", block_num);
    }
    fflush(disk_file);
}

void fs_buff(char buff[1024]) {
    memset(buffer, 0, 1024);
    memcpy(buffer, buff, 1024);
}

int calculate_directory_size(int dir_inode) {
        int entry_count = 2; // Start with 2 for '.' and '..'

        for (int i = 0; i < 126; i++) {
            Inode *inode = &superblock.inode[i];
            if ((inode->used_size & 0x80) && inode->dir_parent == dir_inode) {
                entry_count++; // Count each valid entry
            }
        }

        return entry_count;
}

void fs_ls(void) {
    // Ensure a file system is mounted
    if (!disk_file) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    // List current and parent directories
    int current_dir_size = calculate_directory_size(current_working_dir);
    printf(".       %d\n", current_dir_size);

    if (current_working_dir == 127) { // Root directory special case
        printf("..      %d\n", current_dir_size);
    } else {
        int parent_dir_inode = superblock.inode[current_working_dir].dir_parent;
        int parent_dir_size = calculate_directory_size(parent_dir_inode);
        printf("..      %d\n", parent_dir_size);
    }

    // List all entries in the current directory
    for (int i = 0; i < 126; i++) {
        Inode *inode = &superblock.inode[i];
        if ((inode->used_size & 0x80) && inode->dir_parent == current_working_dir) {
            int entry_size = inode->used_size & 0x7F; // Extract size in blocks
            if (entry_size > 0) {
                printf("%-5.5s %3d KB\n", inode->name, entry_size);
            } else {
                int sub_dir_size = calculate_directory_size(i);
                printf("%-5.5s %3d\n", inode->name, sub_dir_size);
            }
        }
    }
}


void fs_resize(char name[5], int new_size) {
    // Ensure a file system is mounted
    if (!disk_file) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    // Locate the inode for the file in the current directory
    Inode *target_inode = NULL;
    for (int i = 0; i < 126; i++) {
        Inode *inode = &superblock.inode[i];
        if ((inode->used_size & 0x80) && memcmp(inode->name, name, 5) == 0 && inode->dir_parent == current_working_dir) {
            target_inode = inode;
            break;
        }
    }

    // Handle file not found or is a directory
    if (!target_inode || (target_inode->used_size & 0x7F) == 0) {
        fprintf(stderr, "Error: File %.*s does not exist\n", 5, name);
        return;
    }

    int current_size = target_inode->used_size & 0x7F; // Current size in blocks
    int start_block = target_inode->start_block;      // Start block of the file

    if (new_size < current_size) {
        // Shrink the file: Free and zero out unused blocks
        for (int i = start_block + new_size; i < start_block + current_size; i++) {
            superblock.free_block_list[i / 8] &= ~(1 << (7 - (i % 8))); // Mark block as free
            char empty_block[1024] = {0};
            fseek(disk_file, i * 1024, SEEK_SET);
            fwrite(empty_block, 1, 1024, disk_file); // Zero out block
        }
        target_inode->used_size = (target_inode->used_size & 0x80) | new_size; // Update size
    } else if (new_size > current_size) {
        // Expand the file
        int additional_blocks = new_size - current_size;
        int free_blocks_found = 0;
        int move_required = 0;

        // Check for contiguous free blocks after current file
        for (int i = start_block + current_size; i < 128; i++) {
            if (!(superblock.free_block_list[i / 8] & (1 << (7 - (i % 8))))) {
                free_blocks_found++;
                if (free_blocks_found == additional_blocks) break;
            } else {
                free_blocks_found = 0;
                move_required = 1;
                break;
            }
        }

        if (free_blocks_found == additional_blocks && !move_required) {
            // Enough free space after current file
            for (int i = start_block + current_size; i < start_block + new_size; i++) {
                superblock.free_block_list[i / 8] |= (1 << (7 - (i % 8))); // Mark block as used
            }
            target_inode->used_size = (target_inode->used_size & 0x80) | new_size; // Update size
        } else {
            // Try moving the file to a new location
            int new_start_block = -1;
            free_blocks_found = 0;

            for (int i = 1; i < 128; i++) { // Start from block 1
                if (!(superblock.free_block_list[i / 8] & (1 << (7 - (i % 8))))) {
                    if (free_blocks_found == 0) new_start_block = i;
                    free_blocks_found++;
                    if (free_blocks_found == new_size) break;
                } else {
                    free_blocks_found = 0;
                }
            }

            if (free_blocks_found == new_size) {
                // Move file to new location
                char temp_block[1024];
                for (int i = 0; i < current_size; i++) {
                    fseek(disk_file, (start_block + i) * 1024, SEEK_SET);
                    fread(temp_block, 1, 1024, disk_file); // Read old block

                    fseek(disk_file, (new_start_block + i) * 1024, SEEK_SET);
                    fwrite(temp_block, 1, 1024, disk_file); // Write to new block
                }

                // Zero out old blocks and mark as free
                for (int i = start_block; i < start_block + current_size; i++) {
                    superblock.free_block_list[i / 8] &= ~(1 << (7 - (i % 8)));
                    char empty_block[1024] = {0};
                    fseek(disk_file, i * 1024, SEEK_SET);
                    fwrite(empty_block, 1, 1024, disk_file);
                }

                // Mark new blocks as used
                for (int i = 0; i < new_size; i++) {
                    superblock.free_block_list[(new_start_block + i) / 8] |= (1 << (7 - ((new_start_block + i) % 8)));
                }

                // Update inode
                target_inode->start_block = new_start_block;
                target_inode->used_size = (target_inode->used_size & 0x80) | new_size;
            } else {
                // Not enough contiguous free space
                fprintf(stderr, "Error: File %.*s cannot expand to size %d\n", 5, name, new_size);
                return;
            }
        }
    }

    // Save updated superblock to disk
    fseek(disk_file, 0, SEEK_SET);
    fwrite(&superblock, sizeof(Superblock), 1, disk_file);
    fflush(disk_file);
}

void fs_defrag(void) {
    if (!disk_file) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    char *strInBinary = returnBinary(superblock.free_block_list); // Convert free block list to binary representation
    int first = 1;
    int next_start = 1;

    while (first < 128) { // Iterate through all blocks (except reserved block 0)
        if (strInBinary[first] == '1') { // If the block is in use
            int inode_index = -1;

            // Find the inode corresponding to this block
            for (int x = 0; x < 126; x++) {
                if (superblock.inode[x].start_block == first) {
                    inode_index = x;
                    break;
                }
            }

            if (inode_index == -1) {
                fprintf(stderr, "Error: Inconsistent state. No inode found for block %d.\n", first);
                free(strInBinary);
                return;
            }

            int used_size = superblock.inode[inode_index].used_size & 0x7F; // Extract size of file

            // Clear the old block range in the free block list
            setBitInRange(superblock.free_block_list, first, first + used_size - 1, 0);

            // Update inode to point to the new start block
            superblock.inode[inode_index].start_block = next_start;

            // Mark the new range as used
            setBitInRange(superblock.free_block_list, next_start, next_start + used_size - 1, 1);

            // Relocate the data
            uint8_t hold_copy[1024 * used_size];
            uint8_t freed[1024 * used_size];
            memset(freed, 0, sizeof(freed)); // Initialize freed block buffer

            // Read data from the old location
            fseek(disk_file, first * 1024, SEEK_SET);
            if (fread(hold_copy, sizeof(hold_copy), 1, disk_file) != 1) {
                fprintf(stderr, "Error: Failed to read data from block %d.\n", first);
            }

            // Overwrite old blocks with zeros
            fseek(disk_file, first * 1024, SEEK_SET);
            fwrite(freed, sizeof(freed), 1, disk_file);

            // Write data to the new location
            fseek(disk_file, next_start * 1024, SEEK_SET);
            fwrite(hold_copy, sizeof(hold_copy), 1, disk_file);

            // Advance the pointers
            next_start += used_size;
            first += used_size;
        } else {
            first += 1;
        }
    }

    free(strInBinary);

    // Save the updated free block list and inode table to disk
    fseek(disk_file, 0, SEEK_SET);
    if (fwrite(&superblock, sizeof(Superblock), 1, disk_file) != 1) {
        fprintf(stderr, "Error: Failed to write updated superblock to disk.\n");
    }
    fflush(disk_file);
}

void fs_cd(char name[5]) {
    // Handle special cases for "." and ".."
    if (strcmp(name, ".") == 0) {
        return; // Stay in the current directory
    }

    if (strcmp(name, "..") == 0) {
        if (current_working_dir == 127) { // Root directory special case
            fprintf(stderr, "Error: Already at root directory.\n");
            return;
        }
        // Move to the parent directory
        current_working_dir = superblock.inode[current_working_dir].dir_parent;
        return;
    }

    // Search for the specified directory in the current working directory
    for (int i = 0; i < 126; i++) {
        Inode *inode = &superblock.inode[i];

        // Check if the inode is used, belongs to the current directory, and matches the name
        if ((inode->used_size & 0x80) && inode->dir_parent == current_working_dir &&
            strncmp(inode->name, name, 5) == 0) {

            // Ensure it's a directory (size == 0 for directories)
            if ((inode->used_size & 0x7F) == 0) {
                current_working_dir = i; // Change to the specified directory
                return;
            } else {
                // The entry exists but is not a directory
                fprintf(stderr, "Error: %.*s is not a directory.\n", 5, name);
                return;
            }
        }
    }

    // If no matching directory is found
    fprintf(stderr, "Error: Directory '%.*s' does not exist\n", 5, name);
}

void trim_whitespace(char *str) {
    char *end;

    // Trim leading space
    while (isspace((unsigned char)*str)) str++;

    // All spaces
    if (*str == 0) return;

    // Trim trailing space
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;

    // Write null terminator
    *(end + 1) = '\0';
}


// Main function
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <input_file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    FILE *input_file = fopen(argv[1], "r");
    if (!input_file) {
        perror("Error opening input file");
        return EXIT_FAILURE;
    }

    char command[256];
    int line_number = 0;

    while (fgets(command, sizeof(command), input_file)) {
        line_number++;
        // Remove trailing newline character
        command[strcspn(command, "\n")] = 0;

        // Skip empty lines
        if (strlen(command) == 0) continue;

        // Parse and execute commands
        if (strncmp(command, "M ", 2) == 0) 
        {
            char disk_name[128];
            if (sscanf(command + 2, "%127s", disk_name) == 1) {
                // printf("Attempting to mount disk: %s\n", disk_name); // Print the disk name
                fs_mount(disk_name);
            } else {
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_number);
            }
        } 
        else if (strncmp(command, "C ", 2) == 0) {
            char name[5]; // No null termination needed
            int size;

            // Parse name and size
            if (sscanf(command + 2, "%5s %d", name, &size) == 2) {
                // Validate the name length (should be exactly 5 characters or less)
                if (strlen(name) > 5 || size > 127 || size < 0) {
                    fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_number);
                }
                else
                {
                    fs_create(name, size);
                }
            } 
            else 
            {
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_number);
            }
        }
        else if (strncmp(command, "D ", 2) == 0) {
            char name[5]; // Temporary buffer for a 5-character name + null terminator
            int name_length = 0;

            // Parse the name and ensure it doesn't exceed 5 characters
            if (sscanf(command + 2, "%5s%n", name, &name_length) == 1) {
                // Check for additional characters beyond 5
                if (name_length < strlen(command + 2)) {
                    fprintf(stderr, "Error: File name '%s' exceeds the maximum length of 5 characters.\n", command + 2);
                } else {
                    // printf("%s\n", name); // Print the parsed name (up to 5 characters)
                    fs_delete(name); // Call the delete function with the valid name
                }
            } else {
                fprintf(stderr, "Command Error: %s, line %d\n", argv[1], line_number);
            }
        }
        else if (strncmp(command, "R ", 2) == 0) {
            char name[5];
            int block_num;
            if (sscanf(command + 2, "%5s %d", name, &block_num) == 2) {
                fs_read(name, block_num);
            } else {
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_number);
            }
        } 
        else if (strncmp(command, "W ", 2) == 0) {
            char name[5];
            int block_num;
            if (sscanf(command + 2, "%5s %d", name, &block_num) == 2) {
                fs_write(name, block_num);
            } else {
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_number);
            }
        } 
        else if (strncmp(command, "B ", 2) == 0) {
            char buff[1024] = {0}; // Initialize buffer with zeros
            size_t input_length = strlen(command + 2); // Calculate the length of the input

            // Check if the input exceeds 1024 characters
            if (input_length > 1024) {
                fprintf(stderr, "Error: Buffer exceeds maximum size of 1024 characters.\n");
            } else {
                // Copy the input into the buffer and pad with zeros if necessary
                strncpy(buff, command + 2, 1024);
                fs_buff(buff); // Pass the validated buffer to fs_buff
            }
        }

        else if (strncmp(command, "L", 1) == 0) {
            fs_ls();
        } 
        else if (strncmp(command, "E ", 2) == 0) {
            char name[5];
            int new_size;
            if (sscanf(command + 2, "%5s %d", name, &new_size) == 2) {
                fs_resize(name, new_size);
            } else {
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_number);
            }
        } 
        else if (strncmp(command, "O", 1) == 0) {
            fs_defrag();
        } 
        else if (strncmp(command, "Y ", 2) == 0) {
            char dir_name[5];
            if (sscanf(command + 2, "%5s", dir_name) == 1) {
                // printf("%s meow\n", dir_name);
                fs_cd(dir_name);
            } else {
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_number);
            }
        } 
        else {
            fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_number);
        }
    }

    fclose(input_file);
    return EXIT_SUCCESS;
}