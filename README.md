# Amiga block device test utility (devtest)

This utility interacts directly with an Amiga block device driver to test
the driver and physical device. There are several different operation types
that the utility can perform:
   * Device probe
   * Performance
   * Packet support
   * Device Geometry
   * Data integrity

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
    Test a4091.device 1 with Coprocessor RAM
    read  512 KB xfers          5992 KB/sec
    read  128 KB xfers          5783 KB/sec
    read   32 KB xfers          4937 KB/sec
    write 512 KB xfers          5821 KB/sec
    write 128 KB xfers          4887 KB/sec
    write  32 KB xfers          3913 KB/sec
```

Testing with Zorro memory
```
    9.OS322:> devtest a4091.device 1 -bd -m zorro
    Test a4091.device 1 with Zorro III RAM
    read  512 KB xfers          7377 KB/sec
    read  128 KB xfers          7063 KB/sec
    read   32 KB xfers          5915 KB/sec
    write 512 KB xfers          7350 KB/sec
    write 128 KB xfers          5893 KB/sec
    write  32 KB xfers          4031 KB/sec
```

Sometimes there is a type of memory which is at a specific range
of addresses, and you want to use an address in that range.
With the -m option, you may both discover free memory and specify
the exact address to use. Use the `-m -` argument to show memory
blocks in the free list.
```
    9.OS322:> devtest -m -
    Coprocessor RAM at 0x8000000 size=0x8000000
      0x86b6220 0x400
      0x86ba370 0x200
      0x86bca90 0x400
      0x86c8af0 0x220
      0x86d1af8 0x3b8
      0x86d1ef0 0x230
      0x8766bd0 0xa08
      0x876d6c8 0x1650
      0x87892c0 0x7876d40
    MB RAM at 0x7000000 size=0x1000000
      0x7000020 0xffffe0
    Zorro III RAM at 0x40000000 size=0x10000000
      0x40000020 0xfffffe0
    Zorro III RAM at 0x60000000 size=0x10000000
      0x60000020 0xfffffe0
    Chip RAM at 0x004000 size=0x1fc000
      0x0202d8 0x1dfd28
```

Use the ```-m <address>``` option to specify a particular block of memory.
Example:
```
    9.OS322:> devtest a4091.device 1 -bd -m 0x60000020
```

## 3. Packet support
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
Adding a second `-b` option will cause devtest to also measure
latency of a variety of packets. Example:
```
    9.OS322:> devtest -bbd a4091.device 1
    Test a4091.device 1 with Coprocessor RAM
    read  512 KB xfers          5995 KB/sec
    read  128 KB xfers          5768 KB/sec
    read   32 KB xfers          4906 KB/sec
    write 512 KB xfers          5800 KB/sec
    write 128 KB xfers          5027 KB/sec
    write  32 KB xfers          4020 KB/sec
    OpenDevice / CloseDevice    2.090 ms
    OpenDevice multiple         0.003 ms
    CloseDevice multiple        0.001 ms
    TD_GETGEOMETRY sequential   2.021 ms
    TD_GETGEOMETRY parallel     1.100 ms
    TD_CHANGENUM                0.006 ms
    TD_CHANGENUM quick          0.006 ms
    CMD_INVALID                 0.006 ms
    CMD_START                   1.001 ms
    CMD_READ butterfly average  1.063 ms
    CMD_READ butterfly far      1.057 ms
    CMD_READ butterfly constant 1.058 ms
    CMD_READ sequential         2.077 ms
    CMD_READ parallel           2.059 ms
    HD_SCSICMD read sequential  2.072 ms
    HD_SCSICMD read parallel    2.058 ms
    CMD_WRITE sequential        3.052 ms
    CMD_WRITE parallel          3.034 ms
    HD_SCSICMD write sequential 3.054 ms
    HD_SCSICMD write parallel   3.036 ms
```

## 4. Device Geometry
On the Amiga, there are multiple methods to acquire a drive's physical
geometry, including direct SCSI commands. These methods are reported
by devtest.
```
    9.OS322:> devtest -g a4091.device 1
                     SSize TotalSectors   Cyl  Head  Sect  DType Removable
    TD_GETGEOMETRY     512      4194304  8192    32    16  0x00  No
    Inquiry                                                0x00  No
    READ_CAPACITY_10   512      4194304
    READ_CAPACITY_16     -            -                    Fail 52 ERROR_SENSE_CODE
    Read-to capacity   512      4194304
    Mode Page 0x03     512                             63
    Mode Page 0x04                        261   255
```

Not all drivers or devices support all commands or mode pages. A good example
is SCSI READ_CAPACITY_16. This command is practically unnecessary for any
drive smaller than 2 TB. It first appeared in the SCSI specification in the
early 2000's, so older drives will definitely not support it.

## 5. Data integrity
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
for the write data. There are two other generated data modes.
If -ii is specified, the written data will be the byte offset of the data
within the buffer.
    Example: 0x00, 0x01, 0x02, ... 0xfe, 0xff, 0x00, 0x01 ...

If -iii is specified, the written data will be one of 0xa5, 0x5a, 0xc3, 0x3c,
0x81, 0x00, 0xff, in a rotating cycle. The fact that there are 7 values in
the -iii mode is by design. The prime will cause different power-of-two
addresses to experience different data patterns.

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
