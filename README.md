
# Filesystem Manager

## Introduction

This project implements a basic filesystem manager that allows users to create, list, delete files, and manage file permissions in a simulated filesystem stored in a single file (`filesystem.img`).

## Compilation

To compile the program, use the following command:

```bash
gcc -o fs_manager filesystem_manager.c
```

## Usage

### Initialize the Filesystem

Before using any commands, initialize the filesystem:

```bash
./fs_manager init
```

This creates a `filesystem.img` file that acts as your virtual filesystem.

### Create a File

To create a new file in the filesystem with specific permissions:

```bash
./fs_manager create <filename> <permissions>
```

Example:

```bash
./fs_manager create myfile.txt 644
```

### List Files

To view all files in the filesystem:

```bash
./fs_manager list
```

### Delete a File

To delete a file from the filesystem:

```bash
./fs_manager delete <filename>
```

Example:

```bash
./fs_manager delete myfile.txt
```

### Change File Permissions

To modify the permissions of a file:

```bash
./fs_manager chmod <filename> <new_permissions>
```

Example:

```bash
./fs_manager chmod myfile.txt 755
```

## Notes

- The filesystem must be initialized before using it.
- Permissions follow UNIX-style (e.g., `644`, `755`).
- Currently, the system does not support directories or file content storage.
- Future enhancements may include file moving, copying, and directory support.

## Troubleshooting

### "File not found" error?

- Ensure you typed the correct filename.
- Run `./fs_manager list` to check existing files.

### "Filesystem not initialized" error?

- Run `./fs_manager init` first.

### Compilation issues?

- Ensure GCC is installed. You can install it using: 

```bash
sudo apt install gcc
```

## License

This project is released under the MIT License.
