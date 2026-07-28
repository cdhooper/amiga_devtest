# Amiga block device test utility (devtest)

This utility interacts directly with an Amiga block device driver to test
the driver and physical device. There are several operation types that the
utility can perform:
   * Device probe
   * Performance
   * Device Geometry
   * Data integrity
   * Packet support
   * Individual commands

## 1. Device probe

It can probe all targets and logical units of a device and report which
targets are responding. Example:
```
    9.OS322:> devtest -p scsi.device
      0 ZULUSCSI HARDDRIVE        1.1  Disk     512  2147 MB
      5 ZULUSCSI CDROM            1.1  CDROM   2048    77 MB
```

You can also probe a specific target id. If that target id is not present,
devtest will exit with a non-zero code. Example:
```
    9.OS322:> devtest -p scsi.device 0
      0 ZULUSCSI HARDDRIVE        1.1  Disk     512  2147 MB
    9.OS322:> echo $RC
    0
    9.OS322:> devtest -p scsi.device 1
    Open scsi.device Unit 1: Fail 32 TDERR_BadUnitNum
    9.OS322:> echo $RC
    1
```

## 2. Performance
You can measure device read and write performance in the system, using
different Amiga memory types (default is fastmem).
**WARNING: The -d option enables the write benchmark, which is destructive to any data on the drive!**
Examples:
```
    9.OS322:> devtest a4091.device 1 -bd
    Destructive test - are you sure? (y/n) y
    Test a4091.device 1 with MB RAM
    read  512 KB xfers          6242 KB/sec (13% CPU)
    read  128 KB xfers          5781 KB/sec (27% CPU)
    read   32 KB xfers          4449 KB/sec (60% CPU)
    write 512 KB xfers          4099 KB/sec (9% CPU)
    write 128 KB xfers          3358 KB/sec (16% CPU)
    write  32 KB xfers          3064 KB/sec (41% CPU)
```

Testing with Zorro memory
```
    9.OS322:> devtest a4091.device 1 -bdyC -m zorro
    Test a4091.device 1 with Zorro III RAM
    read  512 KB xfers          7377 KB/sec
    read  128 KB xfers          7063 KB/sec
    read   32 KB xfers          5915 KB/sec
    write 512 KB xfers          7350 KB/sec
    write 128 KB xfers          5893 KB/sec
    write  32 KB xfers          4031 KB/sec
```

In the first example, CPU utilization is reported during the test.
Determining CPU utilization has a measured effect on performance,
in the range of 1% to 5%. The -C option can turn off this feature.
While benchmarking a fast device, that device may consume considerable
CPU bus bandwidth. This would increase CPU utilization, as the CPU
needs to wait for access to the bus.

Normally, the destructive test will still attempt to read the data
beforehand and write it back after completing the test. Adding a
second `-d` option will skip the read and restore. This will speed
up the test, but not affect the reported performance.

Sometimes there is a type of memory which is at a specific range
of addresses, and you want to run the benchmark soecifically against
that memoory. You should use an address in that range which is not
currently allocated. With the -m option, you may both discover free
memory and specify the exact address to use. Use the `-m -` argument
to show memory blocks in the free list.
```
    9.OS322:> devtest -m -
    Coprocessor RAM at 0x8000000 size=0x8000000
      0x86b6220 0x400
      0x86d1ef0 0x230
      0x87892c0 0x7876d40
    MB RAM at 0x7000000 size=0x1000000
      0x7000020 0xffffe0
    Zorro III RAM at 0x40000000 size=0x10000000
      0x40000020 0xfffffe0
    Zorro III RAM at 0x60000000 size=0x10000000
      0x60000020 0xfffffe0
    Zorro II RAM at 0x2000000 size=0x2000000
      0x200020 0x1fffe0
    Chip RAM at 0x004000 size=0x1fc000
      0x0202d8 0x1dfd28
```

Use the ```-m <address>``` option to specify a particular block or type
of memory.
Examples:
```
    9.OS322:> devtest a4091.device 1 -bd -m 0x60000020
```
```
    9.OS322:> devtest a4091.device 1 -bd -m zorro2
```

Adding a second `-b` option will cause devtest to also measure
latency of a variety of packets. Example:
```
    9.OS322:> devtest -bbdy a4091.device 1
    Test a4091.device 1 with MB RAM
    read  512 KB xfers          6166 KB/sec (14% CPU)
    read  128 KB xfers          5726 KB/sec (24% CPU)
    read   32 KB xfers          4444 KB/sec (60% CPU)
    write 512 KB xfers          4743 KB/sec (10% CPU)
    write 128 KB xfers          2449 KB/sec (12% CPU)
    write  32 KB xfers          3052 KB/sec (41% CPU)
    OpenDevice / CloseDevice    5.053 ms
    OpenDevice multiple         0.024 ms
    CloseDevice multiple        0.013 ms
    TD_GETGEOMETRY sequential   5.008 ms
    TD_GETGEOMETRY parallel     6.054 ms
    TD_CHANGENUM                0.013 ms
    TD_CHANGENUM quick          0.012 ms
    CMD_INVALID                 0.013 ms
    CMD_START                   2.077 ms
    CMD_READ butterfly average  3.034 ms
    CMD_READ butterfly far      2.099 ms
    CMD_READ butterfly constant 5.012 ms
    CMD_READ sequential         3.085 ms
    CMD_READ parallel           3.067 ms
    HD_SCSICMD read sequential  3.086 ms
    HD_SCSICMD read parallel    3.069 ms
    CMD_WRITE sequential        5.013 ms
    CMD_WRITE parallel          5.005 ms
    HD_SCSICMD write sequential 5.027 ms
    HD_SCSICMD write parallel   4.084 ms
```

## 3. Device Geometry
On the Amiga, there are multiple methods to acquire a drive's physical
geometry, including direct SCSI commands. These methods are reported
by devtest.
```
    9.OS322:> devtest -g a4091.device 1
                     SSize TotalSectors    Cyl Head  Sect DType Removable
    TD_GETGEOMETRY     512      1048576   8192   16     8  0x00 No
    Inquiry                                                0x00 No
    READ_CAPACITY_10   512      1048576
    READ_CAPACITY_16     -            -                    Fail 52  ERROR_SENSE_CODE
    Read-to capacity   512      1048576
    Mode Page 0x03     512                             63
    Mode Page 0x04                          65  255
```

TD_GETGEOMETRY is implemented by the Amiga device driver. It is the
responsibility of the driver to acquire details from the device to fill
the DriveGeometry structure. It is known that Commodore's scsi.device
for IDE is sometimes not able to acquire geometry information from CF
drives, and may fail this command.

Inquiry is a SCSI INQUIRY, sent to the drive using SCSI Direct. The only
details reported by INQUIRY which are relevant here are the drive type
and removable flag.

READ_CAPACITY_10 is the SCSI READ_CAPACITY_10 result. Some very old drives
(SASI) do not support this command, but nearly all drives from the 90's
and newer do.

READ_CAPACITY_16 is unnecessary and likely unimplemented for any drive
smaller than 2 TB. It first appeared in the SCSI specification in the early
2000's, so older drives will definitely not support it. This command is
supported by PiSCSI regardless of the exported device size. All SCSI to
IDE and SD card adapters that have been tried do not yet support this
command.

Read-to capacity is implemented by devtest as a as-fast-as-possible means
to determine the actual readable capacity of the drive. It uses a search
based on power-of-two sector addresses.

Many SCSI drives support mode pages, and depending on the specific page,
provide details such as the number of cylinders, heads, and sectors that
are present on the device.

You may specify a device or volume name instead of a driver and unit.
Additional details are provided when a device or volume name is specified.
```
    9.OS322:> devtest -g Work:
                     SSize TotalSectors    Cyl Head  Sect DType Removable
    Partition          512     16738688   1042    1 16064  scsi.device 0
    Partition-start    512      2104384    131
    Partition-end      512     18843072   1173
    TD_GETGEOMETRY     512     18874368   1174  255    63  0x00 No
    Inquiry                                                0x00 No
    READ_CAPACITY_10   512     18874368
    READ_CAPACITY_16     -            -                    Fail 45  HFERR_BadStatus
    Read-to capacity   512     16738687
    Mode Page 0x03     512                             63
    Mode Page 0x04                        1174  255
```

## 4. Data integrity
The benchmark test is a good tool for verifying the Amiga's bus interface
and timing are being met, but it does no actual data verification. The
devtest utility can perform tests to verify that data can be reliably stored
to and retrieved from the device media.

> CAUTION: This test is destructive when the -d flag is given. Do not
           operate on a drive with data that you intend to keep.

Example:
```
    9.OS322:> devtest a4091.device 1 -i 1024 -d
```

The above command performs a single 1024 byte write to the device, and then
reads that data back. It will report any miscompares, and show the mismatching
data. The test always starts at the beginning of the device's storage. You can
loop on this operation, in which case, all device data will be sequentially
written and then read back.
Example (test 64 MB of device storage in 64KB chunks):
```
    9.OS322:> devtest a4091.device 1 -i 65536 -d -l 1024
    Pass 1  2024-07-07 00:32:46
    Pass 2  2024-07-07 00:32:46
    Pass 3  2024-07-07 00:32:46
    Pass 4  2024-07-07 00:32:46
```
...
```
    Pass 1023  2024-07-07 00:36:17
    Pass 1024  2024-07-07 00:36:17
    1024 passes completed successfully
```

As can be calculated from the above (about 300 KB/sec), the data integrity
test is significantly slower than the performance test.

When the data integrity test detects a failure, it automatically re-reads
the data. Example (512 byte transfers with 2-byte alignment):
```
    9.OS322:> devtest scsi.device 1 -di 512,2
    Miscompare at 0x0
      0001fe: a5 != expected 29 [diff 8c]
      0001ff: a5 != expected 86 [diff 23]
    Re-read of data differs (floating data?)
      0001fe: 5a != expected 29 [diff 73]
      0001ff: 5a != expected 86 [diff dc]
      0001fe: 5a != first read a5 [diff ff]
      0001ff: 5a != first read a5 [diff ff]
```
The above test suggests that the last two bytes of the transfer are
consistently not being written by the SCSI controller. This might happen
in the case of the Amiga SDMAC-04 when paired with a Ramsey-04 (not confirmed).

Some items to note:
1) The pattern written to disk is pseudo-random.
2) The read back buffer is filled with 0xa5 values by the CPU before the
   transfer is initiated.
3) If a miscompare is detected in the read back data, the same data will be
   read again into a second read back buffer. That buffer is pre-filled with
   0x5a values by the CPU before the transfer is initiated.
4) Since we see that both the primary and secondary read-back buffers appear
   to have original pattern contents, we can conclude that those values were
   never updated by the SDMAC on the read back from disk.

The default mode of the integrity test is to generate a pseudo-random pattern
for the write data, alternating between the random values and inverted random
values. There are two other generated data modes. If -ii is specified, the
written data will be the byte offset of the data within the buffer.
    0x00, 0x01, 0x02, ... 0xfe, 0xff, 0x00, 0x01 ...
and alternate:
    0xff, 0xfe, 0xfd, ... 0x01, 0x00, 0xff, 0xfe ...

If -iii is specified, the written data will be one of 7 specific values
in a rotating cycle. The fact that there are 7 values (a prime number) in
the -iii mode is by design. This will cause different power-of-two
addresses to experience different data patterns. Pattern values:
    0xa5, 0x5a, 0xc3, 0x3c, 0x81, 0x00, 0xff
and alternate:
    0x5a, 0xa5, 0x3c, 0xc3, 0x7e, 0xff, 0x00

The simple integrity test, as shown above, performs a sequential check of
all media on the device. At each pass in destructive mode, it writes a chunk,
reads back that chunk, and compares. In non-destructive mode, it reads a
chunk, reads the same chunk again, and compares.

The devtest utility offers another integrity test, butterfly. This test
is specified with the -k option:
```
    -k butterfly
```
The butterfly integrity test alternates between low and high offsets on
the device, each iteration the test moves the low and high closer to the
middle of the device, until they cross over. At which point, the low and
high swap places and then the test moves the low and high further from
the middle of the device. In destructive mode, the test writes data to
the low offset, and different data to the high offset. It then verifies
the low and high data match what was written. In non-destructive mode,
the test reads data at the low and high offset, then reads it a second
time from the low and high offset. It then verifies the low and high
data match what was previously read.
```
    8.SDH0:> devtest scsi.device 1 -i 256k -k butterfly -ddyv -l 300
    Pass 1  2020-03-30 15:04:43
      Zone sector 0 (0.0% of media check #1) 0 KB
    Pass 2  2020-03-30 15:05:00
      Zone sector 512 (1.5% of media check #1) 16 MB
    Pass 3  2020-03-30 15:05:15
      Zone sector 1024 (3.1% of media check #1) 32 MB
...
    Pass 64  2020-03-30 15:20:44
      Zone sector 32256 (98.4% of media check #1) 1023 MB
    Pass 65  2020-03-30 15:20:59
      Zone sector 0 (0.0% of media check #2) 1040 MB
    Pass 66  2020-03-30 15:21:15
...
```
The above test ran at about 1 MB/sec.

## 5. Packet support
    Trackdisk-compatible drivers often don't support all request
    packet types that a filesystem may use. This is especially true
    if it's an older driver that can't handle the packet standards
    which support larger (4GB+) media. Devtest will test most known
    trackdisk commands and report whether they are supported.
**WARNING: The -d option enables the write benchmark, which is destructive to any data on the drive!**
    Example:
```
    9.OS322:> devtest -t a4091.device 1 -d
    TD_GETGEOMETRY     Success  4194304 x 512  C=8192 H=32 S=16 Type=0
    TD_CHANGENUM       Success  Count=1
    TD_CHANGESTATE     Success  Disk present
    TD_PROTSTATUS      Success  Unprotected
    TD_GETDRIVETYPE    Fail -3 IOERR_NOCMD (unsupported)
    TD_GETNUMTRACKS    Fail -3 IOERR_NOCMD (unsupported)
    TD_RAWREAD         Fail -3 IOERR_NOCMD (unsupported)
    SCSICMD Inquiry    Success  V='ZULUSCSI' P='HARDDRIVE' R='2.0' DT=0x0 Linked Sync
    SCSICMD TUR        Success  Ready
    NSCMD_DEVICEQUERY  Success
    CMD_READ           Success
    ETD_READ           Success
    TD_READ64          Success
    NSCMD_TD_READ64    Success
    NSCMD_ETD_READ64   Success
    TD_SEEK            Success
    ETD_SEEK           Success
    TD_SEEK64          Success
    NSCMD_TD_SEEK64    Success
    NSCMD_ETD_SEEK64   Success
    CMD_WRITE          Success
    ETD_WRITE          Success
    TD_WRITE64         Success
    NSCMD_TD_WRITE64   Success
    NSCMD_ETD_WRITE64  Success
    TD_FORMAT          Success
    ETD_FORMAT         Success
    TD_FORMAT64        Success
    NSCMD_TD_FORMAT64  Success
    NSCMD_ETD_FORMAT64 Success
    TD_MOTOR ON        Success
    TD_MOTOR OFF       Success
```

Adding a second `-t` option will cause devtest to test additional
packet types. Some packets may cause your media to eject or trigger
known bugs in Amiga device drivers.

## 5. Individual commands

You can issue specific commands to a driver with the `-c` option.
Example:
```
    9.OS322:> devtest a4091.device 1 -c inquiry
    SCSICMD Inquiry    Success  V='ZULUSCSI' P='HARDDRIVE' R='2.0' DT=0x0 Linked Sync
```

Specify the -c option without an argument to get a list of known
commands which can be sent. Some commands accept arguments, such as READ.
Example (read 2 sectors of CD-ROM starting at sector 1):
```
    9.OS322:> devtest a4091.device 2 -c read(4096,2048
    CMD_READ           Success
```
Command arguments default to decimal, but may be prefixed by `0x` to
specify hexadecimal values.

The SCSI command can be used to send arbitrary SCSI commands using the
HD_SCSICMD trackdisk command. Example (send inquiry command):
```
    9.OS322:> devtest a4091.device 2 -c scsi(28,12,000000,28,0)
    SCSICMD 12         Success
```
Issue a READ (6):
```
    9.OS322:> devtest a4091.device 1 -c scsi(200,8,000000,1,0 -v
replylen=512 cmdlen=6 CMD=08 00 00 00 01 00
SCSICMD 08         Success  00 00 03 f3 00 00 00 00 00 00 00 03 00 00 00 00 00 00 00 02 00 00 45 ee 00 00 00 1f 00 00 04 0a 00 00 03 e9 00 00 45 ee 4e f9 00 00 cf f4 4e 75 20 0c 4e 75 28 6f 00 04 4e 75 00 00 00 24 56 45 52 3a 20 64 65 76 74 65 73 74 20 31 2e 38 62 20 28 4a 75 6c 20 32 37 20 32 30 32 36 29 20 a9 20 43 68 72 69 73 20 48 6f 6f 70 65 72 00 00 2f 0e 70 00 22 00 2c 79 00 00 06 c4 4e ae fe ce 72 0c e2 a8 02 40 00 01 2c 5f 4e 75 25 73 3a 20 79 65 73 0a 00 25 73 20 2d 20 61 72 65 20 79 6f 75 20 73 75 72 65 3f 20 28 79 2f 6e 29 20 00 00 48 e7 30 3e 24 2f 00 20 4a b9 00 00 05 54 66 1e 49 f9 00 00 be 14 4b f9 00 00 bc 2c 47 f9 00 00 bd 44 4d fa ff 9c 26 3c 00 00 bc 1c 60 10 2f 02 48 7a ff aa 4e b9 00 00 be 14 50 8f 60 4e 2f 02 48 7a ff a3 4e 94 20 79 00 00 00 10 2f 28 00 08 4e 95 4f ef 00 0c 60 28 4e 96 4a 40 67 04 70 00 60 2c 20 0a 72 df c0 81 72 59 b2 80 67 1e 72 4e b2 80 67 ea 20 43 4e 90 08 32 00 03 08 01 67 be 4e 93 24 40 70 ff b0 8a 66 ce 60 d2 70 01 4c df 7c 0c 4e 75 74 72 61 63 6b 64 69 73 6b 2e 64 65 76 69 63 65 00 00 2f 0e 2f 02 24 39 00 00 00 2c 48 7a ff e2 2f 02 4e b9 00 00 e1 88 50 8f 4a 80 57 c1 48 81 48 c1 2c 79 00 00 06 c4 20 42 20 39 00 00 00 30 44 81 22 6f 00 0c 4e ae fe 44 48 80 48 c0 24 1f 2c 5f 4e 75 2f 0e 2f 0a 24 6f 00 0c 4a b9 00 00 05 5c 67 32 42 b9 00 00 05 5c 35 7c 00 09 00 1c 42 aa 00 20 42 aa 00 2c 42 aa 00 24 42 aa 00 28 42 2a 00 1e 15 7c 00 a5 00 1f 22 4a 2c 79 00 00 06 c4 4e ae fe 38 22 4a 2c 79 00 00 06 c4 4e ae fe 3e 24 5f 2c 5f 4e 75 25 73 0a 0a 75 73 61 67 65 3a 20 64 65 76 74 65 73 74 20 3c 6f 70
```
Do the same with READ (10):
```
    9.OS322:> devtest a4091.device 1 -c scsi(200,28,0,00000000,0,0001,0
    SCSICMD 28         Success
```
How about READ (16), which is unsupported by this device:
```
    9.OS322:> devtest a4091.device 1 -c scsi(200,88,0,0000000000000000,00000001,0,0
SCSICMD 88         Fail 52  SENSE 5/20/00 Sense Key 5 (ASC=20 ASCQ=00)
```

The SCSI command arguments are always specified in hexadecimal.
The first two arguments are the expected response length and the command.
The expected response length must be at least the length that the SCSI
target needs to send. Values which follow are command option bytes.
You may specify the values as a mix of individual bytes or longer values.
The number of bytes is determined by the number of digits specified.
|  Digits   |  Byte count  |
| :------:  | :----------: |
| 1 or 2    | 1            |
| 3 or 4    | 2            |
| 5 or 6    | 3            |
| 7 or 8    | 4            |
| 9 or more | 8            |

Example:
```
  200,28,0,00000000,0,0001,0
```
This requests a response of 512 bytes. The command sent is:
```
  28 00 00 00 12 34 00 00 01 00
```
Broken down as follows:
| Command | RDR/D/F/R/O/O | LBA      | Rsv/Group | Sectors | Ctrl |
| :-----: | :-----------: | :------- | :-------: | :-----: | :--: |
| 28      | 00            | 00001234 | 00        | 0001    | 00   |

You could have specified `200,28,0,00001234,00000100` for the same packet.
