# Mini Filesystem (mini_fs)

## Introduction

This project implements a complete in-memory filesystem with:
- File and directory operations
- Permission management
- Symbolic and hard links
- Paging system for file storage
- Multi-user support
- Backup/restore functionality

## Building the Project

To compile the program, use the provided Makefile:

```bash
make
```

This will create the `mini_fs` executable.

## Usage

Run the filesystem manager with:

```bash
./mini_fs
```

### Available Commands

Once in the interactive shell, you can use the ```help()``` command


## Troubleshooting

### Compilation Issues
- Ensure you have required dependencies:
  ```bash
  sudo apt install build-essential
  ```
- If you get pthread errors, try:
  ```bash
  make clean
  make
  ```

### Runtime Issues
- "Permission denied" errors: Check file permissions and owner
- "File not found": Verify path and current directory
- "Directory not empty": Remove contents before deleting directory

## Implementation Details

- Uses in-memory storage with simulated paging
- Supports multiple users with authentication
- Implements full path resolution
- Maintains file metadata including:
  - Permissions
  - Ownership
  - Timestamps
  - Link counts

## Future Enhancements

- Persistent storage to disk
- Extended attribute support
- File compression
- Network sharing capabilities

## License

This project is released under the MIT License.
