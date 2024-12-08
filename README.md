# - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
# Name : Arib Amin
# SID  : 1707860
# CCID : arib1
# - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

UNIX File System Simulator

Overview:
This project is a program that simulates a basic UNIX-like file system. It allows you to create, delete, read, write, resize, and move files and directories inside a virtual disk. The virtual disk is a file that acts like storage, and the program reads and writes data to it just like an operating system would.

The program uses a data structure called a superblock to keep track of files, directories, and free space. Commands are read from a text file, and the program performs the operations one by one.

System Calls:
- open: Used to open the virtual disk file for reading and writing.
- read: Reads data from the virtual disk into memory.
- write: Writes data from memory to the virtual disk.
- lseek: Moves the file pointer to specific positions for reading/writing.
- close: Closes the file descriptor for the virtual disk.

Superblock:
The first block in the virtual disk.
Keeps track of free space and stores information about all files and directories.

Inodes:
These store details about each file or directory, like its name, size, and location on the disk.

Data Blocks:
Files are stored in data blocks (chunks of 1 KB).
Each file uses a set of continuous (next to each other) blocks.

Testing:
- Created files and directories of varying sizes to verify space allocation.
- Deleted files and directories to ensure space is freed correctly.
- Resized files to confirm data integrity and proper reallocation.
- Defragmented the disk and checked that free space became continuous.
- Simulated navigation between directories and validated paths.

Commands:
You control the file system using commands in an input file. Each line in the file is a command, like creating or deleting a file.

Features
1. Mounting: Load the virtual disk and prepare it for use.
2. Creating Files/Directories: Add new files or directories with a specific size.
3. Deleting: Remove files or directories and free up space.
4. Reading and Writing: Read data from or write data to files.
5. Resizing: Increase or decrease the size of a file.
6. Defragmenting: Clean up the disk to make free space continuous.
7. Navigation: Move between directories, like in a real file system.

How to Use the Program
Run this command to compile the program:
make

Sources:
- Linux Manual Pages (https://man7.org/linux/man-pages/)
- Operating System Concepts by Silberschatz, Galvin, and Gagne