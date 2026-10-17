# MOCApp script reference

MOCApp is a small line-oriented interpreted language for Michael OC. Script files are ordinary text files ending in .mapp, stored on the persistent MOCFS1 virtual disk. Select a script in the file manager and press Enter to execute it. Other files open in the text editor. Press Escape to return to the file manager; F1 stops a running script.

The interpreter currently runs in kernel context. Scripts do not have a separate address space, privilege boundary, or system-call sandbox. Do not treat the interpreter as safe for untrusted programs.

## Limits and source lines

Each script is limited to 16 KiB and 256 source lines. Commands are case-insensitive, while arguments are passed as written. Each command occupies one line. Blank lines and lines beginning with # after leading spaces are skipped during execution, but they still count when using numeric branch targets. The target line for JUMP and IFKEY is zero-based.

The runtime executes up to 128 instructions per interpreter pass. A script containing an endless unconditional jump loop is stopped when that budget is exhausted. WAITKEY pauses execution until a key-down event is received.

## Instructions

| Instruction | Description |
|---|---|
| PRINT text | Add the remainder of the line to the output window and serial diagnostics. |
| CLEAR | Clear the output window. |
| WAITKEY | Pause and wait for the next key-down event. |
| IFKEY a 6 | If the last printable key equals a, continue at zero-based source line 6; otherwise continue at the next line. |
| JUMP 4 | Continue at zero-based source line 4. |
| SET 10 | Set the integer register to 10. |
| ADD 2 | Add 2 to the integer register. |
| SUB 1 | Subtract 1 from the integer register. |
| PRINTNUM | Print the integer register's decimal value. |
| LS | Print the names in the file manager's current directory. |
| READ filename.txt | Print a text file from the current directory. Files larger than 511 bytes are refused. |
| WRITE filename.txt some text | Create or replace a file in the current directory with the remainder of the line as its contents. |
| EXIT | Stop the script and return to the file manager. |

Unknown commands and malformed numeric arguments stop the script with an error message. A missing or oversized file passed to READ prints a failure line. WRITE prints whether the file operation succeeded.

## Example

    # arithmetic
    SET 40
    ADD 2
    PRINTNUM
    WRITE result.txt HELLO FROM MOCAPP
    READ result.txt
    WAITKEY
    IFKEY a 9
    PRINT WRONG KEY
    JUMP 10
    PRINT KEY RECEIVED
    EXIT

Branch targets use zero-based source lines. Blank and comment lines count as source lines, so update branch targets whenever lines are inserted or removed.

## Storage behavior

LS, READ, and WRITE act in the file manager's current directory. Files are stored on the persistent QEMU disk. WRITE replaces the contents of an existing file. MOCFS1 v2 uses a checksummed write-ahead journal for complete directory-table and bitmap updates and replays a committed metadata update during mount. Format v1 images remain supported without journaling. The filesystem still uses a fixed-size directory table and contiguous extents; in-place file-data writes and operations spanning several independent metadata transactions are not fully atomic.

The fresh-disk image created by scripts/create_disk_image.py contains welcome.mapp as an integration-test example. Deleting the disk image and creating a new one resets persistent data.

## Installing a script created on the host

Quit QEMU before modifying the disk image. From the repository root, run:

    python3 scripts/install_mocapp.py --disk build/michael-oc-disk.img --source ./hello.mapp
    make run

The script is added to the guest filesystem root and can then be selected and run in the guest file manager. The disk installer validates the MOCFS1 superblock, metadata layout, directory entries, sector bitmap, existing file extents, script encoding, extension, and size/line caps. Duplicate names are rejected. Updates are written to a temporary disk image and atomically renamed into place, so a reported failure leaves the original disk image unchanged. Keep QEMU shut down while the host tool edits its disk file.
