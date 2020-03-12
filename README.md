# CS179F Senior Design Project
David Nguyen

dnguy117@ucr.edu 

## XV6 Log Checksum

The current version of xv6's file system utilizes a log to allow for concurrent file system calls. It also has additional functionality with crashes and power loss. The log blocks are one of the most important and most used data blocks. However, it lacks the security and protection against errors that can occur on the disk or from data transfer. 

This project adds a checksum to the log and adds protection to data integrity. After a file system call, a checksum is calculated and stored in both memory and disk. Before commiting the data to the disk, the log will recalculate the checksum and compare it with the checksum in memory. If there is a mismatch, it will cancel the system call and prevent an incorrect data write. In case of crash and power loss, the log will call a recovery function on boot-up and use the checksum in disk to check for errors.

## How To Run

1. Clone directory to UCR's sledge server with

> git clone https://github.com/dnguy117/cs179f_senior_design.git cs179f

2. From sledge's console type

> cd cs179f/xv6

> make

> make qemu-nox
