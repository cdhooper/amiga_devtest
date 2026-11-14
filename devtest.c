/*
 * devtest
 * -------
 * Utility to test AmigaOS block devices (trackdisk.device, scsi.device, etc).
 *
 * Copyright 2024 Chris Hooper. This program and source may be used
 * and distributed freely, for any purpose which benefits the Amiga
 * community. Commercial use of the binary, source, or algorithms requires
 * prior written approval from Chris Hooper <amiga@cdh.eebugs.com>.
 * All redistributions must retain this Copyright notice.
 *
 * DISCLAIMER: THE SOFTWARE IS PROVIDED "AS-IS", WITHOUT ANY WARRANTY.
 * THE AUTHOR ASSUMES NO LIABILITY FOR ANY DAMAGE ARISING OUT OF THE USE
 * OR MISUSE OF THIS UTILITY OR INFORMATION REPORTED BY THIS UTILITY.
 */
const char *version = "\0$VER: devtest " VER " ("__DATE__") \xA9 Chris Hooper";

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <time.h>
#include <exec/types.h>
#include <devices/trackdisk.h>
#include <devices/scsidisk.h>
#include <libraries/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <clib/exec_protos.h>
#include <clib/alib_protos.h>
#include <clib/dos_protos.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <libraries/configregs.h>

#ifdef _DCC
#define CDERR_BadDataType    36
#define CDERR_InvalidState   37
typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned long  uint32_t;
typedef struct { unsigned long hi; unsigned long lo; } uint64_t;
#define __packed
#else
#include <devices/cd.h>
#include <inline/timer.h>
#include <inline/exec.h>
#include <inline/dos.h>
#include <inttypes.h>
struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct Device   *TimerBase;
#endif

/*
 * uint32_t may be unsigned int or unsigned long,
 * depending on the toolchain.
 */
#define U32(v)  ((unsigned int)(uint32_t)(v))


/*
 * Trackdisk-64 enhanced commands
 *
 * Check before defining. AmigaOS 3.2 NDK provides these in trackdisk.h
 */
#ifndef TD_READ64
#define TD_READ64    24      // Read at 64-bit offset
#endif
#ifndef TD_WRITE64
#define TD_WRITE64   25      // Write at 64-bit offset
#endif
#ifndef TD_SEEK64
#define TD_SEEK64    26      // Seek to 64-bit offset
#endif
#ifndef TD_FORMAT64
#define TD_FORMAT64  27      // Format (write) at 64-bit offset
#endif

#define BIT(x) (1U << (x))

/* Internal flag to indicate IOF_QUICK should be set for command */
#define CMD_FLAG_NOT_QUICK BIT(14)

/* NSD commands */
#define NSCMD_DEVICEQUERY   0x4000
#define NSCMD_TD_READ64     0xC000
#define NSCMD_TD_WRITE64    0xC001
#define NSCMD_TD_SEEK64     0xC002
#define NSCMD_TD_FORMAT64   0xC003
#define NSCMD_ETD_READ64    0xE000
#define NSCMD_ETD_WRITE64   0xE001
#define NSCMD_ETD_SEEK64    0xE002
#define NSCMD_ETD_FORMAT64  0xE003

#define NSDEVTYPE_TRACKDISK 5   // Trackdisk-like block storage device

#define ARRAY_SIZE(x) ((sizeof (x) / sizeof ((x)[0])))

/* BCPL conversion functions */
#define BTOC(x) ((x)<<2)
#define CTOB(x) ((x)>>2)

#define BUFSIZE    8192
#define RAWBUFSIZE 32766  // Maximum disk DMA length (16383 words)

#define MEMTYPE_ANY    0
#define MEMTYPE_CHIP   1
#define MEMTYPE_FAST   2
#define MEMTYPE_24BIT  3
#define MEMTYPE_ZORRO  4
#define MEMTYPE_ZORRO2 5
#define MEMTYPE_ZORRO3 6
#define MEMTYPE_ACCEL  7
#define MEMTYPE_COPROC 8
#define MEMTYPE_MB     9
#define MEMTYPE_MAX    9

// memory areas
#define MEMTYPE_CHIP_START      0x00001000u  // chip memory start
#define MEMTYPE_CHIP_SIZE       0x001FF000u  // max. ~2MB chip memory
#define MEMTYPE_SLOW_START      0x00c00000u  // slow expansion memory start
#define MEMTYPE_SLOW_SIZE       0x00180000u  // max. 1.5MB slow expansion memory
#define MEMTYPE_ZORRO3_START    0x10000000u  // Zorro 3 start
#define MEMTYPE_ZORRO3_SIZE     0x70000000u  // 1.5GB Zorro 3 space
#define MEMTYPE_ACCEL_START     0x80000000u  // accelerator memory start
#define MEMTYPE_ACCEL_SIZE      0x60000000u  // max. 1.5GB
#define MEMTYPE_COPROC_START    0x08000000u  // coprocessor space start
#define MEMTYPE_COPROC_SIZE     0x08000000u  // max. 128MB
#define MEMTYPE_MB_START        0x01000000u  // 112MB RAM hack can start here
#define MEMTYPE_MB_SIZE         0x07000000u  // max. 112MB


#define SSD_SENSE_KEY(x)        ((x[2]) & 0x0f)
#define SSD_SENSE_ASC(x)        (x[12])
#define SSD_SENSE_ASCQ(x)       (x[13])

#define SKEY_NOT_READY          0x02

#define SCSI_TEST_UNIT_READY    0x00
struct scsi_test_unit_ready {
    uint8_t opcode;
    uint8_t byte2;
    uint8_t reserved[3];
    uint8_t control;
};

#define INQUIRY                 0x12
typedef struct scsi_inquiry_data {
    uint8_t device;
#define SID_TYPE                0x1f    /* device type mask */
#define SID_QUAL                0xe0    /* device qualifier mask */
#define SID_QUAL_LU_NOTPRESENT  0x20    /* logical unit not present */
    uint8_t dev_qual2;
    uint8_t version;
    uint8_t response_format;
    uint8_t additional_length;
    uint8_t flags1;

    uint8_t flags2;
#define SID_REMOVABLE           0x80
    uint8_t flags3;
#define SID_SftRe       0x01
#define SID_CmdQue      0x02
#define SID_Linked      0x08
#define SID_Sync        0x10
#define SID_WBus16      0x20
#define SID_WBus32      0x40
#define SID_RelAdr      0x80

#define SID_REMOVABLE           0x80
    uint8_t vendor[8];
    uint8_t product[16];
    uint8_t revision[4];
    uint8_t vendor_specific[20];
    uint8_t flags4;
    uint8_t reserved;
    uint8_t version_descriptor[8][2];
} __packed scsi_inquiry_data_t;  // 74 bytes

#define READ_CAPACITY_10        0x25
typedef struct scsi_read_capacity_10_data {
    uint8_t addr[4];
    uint8_t length[4];
} __packed scsi_read_capacity_10_data_t;

#define SERVICE_ACTION_IN       0x9e
typedef struct scsi_read_capacity_16_data {
    uint8_t addr[8];
    uint8_t length[4];
    uint8_t byte13;
#define SRC16D_PROT_EN          0x01
#define SRC16D_RTO_EN           0x02
    uint8_t reserved[19];
} __packed scsi_read_capacity_16_data_t;

typedef struct scsi_generic {
    uint8_t opcode;
    uint8_t bytes[15];
} __packed scsi_generic_t;

#define SCSI_READ_6_COMMAND             0x08
#define SCSI_WRITE_6_COMMAND            0x0a
#define SCSI_READ_10_COMMAND            0x28
#define SCSI_READ_16_COMMAND            0x88
#define SCSI_WRITE_10_COMMAND           0x2a
#define SCSI_WRITE_12_COMMAND           0xaa
#define SCSI_WRITE_16_COMMAND           0x8a
typedef struct scsi_rw_6 {
    uint8_t opcode;
    uint8_t addr[3];
    uint8_t length;
    uint8_t control;
} __packed scsi_rw_6_t;

typedef struct scsi_rw_10 {
    uint8_t opcode;
    uint8_t flags;
    uint8_t addr[4];
    uint8_t group;
    uint8_t length[2];
    uint8_t control;
} __packed scsi_rw_10_t;

typedef struct scsi_rw_16 {
    uint8_t opcode;
    uint8_t flags;
    uint8_t addr[8];
    uint8_t length[4];
    uint8_t group;
    uint8_t control;
} __packed scsi_rw_16_t;

#define MODE_SENSE_6            0x1a
#define SCSI_MODE_PAGES_BUFSIZE 255

#define DISK_PGCODE 0x3F    /* only 6 bits valid */
typedef struct scsi_generic_mode_page {
    uint8_t pg_code;        /* page code */
    uint8_t pg_length;      /* page length in bytes */
    uint8_t pg_bytes[253];  /* this number of bytes or less */
} scsi_generic_mode_page_t;

struct scsi_mode_sense_6 {
        uint8_t opcode;
        uint8_t byte2;
#define SMS_DBD                         0x08 /* disable block descriptors */
        uint8_t page;
#define SMS_PAGE_MASK                   0x3f
#define SMS_PCTRL_MASK                  0xc0
#define SMS_PCTRL_CURRENT               0x00
#define SMS_PCTRL_CHANGEABLE            0x40
#define SMS_PCTRL_DEFAULT               0x80
#define SMS_PCTRL_SAVED                 0xc0
        uint8_t reserved;
        uint8_t length;
        uint8_t control;
};

typedef struct NSDeviceQueryResult
{
    /*
     * Standard information
     */
    ULONG   DevQueryFormat;         /* this is type 0               */
    ULONG   SizeAvailable;          /* bytes available              */

    /*
     * Common information (READ ONLY!)
     */
    UWORD   DeviceType;             /* what the device does         */
    UWORD   DeviceSubType;          /* depends on the main type     */
    UWORD   *SupportedCommands;     /* 0 terminated list of cmd's   */

    /* May be extended in the future! Check SizeAvailable! */
} NSDeviceQueryResult_t;

typedef unsigned int uint;

typedef struct {
    char               alias[15];
    uint8_t            flags;
    uint64_t           mask;
    const char * const args;
    const char * const desc;
} test_cmds_t;

typedef struct {
    uint8_t  arg_count;
    uint32_t arg[5];
} args_t;

static int do_read_cmd(struct IOExtTD *tio, uint64_t offset, uint len,
                       void *buf, int nsd);
static void report_allocmem_fail(uint bufsize, uint memtype);
static void show_diffs(void *expected, void *data, uint len,
                       const char *type);

#define TEST_MAX_CMDS 32
#define BUF_COUNT     7   // General test buffers
#define IBUF_COUNT    6   // Integrity test buffers

BOOL __check_abort_enabled = 0;       // Disable gcc clib2 ^C break handling
/*
 * KS2.x lacks Exec memory pools, so clib2 otherwise tracks malloc chunks
 * with its own Exec list.  Force the slab allocator; BUFSIZE is 8192, so
 * keep this larger than standard I/O buffers.
 */
unsigned long __slab_max_size = 16384;

static int       g_verbose = 0;       // Verbose output
static int       g_changenum = 0;     // Current device media change number
static uint      g_sector_size = 512; // Updated when getting drive geometry
static uint64_t  g_devsize = 0;       // Device total size in bytes
static uint64_t  g_devstart = 0;      // Device start byte (for partitions)
static uint64_t  g_devend = 0;        // Device end byte (for partitions)
static uint      g_lun = 0;           // Target LUN to access
static uint      g_has_nsd = 0;       // Device driver supports NSD
static char     *g_devname = NULL;    // Device name
static uint      g_unitno;            // Device unit and LUN
static uint      g_e_freq;            // Frequency of eclock (715909 tps)
static UWORD     g_sense_length;      // Length of returned sense data (if any)
static UBYTE     g_sense_data[255];   // Sense data buffer

/*
 * Sense data was returned by the last SCSI direct command. The buffer is
 * cleared before each command, so a nonzero response code (byte 0) also
 * shows that sense data was received, even if the driver did not report
 * scsi_SenseActual.
 */
#define SENSE_VALID() ((g_sense_length > 0) || (g_sense_data[0] != 0))
static uint8_t  *g_tbuf[BUF_COUNT];   // General test buffers for certain tests
static uint8_t  *g_ibuf[IBUF_COUNT];  // Integrity test buffers
static uint8_t  *g_align[IBUF_COUNT]; // Integrity test buffers (aligned)
struct DosEnvec *g_envec;             // Filesystem environment vector
static UBYTE     mem_skip_alloc = 0;  // Skip memory allocate
static uint32_t  memtype = MEMTYPE_ANY; // Memory type
static uint64_t  test_cmd_mask[TEST_MAX_CMDS];
static args_t    test_cmd_args[TEST_MAX_CMDS];
static args_t   *cur_test_args = NULL;
static UWORD     flag_destructive = 0;
static uint      flag_cpu_utilization = 1;
static uint      force_yes = 0;
static uint      checknum = 0;
static uint      g_turn_motor_off;    // Drive read/write likely turned motor on
static struct Task     *main_task;
static struct Task     *idle_task;
static volatile int8_t  main_signal;
static volatile uint    idle_task_stop;
static volatile uint    idle_time;
static struct EClockVal idle_stime;

static BOOL
is_user_abort(void)
{
    if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
        return (1);
    return (0);
}

/*
 * are_you_sure() prompts the user to confirm that an operation is intended.
 *
 * @param  [in]  None.
 *
 * @return       TRUE  - User has confirmed (Y).
 * @return       FALSE - User has denied (N).
 */
int
are_you_sure(const char *prompt)
{
    int ch;
    if (force_yes) {
        printf("%s: yes\n", prompt);
        return (TRUE);
    }
ask_again:
    printf("%s - are you sure? (y/n) ", prompt);
    fflush(stdout);
    while ((ch = getchar()) != EOF) {
        if (is_user_abort())
            return (FALSE);
        if ((ch == 'y') || (ch == 'Y'))
            return (TRUE);
        if ((ch == 'n') || (ch == 'N'))
            return (FALSE);
        if (!isspace(ch))
            goto ask_again;
    }
    return (FALSE);
}

static int
turn_motor_off(struct IOExtTD *tio)
{
    /* Spin down device's motor */
    tio->iotd_Req.io_Command = TD_MOTOR;
    tio->iotd_Req.io_Actual  = 0;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 0;
    tio->iotd_Req.io_Data    = NULL;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    return (DoIO((struct IORequest *) tio));
}

static int
open_device(struct IOExtTD *tio)
{
    uint flags = 0;
    if (strcmp(g_devname, "trackdisk.device") == 0)
        flags = TDF_ALLOW_NON_3_5;
    return (OpenDevice(g_devname, g_unitno, (struct IORequest *) tio, flags));
}

static void
close_device(struct IOExtTD *tio)
{
    if (g_turn_motor_off) {
        g_turn_motor_off = 0;
        turn_motor_off(tio);
    }
    CloseDevice((struct IORequest *)tio);
}

static void
usage(void)
{
    /*
     * XXX implement:
     * -k <test> for kind of integrity test to perform:
     *     simple (default) is what is currently implemented
     *     sweep - check all media of device with trailing read (-dd required)
     *     butterfly - pattern several writes, then check them
     *     random - use prime-based random to check entire device
     */
    printf("%s\n\n"
           "usage: devtest <options> <x.device> <unit>\n"
           "   -b                    benchmark device [-bb show latency, "
                                     "-C no CPU %%]\n"
           "   -B <tsize>[,<#tio>]   set benchmark arguments, default: 512k,4\n"
           "   -c <cmd>[(arg,...)]   test a specific device driver request\n"
           "   -d                    also do destructive operations (write)\n"
// Undocumented: -dd skips save/restore of data with -i integrity test
           "   -g                    report drive geometry\n"
           "   -h                    display help\n"
           "   -i <tsize>[,<align>]  integrity test [-d=destructive] "
                    "[-dd=no save]\n"
           "                         [-i=random -ii=address -iii=pattern]\n"
           "   -k <mode>             integrity test mode: simple or butterfly\n"
           "   -l <loops>            run multiple times\n"
           "   -m <addr>             "
                    "use specific memory (Chip Fast Zorro MB Copr -=list)\n"
           "   -mm <addr>            "
                    "use specific address without allocation by OS\n"
           "   -o                    test open/close\n"
           "   -p                    probe SCSI bus for devices "
                    "(unit is optional)\n"
           "   -t                    test all packet types (basic, TD64, NSD);"
                    " -tt=more\n"
           "   -y                    answer all prompts with 'yes'\n",
           version + 7);
}

#define ERROR_SENSE_CODE 52  // a4091.device family specific error code
#define ERROR_PAST_END   70  // past end of device (internal devtest)

typedef struct {
    int errcode;
    const char *const errstr;
} err_to_str_t;

static const err_to_str_t err_to_str[] = {
    { IOERR_OPENFAIL,       "IOERR_OPENFAIL" },             // -1
    { IOERR_ABORTED,        "IOERR_ABORTED" },              // -2
    { IOERR_NOCMD,          "IOERR_NOCMD" },                // -3
    { IOERR_BADLENGTH,      "IOERR_BADLENGTH" },            // -4
    { IOERR_BADADDRESS,     "IOERR_BADADDRESS" },           // -5
    { IOERR_UNITBUSY,       "IOERR_UNITBUSY" },             // -6
    { IOERR_SELFTEST,       "IOERR_SELFTEST" },             // -7
    { TDERR_NotSpecified,   "TDERR_NotSpecified" },         // 20
    { TDERR_NoSecHdr,       "TDERR_NoSecHdr" },             // 21
    { TDERR_BadSecPreamble, "TDERR_BadSecPreamble" },       // 22
    { TDERR_BadSecID,       "TDERR_BadSecID" },             // 23
    { TDERR_BadHdrSum,      "TDERR_BadHdrSum" },            // 24
    { TDERR_BadSecSum,      "TDERR_BadSecSum" },            // 25
    { TDERR_TooFewSecs,     "TDERR_TooFewSecs" },           // 26
    { TDERR_BadSecHdr,      "TDERR_BadSecHdr" },            // 27
    { TDERR_WriteProt,      "TDERR_WriteProt" },            // 28
    { TDERR_DiskChanged,    "TDERR_DiskChanged" },          // 29
    { TDERR_SeekError,      "TDERR_SeekError" },            // 30
    { TDERR_NoMem,          "TDERR_NoMem" },                // 31
    { TDERR_BadUnitNum,     "TDERR_BadUnitNum" },           // 32
    { TDERR_BadDriveType,   "TDERR_BadDriveType" },         // 33
    { TDERR_DriveInUse,     "TDERR_DriveInUse" },           // 34
    { TDERR_PostReset,      "TDERR_PostReset" },            // 35
    { CDERR_BadDataType,    "CDERR_BadDataType" },          // 36
    { CDERR_InvalidState,   "CDERR_InvalidState" },         // 37
    { HFERR_SelfUnit,       "HFERR_SelfUnit" },             // 40
    { HFERR_DMA,            "HFERR_DMA" },                  // 41
    { HFERR_Phase,          "HFERR_Phase" },                // 42
    { HFERR_Parity,         "HFERR_Parity" },               // 42
    { HFERR_SelTimeout,     "HFERR_SelTimeout" },           // 44
    { HFERR_BadStatus,      "HFERR_BadStatus" },            // 45
    { 46,                   "ERROR_INQUIRY_FAILED" },       // 46
    { 47,                   "ERROR_TIMEOUT" },              // 47
    { 48,                   "ERROR_BUS_RESET" },            // 48
    { 49,                   "ERROR_TRY_AGAIN" },            // 49
    { HFERR_NoBoard,        "HFERR_NoBoard" },              // 50
    { 51,                   "ERROR_BAD_BOARD" },            // 51
    { ERROR_SENSE_CODE,     "ERROR_SENSE_CODE" },           // 52
    { 53,                   "ERROR_NOT_READY" },            // 53
    { ERROR_PAST_END,       "ERROR_PAST_END" },             // 60 (internal)
    /* The following unfortunately overlap several error codes above */
    { EACCES,               "EACCES" },                     // 2
    { EIO,                  "EIO" },                        // 5
    { ENOMEM,               "ENOMEM" },                     // 12
    { EBUSY,                "EBUSY" },                      // 16
    { ENODEV,               "ENODEV" },                     // 19
    { EINVAL,               "EINVAL" },                     // 22
    { ENOSPC,               "ENOSPC" },                     // 28
    { EROFS,                "EROFS" },                      // 30
    { EAGAIN,               "EAGAIN" },                     // 35
};

static const char *const floppy_types[] = {
    "Unknown",
    "3.5\"",
    "5.25\"",
    "3.5\" 150RPM",
};

static const char *
floppy_type_string(uint dtype)
{
    if ((dtype > 0) && (dtype <= DRIVE3_5_150RPM))
        return (floppy_types[dtype]);
    return ("Unknown");
}

static void
print_test_name(const char *name)
{
    printf("%-19s", name);
    fflush(stdout);
    fflush(NULL);  // gcc bug? fflush(stdout) doesn't seem to work
}

static void
print_ltest_name(const char *name)
{
    printf("%-28s", name);
    fflush(stdout);
    fflush(NULL);  // gcc bug? fflush(stdout) doesn't seem to work
}

static void
print_fail(int rc)
{
    size_t i;
    printf("Fail %d", rc);
    if ((rc == ERROR_SENSE_CODE) &&
        (SSD_SENSE_KEY(g_sense_data) || SSD_SENSE_ASC(g_sense_data))) {
        printf("  SENSE %x/%02x/%02x",
               SSD_SENSE_KEY(g_sense_data),
               SSD_SENSE_ASC(g_sense_data),
               SSD_SENSE_ASCQ(g_sense_data));
        return;
    }
    for (i = 0; i < ARRAY_SIZE(err_to_str); i++) {
        if (err_to_str[i].errcode == rc) {
            printf("  %s", err_to_str[i].errstr);
            break;
        }
    }
}

static void
print_fail_nl(int rc)
{
    if (rc == 0)
        printf("Success");
    else
        print_fail(rc);
    printf("\n");
}

static char *
llu_to_str(uint64_t value)
{
    unsigned int high = value / 1000000000;
    static char buf[32];

    if (high > 0) {
        sprintf(buf, "%u%09u",
                high, (unsigned int) (value - high * 1000000000));
    } else {
        sprintf(buf, "%u", (unsigned int) value);
    }
    return (buf);
}

static char *
llu_sector_to_str(uint64_t value)
{
    return (llu_to_str(value / g_sector_size));
}

/*
 * part_size() returns the size in bytes of the area being tested: the
 * partition when a DOS device name was given, otherwise the whole device.
 * Returns 0 if the size is not known.
 */
static uint64_t
part_size(void)
{
    if (g_devend != 0)
        return (g_devend - g_devstart);
    return (g_devsize);
}

/*
 * outside_partition() returns TRUE if a partition-relative access of
 * the specified length would extend past the end of the partition.
 * Always FALSE when not operating on a partition.
 */
static int
outside_partition(uint64_t rel_offset, uint len)
{
    return ((g_devend != 0) && (g_devstart + rel_offset + len > g_devend));
}

/*
 * cmd_for_offset() promotes CMD_READ or CMD_WRITE to the equivalent
 * 64-bit command if the access ends above the 4GB boundary.
 */
static UWORD
cmd_for_offset(UWORD iocmd, uint64_t abs_end)
{
    if ((abs_end >> 32) == 0)
        return (iocmd);
    if (iocmd == CMD_READ)
        return (g_has_nsd ? NSCMD_TD_READ64 : TD_READ64);
    if (iocmd == CMD_WRITE)
        return (g_has_nsd ? NSCMD_TD_WRITE64 : TD_WRITE64);
    return (iocmd);
}

/*
 * User-specified memory address (-m <addr>)
 * -----------------------------------------
 * Several buffers may be allocated at once (test buffers, benchmark
 * buffers, integrity buffers, SCSI command structures). The first buffer
 * gets the exact user-specified address, and subsequent buffers are
 * placed at the lowest available address above it, so that buffers never
 * overlap.
 *
 * With -mm, memory is not allocated through the OS, so the regions
 * handed out are tracked here instead.
 */
#define MEM_ALIGN(x) (((x) + MEM_BLOCKMASK) & ~MEM_BLOCKMASK)

typedef struct {
    uint32_t addr;
    uint32_t size;  // 0 = entry not in use
} skip_region_t;

static skip_region_t skip_regions[64];

static APTR
alloc_skip(ULONG byteSize, uint32_t base)
{
    uint32_t cand = base;
    uint     slot = ARRAY_SIZE(skip_regions);
    uint     cur;
    int      moved;

    do {
        moved = 0;
        for (cur = 0; cur < ARRAY_SIZE(skip_regions); cur++) {
            skip_region_t *r = &skip_regions[cur];
            if (r->size == 0)
                continue;
            if ((cand < r->addr + r->size) && (r->addr < cand + byteSize)) {
                /* Overlaps an existing region; move past it */
                cand = MEM_ALIGN(r->addr + r->size);
                moved = 1;
            }
        }
    } while (moved);

    for (cur = 0; cur < ARRAY_SIZE(skip_regions); cur++) {
        if (skip_regions[cur].size == 0) {
            slot = cur;
            break;
        }
    }
    if (slot == ARRAY_SIZE(skip_regions))
        return (NULL);  // Too many concurrent allocations
    skip_regions[slot].addr = cand;
    skip_regions[slot].size = byteSize;
    return ((APTR) cand);
}

static void
free_skip(APTR addr)
{
    uint cur;
    for (cur = 0; cur < ARRAY_SIZE(skip_regions); cur++) {
        if ((skip_regions[cur].size != 0) &&
            (skip_regions[cur].addr == (uint32_t) addr)) {
            skip_regions[cur].size = 0;
            return;
        }
    }
}

static APTR
alloc_user_addr(ULONG byteSize, uint32_t base)
{
    struct ExecBase  *eb = SysBase;
    struct MemHeader *mem;
    struct MemChunk  *chunk;
    APTR              addr;

    /* Exact address requested by the user, if available */
    addr = AllocAbs(byteSize, (APTR) base);
    if (addr != NULL)
        return (addr);

    /* Otherwise, the lowest free address above it which will fit */
    Forbid();
    for (mem = (struct MemHeader *) eb->MemList.lh_Head;
         mem->mh_Node.ln_Succ != NULL;
         mem = (struct MemHeader *) mem->mh_Node.ln_Succ) {
        if ((base < (uint32_t) mem->mh_Lower) ||
            (base >= (uint32_t) mem->mh_Upper))
            continue;  // Not the memory region containing base
        for (chunk = mem->mh_First; chunk != NULL; chunk = chunk->mc_Next) {
            uint32_t start = (uint32_t) chunk;
            uint32_t end   = start + chunk->mc_Bytes;
            if ((start < (uint32_t) mem->mh_Lower) ||
                (end > (uint32_t) mem->mh_Upper))
                break;  // Memory list corrupt - quit now!
            if (start < base)
                start = MEM_ALIGN(base);
            if ((start < end) && (end - start >= byteSize)) {
                addr = AllocAbs(byteSize, (APTR) start);
                break;  // Free list is sorted by address
            }
        }
        break;
    }
    Permit();
    return (addr);
}

APTR
AllocMemType(ULONG byteSize, uint32_t memtype)
{
    APTR addr = NULL;

    /* Don't bother with memory allocate */
    if (mem_skip_alloc)
        return (alloc_skip(byteSize, memtype));

    switch (memtype) {
        case 0:
            /* Highest priority (usually fast) memory */
            addr = AllocMem(byteSize, MEMF_PUBLIC | MEMF_ANY);
            break;
        case MEMTYPE_CHIP:
            /* Chip memory */
            addr = AllocMem(byteSize, MEMF_PUBLIC | MEMF_CHIP);
            break;
        case MEMTYPE_FAST:
            /* Fast memory */
            addr = AllocMem(byteSize, MEMF_PUBLIC | MEMF_FAST);
            break;
        case MEMTYPE_24BIT:
            /* 24-bit memory */
            addr = AllocMem(byteSize, MEMF_PUBLIC | MEMF_24BITDMA);
            break;
        case MEMTYPE_ZORRO:
        case MEMTYPE_ZORRO2:
        case MEMTYPE_ZORRO3:
        case MEMTYPE_COPROC:
        case MEMTYPE_MB:
        case MEMTYPE_ACCEL: {
            /*
             * Zorro or accelerator memory -- walk memory list and find
             * chunk in that memory space
             */
            struct ExecBase  *eb = SysBase;
            struct MemHeader *mem;
            struct MemChunk  *chunk;
            uint32_t          chunksize = 0;
            APTR              chunkaddr = NULL;

            Forbid();
            for (mem = (struct MemHeader *)eb->MemList.lh_Head;
                 mem->mh_Node.ln_Succ != NULL;
                 mem = (struct MemHeader *) mem->mh_Node.ln_Succ) {
                uint32_t size = (uint8_t *) mem->mh_Upper - (uint8_t *) mem;

                if ((memtype == MEMTYPE_MB) &&
                     (((uint32_t) mem < MEMTYPE_MB_START) ||
                      ((uint32_t) mem >= MEMTYPE_MB_START + MEMTYPE_MB_SIZE))) {
                    /* Not in Motherboard address range (A3000/A4000 fastmem) */
                    continue;
                }
                if ((memtype == MEMTYPE_COPROC) &&
                     (((uint32_t) mem < MEMTYPE_COPROC_START) ||
                      ((uint32_t) mem >= MEMTYPE_COPROC_START + MEMTYPE_COPROC_SIZE))) {
                    /* Not in Coprocessor address range */
                    continue;
                }
                if ((memtype == MEMTYPE_ZORRO2) &&
                    (((uint32_t) mem < E_MEMORYBASE) ||
                     ((uint32_t) mem >= E_MEMORYBASE + E_MEMORYSIZE))) {
                    /* Not in Zorro II address range */
                    continue;
                }
                if ((memtype == MEMTYPE_ZORRO3) &&
                     (((uint32_t) mem < MEMTYPE_ZORRO3_START) ||
                      ((uint32_t) mem >= MEMTYPE_ZORRO3_START + MEMTYPE_ZORRO3_SIZE))) {
                    /* Not in Zorro III address range */
                    continue;
                }
                if ((memtype == MEMTYPE_ZORRO) &&
                    ((((uint32_t) mem < E_MEMORYBASE) ||
                      ((uint32_t) mem >= E_MEMORYBASE + E_MEMORYSIZE)) &&
                     (((uint32_t) mem < MEMTYPE_ZORRO3_START) ||    // EZ3_CONFIGAREA
                      ((uint32_t) mem >= MEMTYPE_ZORRO3_START + MEMTYPE_ZORRO3_SIZE)))) { // EZ3_CONFIGAREAEND
                    /* Not in Zorro II or Zorro III address range */
                    continue;
                }
                if ((memtype == MEMTYPE_ACCEL) &&
                     (((uint32_t) mem < MEMTYPE_ACCEL_START) ||
                      ((uint32_t) mem >= MEMTYPE_ACCEL_START + MEMTYPE_ACCEL_SIZE))) {
                    /* Not in Accelerator address range */
                    continue;
                }

                /* Find the smallest chunk where this allocation will fit */
                for (chunk = mem->mh_First; chunk != NULL;
                     chunk = chunk->mc_Next) {
                    uint32_t cursize = chunk->mc_Bytes;
                    if (((uint32_t) chunk < (uint32_t) mem) ||
                        ((uint32_t) chunk > (uint32_t) mem + size) ||
                        ((uint32_t) chunk + cursize > (uint32_t) mem + size)) {
                        break;  // Memory list corrupt - quit now!
                    }

                    if (cursize >= byteSize) {
                        if ((chunkaddr != NULL) && (chunksize <= cursize))
                            continue;
                        chunkaddr = chunk;
                        chunksize = cursize;
                    }
                }
            }
            if (chunkaddr != NULL)
                addr = AllocAbs(byteSize, chunkaddr);
            Permit();
            break;
        }
        default:
            /* Allocate user-specified address */
            if (memtype > MEMTYPE_MAX)
                addr = alloc_user_addr(byteSize, memtype);
            else
                addr = NULL;
            break;
    }
    if (g_verbose > 1)
        printf("Alloc %p %u\n", addr, U32(byteSize));
    return (addr);
}

void
FreeMemType(APTR addr, ULONG byteSize)
{
    if (mem_skip_alloc) {
        free_skip(addr);  /* Memory was not allocated through the OS */
        return;
    }
    FreeMem(addr, byteSize);
}

static void
setup_scsidirect_cmd(struct SCSICmd *scmd, scsi_generic_t *cmd, uint cmdlen,
                     void *res, uint reslen)
{
    memset(scmd, 0, sizeof (*scmd));
    scmd->scsi_Data = (UWORD *) res;
    scmd->scsi_Length = reslen;
    // scmd.scsi_Actual = 0;
    scmd->scsi_Command = (UBYTE *) cmd;
    scmd->scsi_CmdLength = cmdlen;  // sizeof (cmd);
    // scmd.scsi_CmdActual = 0;
    if ((cmd->opcode == SCSI_WRITE_6_COMMAND) ||
        (cmd->opcode == SCSI_WRITE_10_COMMAND) ||
        (cmd->opcode == SCSI_WRITE_12_COMMAND) ||
        (cmd->opcode == SCSI_WRITE_16_COMMAND)) {
        scmd->scsi_Flags = SCSIF_WRITE;
    } else {
        scmd->scsi_Flags = SCSIF_READ;
    }
    scmd->scsi_Flags |= SCSIF_AUTOSENSE;
    // scmd.scsi_Status = 0;
    scmd->scsi_SenseData = g_sense_data;
    scmd->scsi_SenseLength = sizeof (g_sense_data);

    /*
     * Clear sense data from any previous command, so that a failure
     * without sense data is not reported with stale sense data.
     */
    memset(g_sense_data, 0, sizeof (g_sense_data));
    g_sense_length = 0;
    // scmd->scsi_SenseActual = 0;
}

static int
do_trackdisk_inquiry(struct IOExtTD *tio, uint *floppytype, uint *numtracks)
{
    int rc;

    tio->iotd_Req.io_Command = TD_GETDRIVETYPE;
    tio->iotd_Req.io_Actual  = 0;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 0;
    tio->iotd_Req.io_Data    = NULL;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;

    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        *floppytype = tio->iotd_Req.io_Actual,

        tio->iotd_Req.io_Command = TD_GETNUMTRACKS;
        tio->iotd_Req.io_Actual  = 0;
        tio->iotd_Req.io_Offset  = 0;
        tio->iotd_Req.io_Length  = 0;
        tio->iotd_Req.io_Data    = NULL;
        tio->iotd_Req.io_Flags   = 0;
        tio->iotd_Req.io_Error   = 0xa5;

        rc = DoIO((struct IORequest *) tio);
        if (rc == 0) {
            *numtracks = tio->iotd_Req.io_Actual;
        } else {
            *numtracks = 0;
        }
    }
    return (rc);
}

static int
do_scsidirect_cmd(struct IOExtTD *tio, scsi_generic_t *cmd, uint cmdlen,
                  uint reslen, void **resp)
{
    int            rc;
    void          *res;
    struct SCSICmd *scmd;

    /* Callers test the result pointer, so set it on every failure path */
    if (resp != NULL)
        *resp = NULL;

    if (reslen > 0) {
        res = AllocMemType(reslen, memtype);
        if (res == NULL) {
            report_allocmem_fail(reslen, memtype);
            g_sense_length = 0;
            return (ENOMEM);
        }
        memset(res, 0, reslen);
    } else {
        res = NULL;
    }

    scmd = AllocMemType(sizeof (*scmd), memtype);
    if (scmd == NULL) {
        if (reslen != 0)
            FreeMemType(res, reslen);
        report_allocmem_fail(sizeof (*scmd), memtype);
        g_sense_length = 0;
        return (ENOMEM);
    }

    setup_scsidirect_cmd(scmd, cmd, cmdlen, res, reslen);
    tio->iotd_Req.io_Command = HD_SCSICMD;
    tio->iotd_Req.io_Length  = sizeof (*scmd);
    tio->iotd_Req.io_Data    = scmd;

    rc = DoIO((struct IORequest *) tio);
    g_sense_length = scmd->scsi_SenseActual;
    tio->iotd_Req.io_Data = NULL;
    tio->iotd_Req.io_Length = 0;
    FreeMemType(scmd, sizeof (*scmd));

    if (rc != 0) {
        if (reslen != 0) {
            FreeMemType(res, reslen);
            res = NULL;
        }
    }
    if (resp != NULL)
        *resp = res;
    return (rc);
}

static int
do_scsi_inquiry(struct IOExtTD *tio, uint unit, scsi_inquiry_data_t **inq)
{
    scsi_generic_t cmd;
    uint lun = unit / 10;

    memset(&cmd, 0, sizeof (cmd));
    cmd.opcode = INQUIRY;
    cmd.bytes[0] = lun << 5;
    cmd.bytes[1] = 0;  // Page code
    cmd.bytes[2] = 0;
    cmd.bytes[3] = sizeof (scsi_inquiry_data_t);
    cmd.bytes[4] = 0;  // Control

    return (do_scsidirect_cmd(tio, &cmd, 6, sizeof (**inq), (void **) inq));
}

static int
do_scsi_testunitready(struct IOExtTD *tio, uint lun)
{
    scsi_generic_t cmd;

    memset(&cmd, 0, sizeof (cmd));
    cmd.opcode = SCSI_TEST_UNIT_READY;
    cmd.bytes[0] = lun << 5;

    return (do_scsidirect_cmd(tio, &cmd, 6, 0, NULL));
}

static int
do_scsi_read_capacity_10(struct IOExtTD *tio,
                         scsi_read_capacity_10_data_t **cap)
{
    scsi_generic_t cmd;
    uint len = sizeof (scsi_read_capacity_10_data_t);

    memset(&cmd, 0, sizeof (cmd));
    cmd.opcode = READ_CAPACITY_10;
    cmd.bytes[0] = g_lun << 5;

    return (do_scsidirect_cmd(tio, &cmd, 10, len, (void **) cap));
}

#define SRC16_SERVICE_ACTION    0x10  // SCSI_READ_CAPACITY_16
static int
do_scsi_read_capacity_16(struct IOExtTD *tio,
                         scsi_read_capacity_16_data_t **cap)
{
    scsi_generic_t cmd;
    uint len = sizeof (scsi_read_capacity_16_data_t);

    memset(&cmd, 0, sizeof (cmd));
    cmd.opcode = SERVICE_ACTION_IN;
    cmd.bytes[0] = SRC16_SERVICE_ACTION;
    *(uint32_t *)&cmd.bytes[9] = len;

    return (do_scsidirect_cmd(tio, &cmd, 16, len, (void **) cap));
}

static int
do_seek_capacity(struct IOExtTD *tio, uint64_t *sectors)
{
    uint64_t  offset = g_devsize / 2;  // Starting point
    uint64_t  incdec = offset / 2;
    int64_t   min_offset = -1;
    uint64_t  max_offset = 0xffffffffffffffff;
    uint8_t  *buf;
    int       rc = 0;
    int       double_mode = 1;

    if (g_sector_size == 0)
        g_sector_size = 512;

    if (incdec == 0)
        incdec = 1;

    buf = (uint8_t *) AllocMemType(g_sector_size, memtype);
    if (buf == NULL) {
        report_allocmem_fail(g_sector_size, memtype);
        return (1);
    }

    *sectors = 0;

    offset &= ~((uint64_t) g_sector_size - 1);
    while (incdec >= g_sector_size / 2) {
        rc = do_read_cmd(tio, offset, g_sector_size, buf, g_has_nsd);
        if (g_verbose)
            printf("Read %s = %d\n", llu_sector_to_str(offset), rc);

        if (rc == 0) {
            min_offset = offset;
            if (double_mode) {
                incdec = offset;
                offset *= 2;
            } else {
                while (offset + incdec >= max_offset) {
                    incdec /= 2;
                    incdec &= ~((uint64_t) g_sector_size - 1);
                    if (incdec == 0)
                        break;
                }
                offset += incdec;
            }
        } else {
            double_mode = 0;
            max_offset = offset;
            while ((int64_t) (offset - incdec) <= (int64_t) min_offset) {
                incdec /= 2;
                incdec &= ~((uint64_t) g_sector_size - 1);
                if (incdec == 0)
                    break;
            }
            offset -= incdec;
        }
    }

    FreeMemType(buf, g_sector_size);
    *sectors = min_offset / g_sector_size;
    g_turn_motor_off = 1;
    if (min_offset == -1)  // Nothing could be read
        return (rc);
    return (0);
}

static int
do_getgeometry(struct IOExtTD *tio, struct DriveGeometry *dg)
{
    /* Geometry */
    tio->iotd_Req.io_Command = TD_GETGEOMETRY;
    tio->iotd_Req.io_Actual  = 0xa5;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = sizeof (*dg);
    tio->iotd_Req.io_Data    = dg;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    return (DoIO((struct IORequest *) tio));
}

static uint32_t
_2btol(const uint8_t *bytes)
{
    return ((bytes[0] << 8) |
            bytes[1]);
}

static uint32_t
_3btol(const uint8_t *bytes)
{
    return ((bytes[0] << 16) |
            (bytes[1] << 8) |
            bytes[2]);
}

static uint32_t
_4btol(const uint8_t *bytes)
{
    return (((uint32_t)bytes[0] << 24) |
            ((uint32_t)bytes[1] << 16) |
            ((uint32_t)bytes[2] << 8) |
            (uint32_t)bytes[3]);
}

static uint64_t
_5btol(const uint8_t *bytes)
{
    return (((uint64_t)bytes[0] << 32) |
            ((uint64_t)bytes[1] << 24) |
            ((uint64_t)bytes[2] << 16) |
            ((uint64_t)bytes[3] << 8) |
            (uint64_t)bytes[4]);
}

#if 0
static uint64_t
_8btol(const uint8_t *bytes)
{
    return (((uint64_t)bytes[0] << 56) |
            ((uint64_t)bytes[1] << 48) |
            ((uint64_t)bytes[2] << 40) |
            ((uint64_t)bytes[3] << 32) |
            ((uint64_t)bytes[4] << 24) |
            ((uint64_t)bytes[5] << 16) |
            ((uint64_t)bytes[6] << 8) |
            (uint64_t)bytes[7]);
}
#endif

static int
scsi_read_mode_pages(struct IOExtTD *tio, uint8_t **res)
{
    scsi_generic_t cmd;
    int len = SCSI_MODE_PAGES_BUFSIZE;

#define SMS_PAGE_ALL_PAGES              0x3f
#define SMS_PAGE_SUBPAGES               0x00
#define SMS_PAGE_NO_SUBPAGES            0xff
#define SMS_DBD                         0x08 /* disable block descriptors */

    memset(&cmd, 0, sizeof (cmd));
    cmd.opcode = MODE_SENSE_6;
    cmd.bytes[0] = SMS_DBD;
    cmd.bytes[1] = SMS_PAGE_ALL_PAGES;
    cmd.bytes[2] = 0; // reserved
    cmd.bytes[3] = len; // length
    cmd.bytes[4] = 0; // control

    return (do_scsidirect_cmd(tio, &cmd, 6, len, (void **) res));
}

static char *
trim_spaces(char *str, size_t len)
{
    size_t olen = len;
    char *ptr;

    for (ptr = str; len > 0; ptr++, len--)
        if (*ptr != ' ')
            break;

    if (len == 0) {
        /* Completely empty name */
        *str = '\0';
        return (str);
    } else {
        memmove(str, ptr, len);

        while ((len > 0) && (str[len - 1] == ' '))
            len--;

        if (len < olen)  /* Is there space for a NIL character? */
            str[len] = '\0';
        return (str);
    }
    return (str);
}

static const char *
devtype_str(uint dtype)
{
    static const char * const dt_list[] = {
        "Disk", "Tape", "Printer", "Proc",
        "Worm", "CDROM", "Scanner", "Optical",
        "Changer", "Comm", "ASCIT81", "ASCIT82",
    };
    if (dtype < ARRAY_SIZE(dt_list))
        return (dt_list[dtype]);
    return ("Unknown");
}

static int
get_devsize(struct IOExtTD *tio)
{
    int rc;
    uint64_t capacity = 0;
    scsi_read_capacity_10_data_t *cap10;
    rc = do_scsi_read_capacity_10(tio, &cap10);
    if (rc != 0) {
        /* Try TD_GETGEOMETRY */
        struct DriveGeometry dg;
        rc = do_getgeometry(tio, &dg);
        if (rc == 0) {
            g_devsize = (uint64_t) dg.dg_TotalSectors * dg.dg_SectorSize;
            g_sector_size = dg.dg_SectorSize;
        }
        return (rc);
    }
    if (cap10 != NULL) {
        uint ssize = *(uint32_t *) &cap10->length;
        capacity   = *(uint32_t *) &cap10->addr;

        FreeMemType(cap10, sizeof (*cap10));
        if (capacity == 0xffffffff) {
            /* Try to use READ_CAPACITY_16 */
            scsi_read_capacity_16_data_t *cap16;
            rc = do_scsi_read_capacity_16(tio, &cap16);
            if ((rc == 0) && (cap16 != NULL)) {
                capacity = *(uint64_t *) &cap16->addr;
                FreeMemType(cap16, sizeof (*cap16));
            }
        }
        if (capacity != 0)
            capacity++;  // SCSI READ_CAPACITY_* reports last addressable sector
        g_devsize = capacity * ssize;
        g_sector_size = ssize;
    }
    if (capacity == 0)
        return (1);
    return (0);
}

/*
 * bytes_to_human_str() returns the size of count blocks of blocksize bytes
 * as a decimal string with a unit suffix (KB, MB, GB, etc).
 */
static char *
bytes_to_human_str(uint64_t count, uint blocksize)
{
    static char str[16];
    uint     unit = 0;  // KMGTPEZY
    uint64_t value;

    if (blocksize == 0)
        blocksize = 1;
    while (count > UINT64_MAX / blocksize) {
        /* Scale first to avoid overflow; precision loss is negligible */
        count /= 1000;
        unit++;
    }
    value = count * blocksize / 1000;  // KB
    while (value > 9999) {
        value /= 1000;
        unit++;
    }
    sprintf(str, "%u %cB", (uint) value, "KMGTPEZY"[unit]);
    return (str);
}

static int
scsi_probe_unit(uint unit, struct IOExtTD *tio)
{
    int rc;
    int erc;
    scsi_inquiry_data_t *inq_res;

    rc = OpenDevice(g_devname, unit, (struct IORequest *) tio, 0);
    if (rc == 0) {
        printf("%3d", unit);
        erc = do_scsi_inquiry(tio, unit, &inq_res);
        if (erc == ENOMEM) {
            printf("\n");
            close_device(tio);
            return (erc);
        }
        if (erc == 0) {
            printf(" %-*.*s %-*.*s %-*.*s %-7s",
               (int) sizeof (inq_res->vendor),
               (int) sizeof (inq_res->vendor),
               trim_spaces(inq_res->vendor, sizeof (inq_res->vendor)),
               (int) sizeof (inq_res->product),
               (int) sizeof (inq_res->product),
               trim_spaces(inq_res->product, sizeof (inq_res->product)),
               (int) sizeof (inq_res->revision),
               (int) sizeof (inq_res->revision),
               trim_spaces(inq_res->revision, sizeof (inq_res->revision)),
               devtype_str((inq_res->device) & SID_TYPE));
            FreeMemType(inq_res, sizeof (*inq_res));
        } else {
            uint floppytype;
            uint tracks;
            erc = do_trackdisk_inquiry(tio, &floppytype, &tracks);
            if (erc == 0) {
                printf(" Floppy %s %d tracks",
                       floppy_type_string(floppytype), tracks);
            } else {
                printf(" Unknown device type");
            }
        }

        scsi_read_capacity_10_data_t *cap10;
        erc = do_scsi_read_capacity_10(tio, &cap10);
        if ((erc == 0) && (cap10 != NULL)) {
            uint ssize = *(uint32_t *) &cap10->length;
            uint64_t cap   = *(uint32_t *) &cap10->addr;
            if (cap == 0xffffffff) {
                /* Try to use READ_CAPACITY_16 */
                scsi_read_capacity_16_data_t *cap16;
                erc = do_scsi_read_capacity_16(tio, &cap16);
                if ((erc == 0) && (cap16 != NULL)) {
                    cap = *(uint64_t *) &cap16->addr;
                    FreeMemType(cap16, sizeof (*cap16));
                }
            }
            printf("%5u %8s", ssize, bytes_to_human_str(cap + 1, ssize));
            FreeMemType(cap10, sizeof (*cap10));
        }
        printf("\n");
        close_device(tio);
    }
    return (rc);
}

static int
scsi_probe(char *unitstr)
{
    int rc = 0;
    int found = 0;
    int justunit = -1;
    uint unit;
    uint target;
    uint lun;
    struct IOExtTD *tio;
    struct MsgPort *mp;

    if ((unitstr != NULL) &&
        (sscanf(unitstr, "%i", (int *) &unit) == 1)) {
        justunit = unit;
    }
    mp = CreatePort(0, 0);
    if (mp == NULL) {
        printf("Failed to create message port\n");
        return (1);
    }

    tio = (struct IOExtTD *) CreateExtIO(mp, sizeof (struct IOExtTD));
    if (tio == NULL) {
        printf("Failed to create tio struct\n");
        rc = 1;
        goto extio_fail;
    }
    for (target = 0; target < 8; target++) {
        for (lun = 0; lun < 8; lun++) {
            unit = target + lun * 10;
            if ((justunit != -1) && (unit != (uint) justunit))
                continue;
            rc = scsi_probe_unit(unit, tio);
            if (rc == 0) {
                found++;
            } else {
                if (justunit != -1) {
                    printf("Open %s Unit %u: ", g_devname, justunit);
                    print_fail_nl(rc);
                }
                break;  // Stop probing at first failed lun of each target
            }
            if (is_user_abort()) {
                printf("^C\n");
                goto break_abort;
            }
        }
    }
break_abort:
    DeleteExtIO((struct IORequest *) tio);
extio_fail:
    DeletePort(mp);
    if (found == 0) {
        if (justunit == -1) {
            printf("Open %s: ", g_devname);
            if (rc == HFERR_SelfUnit)
                printf("no device found\n");
            else
                print_fail_nl(rc);
        }
        rc = 1;
    } else {
        rc = 0;
    }
    return (rc);
}

/* True if page bytes [pos + off, pos + off + n) were received */
#define PAGE_HAS_BYTES(off, n) (((hlen == 2) && ((off) + (n) <= avail)))

static int
drive_geometry(void)
{
    int    rc;
    struct IOExtTD *tio;
    struct DriveGeometry dg;
    struct MsgPort *mp;
    uint8_t *pages;
    uint32_t sector_size;
    uint64_t last_sector;

    mp = CreatePort(0, 0);
    if (mp == NULL) {
        printf("Failed to create message port\n");
        return (1);
    }

    tio = (struct IOExtTD *) CreateExtIO(mp, sizeof (struct IOExtTD));
    if (tio == NULL) {
        printf("Failed to create tio struct\n");
        rc = 1;
        goto extio_fail;
    }
    if ((rc = open_device(tio)) != 0) {
        printf("Open %s Unit %u: ", g_devname, g_unitno);
        print_fail_nl(rc);
        rc = 1;
        goto opendev_fail;
    }

    tio->iotd_Req.io_Command = TD_GETGEOMETRY;
    tio->iotd_Req.io_Actual  = 0xa5;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = sizeof (dg);
    tio->iotd_Req.io_Data    = &dg;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    printf("                 SSize TotalSectors    Cyl Head  Sect DType "
           "Removable\n");
    if (g_envec != NULL) {
        uint cyls = g_envec->de_HighCyl + 1 - g_envec->de_LowCyl;
        uint64_t blocks;
        blocks = (uint64_t) cyls * g_envec->de_Surfaces *
                 g_envec->de_BlocksPerTrack;
        printf("Partition        %5u %12s %6u %4u %5u  %s %u\n",
               U32(g_sector_size), llu_to_str(blocks), U32(cyls),
               U32(g_envec->de_Surfaces), U32(g_envec->de_BlocksPerTrack),
               g_devname, g_unitno);
        blocks = (uint64_t) g_envec->de_LowCyl *
                 g_envec->de_Surfaces * g_envec->de_BlocksPerTrack;
        printf("Partition-start  %5u %12s %6u\n",
               U32(g_sector_size), llu_to_str(blocks),
               U32(g_envec->de_LowCyl));
        blocks = (uint64_t) (g_envec->de_HighCyl + 1) *
                 g_envec->de_Surfaces * g_envec->de_BlocksPerTrack;
        printf("Partition-end    %5u %12s %6u\n",
               U32(g_sector_size), llu_to_str(blocks),
               U32(g_envec->de_HighCyl + 1));
    }
    printf("TD_GETGEOMETRY ");
    if ((rc = DoIO((struct IORequest *) tio)) != 0) {
        printf("%7c %12c %5c %5c %5c  ",
               '-', '-', '-', '-', '-');
        print_fail_nl(rc);
    } else {
        printf("%7u %12u %6u ",
               U32(dg.dg_SectorSize), U32(dg.dg_TotalSectors),
               U32(dg.dg_Cylinders));
        printf("%*u ",
               (dg.dg_Cylinders < 1000000) ? 4 : 3, U32(dg.dg_Heads));
        printf("%5u  0x%02x %s\n",
               U32(dg.dg_TrackSectors), dg.dg_DeviceType,
               (dg.dg_Flags & DGF_REMOVABLE) ? "Yes" : "No");
        g_devsize = (uint64_t) dg.dg_TotalSectors * dg.dg_SectorSize;
        g_sector_size = dg.dg_SectorSize;
    }

    printf("Inquiry ");
    scsi_inquiry_data_t *inq_res;
    rc = do_scsi_inquiry(tio, g_unitno, &inq_res);
    if (rc != 0) {
        printf("%47s", "");
        print_fail_nl(rc);
    } else {
        printf("%47s0x%02x %s", "",
               inq_res->device & SID_TYPE,
               (inq_res->dev_qual2 & SID_REMOVABLE) ? "Yes" : "No");
        if (inq_res->dev_qual2 & SID_REMOVABLE) {
            printf(" %s", (inq_res->dev_qual2 & SID_QUAL_LU_NOTPRESENT) ?
                   "Removed" : "Present");
        }
        printf("\n");
#if 0
        printf("V='%.*s' P='%.*s' R='%.*s' Qual=0x%x DT=0x%x %s\n",
                sizeof (inq_res->vendor), inq_res->vendor,
                sizeof (inq_res->product), inq_res->product,
                sizeof (inq_res->revision), inq_res->revision,
                inq_res->device & SID_QUAL,
                inq_res->device & SID_TYPE,
                (inq_res->dev_qual2 & SID_REMOVABLE) ? "Removable" : "");
#endif
        FreeMemType(inq_res, sizeof (*inq_res));
    }

    printf("READ_CAPACITY_10 ");
    scsi_read_capacity_10_data_t *cap10;
    rc = do_scsi_read_capacity_10(tio, &cap10);
    if (cap10 == NULL) {
        printf("%5c %12c %19s", '-', '-', "");
        print_fail_nl(rc);
    } else {
        last_sector = *(uint32_t *) &cap10->addr;
        sector_size = *(uint32_t *) &cap10->length;
        FreeMemType(cap10, sizeof (*cap10));
        printf("%5u %12s\n", U32(sector_size), llu_to_str(last_sector + 1));
        if (g_devsize == 0) {
            g_devsize = (uint64_t) sector_size * (last_sector + 1);
            g_sector_size = sector_size;
        }
    }

    printf("READ_CAPACITY_16 ");
    scsi_read_capacity_16_data_t *cap16;
    rc = do_scsi_read_capacity_16(tio, &cap16);
    if (cap16 == NULL) {
        printf("%5c %12c %19s", '-', '-', "");
        print_fail_nl(rc);
    } else {
        last_sector = *(uint64_t *) &cap16->addr;
        sector_size = *(uint32_t *) &cap16->length;
        FreeMemType(cap16, sizeof (*cap16));
        printf("%5u %12s\n", U32(sector_size), llu_to_str(last_sector + 1));
        g_devsize = (uint64_t) sector_size * (last_sector + 1);
        g_sector_size = sector_size;
    }

    printf("Read-to capacity ");
    rc = do_seek_capacity(tio, &last_sector);
    if (rc != 0) {
        printf("%5c %12c %19s", '-', '-', "");
        print_fail_nl(rc);
    } else {
        printf("%5u %12s\n", g_sector_size, llu_to_str(last_sector + 1));
    }

    rc = scsi_read_mode_pages(tio, &pages);
    if (pages == NULL) {
        printf("Mode Pages%45s", "");
        print_fail_nl(rc);
    } else {
        /*
         * MODE SENSE(6) header: byte 0 is the mode data length (not
         * including byte 0 itself) and byte 3 is the block descriptor
         * length. Mode pages follow the block descriptors. Pages may
         * start at odd offsets, so multi-byte fields are assembled a
         * byte at a time (68000 faults on unaligned word access).
         */
        uint pos;
        uint plen;     // Total page length
        uint end = pages[0] + 1;
        if (end > SCSI_MODE_PAGES_BUFSIZE)
            end = SCSI_MODE_PAGES_BUFSIZE;
        for (pos = 4 + pages[3]; pos + 2 <= end; pos += plen) {
            uint page = pages[pos] & DISK_PGCODE;
            uint dlen;     // Page data length (following the header)
            uint hlen;     // Page header length
            uint avail;    // Page bytes present in the buffer

            if (pages[pos] & 0x40) {
                /* Sub-page format: 2-byte page length at bytes 2-3 */
                if (pos + 4 > end)
                    break;
                hlen = 4;
                dlen = _2btol(&pages[pos + 2]);
            } else {
                hlen = 2;
                dlen = pages[pos + 1];
            }
            plen  = hlen + dlen;
            avail = (pos + plen > end) ? end - pos : plen;

            switch (page) {
                case 0x03: // Disk Format
                    if (!PAGE_HAS_BYTES(10, 4))
                        goto show_default;
                    printf("Mode Page 0x%02x", page);
                    printf("%8u %30u\n",
                           _2btol(&pages[pos + 12]),   // Sector size
                           _2btol(&pages[pos + 10]));  // Sectors / track
                    break;
                case 0x04: // Rigid Geometry
                    if (!PAGE_HAS_BYTES(2, 4))
                        goto show_default;
                    printf("Mode Page 0x%02x", page);
                    printf("%28u %4u\n",
                           _3btol(&pages[pos + 2]),    // Cylinders
                           pages[pos + 5]);            // Heads
                    break;
                case 0x05: // Flexible Geometry
                    if (!PAGE_HAS_BYTES(4, 6))
                        goto show_default;
                    printf("Mode Page 0x%02x", page);
                    printf("%8u %18u %6u %4u\n",
                           _2btol(&pages[pos + 6]),    // Sector size
                           _2btol(&pages[pos + 8]),    // Cylinders
                           pages[pos + 4],             // Heads
                           pages[pos + 5]);            // Sectors / track
                    break;
                case 0x06: // Reduced Block Commands Parameters
                    if (!PAGE_HAS_BYTES(3, 7))
                        goto show_default;
                    printf("Mode Page 0x%02x", page);
                    printf("%8u %12s\n",
                           _2btol(&pages[pos + 3]),    // Sector size
                           llu_to_str(_5btol(&pages[pos + 5])));
                    break;
                /* The below pages are commonly seen, but not interesting */
                case 0x00:  // Vendor-specific
                case 0x01:  // Error recovery
                case 0x02:  // Disconnect-Reconnect
                case 0x07:  // Verify Error Recovery
                case 0x08:  // Caching
                case 0x0a:  // Control (TCQ, events, etc)
                case 0x30:  // Apple-specific
                    if (g_verbose)
                        goto show_default;
                    break;
show_default:
                /* The below pages are not commonly seen */
                case 0x0b:  // Medium Types Supported
                case 0x0c:  // Notch and Partition (for variable blk devs)
                default:
                    printf("Mode Page 0x%02x", page);
                    if (hlen == 4)
                        printf(" subpage 0x%02x", pages[pos + 1]);
                    printf(" len=%u", dlen);
                    if (avail < plen)
                        printf(" (truncated to %u)", avail - hlen);
                    printf("\n");
                    break;
            }
            if (g_verbose && (avail > hlen)) {
                uint cur;
                printf("   ");
                for (cur = pos + hlen; cur < pos + avail; cur++)
                    printf(" %02x", pages[cur]);
                printf("\n");
            }
        }
        FreeMemType(pages, SCSI_MODE_PAGES_BUFSIZE);
    }

    close_device(tio);
opendev_fail:
    DeleteExtIO((struct IORequest *) tio);
extio_fail:
    DeletePort(mp);
    return (rc);
}

#define UNIX_TIME_OFFSET 252460800 // (1978 - 1970) * 365.25 * 24 * 60 * 60

static time_t
datestamp_to_time(struct DateStamp *ds)
{
    return (UNIX_TIME_OFFSET +
            ds->ds_Days * (24 * 60* 60) +
            ds->ds_Minute * 60 +
            (ds->ds_Tick / TICKS_PER_SECOND));
}

__stdargs time_t
time(time_t *t)
{
    struct DateStamp ds;
    time_t timeval;

    DateStamp(&ds);
    timeval = datestamp_to_time(&ds);

    if (t != NULL)
        *t = timeval;
    return (timeval);
}

static void
print_time(void)
{
    struct tm *tm;
    time_t timet;
    time(&timet);
    /*
     * AmigaDOS DateStamp() is already local wall-clock time.  Using
     * localtime() here applies the C library timezone a second time and
     * can shift the displayed hour.  Decode it as-is.
     */
    tm = gmtime(&timet);
    printf("%04d-%02d-%02d %02d:%02d:%02d",
           tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
           tm->tm_hour, tm->tm_min, tm->tm_sec);
}

static void
print_perf_type(int is_write, uint xfer_size)
{
    if (g_verbose == 0) {
        printf("%s ", is_write ? "write" : "read ");
        if (xfer_size > 1024)
            printf("%3u KB ", xfer_size / 1024);
        else
            printf("%3u byte ", xfer_size);
        printf("xfers ");
        fflush(stdout);
        fflush(NULL);  // gcc bug? fflush(stdout) doesn't seem to work
    }
}

static uint32_t
diff_e_clock(struct EClockVal *ev1, struct EClockVal *ev2)
{
    return (*((uint64_t *) ev2) - *((uint64_t *) ev1));
}

static void
print_latency(uint ttime, uint iters, char endch)
{
    uint tusec;
    uint tmsec;
    if (iters == 0)
        iters = 1;
    tusec = ((uint64_t) ttime * 1000000 + (uint64_t) g_e_freq * iters / 2) /
            ((uint64_t) g_e_freq * iters);  // Rounded to nearest usec
    tmsec = tusec / 1000;
    tusec %= 1000;

    printf("%u.%03u ms%c", tmsec, tusec, endch);
}

/*
 * print_par_result() reports the result of a parallel latency test: the
 * average latency if all requests succeeded, otherwise how many failed
 * and the first failure (a latency figure would not be meaningful).
 */
static void
print_par_result(uint ttime, uint num_iter, uint failed, int failcode)
{
    if (failed == 0) {
        print_latency(ttime, num_iter, '\n');
    } else {
        printf("%u of %u failed: ", failed, num_iter);
        print_fail_nl(failcode);
    }
}

static int
latency_getgeometry(struct IOExtTD **tio, int max_iter)
{
    int iter;
    int rc = 0;
    int failcode;
    int iters;
    int num_iter = max_iter;
    struct EClockVal stime;
    struct EClockVal etime;
    unsigned int ttime = 0;
    struct DriveGeometry dg;

    memset(&dg, 0, sizeof (dg));
    for (iters = 0; iters < max_iter; iters++) {
        if ((rc = open_device(tio[iters])) != 0) {
            printf("Open %s Unit %u: ", g_devname, g_unitno);
            print_fail_nl(rc);
            break;
        }
    }
    num_iter = iters;
    if (iters == 0)
        return (1);

    print_ltest_name("TD_GETGEOMETRY sequential");

    for (iter = 0; iter < num_iter; iter++) {
        tio[iter]->iotd_Req.io_Command = TD_GETGEOMETRY;
        tio[iter]->iotd_Req.io_Actual  = 0xa5;
        tio[iter]->iotd_Req.io_Offset  = 0;
        tio[iter]->iotd_Req.io_Length  = sizeof (dg);
        tio[iter]->iotd_Req.io_Data    = &dg;
        tio[iter]->iotd_Req.io_Flags   = 0;
        tio[iter]->iotd_Req.io_Error   = 0xa5;
    }

    ReadEClock(&stime);
    for (iter = 0; iter < num_iter; iter++) {
        failcode = DoIO((struct IORequest *) tio[iter]);
        if (failcode != 0) {
            if (++rc < 10) {
                printf("  ");
                print_fail(failcode);
            }
        }

        if (iter & 0xf)
            continue;

        ReadEClock(&etime);
        ttime = diff_e_clock(&stime, &etime);
        if (ttime > g_e_freq * 2) {
            iter++;
            break;
        }
    }
    if (iter >= num_iter) {
        ReadEClock(&etime);
        ttime = diff_e_clock(&stime, &etime);
    }
    print_latency(ttime, iter, '\n');

    if (is_user_abort()) {
        printf("^C abort\n");
        rc++;
        goto finish_fail;
    }

    iters = iter;
    print_ltest_name("TD_GETGEOMETRY parallel");
    for (iter = 0; iter < iters; iter++) {
        tio[iter]->iotd_Req.io_Command = TD_GETGEOMETRY;
        tio[iter]->iotd_Req.io_Actual  = 0xa5;
        tio[iter]->iotd_Req.io_Offset  = 0;
        tio[iter]->iotd_Req.io_Length  = sizeof (dg);
        tio[iter]->iotd_Req.io_Data    = &dg;
        tio[iter]->iotd_Req.io_Flags   = 0;
        tio[iter]->iotd_Req.io_Error   = 0xa5;
    }

    ReadEClock(&stime);
    for (iter = 0; iter < iters; iter++) {
        SendIO((struct IORequest *) tio[iter]);
    }
    {
        uint failed = 0;
        int  first = 0;
        for (iter = 0; iter < iters; iter++) {
            failcode = WaitIO((struct IORequest *) tio[iter]);
            if (failcode != 0) {
                if (failed++ == 0)
                    first = failcode;  // Report the first failure
            }
        }
        ReadEClock(&etime);
        ttime = diff_e_clock(&stime, &etime);
        print_par_result(ttime, iters, failed, first);
        rc += failed;
    }

finish_fail:
    for (iter = 0; iter < num_iter; iter++)
        close_device(tio[iter]);

    /* dg is only valid if at least one TD_GETGEOMETRY succeeded */
    if ((dg.dg_SectorSize != 0) && (dg.dg_TotalSectors != 0)) {
        g_devsize = (uint64_t) dg.dg_TotalSectors * dg.dg_SectorSize;
        g_sector_size = dg.dg_SectorSize;
    }

    return (rc);
}

#define BUTTERFLY_MODE_AVG   0  // Average seek time across device
#define BUTTERFLY_MODE_FAR   1  // Weight toward end of media
#define BUTTERFLY_MODE_CONST 2  // Constant travel half of device

/*
 * Media read patterns
 *
 * |      AVG      |  |       FAR     |  |      CONST    |
 *  *------------->    *------------->    *------->
 *     <----------*     <------------*       <----*
 *     *------->        *----------->        *------->
 *        <----*         <----------*           <----*
 *        *-->           *--------->            *------->
 *          <*            <--------*               <----*
 */
static int
latency_butterfly(UWORD iocmd, uint8_t *buf, int num_iter,
                  struct IOExtTD **tio, int mode)
{
    int iter;
    int rc = 0;
    int failcode = 0;
    struct EClockVal stime;
    struct EClockVal etime;
    uint64_t size;   // Partition (or device) size in bytes
    uint64_t nsec;   // Partition size in sectors
    uint64_t step;   // Step between positions, in sectors
    uint64_t half;   // Half of partition, in sectors

    if (g_sector_size == 0)
        g_sector_size = 512;

    size = part_size();
    if (size == 0)
        size = 720 << 10;  // At least 720K
    nsec = size / g_sector_size;
    if (nsec < 2) {
        printf("Device is too small\n");
        return (1);
    }

    iocmd = cmd_for_offset(iocmd, g_devstart + size);

    step = nsec / num_iter;
    if (mode == BUTTERFLY_MODE_FAR)
        step /= 4;
    if (step == 0) {
        step = 1;
        num_iter /= 4;
        if (num_iter == 0)
            num_iter = 1;
    }
    half = nsec / 2;

    ReadEClock(&stime);
    /*
     * DoIO always tries IOF_QUICK, but will always wait for the I/O
     * to complete. This is regardless of whether the driver can do
     * quick I/O or not.
     */
    for (iter = 0; iter < num_iter; iter++) {
        uint64_t k = iter / 2;
        uint64_t sec;
        uint64_t pos;

        switch (mode) {
            default:
            case BUTTERFLY_MODE_AVG:
            case BUTTERFLY_MODE_FAR:
                if ((iter & 1) == 0)
                    sec = step * k;                 // left --> right
                else
                    sec = nsec - 1 - step * k;      // left <-- right
                break;
            case BUTTERFLY_MODE_CONST:
                sec = step * k;                     // left --> right
                if (iter & 1)
                    sec += half;                    // left <-- right
                break;
        }
        if (sec >= nsec)
            sec = nsec - 1;
        pos = g_devstart + sec * g_sector_size;
        if (g_verbose > 1)
            printf("%s ", llu_sector_to_str(pos));

        tio[0]->iotd_Req.io_Command = iocmd;
        tio[0]->iotd_Req.io_Actual  = pos >> 32;
        tio[0]->iotd_Req.io_Offset  = (uint32_t) pos;
        tio[0]->iotd_Req.io_Length  = g_sector_size;
        tio[0]->iotd_Req.io_Data    = buf;
        tio[0]->iotd_Req.io_Flags   = 0;
        tio[0]->iotd_Req.io_Error   = 0xa5;
        failcode = DoIO((struct IORequest *) tio[0]);
        if (failcode != 0) {
            rc++;
            break;
        }
    }
    ReadEClock(&etime);
    print_latency(diff_e_clock(&stime, &etime), iter, ' ');
    if (rc != 0)  {
        printf(" ");
        print_fail(failcode);
    }
    printf("\n");
    return (rc);
}

static int
latency_cmd_seq(UWORD iocmd, uint8_t *buf, int num_iter, struct IOExtTD **tio)
{
    int iter;
    int rc = 0;
    int failcode = 0;
    UBYTE flags = IOF_QUICK;
    struct EClockVal stime;
    struct EClockVal etime;

    uint64_t offset = 0;

    if (iocmd & CMD_FLAG_NOT_QUICK) {
        iocmd &= ~CMD_FLAG_NOT_QUICK;
        flags = 0;
    }
    if ((iocmd == CMD_READ) || (iocmd == CMD_WRITE)) {
        /* Data transfers start at the beginning of the partition */
        offset = g_devstart;
        iocmd = cmd_for_offset(iocmd, offset + BUFSIZE);
    }

    for (iter = 0; iter < num_iter; iter++) {
        tio[0]->iotd_Req.io_Command = iocmd;
        tio[0]->iotd_Req.io_Actual  = offset >> 32;
        tio[0]->iotd_Req.io_Offset  = (uint32_t) offset;
        tio[0]->iotd_Req.io_Length  = BUFSIZE;
        tio[0]->iotd_Req.io_Data    = buf;
        tio[0]->iotd_Req.io_Flags   = flags;
        tio[0]->iotd_Req.io_Error   = 0xa5;
    }

    ReadEClock(&stime);
    if (flags == 0) {
        /*
         * SendIO sets up asynch I/O, where the reply is always by message.
         * The driver should not attempt quick I/O.
         */
        for (iter = 0; iter < num_iter; iter++) {
            SendIO((struct IORequest *) tio[0]);
            failcode = WaitIO((struct IORequest *) tio[0]);
            if ((failcode != 0) && (iocmd != CMD_INVALID)) {
                rc++;
                break;
            }
        }
    } else {
        /*
         * DoIO always tries IOF_QUICK, but will always wait for the I/O
         * to complete. This is regardless of whether the driver can do
         * quick I/O or not.
         */
        for (iter = 0; iter < num_iter; iter++) {
            failcode = DoIO((struct IORequest *) tio[0]);
            if ((failcode != 0) && (iocmd != CMD_INVALID)) {
                rc++;
                break;
            }
        }
    }

    ReadEClock(&etime);
    print_latency(diff_e_clock(&stime, &etime), iter, ' ');
    if (rc != 0)  {
        printf(" ");
        print_fail(failcode);
    }
    printf("\n");
    return (rc);
}

static int
latency_cmd_par(UWORD iocmd, uint8_t *buf, int num_iter, struct IOExtTD **tio)
{
    int iter;
    int rc = 0;
    int failcode = 0;
    struct EClockVal stime;
    struct EClockVal etime;
    uint64_t offset = 0;

    if ((iocmd == CMD_READ) || (iocmd == CMD_WRITE)) {
        /* Data transfers start at the beginning of the partition */
        offset = g_devstart;
        iocmd = cmd_for_offset(iocmd, offset + BUFSIZE);
    }

    for (iter = 0; iter < num_iter; iter++) {
        tio[iter]->iotd_Req.io_Command = iocmd;
        tio[iter]->iotd_Req.io_Actual  = offset >> 32;
        tio[iter]->iotd_Req.io_Offset  = (uint32_t) offset;
        tio[iter]->iotd_Req.io_Length  = BUFSIZE;
        tio[iter]->iotd_Req.io_Data    = buf;
        tio[iter]->iotd_Req.io_Flags   = 0;
        tio[iter]->iotd_Req.io_Error   = 0xa5;
    }

    ReadEClock(&stime);
    for (iter = 0; iter < num_iter; iter++) {
        SendIO((struct IORequest *) tio[iter]);
    }
    for (iter = 0; iter < num_iter; iter++) {
        int code = WaitIO((struct IORequest *) tio[iter]);
        if (code != 0) {
            if (rc++ == 0)
                failcode = code;  // Report the first failure
        }
    }
    ReadEClock(&etime);
    print_par_result(diff_e_clock(&stime, &etime), num_iter, rc, failcode);
    return (rc);
}

/*
 * setup_rw_cdb() builds a SCSI read or write CDB which transfers BUFSIZE
 * bytes at the start of the partition. A 6-byte CDB (READ(6) / WRITE(6))
 * is used when possible, since this is the common case being measured.
 * READ(6) / WRITE(6) can only address the first 2^21 blocks, so if the
 * partition starts beyond that point, a 10-byte CDB (READ(10) / WRITE(10))
 * is used instead. That can address the first 2^32 blocks; beyond that,
 * a 16-byte CDB (READ(16) / WRITE(16)) is used. The rw6_opcode argument
 * is SCSI_READ_6_COMMAND or SCSI_WRITE_6_COMMAND.
 *
 * Returns the CDB length (6, 10, or 16), or 0 if the test must be skipped.
 */
static uint
setup_rw_cdb(scsi_generic_t *cdb, uint8_t rw6_opcode)
{
    uint64_t lba;
    uint     blocks;
    int      is_write = (rw6_opcode == SCSI_WRITE_6_COMMAND);

    memset(cdb, 0, sizeof (*cdb));
    if (g_sector_size == 0) {
        printf("Skip - unknown sector size\n");
        return (0);
    }
    lba    = g_devstart / g_sector_size;
    blocks = BUFSIZE / g_sector_size;
    if (blocks == 0) {
        printf("Skip - sector size %u is larger than buffer\n",
               g_sector_size);
        return (0);
    }

    if ((lba + blocks <= BIT(21)) && (blocks <= 256)) {
        scsi_rw_6_t *cmd = (scsi_rw_6_t *) cdb;
        cmd->opcode  = rw6_opcode;
        cmd->addr[0] = (lba >> 16) & 0x1f;  // lun << 5 is not used
        cmd->addr[1] = (uint8_t) (lba >> 8);
        cmd->addr[2] = (uint8_t) lba;
        cmd->length  = (uint8_t) blocks;    // 0 means 256 blocks
        return (sizeof (*cmd));
    }

    if (lba + blocks <= (1ULL << 32)) {
        scsi_rw_10_t *cmd = (scsi_rw_10_t *) cdb;
        cmd->opcode    = is_write ? SCSI_WRITE_10_COMMAND :
                                    SCSI_READ_10_COMMAND;
        cmd->addr[0]   = (uint8_t) (lba >> 24);
        cmd->addr[1]   = (uint8_t) (lba >> 16);
        cmd->addr[2]   = (uint8_t) (lba >> 8);
        cmd->addr[3]   = (uint8_t) lba;
        cmd->length[0] = (uint8_t) (blocks >> 8);
        cmd->length[1] = (uint8_t) blocks;
        return (sizeof (*cmd));
    } else {
        scsi_rw_16_t *cmd = (scsi_rw_16_t *) cdb;
        uint pos;
        cmd->opcode = is_write ? SCSI_WRITE_16_COMMAND :
                                 SCSI_READ_16_COMMAND;
        for (pos = 0; pos < 8; pos++)
            cmd->addr[pos] = (uint8_t) (lba >> ((7 - pos) * 8));
        cmd->length[0] = (uint8_t) (blocks >> 24);
        cmd->length[1] = (uint8_t) (blocks >> 16);
        cmd->length[2] = (uint8_t) (blocks >> 8);
        cmd->length[3] = (uint8_t) blocks;
        return (sizeof (*cmd));
    }
}

static int
latency_scsidirect_cmd_seq(uint8_t iocmd, uint8_t *buf, int num_iter,
                           struct IOExtTD **tio)
{
    int iter;
    int rc = 0;
    int failcode;
    scsi_generic_t cmd;
    uint cmdlen;
    struct SCSICmd *scmd;
    struct EClockVal stime;
    struct EClockVal etime;

    scmd = AllocMemType(sizeof (*scmd) * num_iter, memtype);
    if (scmd == NULL) {
        report_allocmem_fail(sizeof (*scmd) * num_iter, memtype);
        return (1);
    }

    cmdlen = setup_rw_cdb(&cmd, iocmd);
    if (cmdlen == 0) {
        FreeMemType(scmd, sizeof (*scmd) * num_iter);
        return (1);
    }

    for (iter = 0; iter < num_iter; iter++) {
        setup_scsidirect_cmd(scmd + iter, &cmd, cmdlen, buf, BUFSIZE);
        tio[iter]->iotd_Req.io_Command = HD_SCSICMD;
        tio[iter]->iotd_Req.io_Length  = sizeof (*scmd);
        tio[iter]->iotd_Req.io_Data    = scmd + iter;
    }

    failcode = 0;
    ReadEClock(&stime);
    for (iter = 0; iter < num_iter; iter++) {
        failcode = DoIO((struct IORequest *) tio[iter]);
        if (failcode != 0) {
            rc++;
            break;
        }
    }
    ReadEClock(&etime);
    if (failcode != 0) {
        print_fail_nl(failcode);
    } else {
        print_latency(diff_e_clock(&stime, &etime), iter, '\n');
    }

    FreeMemType(scmd, sizeof (*scmd) * num_iter);

    return (rc);
}

static int
latency_scsidirect_cmd_par(uint8_t iocmd, uint8_t *buf, int num_iter,
                           struct IOExtTD **tio)
{
    int iter;
    int rc = 0;
    int failcode = 0;
    struct EClockVal stime;
    struct EClockVal etime;
    scsi_generic_t cmd;
    uint cmdlen;
    struct SCSICmd *scmd;

    scmd = AllocMemType(sizeof (*scmd) * num_iter, memtype);
    if (scmd == NULL) {
        report_allocmem_fail(sizeof (*scmd) * num_iter, memtype);
        return (1);
    }

    cmdlen = setup_rw_cdb(&cmd, iocmd);
    if (cmdlen == 0) {
        FreeMemType(scmd, sizeof (*scmd) * num_iter);
        return (1);
    }

    for (iter = 0; iter < num_iter; iter++) {
        setup_scsidirect_cmd(scmd + iter, &cmd, cmdlen, buf, BUFSIZE);
        tio[iter]->iotd_Req.io_Command = HD_SCSICMD;
        tio[iter]->iotd_Req.io_Length  = sizeof (*scmd);
        tio[iter]->iotd_Req.io_Data    = scmd + iter;
    }

    ReadEClock(&stime);
    for (iter = 0; iter < num_iter; iter++) {
        SendIO((struct IORequest *) tio[iter]);
    }
    for (iter = 0; iter < num_iter; iter++) {
        int code = WaitIO((struct IORequest *) tio[iter]);
        if (code != 0) {
            if (rc++ == 0)
                failcode = code;  // Report the first failure
        }
    }
    ReadEClock(&etime);
    print_par_result(diff_e_clock(&stime, &etime), num_iter, rc, failcode);

    FreeMemType(scmd, sizeof (*scmd) * num_iter);

    return (rc);
}

static int
latency_read(struct IOExtTD **tio, int max_iter)
{
    int rc = 0;
    int iter;
    int num_iter;
    uint8_t *buf;

    buf = AllocMemType(BUFSIZE, memtype);
    if (buf == NULL) {
        report_allocmem_fail(BUFSIZE, memtype);
        return (1);
    }

    if (max_iter > 100)
        max_iter = 100;

    for (num_iter = 0; num_iter < max_iter; num_iter++) {
        if ((rc = open_device(tio[num_iter])) != 0) {
            printf("Open %s Unit %u: ", g_devname, g_unitno);
            print_fail_nl(rc);
            break;
        }
    }
    if (num_iter == 0) {
        rc = 1;
        goto finish_fail;
    }

    print_ltest_name("TD_CHANGENUM");
    rc += latency_cmd_seq(TD_CHANGENUM | CMD_FLAG_NOT_QUICK, buf,
                          num_iter, tio);

    if (is_user_abort()) {
user_abort:
        printf("^C abort\n");
        rc++;
        goto finish_fail;
    }

    print_ltest_name("TD_CHANGENUM quick");
    rc += latency_cmd_seq(TD_CHANGENUM, buf, num_iter, tio);

    if (is_user_abort())
        goto user_abort;

    print_ltest_name("CMD_INVALID");
    rc += latency_cmd_seq(CMD_INVALID, buf, num_iter, tio);

    if (is_user_abort())
        goto user_abort;

    print_ltest_name("CMD_START");
    rc += latency_cmd_seq(CMD_START, buf, num_iter, tio);

    if (is_user_abort())
        goto user_abort;

    print_ltest_name("CMD_READ butterfly average");
    rc += latency_butterfly(CMD_READ, buf, num_iter, tio, BUTTERFLY_MODE_AVG);

    if (is_user_abort())
        goto user_abort;

    print_ltest_name("CMD_READ butterfly far");
    rc += latency_butterfly(CMD_READ, buf, num_iter, tio, BUTTERFLY_MODE_FAR);

    if (is_user_abort())
        goto user_abort;

    print_ltest_name("CMD_READ butterfly constant");
    rc += latency_butterfly(CMD_READ, buf, num_iter, tio, BUTTERFLY_MODE_CONST);

    if (is_user_abort())
        goto user_abort;

    print_ltest_name("CMD_READ sequential");
    rc += latency_cmd_seq(CMD_READ, buf, num_iter, tio);

    if (is_user_abort())
        goto user_abort;

    print_ltest_name("CMD_READ parallel");
    rc += latency_cmd_par(CMD_READ, buf, num_iter, tio);

    if (is_user_abort())
        goto user_abort;

    if (g_sector_size != 0) {
        int rc2;
        print_ltest_name("HD_SCSICMD read sequential");
        rc2 = latency_scsidirect_cmd_seq(SCSI_READ_6_COMMAND, buf,
                                         num_iter, tio);
        if (rc2 != 0) {
            rc++;
        } else {
            if (is_user_abort())
                goto user_abort;

            print_ltest_name("HD_SCSICMD read parallel");
            rc += latency_scsidirect_cmd_par(SCSI_READ_6_COMMAND, buf,
                                             num_iter, tio);
        }
    }

finish_fail:
    FreeMemType(buf, BUFSIZE);

    for (iter = 0; iter < num_iter; iter++)
        close_device(tio[iter]);

    return (rc);
}

static int
latency_write(struct IOExtTD **tio, int max_iter)
{
    int rc = 0;
    int iter;
    int num_iter;
    uint8_t *buf;

    buf = AllocMemType(BUFSIZE, memtype);
    if (buf == NULL) {
        report_allocmem_fail(BUFSIZE, memtype);
        return (1);
    }

    if (max_iter > 100)
        max_iter = 100;

    for (num_iter = 0; num_iter < max_iter; num_iter++) {
        if ((rc = open_device(tio[num_iter])) != 0) {
            printf("Open %s Unit %u: ", g_devname, g_unitno);
            print_fail_nl(rc);
            break;
        }
    }
    if (num_iter == 0) {
        FreeMemType(buf, BUFSIZE);
        return (1);
    }

    print_ltest_name("CMD_WRITE sequential");
    rc += latency_cmd_seq(CMD_WRITE, buf, num_iter, tio);

    print_ltest_name("CMD_WRITE parallel");
    rc += latency_cmd_par(CMD_WRITE, buf, num_iter, tio);

    print_ltest_name("HD_SCSICMD write sequential");
    rc += latency_scsidirect_cmd_seq(SCSI_WRITE_6_COMMAND, buf, num_iter, tio);

    print_ltest_name("HD_SCSICMD write parallel");
    rc += latency_scsidirect_cmd_par(SCSI_WRITE_6_COMMAND, buf, num_iter, tio);

    FreeMemType(buf, BUFSIZE);

    for (iter = 0; iter < num_iter; iter++)
        close_device(tio[iter]);

    return (rc);
}

static int
drive_latency(int do_destructive)
{
    int iters;
    int i;
    int rc = 0;
    struct MsgPort *mp;
    struct IOExtTD *tio;
    struct IOExtTD **mtio;
    struct EClockVal stime;
    struct EClockVal etime;
    unsigned int ttime;

    mp = CreatePort(0, 0);
    if (mp == NULL) {
        printf("Failed to create message port\n");
        return (1);
    }

    print_ltest_name("OpenDevice / CloseDevice");

    tio = (struct IOExtTD *) CreateExtIO(mp, sizeof (struct IOExtTD));
    if (tio == NULL) {
        printf("Failed to create tio struct\n");
        rc = 1;
        goto need_delete_port;
    }

#define OPENDEVICE_MAX 10000
    ReadEClock(&stime);
    for (iters = 0; iters < OPENDEVICE_MAX; iters++) {
        if ((rc = open_device(tio)) != 0) {
            printf("Open %s Unit %u: ", g_devname, g_unitno);
            print_fail_nl(rc);
            break;
        }
        close_device(tio);
        if ((iters & 7) == 0) {
            ReadEClock(&etime);
            ttime = diff_e_clock(&stime, &etime);
            if (ttime > g_e_freq * 2) {
                iters++;
                break;
            }
        }
    }

    ReadEClock(&etime);
    ttime = diff_e_clock(&stime, &etime);

    print_latency(ttime, iters, '\n');

    print_ltest_name("OpenDevice multiple");

#define NUM_MTIO 1000
    mtio = AllocMemType(sizeof (*mtio) * NUM_MTIO, memtype);
    if (mtio == NULL) {
        report_allocmem_fail(sizeof (*mtio) * NUM_MTIO, memtype);
        rc = 1;
        goto need_delete_tio;
    }
    memset(mtio, 0, sizeof (*mtio) * NUM_MTIO);

    for (i = 0; i < NUM_MTIO; i++) {
        mtio[i] = (struct IOExtTD *) CreateExtIO(mp, sizeof (struct IOExtTD));
        if (mtio[i] == NULL) {
            printf("Failed to create tio structs\n");
            rc = 1;
            goto need_delete_mtio;
        }
    }

    if ((rc = open_device(tio)) != 0) {
        printf("Open %s Unit %u: ", g_devname, g_unitno);
        print_fail_nl(rc);
        rc = 1;
        goto need_delete_mtio;
    }
    /* Note that tio is left open, so driver is maintaining state here... */

    ReadEClock(&stime);
    for (iters = 0; iters < NUM_MTIO; iters++) {
        if ((rc = open_device(mtio[iters])) != 0) {
            printf("Open %s Unit %u: ", g_devname, g_unitno);
            print_fail_nl(rc);
            break;
        }
        if ((iters & 7) == 0) {
            ReadEClock(&etime);
            ttime = diff_e_clock(&stime, &etime);
            if ((ttime > g_e_freq * 2) ||
                (iters > NUM_MTIO - 7)) {
                iters++;
                break;
            }
        }
    }
    ReadEClock(&etime);
    ttime = diff_e_clock(&stime, &etime);
    print_latency(ttime, iters, '\n');

    print_ltest_name("CloseDevice multiple");
    ReadEClock(&stime);
    for (i = 0; i < iters; i++)
        close_device(mtio[i]);
    ReadEClock(&etime);
    ttime = diff_e_clock(&stime, &etime);
    print_latency(ttime, iters, '\n');

    if ((latency_getgeometry(mtio, NUM_MTIO / 4)) ||
        (latency_read(mtio, NUM_MTIO)) ||
        (do_destructive && latency_write(mtio, NUM_MTIO))) {
        rc = 1;
    }

    close_device(tio);

need_delete_mtio:
    for (i = 0; i < NUM_MTIO; i++)
        if (mtio[i] != NULL)
            DeleteExtIO((struct IORequest *) mtio[i]);
    FreeMemType(mtio, sizeof (*mtio) * NUM_MTIO);

need_delete_tio:
    DeleteExtIO((struct IORequest *) tio);

need_delete_port:
    DeletePort(mp);
    return (rc);
}

#define PERF_BUF_SIZE (512 << 10)
#define NUM_TIO       4
#define MAX_NUM_TIO   32

/*
 * print_perf() reports the transfer rate of bytes transferred in ttime
 * EClock ticks (freq ticks per second). Rates are in units of 1000 bytes
 * (KB/sec), or 1000000 bytes (MB/sec) at 100 MB/sec or more. The rate is
 * calculated from the byte count before it is scaled for display, so
 * that a small transfer total does not lose precision.
 */
static void
print_perf(uint ttime, uint itime, uint freq, uint64_t bytes, int is_write,
           uint xfer_size)
{
    uint tsec;
    uint trem;
    uint rep = (uint) (bytes / 1000);
    char c1 = 'K';
    char c2 = 'K';
    uint cpu_load;
    uint64_t rate;

    if (rep >= 10000) {
        rep /= 1000;
        c1 = 'M';
    }
    if (ttime == 0)
        ttime = 1;
    if (itime > ttime)
        itime = ttime;
    cpu_load = (ttime - itime) * 100 / ttime;  // Round down on purpose
    tsec = ttime / freq;
    trem = ttime % freq;

    rate = bytes * freq / ttime;  // Bytes per second
    if (rate >= 100000000) {
        /* Transfer rate >= 100 MB/sec */
        rate /= 1000000;
        c2 = 'M';
    } else {
        rate /= 1000;
    }

    if (g_verbose) {
        printf("%4u %cB %s in %2u.%02u sec: %3u KB xfer: ",
               rep, c1, is_write ? "write" : "read ",
               tsec, trem * 100 / freq, xfer_size / 1024);
    }
    printf("%13u %cB/sec", (uint) rate, c2);
    if (flag_cpu_utilization)
        printf(" (%u%% CPU)", cpu_load);
    printf("\n");
}

static uint32_t user_perf_size = 0;
static uint     user_num_tio   = 0;

/*
 * bandwidth_wait() waits for a benchmark request to complete, and adds the
 * bytes it transferred (io_Actual) to *bytes. Returns 0 if the full length
 * was transferred; otherwise reports the error or short transfer, and
 * returns nonzero.
 */
static int
bandwidth_wait(UWORD iocmd, struct IOExtTD *tio, uint64_t *bytes)
{
    int failcode = WaitIO((struct IORequest *) tio);
    uint32_t len = tio->iotd_Req.io_Length;
    uint32_t actual = tio->iotd_Req.io_Actual;

    if (failcode == 0)
        failcode = tio->iotd_Req.io_Error;
    if (failcode == 0) {
        *bytes += (actual < len) ? actual : len;
        if (actual >= len)
            return (0);
    }
    printf("  %s ", (iocmd == CMD_READ) ? "Read" : "Write");
    if (failcode == 0)
        printf("short transfer (%u of %u bytes)", U32(actual), U32(len));
    else
        print_fail(failcode);
    printf(" at 0x%x\n", U32(tio->iotd_Req.io_Offset));
    return (1);
}

#define BW_STEP_SECS 1  // Time to run each transfer size

static int
run_bandwidth(UWORD iocmd, struct IOExtTD **tio, uint8_t **buf,
              uint32_t bufsize, uint num_tio)
{
    uint i;
    int rc = 0;
    int aborted = 0;
    int rc_step;
    uint32_t issued;
    uint cur = 0;
    uint64_t pos;
    uint64_t limit = part_size();
    struct EClockVal stime;
    struct EClockVal etime;
    uint32_t freq;
    uint32_t diff_ticks;
    uint64_t xfer_bytes;
    uint32_t idle_ticks;

    int rep;

    for (rep = 0; rep < 10; rep++) {
        pos = 0;
        issued = 0;

        if ((limit != 0) && (bufsize > limit)) {
            printf("%s is too small for %u byte transfers\n",
                   (g_devend != 0) ? "Partition" : "Device", U32(bufsize));
            rc++;
            break;
        }

        rc_step = rc;
        print_perf_type((iocmd == CMD_READ) ? 0 : 1, bufsize);
        ReadEClock(&stime);
        idle_time = 0;
        xfer_bytes = 0;
        for (;;) {
            if (issued & BIT(cur)) {
                issued &= ~BIT(cur);
                if (bandwidth_wait(iocmd, tio[cur], &xfer_bytes) != 0) {
                    rc++;
                    break;
                }
                if (is_user_abort()) {
                    aborted = 1;
                    break;
                }
                /*
                 * Each transfer size runs for a fixed time, rather than a
                 * fixed number of transfers, so that the fixed costs (the
                 * first request's latency and filling and draining the
                 * queue) are a similar small part of every measurement.
                 */
                ReadEClock(&etime);
                if (diff_e_clock(&stime, &etime) >= g_e_freq * BW_STEP_SECS)
                    break;
            }

            if ((limit != 0) && (pos + bufsize > limit))
                pos = 0;  // Wrap to stay within the partition / device
            {
                uint64_t abs = g_devstart + pos;
                tio[cur]->iotd_Req.io_Command =
                    cmd_for_offset(iocmd, abs + bufsize);
                tio[cur]->iotd_Req.io_Actual = abs >> 32;
                tio[cur]->iotd_Req.io_Offset = (uint32_t) abs;
            }
            tio[cur]->iotd_Req.io_Data = buf[cur];
            tio[cur]->iotd_Req.io_Length = bufsize;
            SendIO((struct IORequest *) tio[cur]);
            issued |= BIT(cur);
            pos += bufsize;
            if (++cur >= num_tio)
                cur = 0;
        }
        /*
         * Wait for all outstanding requests (also after error or ^C).
         * After ^C, they are aborted first: a slow device (floppy) could
         * otherwise take minutes to finish them. Drivers which do not
         * support AbortIO() just complete them normally.
         */
        if (aborted) {
            for (i = 0; i < num_tio; i++)
                if (issued & BIT(i))
                    AbortIO((struct IORequest *) tio[i]);
        }
        for (i = 0; i < num_tio; i++) {
            if (issued & BIT(cur)) {
                issued &= ~BIT(cur);
                if (aborted)
                    (void) WaitIO((struct IORequest *) tio[cur]);
                else if (bandwidth_wait(iocmd, tio[cur], &xfer_bytes) != 0)
                    rc++;
            }
            if (++cur >= num_tio)
                cur = 0;
        }
        if (aborted) {
            printf("^C abort\n");
            rc++;
            break;
        }

        idle_ticks = idle_time;
        freq = ReadEClock(&etime);
        diff_ticks = diff_e_clock(&stime, &etime);

        /* After an I/O error, a rate would be misleading: only the error */
        if (rc == rc_step) {
            print_perf(diff_ticks, idle_ticks, freq, xfer_bytes,
                       (iocmd == CMD_READ) ? 0 : 1, bufsize);
        }
        bufsize >>= 2;
        if (bufsize < 16384)
            break;

        if (is_user_abort()) {
            printf("^C abort\n");
            rc++;
            break;
        }
        if (user_perf_size != 0)
            break;  // Only run the specified size
    }

    return (rc);
}

static const char *
memtype_str(uint32_t mem)
{
    const char *type;
    if (((mem >= MEMTYPE_CHIP_START) && (mem < MEMTYPE_CHIP_START + MEMTYPE_CHIP_SIZE)) || (mem == MEMTYPE_CHIP)) {
        type = "Chip";
    } else if ((mem >= MEMTYPE_SLOW_START) && (mem < MEMTYPE_SLOW_START + MEMTYPE_SLOW_SIZE)) {
        type = "Slow";
    } else if (((mem >= MEMTYPE_MB_START) && (mem < MEMTYPE_MB_START + MEMTYPE_MB_SIZE)) ||
               (mem == MEMTYPE_FAST))  {
        type = "MB";
    } else if (((mem >= MEMTYPE_COPROC_START) && (mem < MEMTYPE_COPROC_START + MEMTYPE_COPROC_SIZE)) ||
               (mem == MEMTYPE_COPROC))  {
        type = "Coprocessor";
    } else if (mem == MEMTYPE_ZORRO) {
        type = "Zorro";
    } else if ((mem == MEMTYPE_ZORRO2) ||
               ((mem >= E_MEMORYBASE) && (mem < E_MEMORYBASE + E_MEMORYSIZE))) {
        type = "Zorro II";
    } else if ((mem == MEMTYPE_ZORRO3) ||
               ((mem >= MEMTYPE_ZORRO3_START) && (mem < MEMTYPE_ZORRO3_START + MEMTYPE_ZORRO3_SIZE))) {
        type = "Zorro III";
    } else if (((mem >= MEMTYPE_ACCEL_START) && (mem < MEMTYPE_ACCEL_START + MEMTYPE_ACCEL_SIZE)) ||
                (mem == MEMTYPE_ACCEL)) {
        type = "Accelerator";
    } else {
        type = "Unknown";
    }
    return (type);
}

static void
report_allocmem_fail(uint bufsize, uint memtype)
{
    printf("  AllocMem %x (%s) fail\n", bufsize, memtype_str(memtype));
}

static void
show_memlist(void)
{
    struct ExecBase *eb = SysBase;
    struct MemHeader *mem;
    struct MemChunk  *chunk;

    Forbid();
    for (mem = (struct MemHeader *)eb->MemList.lh_Head;
         mem->mh_Node.ln_Succ != NULL;
         mem = (struct MemHeader *)mem->mh_Node.ln_Succ) {
        uint32_t    size = (uint8_t *) mem->mh_Upper - (uint8_t *) mem;
        uint32_t    upper = (uintptr_t) mem->mh_Upper;
        uint16_t    attr  = mem->mh_Attributes;
        const char *type = memtype_str((uint32_t) mem);

        printf("%s RAM at %p size=0x%x", type, (void *) mem, U32(size));
        if (g_verbose) {
            if (attr & MEMF_PUBLIC)
                printf(" PUBLIC");
            if (attr & MEMF_CHIP)
                printf(" CHIP");
            if (attr & MEMF_FAST)
                printf(" FAST");
            if (attr & MEMF_LOCAL)
                printf(" LOCAL");
            if (attr & MEMF_24BITDMA)
                printf(" 24BITDMA");
            if (attr & MEMF_KICK)
                printf(" KICK");
            printf(" pri=%d \"%.20s\"",
                   mem->mh_Node.ln_Pri, mem->mh_Node.ln_Name);
        }
        printf("\n");

        for (chunk = mem->mh_First; chunk != NULL;
             chunk = chunk->mc_Next) {
            uint bytes = chunk->mc_Bytes;
            if ((g_verbose > 1) || (chunk->mc_Bytes >= 512)) {
                printf("  %p 0x%x", (void *) chunk, bytes);
                if ((uintptr_t) chunk + bytes > upper)
                    printf(" ** CORRUPT: 0x%x is maximum size",
                           U32(upper - (uintptr_t) chunk));
                printf("\n");
            }
            if ((uintptr_t) chunk + bytes >= upper)
                break;  // Corrupt memory list?
        }
    }
    Permit();
}

/*
 * drive_is_slow() reads 4 KB from the device to determine whether it is
 * slow, such as a floppy drive, where the 512 KB and 128 KB transfer
 * benchmarks would take minutes. The first read is not timed, as it may
 * include starting the motor (floppy) or spinning up from standby. The
 * timed read is at a different offset: a floppy driver reads whole
 * tracks, so reading the same data again would come from its buffer.
 * A floppy takes at least a revolution (200 ms) plus a seek, while a hard
 * drive or SSD takes a few ms. Errors are not reported here (the
 * benchmark will report them); the device is then considered not slow.
 */
#define SLOW_PROBE_LEN    4096
#define SLOW_PROBE_OFFSET (64 << 10)
#define SLOW_PROBE_MS     100

static int
drive_is_slow(struct IOExtTD *tio, uint8_t *buf)
{
    struct EClockVal stime;
    struct EClockVal etime;
    uint64_t limit = part_size();
    uint64_t offset = SLOW_PROBE_OFFSET;

    if (limit != 0) {
        if (limit < 2 * SLOW_PROBE_LEN)
            return (0);  // Too small to matter
        if (offset + SLOW_PROBE_LEN > limit)
            offset = limit - SLOW_PROBE_LEN;
    }
    if (do_read_cmd(tio, 0, SLOW_PROBE_LEN, buf, g_has_nsd) != 0)
        return (0);
    ReadEClock(&stime);
    if (do_read_cmd(tio, offset, SLOW_PROBE_LEN, buf, g_has_nsd) != 0)
        return (0);
    ReadEClock(&etime);
    return (diff_e_clock(&stime, &etime) > g_e_freq / 1000 * SLOW_PROBE_MS);
}

static int
drive_benchmark(int do_destructive, uint32_t memtype)
{
    uint32_t bench_size;
    struct IOExtTD *tio[MAX_NUM_TIO];
    uint8_t *buf[MAX_NUM_TIO];
    uint32_t opened;
    uint32_t perf_buf_size = PERF_BUF_SIZE;
    struct MsgPort *mp;
    size_t i;
    int rc = 0;
    uint num_tio = NUM_TIO;

    if (user_perf_size != 0)
        perf_buf_size = user_perf_size;
    if (user_num_tio != 0)
        num_tio = user_num_tio;
    if (num_tio > MAX_NUM_TIO)
        num_tio = MAX_NUM_TIO;

    mp = CreatePort(0, 0);
    if (mp == NULL) {
        printf("Failed to create message port\n");
        return (1);
    }

    memset(tio, 0, sizeof (tio));
    for (i = 0; i < num_tio; i++) {
        tio[i] = (struct IOExtTD *) CreateExtIO(mp, sizeof (struct IOExtTD));
        if (tio[i] == NULL) {
            printf("Failed to create tio struct\n");
            rc = 1;
            goto create_tio_fail;
        }
    }

    opened = 0;
    for (i = 0; i < num_tio; i++) {
        if ((rc = open_device(tio[i])) != 0) {
            printf("Open %s Unit %u: ", g_devname, g_unitno);
            print_fail_nl(rc);
            rc = 1;
            goto opendevice_fail;
        }
        opened |= BIT(i);
    }

    /*
     * The device size is needed so that transfers stay within the device
     * (run_bandwidth() wraps at the end). If it can not be determined, the
     * benchmark still runs, but transfers are not bounded.
     */
    if (g_devsize == 0)
        (void) get_devsize(tio[0]);

    memset(buf, 0, sizeof (buf));
try_again:
    for (i = 0; i < num_tio; i++) {
        buf[i] = (uint8_t *) AllocMemType(perf_buf_size, memtype);
        if (buf[i] == NULL) {
            if (perf_buf_size > 8192) {
                /* Restart loop, asking for a smaller buffer */
                size_t j;
                for (j = 0; j < i; j++) {
                    FreeMemType(buf[j], perf_buf_size);
                    buf[j] = NULL;
                }
                perf_buf_size /= 2;
                goto try_again;
            }
            printf("Unable to allocate ");
            if (memtype != MEMTYPE_ANY)
                printf("%s ", memtype_str((uint32_t) memtype));
            printf("RAM");
            if (memtype > MEMTYPE_MAX)
                printf(" at 0x%08x", U32(memtype));
            printf("\n");
            rc = 1;
            goto allocmem_fail;
        }
    }
    printf("Test %s %u with %s RAM",
           g_devname, g_unitno, memtype_str((uint32_t) buf[0]));
    if (g_verbose) {
        for (i = 0; i < num_tio; i++)
            printf(" %08x", U32(buf[i]));
    }
    printf("\n");

    /* On a slow device (floppy), skip the 512 KB and 128 KB transfers */
    bench_size = perf_buf_size;
    if ((user_perf_size == 0) && (bench_size > (32 << 10)) &&
        drive_is_slow(tio[0], buf[0])) {
        bench_size = 32 << 10;
        if (g_verbose)
            printf("Slow device: only 32 KB transfers are tested\n");
    }

    rc += run_bandwidth(CMD_READ, tio, buf, bench_size, num_tio);

    if (do_destructive && (rc == 0))
        rc += run_bandwidth(CMD_WRITE, tio, buf, bench_size, num_tio);

    g_turn_motor_off = 1;

allocmem_fail:
    for (i = 0; i < num_tio; i++)
        if (buf[i] != NULL)
            FreeMemType(buf[i], perf_buf_size);

opendevice_fail:
    for (i = 0; i < num_tio; i++)
        if ((opened & BIT(i)) != 0)
            close_device(tio[i]);

create_tio_fail:
    for (i = 0; i < num_tio; i++)
        if (tio[i] != NULL)
            DeleteExtIO((struct IORequest *) tio[i]);

    DeletePort(mp);

    return (rc);
}

static int
do_read_cmd(struct IOExtTD *tio, uint64_t offset, uint len, void *buf, int nsd)
{
    offset += g_devstart;
    tio->iotd_Req.io_Command = CMD_READ;
    tio->iotd_Req.io_Actual  = 0;
    tio->iotd_Req.io_Offset  = offset;
    tio->iotd_Req.io_Length  = len;
    tio->iotd_Req.io_Data    = buf;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;

    /* Ensure read does not go past end of partition (only in partition mode */
    if ((g_devend != 0) && (offset + len > g_devend))
        return (ERROR_PAST_END);

    if (((offset + len) >> 32) > 0) {
        /* Need TD64 or NSD */
        if (nsd)
            tio->iotd_Req.io_Command = NSCMD_TD_READ64;
        else
            tio->iotd_Req.io_Command = TD_READ64;
        tio->iotd_Req.io_Actual = offset >> 32;
    }
    return (DoIO((struct IORequest *) tio));
}

static int
do_write_cmd(struct IOExtTD *tio, uint64_t offset, uint len, void *buf, int nsd)
{
    offset += g_devstart;
    tio->iotd_Req.io_Command = CMD_WRITE;
    tio->iotd_Req.io_Actual  = 0;
    tio->iotd_Req.io_Offset  = offset;
    tio->iotd_Req.io_Length  = len;
    tio->iotd_Req.io_Data    = buf;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;

    if ((g_devend != 0) && (offset + len > g_devend))
        return (ERROR_PAST_END);

    if (((offset + len) >> 32) > 0) {
        /* Need TD64 or NSD */
        if (nsd)
            tio->iotd_Req.io_Command = NSCMD_TD_WRITE64;
        else
            tio->iotd_Req.io_Command = TD_WRITE64;
        tio->iotd_Req.io_Actual = offset >> 32;
    }
    return (DoIO((struct IORequest *) tio));
}

static int
check_write(struct IOExtTD *tio, uint8_t *wbuf, uint8_t *rbuf, uint bufsize,
            uint64_t offset, int has_nsd)
{
    int rc;
    memset(rbuf, 0xa5, bufsize);
    rc = do_read_cmd(tio, offset, bufsize, rbuf, has_nsd);
    if (rc == 0) {
        if (memcmp(wbuf, rbuf, bufsize) == 0) {
            printf("Success");
        } else {
            printf("Miscompare");
            return (1);
        }
    } else {
        printf("V");
        print_fail(rc);
    }
    return (rc);
}

static int
test_cmd_getgeometry(struct IOExtTD *tio)
{
    int rc;
    struct DriveGeometry dg;

    print_test_name("TD_GETGEOMETRY");

    rc = do_getgeometry(tio, &dg);
    if (rc == 0) {
        printf("Success  %u x %u  C=%u H=%u "
               "S=%u Type=%u%s\n",
               U32(dg.dg_TotalSectors), U32(dg.dg_SectorSize), U32(dg.dg_Cylinders),
               U32(dg.dg_Heads), U32(dg.dg_TrackSectors), U32(dg.dg_DeviceType),
               (dg.dg_Flags & DGF_REMOVABLE) ? " Removable" : "");
        g_devsize = (uint64_t) dg.dg_TotalSectors * dg.dg_SectorSize;
        g_sector_size = dg.dg_SectorSize;
    } else {
        print_fail_nl(rc);
    }
    return (rc);
}

static int
test_td_changenum(struct IOExtTD *tio)
{
    int rc;

    tio->iotd_Req.io_Command = TD_CHANGENUM;
    tio->iotd_Req.io_Actual  = 0xa5;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 0;
    tio->iotd_Req.io_Data    = NULL;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("TD_CHANGENUM");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        g_changenum = tio->iotd_Req.io_Actual;
        printf("Success  Count=%u\n", U32(tio->iotd_Req.io_Actual));
    } else {
        print_fail_nl(rc);
    }
    return (rc);
}

static int
test_td_changestate(struct IOExtTD *tio)
{
    int rc;
    tio->iotd_Req.io_Command = TD_CHANGESTATE;
    tio->iotd_Req.io_Actual  = 0xa5;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 0;
    tio->iotd_Req.io_Data    = NULL;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("TD_CHANGESTATE");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        printf("Success  ");
        if (tio->iotd_Req.io_Actual == 0)
            printf("Disk present\n");
        else
            printf("No disk present\n");
    } else {
        print_fail_nl(rc);
    }
    return (rc);
}

static int
test_td_protstatus(struct IOExtTD *tio)
{
    int rc;
    tio->iotd_Req.io_Command = TD_PROTSTATUS;
    tio->iotd_Req.io_Actual  = 0xa5;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 0;
    tio->iotd_Req.io_Data    = 0;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("TD_PROTSTATUS");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        printf("Success  ");
        if (tio->iotd_Req.io_Actual == 0)
            printf("Unprotected\n");
        else
            printf("Protected\n");
    } else {
        print_fail_nl(rc);
    }
    return (rc);
}

static int
test_td_getdrivetype(struct IOExtTD *tio)
{
    int rc;
    tio->iotd_Req.io_Command = TD_GETDRIVETYPE;
    tio->iotd_Req.io_Actual  = 0xa5;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 0;
    tio->iotd_Req.io_Data    = NULL;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("TD_GETDRIVETYPE");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        printf("Success  Type=%u %s\n",
               U32(tio->iotd_Req.io_Actual),
               floppy_type_string(tio->iotd_Req.io_Actual));
    } else {
        print_fail_nl(rc);
    }
    return (rc);
}

static int
test_td_getnumtracks(struct IOExtTD *tio)
{
    int rc;
    tio->iotd_Req.io_Command = TD_GETNUMTRACKS;
    tio->iotd_Req.io_Actual  = 0xa5;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 0;
    tio->iotd_Req.io_Data    = NULL;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("TD_GETNUMTRACKS");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        printf("Success  Tracks=%u\n", U32(tio->iotd_Req.io_Actual));
    } else {
        print_fail_nl(rc);
    }
    return (rc);
}

static int
test_hd_scsicmd_inquiry(struct IOExtTD *tio)
{
    /* HD_SCSICMD (SCSI direct) */
    int rc;

    /* do_scsi_inquiry() takes the unit number; it derives the LUN itself */
    scsi_inquiry_data_t *inq_res;
    rc = do_scsi_inquiry(tio, g_unitno, &inq_res);
    print_test_name("SCSICMD Inquiry");
    if (rc == 0) {
        printf("Success  V='%.*s' P='%.*s' R='%.*s' DT=0x%x",
               (int) sizeof (inq_res->vendor),
               trim_spaces(inq_res->vendor, sizeof (inq_res->vendor)),
               (int) sizeof (inq_res->product),
               trim_spaces(inq_res->product, sizeof (inq_res->product)),
               (int) sizeof (inq_res->revision),
               trim_spaces(inq_res->revision, sizeof (inq_res->revision)),
               U32(inq_res->device & SID_TYPE));
        if (inq_res->dev_qual2 & SID_QUAL_LU_NOTPRESENT)
            printf(" Removed");
        else if (inq_res->dev_qual2 & SID_REMOVABLE)
            printf(" Removable");
        if (inq_res->flags3 & SID_SftRe)
            printf(" SftRe");
        if (inq_res->flags3 & SID_CmdQue)
            printf(" CmdQue");
        if (inq_res->flags3 & SID_Linked)
            printf(" Linked");
        if (inq_res->flags3 & SID_Sync)
            printf(" Sync");
        if (inq_res->flags3 & (SID_WBus16 | SID_WBus32))
            printf(" Wide");
        if (inq_res->flags3 & SID_RelAdr)
            printf(" Rel");
        printf("\n");
        FreeMemType(inq_res, sizeof (*inq_res));
    } else {
        print_fail_nl(rc);
    }
    return (rc);
}

/*
 * test_hd_scsicmd_tur
 * -------------------
 * Issue a Test Unit Ready command using HD_SCSICMD (SCSI direct)
 */
static int
test_hd_scsicmd_tur(struct IOExtTD *tio)
{
    int rc = do_scsi_testunitready(tio, g_lun);
    print_test_name("SCSICMD TUR");
    if (rc == 0) {
        printf("Success  Ready\n");
    } else {
        int key = SSD_SENSE_KEY(g_sense_data);
        if (SENSE_VALID() && (key == SKEY_NOT_READY)) {
            printf("Success  ");
            if (SSD_SENSE_ASC(g_sense_data) == 0x3a) {
                printf("Media not present\n");
            } else {
                printf("Not ready (ASC=%02x ASCQ=%02x)\n",
                       SSD_SENSE_ASC(g_sense_data),
                       SSD_SENSE_ASCQ(g_sense_data));
            }
        } else {
            print_fail(rc);
            if (SENSE_VALID()) {
                printf(" Sense Key %x (ASC=%02x ASCQ=%02x)",
                        SSD_SENSE_KEY(g_sense_data),
                        SSD_SENSE_ASC(g_sense_data),
                        SSD_SENSE_ASCQ(g_sense_data));
            }
            printf("\n");
            return (rc);
        }
    }

    /* Ready, or a valid "not ready" (such as no media) response */
    return (0);
}

static int
get_changenum(struct IOExtTD *tio)
{
    int rc;
    tio->iotd_Req.io_Command = TD_CHANGENUM;
    tio->iotd_Req.io_Actual  = 0xa5;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 0;
    tio->iotd_Req.io_Data    = NULL;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0)
        g_changenum = tio->iotd_Req.io_Actual;

    return (rc);
}

/*
 * test_etd_command
 * ----------------
 * Issue an extended trackdisk command first with an invalid change count
 * (which should be rejected) and then with the correct change count.
 * The offset is relative to the start of the partition (if any). For the
 * NSCMD_ETD_*64 commands, io_Actual holds the high 32 bits of the offset.
 * The 32-bit ETD commands can not address beyond 4GB.
 *
 * Returns -1 if the command was skipped.
 */
static int
test_etd_command(struct IOExtTD *tio, UWORD cmd, const char *cmd_name,
                 uint len, void *buf, uint64_t rel_offset)
{
    int      rc;
    uint64_t abs = g_devstart + rel_offset;
    int      is64 = ((cmd & 0xfffc) == NSCMD_ETD_READ64);
    ULONG    io_actual = is64 ? (ULONG) (abs >> 32) : 0;

    if (cmd_name != NULL)
        print_test_name(cmd_name);
    if (!is64 && (((abs + len) >> 32) != 0)) {
        printf("Skip - offset is beyond 4GB\n");
        return (-1);
    }
    if (outside_partition(rel_offset, len)) {
        printf("Skip - beyond end of partition\n");
        return (-1);
    }

    tio->iotd_Req.io_Command = cmd;
    tio->iotd_Req.io_Offset  = (uint32_t) abs;
    tio->iotd_Req.io_Actual  = io_actual;
    tio->iotd_Req.io_Length  = len;
    tio->iotd_Req.io_Data    = buf;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    tio->iotd_Count          = 0;  /* Should cause TDERR_DiskChanged */
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        printf("Fail - command accepted with invalid iotd_Count\n");
        return (1);
    }
    if (rc != TDERR_DiskChanged) {
        print_fail_nl(rc);
        return (1);
    }

    /* Try with expected change number */
    if (get_changenum(tio) != 0) {
        printf("Fail - could not acquire change count with TD_CHANGENUM\n");
        return (1);
    }

    tio->iotd_Req.io_Command = cmd;
    tio->iotd_Req.io_Offset  = (uint32_t) abs;
    tio->iotd_Req.io_Actual  = io_actual;
    tio->iotd_Req.io_Length  = len;
    tio->iotd_Req.io_Data    = buf;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    tio->iotd_Count          = g_changenum;
    rc = DoIO((struct IORequest *) tio);
    if (rc != 0)
        print_fail_nl(rc);

    return (rc);
}

static int
get_args_1(uint *arg1)
{
    if (cur_test_args == NULL)
        return (0);

    if (cur_test_args->arg_count > 0)
        *arg1 = cur_test_args->arg[0];

    if (cur_test_args->arg_count > 1) {
        printf("Too many args for this command\n");
        return (1);
    }
    return (0);
}

static int
get_args_2(uint *arg1, uint *arg2)
{
    if (cur_test_args == NULL)
        return (0);

    if (cur_test_args->arg_count > 0)
        *arg1 = cur_test_args->arg[0];

    if (cur_test_args->arg_count > 1)
        *arg2 = cur_test_args->arg[1];

    if (cur_test_args->arg_count > 2) {
        printf("Too many args for this command\n");
        return (1);
    }
    return (0);
}

static int
get_args_3(uint *arg1, uint *arg2, uint *arg3)
{
    if (cur_test_args == NULL)
        return (0);

    if (cur_test_args->arg_count > 0)
        *arg1 = cur_test_args->arg[0];

    if (cur_test_args->arg_count > 1)
        *arg2 = cur_test_args->arg[1];

    if (cur_test_args->arg_count > 2)
        *arg3 = cur_test_args->arg[2];

    if (cur_test_args->arg_count > 3) {
        printf("Too many args for this command\n");
        return (1);
    }
    return (0);
}

static int
test_cmd_read(struct IOExtTD *tio)
{
    int rc;
    uint8_t **buf = g_tbuf;
    uint readoffset = 0;
    uint bufsize = BUFSIZE;

    if (get_args_2(&bufsize, &readoffset))
        return (1);
    if (bufsize > BUFSIZE)
        bufsize = BUFSIZE;

    /* Read */
    memset(buf[0], 0x5a, bufsize);
    print_test_name("CMD_READ");
    rc = do_read_cmd(tio, readoffset, bufsize, buf[0], 0);
    if (rc == 0) {
        uint count;
        for (count = 0; count < bufsize; count++)
            if (buf[0][count] != 0x5a)
                break;
        if (count == bufsize) {
            printf("No data\n");
            return (1);
        } else {
            printf("Success\n");
        }
    } else {
        print_fail_nl(rc);
        return (1);
    }
    memcpy(buf[2], buf[0], bufsize);  // Keep a copy

    return (rc);
}

static int
test_etd_read(struct IOExtTD *tio)
{
    int rc;
    uint8_t **buf = g_tbuf;
    uint readoffset = 0;
    uint bufsize = BUFSIZE;

    if (get_args_2(&bufsize, &readoffset))
        return (1);
    if (bufsize > BUFSIZE)
        bufsize = BUFSIZE;

    memset(buf[1], 0xa5, bufsize);
    rc = test_etd_command(tio, ETD_READ, "ETD_READ", bufsize, buf[1],
                          readoffset);
    if (rc == -1)
        return (0);  // Skipped
    if (rc == 0) {
        rc = do_read_cmd(tio, readoffset, bufsize, buf[0], 0);
        if (rc != 0) {
            print_fail(rc);
            printf(" - read verify operation failed\n");
            return (1);
        }
        if (memcmp(buf[0], buf[1], bufsize) == 0) {
            printf("Success\n");
        } else {
            printf("Miscompare\n");
            return (1);
        }
    }
    return (rc);
}

static int
test_td_read64(struct IOExtTD *tio)
{
    int rc;
    uint8_t **buf = g_tbuf;
    uint bufsize = BUFSIZE;
    uint readoffset = 0;
    uint readoffsethi = 0;
    uint64_t rel_offset;
    uint64_t io_offset;

    if (get_args_3(&bufsize, &readoffset, &readoffsethi))
        return (1);

    if (bufsize > BUFSIZE)
        bufsize = BUFSIZE;

    rel_offset = ((uint64_t) readoffsethi << 32) | readoffset;
    io_offset = rel_offset + g_devstart;

    memset(buf[1], 0xa5, bufsize);
    tio->iotd_Req.io_Command = TD_READ64;
    tio->iotd_Req.io_Actual  = io_offset >> 32;  // High 32 bits
    tio->iotd_Req.io_Offset  = (uint32_t) io_offset;
    tio->iotd_Req.io_Length  = bufsize;
    tio->iotd_Req.io_Data    = buf[1];
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("TD_READ64");
    rc = DoIO((struct IORequest *) tio);
    if ((rc == 0) && (((io_offset + bufsize) >> 32) == 0)) {
        /* Verify with CMD_READ (partition-relative offset) */
        rc = do_read_cmd(tio, rel_offset, bufsize, buf[0], 0);
        if (rc != 0) {
            print_fail(rc);
            printf(" - read verify operation failed\n");
            return (1);
        }
        if (memcmp(buf[0], buf[1], bufsize) == 0) {
            printf("Success\n");
        } else {
            printf("Miscompare\n");
            return (1);
        }
    } else {
        print_fail_nl(rc);
    }
    return (rc);
}

static int
test_nsd_devicequery(struct IOExtTD *tio)
{
    int rc;
    uint8_t **buf = g_tbuf;

    NSDeviceQueryResult_t *nsd_r = (NSDeviceQueryResult_t *) buf[1];
    memset(buf[1], 0xa5, BUFSIZE);
    nsd_r->DevQueryFormat = 0;
    tio->iotd_Req.io_Command = NSCMD_DEVICEQUERY;
    tio->iotd_Req.io_Actual  = 0;  // High 64 bits
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = sizeof (NSDeviceQueryResult_t);
    tio->iotd_Req.io_Data    = buf[1];
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("NSCMD_DEVICEQUERY");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        if (nsd_r->DevQueryFormat != 0) {
            printf("Unexpected DevQueryFormat %"PRIx32, nsd_r->DevQueryFormat);
            g_has_nsd = 0;
        } else if (nsd_r->DeviceType != NSDEVTYPE_TRACKDISK) {
            printf("Unexpected DeviceType %x", U32(nsd_r->DeviceType));
            g_has_nsd = 0;
        } else {
            printf("Success");
            g_has_nsd = 1;
        }

        memset(buf[1], 0xa5, BUFSIZE);
    } else {
        print_fail(rc);
        g_has_nsd = 0;
    }
    printf("\n");
    return (rc);
}

static int
test_nscmd_td_read64(struct IOExtTD *tio)
{
    int rc;
    uint8_t **buf = g_tbuf;
    uint bufsize = BUFSIZE;
    uint readoffset = 0;
    uint readoffsethi = 0;
    uint64_t rel_offset;
    uint64_t io_offset;

    if (get_args_3(&bufsize, &readoffset, &readoffsethi))
        return (1);

    if (bufsize > BUFSIZE)
        bufsize = BUFSIZE;

    rel_offset = ((uint64_t) readoffsethi << 32) | readoffset;
    io_offset = rel_offset + g_devstart;

    memset(buf[1], 0xa5, bufsize);
    tio->iotd_Req.io_Command = NSCMD_TD_READ64;
    tio->iotd_Req.io_Actual  = io_offset >> 32;  // High 32 bits
    tio->iotd_Req.io_Offset  = (uint32_t) io_offset;
    tio->iotd_Req.io_Length  = bufsize;
    tio->iotd_Req.io_Data    = buf[1];
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("NSCMD_TD_READ64");
    rc = DoIO((struct IORequest *) tio);
    if ((rc == 0) && (((io_offset + bufsize) >> 32) == 0)) {
        /* Verify with CMD_READ (partition-relative offset) */
        rc = do_read_cmd(tio, rel_offset, bufsize, buf[0], 0);
        if (rc != 0) {
            print_fail(rc);
            printf(" - read verify operation failed\n");
            return (1);
        }
        if (memcmp(buf[0], buf[1], bufsize) == 0) {
            printf("Success\n");
        } else {
            printf("Miscompare\n");
            return (1);
        }
    } else {
        print_fail_nl(rc);
    }
    return (rc);
}

static int
test_nscmd_etd_read64(struct IOExtTD *tio)
{
    int rc;
    uint8_t **buf = g_tbuf;
    uint bufsize = BUFSIZE;
    uint readoffset = 0;
    uint readoffsethi = 0;
    uint64_t rel_offset;

    if (get_args_3(&bufsize, &readoffset, &readoffsethi))
        return (1);

    if (bufsize > BUFSIZE)
        bufsize = BUFSIZE;

    rel_offset = ((uint64_t) readoffsethi << 32) | readoffset;

    memset(buf[1], 0xa6, bufsize);
    rc = test_etd_command(tio, NSCMD_ETD_READ64, "NSCMD_ETD_READ64",
                          bufsize, buf[1], rel_offset);
    if (rc == -1)
        return (0);  // Skipped
    if (rc == 0) {
        rc = do_read_cmd(tio, rel_offset, bufsize, buf[0], 1);
        if (rc != 0) {
            print_fail(rc);
            printf(" - read verify operation failed\n");
            return (1);
        }
        if (memcmp(buf[0], buf[1], bufsize) == 0) {
            printf("Success\n");
        } else {
            printf("Miscompare\n");
            return (1);
        }
    }
    return (rc);
}

static int
test_td_seek(struct IOExtTD *tio)
{
    int rc;
    uint seekoffset = 0;
    uint64_t abs;

    if (get_args_1(&seekoffset))
        return (1);

    abs = g_devstart + seekoffset;
    if ((abs >> 32) != 0) {
        print_test_name("TD_SEEK");
        printf("Skip - offset is beyond 4GB\n");
        return (0);
    }
    seekoffset = (uint) abs;

    /* Seek */
    tio->iotd_Req.io_Command = TD_SEEK;
    tio->iotd_Req.io_Offset  = seekoffset;
    tio->iotd_Req.io_Length  = 0;
    tio->iotd_Req.io_Data    = NULL;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("TD_SEEK");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        printf("Success\n");
    } else {
        /* Commodore scsi.device requires a buffer for TD_SEEK? */
        uint8_t **buf = g_tbuf;
        memset(buf[1], 0xa5, BUFSIZE);
        tio->iotd_Req.io_Command = TD_SEEK;
        tio->iotd_Req.io_Offset  = seekoffset;
        tio->iotd_Req.io_Length  = BUFSIZE;
        tio->iotd_Req.io_Data    = buf[1];
        tio->iotd_Req.io_Flags   = 0;
        tio->iotd_Req.io_Error   = 0xa5;
        memset(buf[1], 0xa5, BUFSIZE);
        rc = DoIO((struct IORequest *) tio);
        if (rc == 0) {
            printf("Success  (Bug: requires io_Length)\n");
        } else {
            print_fail_nl(rc);
        }
    }

    return (rc);
}

static int
test_etd_seek(struct IOExtTD *tio)
{
    int rc;
    uint seekoffset = 0;

    if (get_args_1(&seekoffset))
        return (1);

    rc = test_etd_command(tio, ETD_SEEK, "ETD_SEEK", BUFSIZE, g_tbuf[1],
                          seekoffset);
    if (rc == -1)
        return (0);  // Skipped
    if (rc == 0)
        printf("Success\n");
    return (rc);
}

static int
test_td_seek64(struct IOExtTD *tio)
{
    int rc;
    uint seekoffset = 0;
    uint seekoffsethi = 0;
    uint64_t io_offset;

    if (get_args_2(&seekoffset, &seekoffsethi))
        return (1);

    io_offset = ((uint64_t) seekoffsethi << 32) | seekoffset;
    io_offset += g_devstart;
    seekoffsethi = io_offset >> 32;
    seekoffset = (uint32_t) io_offset;

    tio->iotd_Req.io_Command = TD_SEEK64;
    tio->iotd_Req.io_Actual  = seekoffsethi;  // High 64 bits
    tio->iotd_Req.io_Offset  = seekoffset;
    tio->iotd_Req.io_Length  = BUFSIZE;
    tio->iotd_Req.io_Data    = g_tbuf[1];
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("TD_SEEK64");
    rc = DoIO((struct IORequest *) tio);
    print_fail_nl(rc);

    return (rc);
}

static int
test_nscmd_td_seek64(struct IOExtTD *tio)
{
    int rc;
    uint seekoffset = 0;
    uint seekoffsethi = 0;
    uint64_t io_offset;

    if (get_args_2(&seekoffset, &seekoffsethi))
        return (1);

    io_offset = ((uint64_t) seekoffsethi << 32) | seekoffset;
    io_offset += g_devstart;
    seekoffsethi = io_offset >> 32;
    seekoffset = (uint32_t) io_offset;

    tio->iotd_Req.io_Command = NSCMD_TD_SEEK64;
    tio->iotd_Req.io_Actual  = seekoffsethi;  // High 64 bits
    tio->iotd_Req.io_Offset  = seekoffset;
    tio->iotd_Req.io_Length  = BUFSIZE;
    tio->iotd_Req.io_Data    = g_tbuf[1];
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("NSCMD_TD_SEEK64");
    rc = DoIO((struct IORequest *) tio);
    print_fail_nl(rc);

    return (rc);
}

static int
test_nscmd_etd_seek64(struct IOExtTD *tio)
{
    int rc;
    uint seekoffset = 0;
    uint seekoffsethi = 0;

    if (get_args_2(&seekoffset, &seekoffsethi))
        return (1);

    rc = test_etd_command(tio, NSCMD_ETD_SEEK64, "NSCMD_ETD_SEEK64",
                          BUFSIZE, g_tbuf[1],
                          ((uint64_t) seekoffsethi << 32) | seekoffset);
    if (rc == -1)
        return (0);  // Skipped
    if (rc == 0)
        printf("Success\n");
    return (rc);
}

static int
test_cmd_stop(struct IOExtTD *tio)
{
    int rc;

    /* Stop device */
    tio->iotd_Req.io_Command = CMD_STOP;
    tio->iotd_Req.io_Actual  = 0;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 0;
    tio->iotd_Req.io_Data    = NULL;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("CMD_STOP");
    rc = DoIO((struct IORequest *) tio);
    print_fail_nl(rc);

    return (rc);
}

static int
test_cmd_start(struct IOExtTD *tio)
{
    int rc;

    /* Start device */
    tio->iotd_Req.io_Command = CMD_START;
    tio->iotd_Req.io_Actual  = 0;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 0;
    tio->iotd_Req.io_Data    = NULL;
    tio->iotd_Req.io_Flags   = IOF_QUICK;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("CMD_START");
    rc = DoIO((struct IORequest *) tio);
    print_fail_nl(rc);

    return (rc);
}

static int
test_td_motor_off(struct IOExtTD *tio)
{
    int rc;

    print_test_name("TD_MOTOR OFF");
    rc = turn_motor_off(tio);
    print_fail_nl(rc);

    return (rc);
}

static int
test_td_motor_on(struct IOExtTD *tio)
{
    int rc;

    /* Spin up device's motor */
    tio->iotd_Req.io_Command = TD_MOTOR;
    tio->iotd_Req.io_Actual  = 0;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 1;
    tio->iotd_Req.io_Data    = NULL;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("TD_MOTOR ON");
    rc = DoIO((struct IORequest *) tio);
    print_fail_nl(rc);

    return (rc);
}

static int
test_td_eject(struct IOExtTD *tio)
{
    int rc;

    /* Eject device */
    tio->iotd_Req.io_Command = TD_EJECT;
    tio->iotd_Req.io_Actual  = 0;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 1;
    tio->iotd_Req.io_Data    = NULL;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("TD_EJECT");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        printf("Success  Previous state: %s\n",
               (tio->iotd_Req.io_Actual == 0) ? "loaded" : "ejected");
    } else {
        print_fail_nl(rc);
    }

    return (rc);
}

static int
test_td_load(struct IOExtTD *tio)
{
    int rc;

    /* Load device */
    tio->iotd_Req.io_Command = TD_EJECT;
    tio->iotd_Req.io_Actual  = 0;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 0;
    tio->iotd_Req.io_Data    = NULL;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("TD_EJECT LOAD");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        printf("Success  Previous state: %s\n",
               (tio->iotd_Req.io_Actual == 0) ? "loaded" : "ejected");
    } else {
        print_fail_nl(rc);
    }

    return (rc);
}

static void
diskchange_int_addrem(void)
{
    register uint32_t *count asm("a1");
    (*count)++;
}

static void
diskchange_int_remove(void)
{
    register uint32_t *count asm("a1");
    (*count)++;
}

/*
 * get_changestate
 * ---------------
 * Reports the current media change state.
 * Return: 0 = Media present
 *         1 = Media removed
 */
static int
get_changestate(struct IOExtTD *tio)
{
    int rc;
    tio->iotd_Req.io_Command = TD_CHANGESTATE;
    tio->iotd_Req.io_Actual  = 0xa5;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 0;
    tio->iotd_Req.io_Data    = NULL;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;

    rc = DoIO((struct IORequest *) tio);
    if (rc != 0)
        return (0);
    return (tio->iotd_Req.io_Actual);  // 0=Present, 1=Removed
}

/*
 * The change interrupt structures are given to the driver, which keeps
 * them until they are removed. They are static rather than on the stack
 * so that they remain valid for as long as the program is running.
 */
static struct Interrupt  int_addrem;
static struct Interrupt  int_remove;
static volatile uint32_t int_count_addrem;
static volatile uint32_t int_count_remove;

static int
test_addremchangeint(struct IOExtTD *tio)
{
    struct MsgPort *mp;
    struct IOExtTD *itio;
    int rc;
    int rc2;
    int changestate;
    int have_td_remove = 0;
    int have_td_addrem = 0;
    int addrem_pending = 0;  // Driver may still hold the TD_ADDCHANGEINT
    uint tick;

    int_count_addrem = 0;
    int_count_remove = 0;

    mp = CreatePort(0, 0);
    if (mp == NULL) {
        printf("Failed to create message port\n");
        return (1);
    }

    itio = (struct IOExtTD *) CreateExtIO(mp, sizeof (struct IOExtTD));
    if (itio == NULL) {
        printf("Failed to create tio struct\n");
        rc = 1;
        goto ar_extio_fail;
    }
    if ((rc = open_device(itio)) != 0) {
        printf("Open %s Unit %u: ", g_devname, g_unitno);
        print_fail_nl(rc);
        goto ar_opendev_fail;
    }

    changestate = get_changestate(tio);

    int_remove.is_Node.ln_Type = NT_INTERRUPT;
    int_remove.is_Node.ln_Pri  = 32;
    int_remove.is_Node.ln_Name = "diskchange_isr_remove";
    int_remove.is_Data         = (void *) &int_count_remove;
    int_remove.is_Code         = diskchange_int_remove;

    tio->iotd_Req.io_Command  = TD_REMOVE;
    tio->iotd_Req.io_Actual   = 0;
    tio->iotd_Req.io_Offset   = 0;
    tio->iotd_Req.io_Length   = sizeof (int_remove);
    tio->iotd_Req.io_Data     = &int_remove;
    tio->iotd_Req.io_Flags    = 0;
    tio->iotd_Req.io_Error    = 0xa5;

    print_test_name("TD_REMOVE");
    rc2 = DoIO((struct IORequest *) tio);
    print_fail_nl(rc2);
    if (rc == 0)
        rc = rc2;
    if (rc2 == 0)
        have_td_remove = 1;

    int_addrem.is_Node.ln_Type = NT_INTERRUPT;
    int_addrem.is_Node.ln_Pri  = 32;
    int_addrem.is_Node.ln_Name = "diskchange_isr_addrem";
    int_addrem.is_Data         = (void *) &int_count_addrem;
    int_addrem.is_Code         = diskchange_int_addrem;

    itio->iotd_Req.io_Command  = TD_ADDCHANGEINT;
    itio->iotd_Req.io_Actual   = 0;
    itio->iotd_Req.io_Offset   = 0;
    itio->iotd_Req.io_Length   = sizeof (int_addrem);
    itio->iotd_Req.io_Data     = &int_addrem;
    itio->iotd_Req.io_Flags    = IOF_QUICK;
    itio->iotd_Req.io_Error    = 127;

    print_test_name("TD_ADDCHANGEINT");
    SendIO((struct IORequest *) itio);

    int timeout = TICKS_PER_SECOND * 2;  // Wait up to 2 seconds
    while ((rc2 = itio->iotd_Req.io_Error) == 127) {
        if (timeout-- == 0)
            break;
        Delay(1);  // Allow driver to run
    }

    if (rc2 == 127) {
        printf("Fail - handler did not clear io_Error\n");
        /* The request may still be queued in the driver */
        addrem_pending = 1;
    } else {
        print_fail_nl(rc2);
    }
    if (rc == 0)
        rc = rc2;
    if (rc2 == 0)
        have_td_addrem = 1;

    if (int_count_remove != 0) {
        print_test_name("TD_REMOVE");
        printf("Fail - Premature interrupt: %u\n", U32(int_count_remove));
        if (rc == 0)
            rc = 1;
        int_count_remove = 0;
    }
    if (int_count_addrem != 0) {
        print_test_name("TD_ADDCHANGEINT");
        printf("Fail - Premature interrupt: %u\n", U32(int_count_addrem));
        if (rc == 0)
            rc = 1;
        int_count_addrem = 0;
    }

    if (have_td_remove || have_td_addrem) {
        /* Eject device */
        if (changestate == 0) {
            tio->iotd_Req.io_Command = TD_EJECT;
            tio->iotd_Req.io_Actual  = 0;
            tio->iotd_Req.io_Offset  = 0;
            tio->iotd_Req.io_Length  = 1;  // Eject
            tio->iotd_Req.io_Data    = NULL;
            tio->iotd_Req.io_Flags   = 0;
            tio->iotd_Req.io_Error   = 0xa5;
            rc2 = DoIO((struct IORequest *) tio);
            if (rc2 != 0) {
                print_test_name("TD_EJECT");
                print_fail_nl(rc2);
            }

            if (is_user_abort()) {
                rc++;
                goto test_early_end;
            }
            if ((rc2 != 0) && (rc2 != IOERR_NOCMD)) {
                /* Try again */
                rc2 = DoIO((struct IORequest *) tio);
                if (rc2 != 0) {
                    print_test_name("TD_EJECT");
                    print_fail_nl(rc2);
                }
            }
            if (rc == 0)
                rc = rc2;
            if (rc2 == 0) {
                /* Wait up to 15 seconds for interrupt */
                for (tick = 0; tick < TICKS_PER_SECOND * 15; tick++) {
                    Delay(1);
                    if (((have_td_remove == 0) || (int_count_remove > 0)) &&
                        ((have_td_addrem == 0) || (int_count_addrem > 0))) {
                        /* Got interrupt */
                        break;
                    }
                }

                if (have_td_remove) {
                    print_test_name("Eject REMOVE Int");
                    if (int_count_remove == 0) {
                        printf("Fail - Interrupt didn't trigger after eject\n");
                        if (rc == 0)
                            rc = 1;
                    } else {
                        print_fail_nl(0);
                    }
                }
                int_count_remove = 0;

                if (have_td_addrem) {
                    print_test_name("Eject CHANGE Int");
                    if (int_count_addrem == 0) {
                        printf("Fail - Interrupt didn't trigger after eject\n");
                        if (rc == 0)
                            rc = 1;
                    } else {
                        print_fail_nl(0);
                    }
                }
                int_count_addrem = 0;
            }
        }

        /* Load device */
        tio->iotd_Req.io_Command = TD_EJECT;
        tio->iotd_Req.io_Actual  = 0;
        tio->iotd_Req.io_Offset  = 0;
        tio->iotd_Req.io_Length  = 0;  // Load
        tio->iotd_Req.io_Data    = NULL;
        tio->iotd_Req.io_Flags   = 0;
        tio->iotd_Req.io_Error   = 0xa5;
        rc2 = DoIO((struct IORequest *) tio);
        if (rc2 != 0) {
            print_test_name("TD_EJECT LOAD");
            print_fail_nl(rc2);
        }

        if ((rc2 != 0) && (rc2 != IOERR_NOCMD)) {
            /* Try again */
            rc2 = DoIO((struct IORequest *) tio);
            if (rc2 != 0) {
                print_test_name("TD_EJECT LOAD");
                print_fail_nl(rc2);
            }
        }
        if (rc == 0)
            rc = rc2;
        if (rc2 == 0) {
            /* Wait up to 15 seconds for interrupt */
            for (tick = 0; tick < TICKS_PER_SECOND * 15; tick++) {
                Delay(1);
                if (((have_td_remove == 0) || (int_count_remove > 0)) &&
                    ((have_td_addrem == 0) || (int_count_addrem > 0))) {
                    /* Got interrupt */
                    break;
                }
            }

            if (have_td_remove) {
                print_test_name("Load REMOVE Int");
                if (int_count_remove == 0) {
                    printf("Fail - Interrupt didn't trigger after load\n");
                    if (rc == 0)
                        rc = 1;
                } else {
                    print_fail_nl(0);
                }
            }
            int_count_remove = 0;

            if (have_td_addrem) {
                print_test_name("Load CHANGE Int");
                if (int_count_addrem == 0) {
                    printf("Fail - Interrupt didn't trigger after load\n");
                    if (rc == 0)
                        rc = 1;
                } else {
                    print_fail_nl(0);
                }
            }
            int_count_addrem = 0;
        }
    }

test_early_end:
    if (have_td_addrem || addrem_pending) {
        /*
         * TD_REMCHANGEINT must use the same I/O request as was used for
         * TD_ADDCHANGEINT. DoIO() waits for completion, so the request
         * is not still in use when it is freed below.
         */
        itio->iotd_Req.io_Command = TD_REMCHANGEINT;
        itio->iotd_Req.io_Length  = sizeof (int_addrem);
        itio->iotd_Req.io_Data    = &int_addrem;
        itio->iotd_Req.io_Flags   = 0;
        print_test_name("TD_REMCHANGEINT");
        rc2 = DoIO((struct IORequest *) itio);
        print_fail_nl(rc2);
        if (rc == 0)
            rc = rc2;
        if (rc2 != 0) {
            /*
             * The driver may still hold the change interrupt request.
             * Freeing the request or its reply port could crash the
             * system on the next disk change, so leave them allocated.
             */
            printf("Warning: %s did not remove the change interrupt; "
                   "rebooting is advised\n", g_devname);
            itio = NULL;
            mp = NULL;
        }
    }

    if (have_td_remove) {
        tio->iotd_Req.io_Command  = TD_REMOVE;
        tio->iotd_Req.io_Actual   = 0;
        tio->iotd_Req.io_Offset   = 0;
        tio->iotd_Req.io_Length   = 0;
        tio->iotd_Req.io_Data     = NULL;
        tio->iotd_Req.io_Flags    = 0;
        tio->iotd_Req.io_Error    = 0xa5;

        print_test_name("TD_REMOVE delete");
        rc2 = DoIO((struct IORequest *) tio);
        print_fail_nl(rc2);
        if (rc == 0)
            rc = rc2;
    }

    if (itio != NULL)
        close_device(itio);
ar_opendev_fail:
    if (itio != NULL)
        DeleteExtIO((struct IORequest *) itio);
ar_extio_fail:
    if (mp != NULL)
        DeletePort(mp);

    return (rc);
}

/*
 * The write and format tests below operate at these partition-relative
 * offsets, which are saved before and restored after the tests:
 *     0 to BUFSIZE * 2
 *     4GB to 4GB + BUFSIZE * 2 (only if the partition is large enough)
 */
#define HIGH_TEST_OK() (part_size() >= ((1ULL << 32) + BUFSIZE * 2))

static void
save_overwritten_data(struct IOExtTD *tio, uint8_t **buf)
{
    if (flag_destructive > 1)  // Do not save or restore
        return;

    /* Save data which might be overwritten into a RAM buffer */
    do_read_cmd(tio, 0, BUFSIZE, buf[2], 0);
    do_read_cmd(tio, BUFSIZE, BUFSIZE, buf[3], 0);

    if (HIGH_TEST_OK()) {
        /* Partition is large enough to test 4GB boundary */
        do_read_cmd(tio, 1ULL << 32, BUFSIZE, buf[4], g_has_nsd);
        do_read_cmd(tio, (1ULL << 32) + BUFSIZE, BUFSIZE, buf[5], g_has_nsd);
    }
}

static void
restore_overwritten_data(struct IOExtTD *tio, uint8_t **buf, int high)
{
    int has_nsd = high & 2;

    if (flag_destructive > 1)  // Do not save or restore
        return;

    /* Restore overwritten data */
    do_write_cmd(tio, 0, BUFSIZE, buf[2], 0);
    do_write_cmd(tio, BUFSIZE, BUFSIZE, buf[3], 0);
    if (high && HIGH_TEST_OK()) {
        /* Addresses above 4GB were tested */
        do_write_cmd(tio, 1ULL << 32, BUFSIZE, buf[4], has_nsd);
        do_write_cmd(tio, (1ULL << 32) + BUFSIZE, BUFSIZE, buf[5], has_nsd);
    }
}

/*
 * A write test may be given an offset (-c write(size,offset)) which is
 * outside the areas saved by save_overwritten_data(). The original data
 * at the target offset is saved in g_tbuf[6] before the write, and written
 * back by restore_target_data() after the test.
 */
static uint64_t g_target_offset;
static uint     g_target_len;  // 0 = nothing saved

/*
 * save_target_data() saves the data at a partition-relative offset which
 * is about to be overwritten. Returns nonzero if the data could not be
 * saved, in which case the write must not be done.
 */
static int
save_target_data(struct IOExtTD *tio, uint64_t rel_offset, uint len)
{
    int rc;

    g_target_len = 0;
    if (flag_destructive > 1)  // Do not save or restore
        return (0);
    if (outside_partition(rel_offset, len))
        return (0);  // The write will be refused

    memset(g_tbuf[6], 0xa5, len);
    rc = do_read_cmd(tio, rel_offset, len, g_tbuf[6], g_has_nsd);
    if (rc != 0) {
        printf("Fail - could not save original data: ");
        print_fail(rc);
        return (rc);
    }
    g_target_offset = rel_offset;
    g_target_len = len;
    return (0);
}

/* discard_target_data() forgets saved data when the write was not done. */
static void
discard_target_data(void)
{
    g_target_len = 0;
}

static void
restore_target_data(struct IOExtTD *tio)
{
    int rc;

    if (g_target_len == 0)
        return;
    rc = do_write_cmd(tio, g_target_offset, g_target_len, g_tbuf[6],
                      g_has_nsd);
    if (rc != 0) {
        printf("  Restore of original data at sector %s failed: ",
               llu_sector_to_str(g_target_offset));
        print_fail_nl(rc);
    }
    g_target_len = 0;
}

/*
 * setup_raw_io() sets up a request for a command at a partition-relative
 * offset. For 64-bit commands (is64 != 0), io_Actual holds the high 32 bits
 * of the absolute offset.
 *
 * Returns nonzero (after printing why) if the request may not be issued:
 * a 32-bit command beyond 4GB, or an access past the end of the partition.
 */
static int
setup_raw_io(struct IOExtTD *tio, UWORD cmd, int is64, uint64_t rel_offset,
             uint len, void *buf)
{
    uint64_t abs = g_devstart + rel_offset;

    if (!is64 && (((abs + len) >> 32) != 0)) {
        printf("Skip - offset is beyond 4GB");
        return (1);
    }
    if (outside_partition(rel_offset, len)) {
        printf("Skip - beyond end of partition");
        return (1);
    }
    tio->iotd_Req.io_Command = cmd;
    tio->iotd_Req.io_Actual  = is64 ? (ULONG) (abs >> 32) : 0;
    tio->iotd_Req.io_Offset  = (uint32_t) abs;
    tio->iotd_Req.io_Length  = len;
    tio->iotd_Req.io_Data    = buf;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    return (0);
}

static int
test_cmd_write(struct IOExtTD *tio)
{
    int rc;
    uint8_t **buf = g_tbuf;
    uint writeoffset = 0;
    uint bufsize = BUFSIZE;

    if (get_args_2(&bufsize, &writeoffset))
        return (1);
    if (bufsize > BUFSIZE)
        bufsize = BUFSIZE;

    memset(buf[0], 0xdb, bufsize);
    print_test_name("CMD_WRITE");
    if ((rc = save_target_data(tio, writeoffset, bufsize)) != 0) {
        printf("\n");
        return (rc);
    }
    if (setup_raw_io(tio, CMD_WRITE, 0, writeoffset, bufsize, buf[0])) {
        discard_target_data();
        printf("\n");
        return (0);
    }
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        check_write(tio, buf[0], buf[1], bufsize, writeoffset, 0);
    } else {
        print_fail(rc);
    }
    printf("\n");
    restore_overwritten_data(tio, buf, 0);
    restore_target_data(tio);

    return (rc);
}

static int
test_etd_write(struct IOExtTD *tio)
{
    int rc;
    uint8_t **buf = g_tbuf;
    uint writeoffset = 0;
    uint bufsize = BUFSIZE;

    if (get_args_2(&bufsize, &writeoffset))
        return (1);
    if (bufsize > BUFSIZE)
        bufsize = BUFSIZE;

    memset(buf[0], 0xc9, bufsize);
    print_test_name("ETD_WRITE");
    if ((rc = save_target_data(tio, writeoffset, bufsize)) != 0) {
        printf("\n");
        return (rc);
    }
    rc = test_etd_command(tio, ETD_WRITE, NULL, bufsize, buf[0],
                          writeoffset);
    if (rc == -1) {
        discard_target_data();
        return (0);  // Skipped
    }
    if (rc == 0) {
        rc = check_write(tio, buf[0], buf[1], bufsize, writeoffset, 0);
        printf("\n");
    }
    restore_overwritten_data(tio, buf, 0);
    restore_target_data(tio);

    return (rc);
}

static int
test_td_write64(struct IOExtTD *tio)
{
    int rc;
    uint8_t **buf = g_tbuf;
    uint bufsize = BUFSIZE;
    uint writeoffset = 0;
    uint writeoffsethi = 0;
    uint64_t rel_offset;

    if (get_args_3(&bufsize, &writeoffset, &writeoffsethi))
        return (1);
    if (bufsize > BUFSIZE)
        bufsize = BUFSIZE;

    rel_offset = ((uint64_t) writeoffsethi << 32) | writeoffset;

    memset(buf[0], 0xd6, bufsize);
    print_test_name("TD_WRITE64");
    if ((rc = save_target_data(tio, rel_offset, bufsize)) != 0) {
        printf("\n");
        return (rc);
    }
    if (setup_raw_io(tio, TD_WRITE64, 1, rel_offset, bufsize, buf[0])) {
        discard_target_data();
        printf("\n");
        return (0);
    }
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        if (check_write(tio, buf[0], buf[1], bufsize, rel_offset, 0) == 0) {
            if (HIGH_TEST_OK() && (rel_offset == 0)) {
                printf("  4GB:");
                memset(buf[0], 0xd7, bufsize);
                if (setup_raw_io(tio, TD_WRITE64, 1, 1ULL << 32, bufsize,
                                 buf[0]) == 0) {
                    rc = DoIO((struct IORequest *) tio);
                    if (rc == 0) {
                        check_write(tio, buf[0], buf[1], bufsize,
                                    1ULL << 32, 0);
                    } else {
                        print_fail(rc);
                    }
                }
            }
        }
    } else {
        print_fail(rc);
    }
    printf("\n");
    restore_overwritten_data(tio, buf, 1);
    restore_target_data(tio);

    return (rc);
}

static int
test_nscmd_td_write64(struct IOExtTD *tio)
{
    int rc;
    uint8_t **buf = g_tbuf;
    uint bufsize = BUFSIZE;
    uint writeoffset = 0;
    uint writeoffsethi = 0;
    uint64_t rel_offset;

    if (get_args_3(&bufsize, &writeoffset, &writeoffsethi))
        return (1);
    if (bufsize > BUFSIZE)
        bufsize = BUFSIZE;

    rel_offset = ((uint64_t) writeoffsethi << 32) | writeoffset;

    memset(buf[0], 0xe5, bufsize);
    print_test_name("NSCMD_TD_WRITE64");
    if ((rc = save_target_data(tio, rel_offset, bufsize)) != 0) {
        printf("\n");
        return (rc);
    }
    if (setup_raw_io(tio, NSCMD_TD_WRITE64, 1, rel_offset, bufsize, buf[0])) {
        discard_target_data();
        printf("\n");
        return (0);
    }
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        if (check_write(tio, buf[0], buf[1], bufsize, rel_offset, 1) == 0) {
            if (HIGH_TEST_OK() && (rel_offset == 0)) {
                /* Partition is large enough to test 4GB boundary */
                printf("  4GB:");
                memset(buf[0], 0xe6, bufsize);
                if (setup_raw_io(tio, NSCMD_TD_WRITE64, 1, 1ULL << 32,
                                 bufsize, buf[0]) == 0) {
                    rc = DoIO((struct IORequest *) tio);
                    if (rc == 0) {
                        check_write(tio, buf[0], buf[1], bufsize,
                                    1ULL << 32, 1);
                    } else {
                        print_fail(rc);
                    }
                }
            }
        }
    } else {
        print_fail(rc);
    }
    printf("\n");
    restore_overwritten_data(tio, buf, 2);
    restore_target_data(tio);

    return (rc);
}

static int
test_nscmd_etd_write64(struct IOExtTD *tio)
{
    int rc;
    uint8_t **buf = g_tbuf;
    uint bufsize = BUFSIZE;
    uint writeoffset = 0;
    uint writeoffsethi = 0;
    uint64_t rel_offset;

    if (get_args_3(&bufsize, &writeoffset, &writeoffsethi))
        return (1);
    if (bufsize > BUFSIZE)
        bufsize = BUFSIZE;

    rel_offset = ((uint64_t) writeoffsethi << 32) | writeoffset;

    memset(buf[0], 0xe5, bufsize);
    print_test_name("NSCMD_ETD_WRITE64");
    if ((rc = save_target_data(tio, rel_offset, bufsize)) != 0) {
        printf("\n");
        return (rc);
    }
    rc = test_etd_command(tio, NSCMD_ETD_WRITE64, NULL, bufsize, buf[0],
                          rel_offset);
    if (rc == -1) {
        discard_target_data();
        return (0);  // Skipped
    }
    if (rc == 0) {
        rc = check_write(tio, buf[0], buf[1], bufsize, rel_offset, 1);
        if ((rc == 0) && HIGH_TEST_OK() && (rel_offset == 0)) {
            /* Partition is large enough to test 4GB boundary */
            printf("  4GB:");
            memset(buf[0], 0xe6, bufsize);
            rc = test_etd_command(tio, NSCMD_ETD_WRITE64, NULL, bufsize,
                                  buf[0], 1ULL << 32);
            if (rc == -1) {
                rc = 0;  // Skipped
            } else if (rc == 0) {
                rc = check_write(tio, buf[0], buf[1], bufsize,
                                 1ULL << 32, 1);
                printf("\n");
            }
        } else {
            printf("\n");
        }
    }
    restore_overwritten_data(tio, buf, 2);
    restore_target_data(tio);

    return (rc);
}

static int
test_td_format(struct IOExtTD *tio)
{
    int rc;
    uint8_t **buf = g_tbuf;

    /* Format (acts the same same as write) */
    memset(buf[0], 0xdb, BUFSIZE);
    print_test_name("TD_FORMAT");
    if (setup_raw_io(tio, TD_FORMAT, 0, BUFSIZE, BUFSIZE, buf[0])) {
        printf("\n");
        return (0);
    }
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        check_write(tio, buf[0], buf[1], BUFSIZE, BUFSIZE, 0);
    } else {
        print_fail(rc);
    }
    printf("\n");
    restore_overwritten_data(tio, buf, 0);

    return (rc);
}

static int
test_etd_format(struct IOExtTD *tio)
{
    int rc;
    uint8_t **buf = g_tbuf;

    memset(buf[0], 0xca, BUFSIZE);
    rc = test_etd_command(tio, ETD_FORMAT, "ETD_FORMAT", BUFSIZE, buf[0], 0);
    if (rc == -1)
        return (0);  // Skipped
    if (rc == 0) {
        check_write(tio, buf[0], buf[1], BUFSIZE, 0, 0);
        printf("\n");
    }
    restore_overwritten_data(tio, buf, 0);

    return (rc);
}

static int
test_td_format64(struct IOExtTD *tio)
{
    int rc;
    uint8_t **buf = g_tbuf;

    memset(buf[0], 0xf4, BUFSIZE);
    print_test_name("TD_FORMAT64");
    if (setup_raw_io(tio, TD_FORMAT64, 1, BUFSIZE, BUFSIZE, buf[0])) {
        printf("\n");
        return (0);
    }
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        if (check_write(tio, buf[0], buf[1], BUFSIZE, BUFSIZE, 0) == 0) {
            if (HIGH_TEST_OK()) {
                /* Partition is large enough to test 4GB boundary */
                printf("  4GB:");
                memset(buf[0], 0xf5, BUFSIZE);
                if (setup_raw_io(tio, TD_FORMAT64, 1, (1ULL << 32) + BUFSIZE,
                                 BUFSIZE, buf[0]) == 0) {
                    rc = DoIO((struct IORequest *) tio);
                    if (rc == 0) {
                        check_write(tio, buf[0], buf[1], BUFSIZE,
                                    (1ULL << 32) + BUFSIZE, 0);
                    } else {
                        print_fail(rc);
                    }
                }

                /* Restore overwritten data above 4GB boundary */
                do_write_cmd(tio, 1ULL << 32, BUFSIZE, buf[4], 1);
                do_write_cmd(tio, (1ULL << 32) + BUFSIZE, BUFSIZE, buf[5], 1);
            }
        }
    } else {
        print_fail(rc);
    }
    printf("\n");
    restore_overwritten_data(tio, buf, 1);

    return (rc);
}

static int
test_nscmd_td_format64(struct IOExtTD *tio)
{
    int rc;
    uint8_t **buf = g_tbuf;

    memset(buf[0], 0x1e, BUFSIZE);
    print_test_name("NSCMD_TD_FORMAT64");
    if (setup_raw_io(tio, NSCMD_TD_FORMAT64, 1, BUFSIZE, BUFSIZE, buf[0])) {
        printf("\n");
        return (0);
    }
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        if (check_write(tio, buf[0], buf[1], BUFSIZE, BUFSIZE, 1) == 0) {
            if (HIGH_TEST_OK()) {
                /* Partition is large enough to test 4GB boundary */
                printf("  4GB:");
                memset(buf[0], 0x1d, BUFSIZE);
                if (setup_raw_io(tio, NSCMD_TD_FORMAT64, 1,
                                 (1ULL << 32) + BUFSIZE, BUFSIZE,
                                 buf[0]) == 0) {
                    rc = DoIO((struct IORequest *) tio);
                    if (rc == 0) {
                        check_write(tio, buf[0], buf[1], BUFSIZE,
                                    (1ULL << 32) + BUFSIZE, 1);
                    } else {
                        print_fail(rc);
                    }
                }

                /* Restore overwritten data above 4GB boundary */
                do_write_cmd(tio, 1ULL << 32, BUFSIZE, buf[4], 1);
                do_write_cmd(tio, (1ULL << 32) + BUFSIZE, BUFSIZE, buf[5], 1);
            }
        }
    } else {
        print_fail(rc);
    }
    printf("\n");
    restore_overwritten_data(tio, buf, 2);

    return (rc);
}

static int
test_nscmd_etd_format64(struct IOExtTD *tio)
{
    int rc;
    uint8_t **buf = g_tbuf;

    memset(buf[0], 0xe5, BUFSIZE);
    rc = test_etd_command(tio, NSCMD_ETD_FORMAT64, "NSCMD_ETD_FORMAT64",
                          BUFSIZE, buf[0], 0);
    if (rc == -1)
        return (0);  // Skipped
    if (rc == 0) {
        if ((rc = check_write(tio, buf[0], buf[1], BUFSIZE, 0, 0)) == 0) {
            if (HIGH_TEST_OK()) {
                /* Partition is large enough to test 4GB boundary */
                printf("  4GB:");
                memset(buf[0], 0xe6, BUFSIZE);
                rc = test_etd_command(tio, NSCMD_ETD_FORMAT64,
                                      NULL, BUFSIZE, buf[0], 1ULL << 32);
                if (rc == -1) {
                    rc = 0;  // Skipped
                } else if (rc == 0) {
                    rc = check_write(tio, buf[0], buf[1], BUFSIZE,
                                     1ULL << 32, 1);
                    printf("\n");
                }
            } else {
                printf("\n");
            }
        } else {
            printf("\n");
        }
    }
    restore_overwritten_data(tio, buf, 2);

    return (rc);
}

/*
 * AmigaDOS MFM track format
 * -------------------------
 * Each sector on an AmigaDOS floppy track is encoded as:
 *     0xAAAA 0xAAAA         MFM-encoded zeros (preamble)
 *     0x4489 0x4489         Sync words
 *     4 + 4 bytes           Info: format (0xff), track, sector, to-gap
 *     16 + 16 bytes         Sector label (OS recovery info)
 *     4 + 4 bytes           Header checksum
 *     4 + 4 bytes           Data checksum
 *     512 + 512 bytes       Data
 * Each field is stored as its odd bits followed by its even bits. A value
 * is decoded with ((odd & 0x55555555) << 1) | (even & 0x55555555). The
 * checksums are the XOR of the encoded longwords, masked with 0x55555555.
 */
#define MFM_SYNC        0x4489
#define MFM_MASK        0x55555555
#define MFM_SECTOR_LEN  1080  // Encoded sector following the sync words
#define MFM_SECTOR_RAW  1088  // Encoded sector including preamble and sync
#define MFM_HDR_OFF     0     // Info and label (40 encoded bytes)
#define MFM_HDRSUM_OFF  40
#define MFM_DATASUM_OFF 48
#define MFM_DATA_OFF    56    // 512 odd bytes then 512 even bytes

static uint32_t
mfm_decode32(const uint8_t *odd, const uint8_t *even)
{
    return (((_4btol(odd) & MFM_MASK) << 1) | (_4btol(even) & MFM_MASK));
}

static uint32_t
mfm_checksum(const uint8_t *raw, uint len)
{
    uint32_t sum = 0;
    uint     pos;
    for (pos = 0; pos < len; pos += 4)
        sum ^= _4btol(raw + pos);
    return (sum & MFM_MASK);
}

typedef struct {
    uint     sectors;      // AmigaDOS sectors found (including duplicates)
    uint     errors;       // Sectors with errors
    uint32_t found;        // Bitmap of good sectors decoded
    char     first_error[64];
} mfm_result_t;

static void
mfm_error(mfm_result_t *res, const char *fmt, uint val)
{
    if (res->errors++ == 0)
        sprintf(res->first_error, fmt, val);
}

/*
 * mfm_align() finds the first pair of sync words (0x4489 0x4489) at any bit
 * position in the raw track buffer, and shifts the buffer contents in place
 * so that the sync words are on 16-bit boundaries. TD_RAWREAD does not
 * necessarily return word-aligned data, even with IOTDF_WORDSYNC.
 * Returns the new length of valid data, or 0 if no sync was found.
 */
static uint
mfm_align(uint8_t *raw, uint len, uint *shift)
{
    uint pos;
    uint bit;
    uint bitoff = 0;
    uint byteoff;
    int  found = 0;

    for (pos = 0; (pos + 5 <= len) && !found; pos++) {
        uint32_t hi = _4btol(raw + pos);
        uint32_t lo = raw[pos + 4];
        for (bit = 0; bit < 8; bit++) {
            uint32_t win = (hi << bit) | (lo >> (8 - bit));
            if (bit == 0)
                win = hi;
            if (win == ((MFM_SYNC << 16) | MFM_SYNC)) {
                bitoff = pos * 8 + bit;
                found = 1;
                break;
            }
        }
    }
    if (!found)
        return (0);

    /* Shift so the stream starts on the same 16-bit phase as the sync */
    bitoff %= 16;
    *shift = bitoff;
    byteoff = bitoff / 8;
    bit     = bitoff % 8;
    if (bitoff == 0)
        return (len);
    len -= byteoff + 1;
    for (pos = 0; pos < len; pos++) {
        raw[pos] = (uint8_t) ((raw[pos + byteoff] << bit) |
                              (bit ? (raw[pos + byteoff + 1] >> (8 - bit)) :
                                     0));
    }
    return (len & ~1);
}

/*
 * mfm_verify_track() decodes the AmigaDOS sectors in a raw track buffer,
 * verifies their headers and checksums, and stores the data of each good
 * sector at decoded + sector * 512. A sector may appear more than once if
 * the buffer covers more than one revolution; the first good copy is used.
 * Partial sectors at the start and end of the buffer are ignored. The
 * buffer is modified (realigned to the sync words).
 */
static void
mfm_verify_track(uint8_t *raw, uint len, uint track, uint nsec,
                 uint8_t *decoded, mfm_result_t *res)
{
    uint pos = 0;
    uint shift = 0;

    memset(res, 0, sizeof (*res));
    /*
     * With IOTDF_WORDSYNC, the hardware consumes the first sync word, so
     * the buffer starts with the second one: already aligned.
     */
    if ((len < 2) || (((raw[0] << 8) | raw[1]) != MFM_SYNC))
        len = mfm_align(raw, len, &shift);
    if (g_verbose > 1)
        printf("  [sync bit offset %u]", shift);
    while (pos + 2 <= len) {
        const uint8_t *sec;
        uint32_t info;
        uint     fmt;
        uint     trk;
        uint     snum;
        uint     word;

        if (((raw[pos] << 8) | raw[pos + 1]) != MFM_SYNC) {
            pos += 2;
            continue;
        }
        /* Skip any additional sync words */
        while ((pos + 2 <= len) &&
               (((raw[pos] << 8) | raw[pos + 1]) == MFM_SYNC))
            pos += 2;
        if (pos + MFM_SECTOR_LEN > len)
            break;  // Partial sector at end of buffer

        sec  = raw + pos;
        info = mfm_decode32(sec + MFM_HDR_OFF, sec + MFM_HDR_OFF + 4);
        fmt  = info >> 24;
        trk  = (info >> 16) & 0xff;
        snum = (info >> 8) & 0xff;
        if (fmt != 0xff)
            continue;  // Not an AmigaDOS sector header; keep searching
        res->sectors++;

        if (mfm_checksum(sec + MFM_HDR_OFF, 40) !=
            mfm_decode32(sec + MFM_HDRSUM_OFF, sec + MFM_HDRSUM_OFF + 4)) {
            mfm_error(res, "header checksum error (header says sector %u)",
                      snum);
        } else if (trk != track) {
            mfm_error(res, "sector header has wrong track %u", trk);
        } else if (snum >= nsec) {
            mfm_error(res, "sector header has invalid sector %u", snum);
        } else if (mfm_checksum(sec + MFM_DATA_OFF, 1024) !=
                   mfm_decode32(sec + MFM_DATASUM_OFF,
                                sec + MFM_DATASUM_OFF + 4)) {
            mfm_error(res, "sector %u data checksum error", snum);
        } else if ((res->found & BIT(snum)) == 0) {
            uint8_t *out = decoded + snum * 512;
            for (word = 0; word < 128; word++) {
                uint32_t val = mfm_decode32(sec + MFM_DATA_OFF + word * 4,
                                            sec + MFM_DATA_OFF + 512 +
                                            word * 4);
                out[word * 4]     = (uint8_t) (val >> 24);
                out[word * 4 + 1] = (uint8_t) (val >> 16);
                out[word * 4 + 2] = (uint8_t) (val >> 8);
                out[word * 4 + 3] = (uint8_t) val;
            }
            res->found |= BIT(snum);
        }
        if (g_verbose > 1)
            printf("  [%u: trk=%u sec=%u gap=%u]", pos, trk, snum,
                   (uint) (info & 0xff));
        pos += MFM_SECTOR_LEN;
    }
}

/*
 * mfm_add_clocks() adds MFM clock bits to a buffer which holds only data
 * bits (0x55 positions). A clock bit is 1 only when the data bits on both
 * sides of it are 0. prevbit is the last bit written before the buffer.
 */
static void
mfm_add_clocks(uint8_t *p, uint len, uint prevbit)
{
    uint pos;
    for (pos = 0; pos < len; pos++) {
        uint data  = p[pos] & 0x55;
        uint clock = ~((data << 1) | (data >> 1) | (prevbit << 7)) & 0xaa;
        p[pos] = (uint8_t) (data | clock);
        prevbit = p[pos] & 1;
    }
}

static void
mfm_put_data32(uint8_t *odd, uint8_t *even, uint32_t val)
{
    uint32_t o = (val >> 1) & MFM_MASK;
    uint32_t e = val & MFM_MASK;
    odd[0]  = (uint8_t) (o >> 24);
    odd[1]  = (uint8_t) (o >> 16);
    odd[2]  = (uint8_t) (o >> 8);
    odd[3]  = (uint8_t) o;
    even[0] = (uint8_t) (e >> 24);
    even[1] = (uint8_t) (e >> 16);
    even[2] = (uint8_t) (e >> 8);
    even[3] = (uint8_t) e;
}

/*
 * mfm_encode_track() builds a raw AmigaDOS track for TD_RAWWRITE from
 * nsec sectors of 512 bytes, in order from sector 0. A gap of MFM zeros
 * precedes the sectors: the write covers more than a revolution, so the
 * end of the write wraps around over the start of the gap rather than over
 * sector data. A short gap also follows the last sector: a floppy emulator
 * (seen with amiga3's) did not store the last sector of a write which
 * ended right after its data. Returns the length of the raw track in bytes.
 */
#define MFM_TRACK_GAP(nsec) ((nsec) * 100 - MFM_TRACK_TRAIL)  // For wrap
#define MFM_TRACK_TRAIL     64  // Gap after the last sector

static uint
mfm_encode_track(uint8_t *raw, const uint8_t *data, uint nsec, uint track)
{
    uint gap = MFM_TRACK_GAP(nsec);
    uint len = gap + nsec * MFM_SECTOR_RAW + MFM_TRACK_TRAIL;
    uint pos;
    uint i;

    memset(raw, 0, len);  // Data bits all zero: gap and preambles
    for (i = 0; i < nsec; i++) {
        uint     snum = i;
        uint8_t *sec  = raw + gap + i * MFM_SECTOR_RAW;
        uint8_t *body = sec + 8;  // After preamble and sync words
        uint32_t info = (0xffU << 24) | (track << 16) | (snum << 8) |
                        (nsec - i);
        const uint8_t *src = data + snum * 512;

        sec[4] = 0x44;  // Sync words (preamble data bits are already 0)
        sec[5] = 0x89;
        sec[6] = 0x44;
        sec[7] = 0x89;
        mfm_put_data32(body + MFM_HDR_OFF, body + MFM_HDR_OFF + 4, info);
        /* Sector label (16 + 16 bytes) is left zero */
        for (pos = 0; pos < 128; pos++) {
            mfm_put_data32(body + MFM_DATA_OFF + pos * 4,
                           body + MFM_DATA_OFF + 512 + pos * 4,
                           _4btol(src + pos * 4));
        }
        mfm_put_data32(body + MFM_HDRSUM_OFF, body + MFM_HDRSUM_OFF + 4,
                       mfm_checksum(body + MFM_HDR_OFF, 40));
        mfm_put_data32(body + MFM_DATASUM_OFF, body + MFM_DATASUM_OFF + 4,
                       mfm_checksum(body + MFM_DATA_OFF, 1024));
    }

    /* Add clock bits everywhere except the sync words */
    mfm_add_clocks(raw, gap + 4, 0);
    for (i = 0; i < nsec; i++) {
        uint8_t *sec  = raw + gap + i * MFM_SECTOR_RAW;
        uint     next = (i + 1 < nsec) ? 4 : MFM_TRACK_TRAIL;  // Preamble
        mfm_add_clocks(sec + 8, MFM_SECTOR_LEN + next, 1);  // 0x89 ends in 1
    }
    return (len);
}

/*
 * do_rawread() / do_rawwrite() issue TD_RAWREAD / TD_RAWWRITE on a track.
 * DoIO() replaces io_Flags with IOF_QUICK, which would discard the sync
 * flags, so BeginIO() is used, which passes io_Flags to the driver.
 */
static int
do_rawio(struct IOExtTD *tio, UWORD cmd, uint track, uint8_t *buf, uint len,
         UBYTE flags)
{
    tio->iotd_Req.io_Command = cmd;
    tio->iotd_Req.io_Actual  = 0;
    tio->iotd_Req.io_Offset  = track;
    tio->iotd_Req.io_Length  = len;
    tio->iotd_Req.io_Data    = buf;
    tio->iotd_Req.io_Flags   = flags;
    tio->iotd_Req.io_Error   = 0xa5;
    BeginIO((struct IORequest *) tio);
    return (WaitIO((struct IORequest *) tio));
}

static int
do_rawread(struct IOExtTD *tio, uint track, uint8_t *buf, uint len)
{
    memset(buf, 0x1e, len);
    return (do_rawio(tio, TD_RAWREAD, track, buf, len, IOTDF_WORDSYNC));
}

static int
do_rawwrite(struct IOExtTD *tio, uint track, uint8_t *buf, uint len)
{
    return (do_rawio(tio, TD_RAWWRITE, track, buf, len, IOTDF_INDEXSYNC));
}

/*
 * rawread_len() returns the TD_RAWREAD length for a track of nsec sectors.
 * With IOTDF_WORDSYNC, the read starts at a sector, so one revolution
 * (all sectors plus the track gap) is enough to see every sector.
 * trackdisk.device fails a much longer sync read (TDERR_NoSecHdr was
 * seen with 32766 bytes), so do not read more than needed.
 */
static uint
rawread_len(uint nsec)
{
    uint len = (nsec + 2) * MFM_SECTOR_RAW;
    return ((len > RAWBUFSIZE) ? RAWBUFSIZE : len);
}

/*
 * test_td_rawread() reads track 0 with TD_RAWREAD. If the track is in
 * AmigaDOS format, the MFM data is decoded and verified (sector headers and
 * checksums), and then compared against the same track read with CMD_READ.
 */
static int
test_td_rawread(struct IOExtTD *tio)
{
    int      rc;
    uint     nsec = 0;
    uint     sec;
    uint     tracklen;
    uint     rawlen = 16384;
    uint8_t *buf;
    uint8_t *decoded = NULL;
    uint8_t *cmpbuf = NULL;
    struct DriveGeometry dg;
    mfm_result_t res;

    /* Sectors per track, to know which sectors to expect */
    if ((do_getgeometry(tio, &dg) == 0) && (dg.dg_SectorSize == 512) &&
        (dg.dg_TrackSectors > 0) && (dg.dg_TrackSectors <= 32))
        nsec = dg.dg_TrackSectors;
    tracklen = nsec * 512;
    if (nsec != 0)
        rawlen = rawread_len(nsec);

    /* TD_RAWREAD and TD_RAWWRITE must be in Chip RAM */
    buf = (uint8_t *) AllocMem(RAWBUFSIZE, MEMF_PUBLIC | MEMF_CHIP);
    if (buf == NULL) {
        printf("Unable to allocate %d bytes\n", RAWBUFSIZE);
        return (1);
    }

    print_test_name("TD_RAWREAD");
    rc = do_rawread(tio, 0, buf, rawlen);  // Track 0 (cylinder 0, head 0)
    if (rc != 0) {
        print_fail_nl(rc);
        goto rawread_done;
    }
    g_turn_motor_off = 1;

    if (nsec == 0) {
        printf("Success  (track geometry unknown; data not verified)\n");
        goto rawread_done;
    }
    decoded = AllocMemType(tracklen, memtype);
    cmpbuf  = AllocMemType(tracklen, memtype);
    if ((decoded == NULL) || (cmpbuf == NULL)) {
        printf("Success  (no memory to verify data)\n");
        goto rawread_done;
    }

    mfm_verify_track(buf, rawlen, 0, nsec, decoded, &res);
    if (g_verbose > 1)
        printf("\n%-19s", "");
    if (res.sectors == 0) {
        printf("Success  (not an AmigaDOS track; data not verified)\n");
        goto rawread_done;
    }
    if (res.errors != 0) {
        printf("Fail - %s", res.first_error);
        if (res.errors > 1)
            printf(" (+%u more)", res.errors - 1);
        printf("\n");
        rc = 1;
        goto rawread_done;
    }
    for (sec = 0; sec < nsec; sec++) {
        if ((res.found & BIT(sec)) == 0) {
            printf("Fail - sector %u not found in raw track\n", sec);
            rc = 1;
            goto rawread_done;
        }
    }

    /* Compare against the same track read through the normal path */
    memset(cmpbuf, 0xa5, tracklen);
    tio->iotd_Req.io_Command = CMD_READ;
    tio->iotd_Req.io_Actual  = 0;
    tio->iotd_Req.io_Offset  = 0;  // Track 0 (not partition-relative)
    tio->iotd_Req.io_Length  = tracklen;
    tio->iotd_Req.io_Data    = cmpbuf;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    rc = DoIO((struct IORequest *) tio);
    if (rc != 0) {
        printf("Success  %u sectors decoded; CMD_READ ", nsec);
        print_fail_nl(rc);
        goto rawread_done;
    }
    for (sec = 0; sec < nsec; sec++) {
        if (memcmp(decoded + sec * 512, cmpbuf + sec * 512, 512) != 0) {
            printf("Fail - sector %u data differs from CMD_READ\n", sec);
            if (g_verbose)
                show_diffs(cmpbuf + sec * 512, decoded + sec * 512, 512,
                           "CMD_READ");
            rc = 1;
            goto rawread_done;
        }
    }
    printf("Success  %u sectors decoded, match CMD_READ\n", nsec);

rawread_done:
    if (decoded != NULL)
        FreeMemType(decoded, tracklen);
    if (cmpbuf != NULL)
        FreeMemType(cmpbuf, tracklen);
    /* TD_RAWREAD and TD_RAWWRITE must be in Chip RAM */
    FreeMem(buf, RAWBUFSIZE);
    return (rc);
}

/*
 * cmd_read_track() reads a track with CMD_READ, after discarding the
 * driver's track buffer (CMD_CLEAR) so that the media is actually read.
 * len is the size of a track in bytes.
 */
static int
cmd_read_track(struct IOExtTD *tio, uint track, uint8_t *buf, uint len)
{
    tio->iotd_Req.io_Command = CMD_CLEAR;
    tio->iotd_Req.io_Actual  = 0;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 0;
    tio->iotd_Req.io_Data    = NULL;
    tio->iotd_Req.io_Flags   = 0;
    (void) DoIO((struct IORequest *) tio);

    memset(buf, 0xa5, len);
    tio->iotd_Req.io_Command = CMD_READ;
    tio->iotd_Req.io_Actual  = 0;
    tio->iotd_Req.io_Offset  = track * len;
    tio->iotd_Req.io_Length  = len;
    tio->iotd_Req.io_Data    = buf;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    return (DoIO((struct IORequest *) tio));
}

/*
 * verify_track() reads a track with TD_RAWREAD and CMD_READ, and checks
 * that every sector decodes correctly, that the data matches expect (if
 * not NULL), and that CMD_READ returns the same data. The data read is
 * returned in got. Returns 0 if the track is good; otherwise prints
 * "Fail - ..." (no newline) and returns nonzero.
 */
static int
verify_track(struct IOExtTD *tio, uint track, uint8_t *raw, uint nsec,
             const uint8_t *expect, uint8_t *got, uint8_t *cmpbuf)
{
    uint tracklen = nsec * 512;
    uint sec;
    int  rc;
    mfm_result_t res;

    rc = do_rawread(tio, track, raw, rawread_len(nsec));
    if (rc != 0) {
        printf("Fail - TD_RAWREAD ");
        print_fail(rc);
        return (rc);
    }
    mfm_verify_track(raw, rawread_len(nsec), track, nsec, got, &res);
    if (res.sectors == 0) {
        printf("Fail - not an AmigaDOS track");
        return (1);
    }
    if (res.errors != 0) {
        printf("Fail - %s", res.first_error);
        return (1);
    }
    for (sec = 0; sec < nsec; sec++) {
        if ((res.found & BIT(sec)) == 0) {
            printf("Fail - sector %u not found", sec);
            return (1);
        }
        if ((expect != NULL) &&
            (memcmp(got + sec * 512, expect + sec * 512, 512) != 0)) {
            printf("Fail - sector %u data differs from what was written",
                   sec);
            return (1);
        }
    }

    rc = cmd_read_track(tio, track, cmpbuf, tracklen);
    if (rc != 0) {
        printf("Fail - CMD_READ ");
        print_fail(rc);
        return (rc);
    }
    for (sec = 0; sec < nsec; sec++) {
        if (memcmp(got + sec * 512, cmpbuf + sec * 512, 512) != 0) {
            printf("Fail - sector %u data differs from CMD_READ", sec);
            return (1);
        }
    }
    return (0);
}

/*
 * test_td_rawwrite() tests TD_RAWWRITE on track RAWWRITE_TRACK, and leaves
 * the data stored there as it was:
 *   1. The track is read and verified (TD_RAWREAD decode and CMD_READ).
 *   2. The sector data, inverted, is MFM-encoded and written with
 *      TD_RAWWRITE, and the track is verified to hold the inverted data.
 *      Changed data proves that the write took place. (Writing the same
 *      data in a different sector order would not: a floppy emulator
 *      stores only the sector data, and regenerates the track layout.)
 *   3. The original data is written back with TD_RAWWRITE, and verified.
 * If the track does not verify after a write, the original data is written
 * back with TD_FORMAT, which writes a whole track without reading it.
 */
#define RAWWRITE_TRACK 2  // Cylinder 1, head 0: not the boot block

static int
test_td_rawwrite(struct IOExtTD *tio)
{
    const uint track = RAWWRITE_TRACK;
    int      rc;
    uint     nsec = 0;
    uint     tracklen = 0;
    uint     len;
    uint     pos;
    uint8_t *raw = NULL;
    uint8_t *orig = NULL;
    uint8_t *inv = NULL;
    uint8_t *got = NULL;
    uint8_t *cmpbuf = NULL;
    struct DriveGeometry dg;
    const char *step;

    print_test_name("TD_RAWWRITE");
    if ((do_getgeometry(tio, &dg) == 0) && (dg.dg_SectorSize == 512) &&
        (dg.dg_TrackSectors > 0) && (dg.dg_TrackSectors <= 32) &&
        (dg.dg_Cylinders * dg.dg_Heads > track))
        nsec = dg.dg_TrackSectors;
    if (nsec == 0) {
        printf("Skip - track geometry unknown\n");
        return (0);
    }
    tracklen = nsec * 512;

    tio->iotd_Req.io_Command = TD_PROTSTATUS;
    tio->iotd_Req.io_Actual  = 0;
    tio->iotd_Req.io_Flags   = 0;
    if ((DoIO((struct IORequest *) tio) == 0) &&
        (tio->iotd_Req.io_Actual != 0)) {
        printf("Skip - disk is write protected\n");
        return (0);
    }

    /* TD_RAWREAD and TD_RAWWRITE must be in Chip RAM */
    raw    = (uint8_t *) AllocMem(RAWBUFSIZE, MEMF_PUBLIC | MEMF_CHIP);
    orig   = AllocMemType(tracklen, memtype);
    inv    = AllocMemType(tracklen, memtype);
    got    = AllocMemType(tracklen, memtype);
    cmpbuf = AllocMemType(tracklen, memtype);
    if ((raw == NULL) || (orig == NULL) || (inv == NULL) || (got == NULL) ||
        (cmpbuf == NULL)) {
        printf("Fail - unable to allocate buffers\n");
        rc = ENOMEM;
        goto rawwrite_done;
    }
    g_turn_motor_off = 1;

    /* 1. Original data must be fully verified before anything is written */
    if (verify_track(tio, track, raw, nsec, NULL, orig, cmpbuf) != 0) {
        printf("; not writing\n");
        rc = 0;  // Nothing tested
        goto rawwrite_done;
    }

    /* 2. Write the inverted data, and verify it */
    for (pos = 0; pos < tracklen; pos++)
        inv[pos] = (uint8_t) ~orig[pos];
    step = "inverted data";
    len = mfm_encode_track(raw, inv, nsec, track);
    rc = do_rawwrite(tio, track, raw, len);
    if (rc != 0) {
        printf("Fail - write ");
        print_fail(rc);
        goto rawwrite_restore;
    }
    if ((rc = verify_track(tio, track, raw, nsec, inv, got, cmpbuf)) != 0)
        goto rawwrite_restore;

    /* 3. Write the original data back, and verify it */
    step = "original data";
    len = mfm_encode_track(raw, orig, nsec, track);
    rc = do_rawwrite(tio, track, raw, len);
    if (rc != 0) {
        printf("Fail - rewrite ");
        print_fail(rc);
        goto rawwrite_restore;
    }
    if ((rc = verify_track(tio, track, raw, nsec, orig, got, cmpbuf)) != 0)
        goto rawwrite_restore;

    printf("Success  %u sectors written changed and restored, verified\n",
           nsec);
    goto rawwrite_done;

rawwrite_restore:
    /* The track did not verify: write back the original data */
    printf(" (%s write)\n", step);
    tio->iotd_Req.io_Command = TD_FORMAT;
    tio->iotd_Req.io_Actual  = 0;
    tio->iotd_Req.io_Offset  = track * tracklen;
    tio->iotd_Req.io_Length  = tracklen;
    tio->iotd_Req.io_Data    = orig;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("  Restore track");
    if (DoIO((struct IORequest *) tio) != 0) {
        printf("Fail - TD_FORMAT ");
        print_fail_nl(tio->iotd_Req.io_Error);
    } else if (verify_track(tio, track, raw, nsec, orig, got, cmpbuf) != 0) {
        printf("\n");
    } else {
        printf("Success  original data rewritten\n");
    }
    if (rc == 0)
        rc = 1;

rawwrite_done:
    if (cmpbuf != NULL)
        FreeMemType(cmpbuf, tracklen);
    if (got != NULL)
        FreeMemType(got, tracklen);
    if (inv != NULL)
        FreeMemType(inv, tracklen);
    if (orig != NULL)
        FreeMemType(orig, tracklen);
    if (raw != NULL)
        FreeMem(raw, RAWBUFSIZE);
    return (rc);
}

static int
test_cmd_update(struct IOExtTD *tio)
{
    int rc;

    /* Spin up device's motor */
    tio->iotd_Req.io_Command = CMD_UPDATE;
    tio->iotd_Req.io_Actual  = 0;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 1;
    tio->iotd_Req.io_Data    = NULL;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("CMD_UPDATE");
    rc = DoIO((struct IORequest *) tio);
    print_fail_nl(rc);

    return (rc);
}

static int
test_cmd_clear(struct IOExtTD *tio)
{
    int rc;

    /* Spin up device's motor */
    tio->iotd_Req.io_Command = CMD_CLEAR;
    tio->iotd_Req.io_Actual  = 0;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 1;
    tio->iotd_Req.io_Data    = NULL;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("CMD_CLEAR");
    rc = DoIO((struct IORequest *) tio);
    print_fail_nl(rc);

    return (rc);
}

static int
test_cmd_scsi(struct IOExtTD *tio)
{
    int rc;
    scsi_generic_t cmd;
    uint maxreplylen = 16384;  // maximum
    uint replylen;
    uint cmdlen;
    uint8_t *replybuf;
    char cmdstr[16];
    args_t *args = cur_test_args;
    uint pos;

    if (args->arg_count == 0) {
        printf("SCSI command requires at least two arguments (len and cmd)\n");
        return (EINVAL);
    }
    replylen = (uint16_t) cur_test_args->arg[0];
    cmdlen   = (uint8_t) (cur_test_args->arg[0] >> 16);
    if (cmdlen == 0) {
        printf("SCSI command requires at least two arguments (len and cmd)\n");
        return (EINVAL);
    }
    if (cmdlen > sizeof (cmd.bytes) + 1) {
        printf("Command length 0x%x is too long\n", cmdlen);
        return (EINVAL);
    }
    if (replylen > maxreplylen)
        replylen = maxreplylen;

    memset(&cmd, 0, sizeof (cmd));
    cmd.opcode = (uint8_t) (cur_test_args->arg[0] >> 24);
    memcpy(&cmd.bytes, &cur_test_args->arg[1], cmdlen - 1);
    if (g_verbose) {
        printf("replylen=%u cmdlen=%u CMD=%02x",
               replylen, cmdlen, cmd.opcode);
        for (pos = 0; pos < cmdlen - 1; pos++) {
            printf(" %02x", cmd.bytes[pos]);
        }
        printf("\n");
    }

    rc = do_scsidirect_cmd(tio, &cmd, cmdlen, replylen, (void **) &replybuf);

    sprintf(cmdstr, "SCSICMD %02x", cmd.opcode);
    print_test_name(cmdstr);
    if (rc == 0) {
        printf("Success");
        if (replybuf != NULL) {
            if (g_verbose) {
                uint pos;
                printf(" ");
                for (pos = 0; pos < replylen; pos++)
                    printf(" %02x", replybuf[pos]);
            }
            FreeMemType(replybuf, replylen);
        }
        printf("\n");
    } else {
        int key = SSD_SENSE_KEY(g_sense_data);
        if (SENSE_VALID() && (key == SKEY_NOT_READY)) {
            printf("Success  ");
            if (SSD_SENSE_ASC(g_sense_data) == 0x3a) {
                printf("Media not present\n");
            } else {
                printf("Not ready (ASC=%02x ASCQ=%02x)\n",
                       SSD_SENSE_ASC(g_sense_data),
                       SSD_SENSE_ASCQ(g_sense_data));
            }
        } else {
            print_fail(rc);
            if ((rc != ERROR_SENSE_CODE) && SENSE_VALID()) {
                printf("  SENSE %x/%02x/%02x",
                       SSD_SENSE_KEY(g_sense_data),
                       SSD_SENSE_ASC(g_sense_data),
                       SSD_SENSE_ASCQ(g_sense_data));
            }
            printf("\n");
            return (rc);
        }
    }

    /* Success, or a valid "not ready" (such as no media) response */
    return (0);
}

#define UBIT(x) (1ULL << (x))

#define TEST_CMD_GETGEOMETRY    UBIT(0)
#define TEST_TD_GETDRIVETYPE    UBIT(1)
#define TEST_TD_GETNUMTRACKS    UBIT(2)
#define TEST_TD_CHANGENUM       UBIT(3)
#define TEST_TD_CHANGESTATE     UBIT(4)
#define TEST_TD_PROTSTATUS      UBIT(5)
#define TEST_HD_SCSICMD_TUR     UBIT(6)
#define TEST_HD_SCSICMD_INQ     UBIT(7)
#define TEST_NSD_DEVICEQUERY    UBIT(8)
#define TEST_CMD_READ           UBIT(9)
#define TEST_ETD_READ           UBIT(10)
#define TEST_TD_READ64          UBIT(11)
#define TEST_NSCMD_TD_READ64    UBIT(12)
#define TEST_NSCMD_ETD_READ64   UBIT(13)
#define TEST_TD_SEEK            UBIT(14)
#define TEST_ETD_SEEK           UBIT(15)
#define TEST_TD_SEEK64          UBIT(16)
#define TEST_NSCMD_TD_SEEK64    UBIT(17)
#define TEST_NSCMD_ETD_SEEK64   UBIT(18)
#define TEST_CMD_WRITE          UBIT(19)
#define TEST_ETD_WRITE          UBIT(20)
#define TEST_TD_WRITE64         UBIT(21)
#define TEST_NSCMD_TD_WRITE64   UBIT(22)
#define TEST_NSCMD_ETD_WRITE64  UBIT(23)
#define TEST_TD_FORMAT          UBIT(24)
#define TEST_ETD_FORMAT         UBIT(25)
#define TEST_TD_FORMAT64        UBIT(26)
#define TEST_NSCMD_TD_FORMAT64  UBIT(27)
#define TEST_NSCMD_ETD_FORMAT64 UBIT(28)
#define TEST_TD_MOTOR_OFF       UBIT(29)
#define TEST_TD_MOTOR_ON        UBIT(30)
#define TEST_CMD_STOP           UBIT(31)
#define TEST_CMD_START          UBIT(32)
#define TEST_TD_EJECT           UBIT(33)
#define TEST_TD_LOAD            UBIT(34)
#define TEST_ADDREMCHANGEINT    UBIT(35)
#define TEST_TD_RAWREAD         UBIT(36)
#define TEST_TD_RAWWRITE        UBIT(37)
#define TEST_ETD_RAWREAD        UBIT(38)
#define TEST_ETD_RAWWRITE       UBIT(39)
#define TEST_CMD_UPDATE         UBIT(40)
#define TEST_CMD_CLEAR          UBIT(41)
#define TEST_CMD_SCSI           UBIT(42)

static const test_cmds_t test_cmds[] = {
    { "CHANGEINT",   2, TEST_ADDREMCHANGEINT, NULL,
                        "TD_ADDREMCHANGEINT Test change interrupt" },
    { "CHANGENUM",   0, TEST_TD_CHANGENUM, NULL,
                        "CMD_CHANGENUM Get media change count" },
    { "CHANGESTATE", 0, TEST_TD_CHANGESTATE, NULL,
                        "CMD_CHANGESTATE Get media change state" },
    { "DRIVETYPE",   0, TEST_TD_GETDRIVETYPE, NULL,
                        "CMD_GETDRIVETYPE Get drive type" },
    { "GEOMETRY",    0, TEST_CMD_GETGEOMETRY, NULL,
                        "CMD_GETGEOMETRY Get device geometry" },
    { "NSD",         0, TEST_NSD_DEVICEQUERY, NULL,
                        "CMD_NSD_DEVICEQUERY Query for NSD" },
    { "NUMTRACKS",   0, TEST_TD_GETNUMTRACKS, NULL,
                        "TD_GETNUMTRACKS Get track count" },
    { "INQUIRY",     0, TEST_HD_SCSICMD_INQ, NULL,
                        "HD_SCSICMD_INQ SCSI Inquiry command" },
    { "PROTSTATUS",  0, TEST_TD_PROTSTATUS, NULL,
                        "CMD_PROTSTATUS Get protected state" },
    { "TUR",         0, TEST_HD_SCSICMD_TUR, NULL,
                        "HD_SCSICMD_TUR SCSI Test Unit Ready command" },
    { "RAWREAD",     0, TEST_TD_RAWREAD, NULL,
                        "TD_RAWREAD read raw track from (floppy) device" },
    { "RAWWRITE",    1, TEST_TD_RAWWRITE, NULL,
                        "TD_RAWWRITE write raw track 2 (floppy) and restore" },
#if 0
    /* These are not implemented yet */
    { "ERAWREAD",    0, TEST_ETD_RAWREAD, NULL,
                        "ETD_RAWREAD read raw track from (floppy) device" },
    { "ERAWWRITE",   0, TEST_ETD_RAWWRITE, NULL,
                        "ETD_RAWWRITE write raw track to (floppy) device" },
#endif
    { "READ",        0, TEST_CMD_READ, "size,offset",
                        "CMD_READ Read from device" },
    { "EREAD",       0, TEST_ETD_READ, "size,offset",
                        "ETD_READ Extended read from device" },
    { "READ64",      0, TEST_TD_READ64, "size,offset,offsethi",
                        "TD_READ64 TD64 read from device" },
    { "NSDREAD",     0, TEST_NSCMD_TD_READ64, "size,offset,offsethi",
                        "NSCMD_TD_READ64 NSD Read from device" },
    { "NSDEREAD",    0, TEST_NSCMD_ETD_READ64, "size,offset,offsethi",
                        "NSCMD_ETD_READ64 NSD extended read from device" },
    { "SEEK",        0, TEST_TD_SEEK, "offset",
                        "TD_SEEK Seek to offset" },
    { "ESEEK",       0, TEST_ETD_SEEK, "offset",
                        "ETD_SEEK sxtended seek" },
    { "SEEK64",      0, TEST_TD_SEEK64, "offset,offsethi",
                        "TD_SEEK64 TD64 seek" },
    { "NSDSEEK",     0, TEST_NSCMD_TD_SEEK64, "offset,offsethi",
                        "NSCMD_TD_SEEK64 NSD seek" },
    { "NSDESEEK",    0, TEST_NSCMD_ETD_SEEK64, "offset,offsethi",
                        "NSCMD_ETD_SEEK64 NSD extended seek" },
    { "WRITE",       1, TEST_CMD_WRITE, "size,offset",
                        "CMD_WRITE write to device", },
    { "EWRITE",      1, TEST_ETD_WRITE, "size,offset",
                        "ETD_WRITE extended write", },
    { "WRITE64",     1, TEST_TD_WRITE64, "size,offset,offsethi",
                        "TD_WRITE64 TD64 write" },
    { "NSDWRITE",    1, TEST_NSCMD_TD_WRITE64, "size,offset,offsethi",
                        "NSCMD_TD_WRITE64 NSD write" },
    { "NSDEWRITE",   1, TEST_NSCMD_ETD_WRITE64, "size,offset,offsethi",
                        "NSCMD_ETD_WRITE64 NSD extended write" },
    { "FORMAT",      1, TEST_TD_FORMAT, NULL,
                        "TD_FORMAT format device" },
    { "EFORMAT",     1, TEST_ETD_FORMAT, NULL,
                        "ETD_FORMAT extended format device" },
    { "FORMAT64",    1, TEST_TD_FORMAT64, NULL,
                        "TD_FORMAT64 TD64 format device" },
    { "NSDFORMAT",   1, TEST_NSCMD_TD_FORMAT64, NULL,
                        "NSCMD_TD_FORMAT64 NSD format device" },
    { "NSDEFORMAT",  1, TEST_NSCMD_ETD_FORMAT64, NULL,
                        "NSCMD_ETD_FORMAT64 NSD extended format" },
    { "UPDATE",      0, TEST_CMD_UPDATE, NULL,
                        "CMD_UPDATE flushes the floppy track buffer" },
    { "CLEAR",       1, TEST_CMD_CLEAR, NULL,
                        "CMD_CLEAR discards the floppy track buffer" },
    { "MOTOROFF",    2, TEST_TD_MOTOR_OFF, NULL,
                        "TD_MOTOR OFF stop motor (spin down)" },
    { "MOTORON",     2, TEST_TD_MOTOR_ON, NULL,
                        "TD_MOTOR ON start motor (spin up)" },
    { "SCSI",        2, TEST_CMD_SCSI, "rlen,cb0,cb1,cb2...",
                        "HD_SCSICMD (SCSI direct)" },
    { "START",       2, TEST_CMD_START, NULL,
                        "CMD_START spin up device" },
    { "STOP",        2, TEST_CMD_STOP, NULL,
                        "CMD_STOP spin down device" },
    { "EJECT",       2, TEST_TD_EJECT, NULL,
                        "TD_EJECT device" },
    { "LOAD",        2, TEST_TD_LOAD, NULL,
                        "TD_LOAD device (insert media)" },
};

static int
test_packets_ll(uint64_t test_mask, struct IOExtTD *tio)
{
    int rc = 0;
    /*
     * To do:
     * TD_MOTOR
     * TD_RAWREAD
     * TD_RAWWRITE
     */

    if ((test_mask & TEST_CMD_GETGEOMETRY) && test_cmd_getgeometry(tio))
        rc++;
    if ((test_mask & TEST_TD_CHANGENUM) && test_td_changenum(tio))
        rc++;
    if ((test_mask & TEST_TD_CHANGESTATE) && test_td_changestate(tio))
        rc++;
    if ((test_mask & TEST_TD_PROTSTATUS) && test_td_protstatus(tio))
        rc++;
    if ((test_mask & TEST_TD_GETDRIVETYPE) && test_td_getdrivetype(tio))
        rc++;
    if ((test_mask & TEST_TD_GETNUMTRACKS) && test_td_getnumtracks(tio))
        rc++;
    if ((test_mask & TEST_TD_RAWREAD) && test_td_rawread(tio)) {
        test_mask &= ~TEST_TD_RAWWRITE;
        rc++;
    }
    if ((test_mask & TEST_CMD_UPDATE) && test_cmd_update(tio))
        rc++;
    if ((test_mask & TEST_CMD_CLEAR) && test_cmd_clear(tio))
        rc++;
    if ((test_mask & TEST_CMD_SCSI) && test_cmd_scsi(tio))
        rc++;

    if ((test_mask & TEST_TD_RAWWRITE) && test_td_rawwrite(tio))
        rc++;
    if ((test_mask & TEST_HD_SCSICMD_INQ) && test_hd_scsicmd_inquiry(tio))
        rc++;
    if ((test_mask & TEST_HD_SCSICMD_TUR) && test_hd_scsicmd_tur(tio))
        rc++;
    if ((test_mask & TEST_NSD_DEVICEQUERY) && test_nsd_devicequery(tio)) {
        test_mask &= ~(TEST_NSCMD_TD_READ64 | TEST_NSCMD_ETD_READ64 |
                       TEST_NSCMD_TD_SEEK64 | TEST_NSCMD_ETD_SEEK64 |
                       TEST_NSCMD_TD_WRITE64 | TEST_NSCMD_ETD_WRITE64 |
                       TEST_NSCMD_TD_FORMAT64 | TEST_NSCMD_ETD_FORMAT64);
        rc++;
    }

    if (is_user_abort()) {
        rc = -1;
        goto test_early_end;
    }

    if ((test_mask & TEST_CMD_READ) && test_cmd_read(tio)) {
        test_mask &= ~(TEST_ETD_READ | TEST_TD_READ64 |
                       TEST_NSCMD_TD_READ64 | TEST_NSCMD_ETD_READ64);
        rc++;
    }
    if ((test_mask & TEST_ETD_READ) && test_etd_read(tio))
        rc++;
    if ((test_mask & TEST_TD_READ64) && test_td_read64(tio))
        rc++;
    if ((test_mask & TEST_NSCMD_TD_READ64) && test_nscmd_td_read64(tio))
        rc++;
    if ((test_mask & TEST_NSCMD_ETD_READ64) && test_nscmd_etd_read64(tio))
        rc++;

    if (is_user_abort()) {
        rc = -1;
        goto test_early_end;
    }

    if ((test_mask & TEST_TD_SEEK) && test_td_seek(tio)) {
        test_mask &= ~(TEST_ETD_SEEK | TEST_TD_SEEK64 |
                       TEST_NSCMD_TD_SEEK64 | TEST_NSCMD_ETD_SEEK64);
        rc++;
    }
    if ((test_mask & TEST_ETD_SEEK) && test_etd_seek(tio))
        rc++;
    if ((test_mask & TEST_TD_SEEK64) && test_td_seek64(tio))
        rc++;
    if ((test_mask & TEST_NSCMD_TD_SEEK64) && test_nscmd_td_seek64(tio))
        rc++;
    if ((test_mask & TEST_NSCMD_ETD_SEEK64) && test_nscmd_etd_seek64(tio))
        rc++;

    if (is_user_abort()) {
        rc = -1;
        goto test_early_end;
    }

    if ((test_mask & TEST_CMD_STOP) && test_cmd_stop(tio))
        rc++;
    if ((test_mask & TEST_CMD_START) && test_cmd_start(tio))
        rc++;

    if ((test_mask & TEST_TD_EJECT) && test_td_eject(tio))
        rc++;
    if (is_user_abort()) {
        rc = -1;
        goto test_early_end;
    }
    if ((test_mask & TEST_TD_LOAD) && test_td_load(tio))
        rc++;
    if ((test_mask & TEST_ADDREMCHANGEINT) && test_addremchangeint(tio))
        rc++;

    if (is_user_abort()) {
        rc = -1;
        goto test_early_end;
    }

    /* Write */
    if (test_mask & (TEST_CMD_WRITE | TEST_ETD_WRITE |
                     TEST_TD_WRITE64 |
                     TEST_NSCMD_TD_WRITE64 | TEST_NSCMD_ETD_WRITE64 |
                     TEST_TD_FORMAT | TEST_ETD_FORMAT |
                     TEST_TD_FORMAT64 |
                     TEST_NSCMD_TD_FORMAT64 | TEST_NSCMD_ETD_FORMAT64)) {
        /*
         * Capture data before it is overwritten by any of the
         * destructive commands.
         */
        save_overwritten_data(tio, g_tbuf);

        if ((test_mask & TEST_CMD_WRITE) && test_cmd_write(tio)) {
            test_mask &= ~(TEST_ETD_WRITE | TEST_TD_WRITE64 |
                           TEST_NSCMD_TD_WRITE64 | TEST_NSCMD_ETD_WRITE64);
            rc++;
        }
        if ((test_mask & TEST_ETD_WRITE) && test_etd_write(tio))
            rc++;
        if ((test_mask & TEST_TD_WRITE64) && test_td_write64(tio))
            rc++;
        if ((test_mask & TEST_NSCMD_TD_WRITE64) && test_nscmd_td_write64(tio))
            rc++;
        if ((test_mask & TEST_NSCMD_ETD_WRITE64) && test_nscmd_etd_write64(tio))
            rc++;

        if (is_user_abort()) {
            rc = -1;
            goto test_early_end;
        }

        if ((test_mask & TEST_TD_FORMAT) && test_td_format(tio)) {
            test_mask &= ~(TEST_ETD_FORMAT | TEST_TD_FORMAT64 |
                           TEST_NSCMD_TD_FORMAT64 | TEST_NSCMD_ETD_FORMAT64);
            rc++;
        }
        if ((test_mask & TEST_ETD_FORMAT) && test_etd_format(tio))
            rc++;
        if ((test_mask & TEST_TD_FORMAT64) && test_td_format64(tio))
            rc++;
        if ((test_mask & TEST_NSCMD_TD_FORMAT64) && test_nscmd_td_format64(tio))
            rc++;
        if ((test_mask & TEST_NSCMD_ETD_FORMAT64) &&
            test_nscmd_etd_format64(tio))
            rc++;
    }
    if ((test_mask & TEST_TD_MOTOR_ON) && test_td_motor_on(tio))
        rc++;

    /* Motor off should be the last test, as a courtesy to floppy drives */
    if ((test_mask & TEST_TD_MOTOR_OFF) && test_td_motor_off(tio))
        rc++;

test_early_end:
    return (rc);
}

static int
test_packets(int do_destructive, int test_level,
             uint test_count, uint64_t test_masks[32])
{
    int    rc = 1;
    uint   cur;
    struct IOExtTD *tio;
    struct MsgPort *mp = NULL;
    uint8_t *buf[BUF_COUNT];
    uint lun = g_unitno / 10;
    size_t i;

    mp = CreatePort(0, 0);
    if (mp == NULL) {
        printf("Failed to create message port\n");
        return (1);
    }

    tio = (struct IOExtTD *) CreateExtIO(mp, sizeof (struct IOExtTD));
    if (tio == NULL) {
        printf("Failed to create tio struct\n");
        rc = 1;
        goto extio_fail;
    }
    if ((rc = open_device(tio)) != 0) {
        printf("Open %s Unit %u: ", g_devname, g_unitno);
        print_fail_nl(rc);
        rc = 1;
        goto opendev_fail;
    }

    memset(buf, 0, sizeof (buf));
    for (i = 0; i < ARRAY_SIZE(buf); i++) {
        buf[i] = (uint8_t *) AllocMemType(BUFSIZE, memtype);
        if (buf[i] == NULL) {
            report_allocmem_fail(BUFSIZE, memtype);
            goto allocmem_fail;
        }
    }
    g_lun = lun;
//  g_tbuf = buf;

    if (test_count == 0) {
        /* Run all available tests */
        uint64_t test_mask = 0xffffffffffffffff;
        cur_test_args = NULL;
        if (test_level <= 1) {
            /* Only do extended commands if requested "-tt" */
            test_mask &= ~(TEST_CMD_START | TEST_CMD_STOP |
                           TEST_TD_EJECT | TEST_TD_LOAD |
                           TEST_ADDREMCHANGEINT);
        }
        if (do_destructive == 0) {
            /* Only do destructive commands if requested "-d" */
            test_mask &= ~(TEST_CMD_WRITE | TEST_ETD_WRITE |
                           TEST_TD_WRITE64 |
                           TEST_NSCMD_TD_WRITE64 | TEST_NSCMD_ETD_WRITE64 |
                           TEST_TD_FORMAT | TEST_ETD_FORMAT |
                           TEST_TD_FORMAT64 |
                           TEST_NSCMD_TD_FORMAT64 | TEST_NSCMD_ETD_FORMAT64 |
                           TEST_TD_RAWWRITE |
                           TEST_CMD_CLEAR);
        }

        /* Command line only */
        test_mask &= ~TEST_CMD_SCSI;

        rc = test_packets_ll(test_mask, tio);
    } else {
        /* Run specified tests */
        int rc2;
        for (cur = 0; cur < test_count; cur++) {
            cur_test_args = &test_cmd_args[cur];
            if (test_masks[cur] &
                (TEST_NSCMD_TD_READ64 | TEST_NSCMD_ETD_READ64 |
                 TEST_NSCMD_TD_SEEK64 | TEST_NSCMD_ETD_SEEK64 |
                 TEST_NSCMD_TD_WRITE64 | TEST_NSCMD_ETD_WRITE64 |
                 TEST_NSCMD_TD_FORMAT64 | TEST_NSCMD_ETD_FORMAT64)) {
                g_has_nsd = 1;  // Assume NSD is available
            } else {
                g_has_nsd = 0;
            }
            if ((rc2 = test_packets_ll(test_masks[cur], tio)) == -1) {
                if (rc == 0)
                    rc = -1;
                break;  // Aborted early
            }
            if (rc == 0)
                rc = rc2;
        }
    }

    if (test_count == 0) {
        /*
         * Ignore individual test failures above, since no driver passes all
         * tests. If specific tests were requested, then the failure status
         * of those specific tests will be reported to the caller.
         */
        rc = 0;
    }

allocmem_fail:
    for (i = 0; i < ARRAY_SIZE(buf); i++)
        if (buf[i] != NULL)
            FreeMemType(buf[i], BUFSIZE);
    if (tio != NULL)
        close_device(tio);
opendev_fail:
    if (tio != NULL)
        DeleteExtIO((struct IORequest *) tio);
extio_fail:
    DeletePort(mp);
    return (rc);
}

/*
 * valid_startup() returns nonzero if a DOS device's startup value is a real
 * FileSysStartupMsg. Handlers which are not disk filesystems (RAM:, CON:,
 * PIPE:, etc) store a small integer or a string pointer there instead.
 */
static int
valid_startup(struct FileSysStartupMsg *fssm)
{
    struct DosEnvec *envec;
    uint8_t         *devname;

    if (((uintptr_t) fssm < 0x400) || (TypeOfMem(fssm) == 0))
        return (0);

    devname = (uint8_t *) BTOC(fssm->fssm_Device);
    envec   = (struct DosEnvec *) BTOC(fssm->fssm_Environ);
    if (((uintptr_t) devname < 0x400) || (TypeOfMem(devname) == 0) ||
        ((uintptr_t) envec < 0x400) || (TypeOfMem(envec) == 0))
        return (0);

    /*
     * Device name is a BSTR which is also NIL-terminated, so it can be
     * used as a C string. Some (such as DF0:) include the NIL in the BSTR
     * length; others have it just past the end of the BSTR.
     */
    if ((devname[0] == 0) ||
        (memchr(devname + 1, '\0', devname[0] + 1) == NULL))
        return (0);

    /* Environment must include geometry up to the high cylinder */
    if (envec->de_TableSize < DE_UPPERCYL)
        return (0);

    return (1);
}

/*
 * find_startup() finds the FileSysStartupMsg of the DOS device named
 * (for example "DH1:"). If the name is a volume or assign, the DOS device
 * which is handling it is used. Returns NULL (after reporting why) if no
 * disk device can be found.
 */
static struct FileSysStartupMsg *
find_startup(const char *name)
{
    struct FileSysStartupMsg *startup = NULL;
    struct DosLibrary        *DosBase;
    struct RootNode          *rootnode;
    struct DosInfo           *dosinfo;
    struct DevInfo           *devinfo;
    struct DevInfo           *found = NULL;
    const char               *pos;
    const char               *why = "not found";
    size_t                    namelen;

    pos = strchr(name, ':');
    namelen = (pos != NULL) ? (size_t) (pos - name) : strlen(name);

    DosBase = (struct DosLibrary *) OpenLibrary("dos.library", 0L);
    if (DosBase == NULL)
        return (NULL);

    rootnode = DosBase->dl_Root;
    dosinfo  = (struct DosInfo *) BTOC(rootnode->rn_Info);

    Forbid();
    for (devinfo = (struct DevInfo *) BTOC(dosinfo->di_DevInfo);
         devinfo != NULL;
         devinfo = (struct DevInfo *) BTOC(devinfo->dvi_Next)) {
        /* Compare against the BSTR name, including its length */
        uint8_t *devname = (uint8_t *) BTOC(devinfo->dvi_Name);
        if ((devname != NULL) && (devname[0] == namelen) &&
            (strncasecmp((char *) devname + 1, name, namelen) == 0)) {
            found = devinfo;
            break;
        }
    }

    if (found == NULL) {
        /* why = "not found" */
    } else if (found->dvi_Type == DLT_DEVICE) {
        startup = (struct FileSysStartupMsg *) BTOC(found->dvi_Startup);
        if (!valid_startup(startup)) {
            startup = NULL;
            why = "is not a disk device";
        }
    } else if (found->dvi_Task == NULL) {
        /*
         * A volume which is not mounted (or an assign with no handler).
         * Searching for a NULL handler task would match any DOS device
         * whose handler has not been started, which is the wrong device.
         */
        why = "is not mounted";
    } else {
        /* Volume or assign: find the DOS device with the same handler */
        APTR task = found->dvi_Task;
        why = "is not on a disk device";
        for (devinfo = (struct DevInfo *) BTOC(dosinfo->di_DevInfo);
             devinfo != NULL;
             devinfo = (struct DevInfo *) BTOC(devinfo->dvi_Next)) {
            if ((devinfo->dvi_Task == task) &&
                (devinfo->dvi_Type == DLT_DEVICE)) {
                startup = (struct FileSysStartupMsg *)
                          BTOC(devinfo->dvi_Startup);
                if (!valid_startup(startup))
                    startup = NULL;
                break;
            }
        }
    }
    Permit();

    CloseLibrary((struct Library *) DosBase);

    if (startup == NULL)
        printf("%.*s: %s\n", (int) namelen, name, why);
    return (startup);
}

/*
 * rand32
 * ------
 * Very simple pseudo-random number generator
 */
static uint32_t rand_seed = 0;
static uint32_t
rand32(void)
{
    rand_seed = (rand_seed * 25173) + 13849;
    return (rand_seed);
}

/*
 * srand32
 * -------
 * Very simple random number seed
 */
static void
srand32(uint32_t seed)
{
    rand_seed = seed;
}

static void
show_diffs(void *expected, void *data, uint len, const char *type)
{
    uint     pos;
    uint     miscompares = 0;
    uint8_t *eptr = (uint8_t *) expected;
    uint8_t *dptr = (uint8_t *) data;
    for (pos = 0; pos < len; pos++) {
        if (*eptr != *dptr) {
            if ((miscompares++ < 9) || (g_verbose > 1))
                printf("  %06x: %02x != %s %02x [diff %02x]\n",
                       pos, *dptr, type, *eptr, *dptr ^ *eptr);
        }
        dptr++;
        eptr++;
    }
    if (miscompares >= 8)
        printf("  %u miscompares\n", miscompares);
}

static uint
memcmp_const(uint8_t *buf, uint bufsize, uint val)
{
    uint32_t *buf32 = (uint32_t *) buf;
    val |= (val << 8);
    val |= (val << 16);
    bufsize /= 4;
    while (bufsize-- > 0) {
        if (*buf32 != val)
            return (1);
        buf32++;
    }
    return (0);
}

static const uint8_t chkpat[] = {
    0xa5, 0x5a, 0xc3, 0x3c, 0x81, 0x00, 0xff
};

static int
test_integrity_simple(struct IOExtTD *tio, uint bufsize)
{
    int             rc = 0;
    static uint64_t pos = 0;
    static uint8_t  curbuf = 0;
    uint64_t        devend;
    uint64_t        devsize;

    if (g_devend != 0)
        devend = g_devend;
    else
        devend = g_devsize;
    devsize = devend - g_devstart;

    /*
     * g_ibuf is the collection of buffers for integrity testing.
     *     g_ialign is located within the buffer g_ibuf
     *
     * g_align[0] is the data pattern to write
     * g_align[1] is the alternate (inverted) data pattern to write
     * g_align[2] is read buffer; initially patterned with 0x5a, but
     *               not re-patterned at every read
     * g_align[3] is second read buffer (when miscompare happens
     *               on the first read). Re-patterned with 0xa5 at
     *               every re-read.
     * g_ibuf[4] is the original data buffer, unless -dd specified
     * g_ibuf[5] is not used for this test
     */

    if (pos + bufsize > devsize) {
        pos = 0;
        checknum++;
    }

    if (flag_destructive < 2) {
        /*
         * Save a copy of the original data. If this read fails, the
         * original data is unknown, so it must not be overwritten (it
         * could not be restored).
         */
        rc = do_read_cmd(tio, pos, bufsize, g_ibuf[4], g_has_nsd);
        if (rc != 0) {
            printf("read of original data failed at %s: ",
                   llu_sector_to_str(pos));
            print_fail_nl(rc);
            return (rc);
        }
    }

    if (g_verbose) {
        uint percent = pos * 1000 / devsize;
        printf("Sector %s (%u.%u%% of media check #%u)\n",
               llu_sector_to_str(pos), percent / 10, percent % 10, checknum);
    }
    if (flag_destructive) {
        rc = do_write_cmd(tio, pos, bufsize, g_align[curbuf], g_has_nsd);
        if (rc != 0) {
            printf("write failed at %s\n", llu_sector_to_str(pos));
            goto integrity_fail;
        }
    }
    rc = do_read_cmd(tio, pos, bufsize, g_align[2], g_has_nsd);
    if (rc != 0) {
        printf("read failed at %s\n", llu_sector_to_str(pos));
        goto integrity_fail;
    }
    if ((flag_destructive &&
         (memcmp(g_align[curbuf], g_align[2], bufsize) != 0)) ||
        (!flag_destructive &&
         (memcmp(g_ibuf[4], g_align[2], bufsize) != 0))) {
        /*
         * Read data either mismatches the newly written pattern
         * (destructive mode) or mismatches the previously read data
         * (read-only mode).
         */
        printf("Miscompare at %s\n", llu_sector_to_str(pos));
        if (memcmp_const(g_align[2], bufsize, 0xa5) == 0) {
            printf("Read buffer was not updated\n");
        } else if (flag_destructive) {
            show_diffs(g_align[curbuf], g_align[2], bufsize, "expected");
        } else {
            show_diffs(g_ibuf[4], g_align[2], bufsize, "original");
        }

        /* Pattern the second read buffer */
        memset(g_align[3], 0x5a, bufsize);

        rc = do_read_cmd(tio, pos, bufsize, g_align[3], g_has_nsd);
        if (rc != 0) {
            printf("Re-read failed at %s\n", llu_sector_to_str(pos));
            goto integrity_fail;
        }
        if (memcmp_const(g_align[3], bufsize, 0x5a) == 0) {
            printf("Re-read buffer was not updated\n");
        } else if (memcmp(g_align[2], g_align[3], bufsize) == 0) {
            if (flag_destructive) {
                printf("Re-read of data matches what was read "
                       "(write failure?)\n");
            } else {
                printf("Re-read of data matches what was read "
                       "the second time\n");
            }
        } else if (flag_destructive &&
                   (memcmp(g_align[curbuf], g_align[3], bufsize) == 0)) {
            printf("Re-read of data matches what was written "
                   "(read failure?)\n");
        } else if ((flag_destructive < 2) &&
                   (memcmp(g_ibuf[4], g_align[3], bufsize) == 0)) {
            /* g_ibuf[4] (original data) is not allocated with -dd */
            printf("Re-read of data matches what was originally on disk\n");
        } else {
            printf("Re-read of data differs (floating data?)\n");
            if (flag_destructive) {
                show_diffs(g_align[curbuf], g_align[3], bufsize, "expected");
                show_diffs(g_align[2], g_align[3], bufsize, "first read");
            } else {
                show_diffs(g_ibuf[4], g_align[3], bufsize, "first read");
                show_diffs(g_align[2], g_align[3], bufsize, "second read");
            }
        }
        CacheClearU();
        if (flag_destructive) {
            if (memcmp(g_align[curbuf], g_align[2], bufsize) == 0) {
                printf("Initial read data now matches what was written "
                       "(CPU cache or memory failure?)\n");
            }
        } else {
            if (memcmp(g_ibuf[4], g_align[2], bufsize) == 0) {
                printf("Initial read data now matches second read "
                       "(CPU cache or memory failure?)\n");
            }
        }
        rc = 1;
        goto integrity_fail;
    }

integrity_fail:
    if (flag_destructive == 1) {
        int rc2 = do_write_cmd(tio, pos, bufsize, g_ibuf[4], g_has_nsd);
        if (rc2 != 0) {
            /* Bad day: you may have lost data */
            printf("restore of original data failed at %s\n",
                   llu_sector_to_str(pos));
            if (rc == 0)
                rc = rc2;
        }
    }
    if (rc == 0) {
        pos += bufsize;
        curbuf ^= 1;
    }
    return (rc);
}

/*
 * test_integrity_butterfly
 * ------------------------
 * This test will repeatedly do a butterfly pattern on the media.
 * The entire device is not done in a single test loop. Instead, if we
 * segment the media into zones of contiguous blocks, each zone is touched
 * in a butterfly pattern at each loop.
 *
 * For example: Consider a device with 56 blocks. It is divided into
 *              8 zones, each one with 7 blocks (A...G)
 *
 * Pass| Zone0 | Zone1 | Zone2 | Zone3 | Zone4 | Zone5 | Zone6 | Zone7 |
 *      ABCDEFG ABCDEFG ABCDEFG ABCDEFG ABCDEFG ABCDEFG ABCDEFG ABCDEFG
 *  1   1                                                       2
 *  1           3                                       4
 *  1                   5                       6
 *  1                            7      8
 *  1                             9      10
 *  1                    11                      12
 *  1            13                                      14
 *  1    15                                                      16
 *
 *  The above ends the first pass. This function returns. When called
 *  again, it will resume the butterfly test at block C in all zones.
 *  This will continue until all blocks in all zones have been touched,
 *  in which case the test will start over with block A.
 *
 * Pass| Zone0 | Zone1 | Zone2 | Zone3 | Zone4 | Zone5 | Zone6 | Zone7 |
 *      ABCDEFG ABCDEFG ABCDEFG ABCDEFG ABCDEFG ABCDEFG ABCDEFG ABCDEFG
 *  2     1                                                       2
 *  2             3                                       4
 *  2                     5                       6
 *  2                              7      8
 *  2                               9      10
 *  2                      11                      12
 *  2              13                                      14
 *  2      15                                                      16
 */
static int
test_integrity_butterfly(struct IOExtTD *tio, uint bufsize)
{
    int rc = 0;
    static uint64_t zoneoffset = 0;
    uint64_t devblocks;
    uint64_t bytes_per_zone;
    uint64_t leftoffset;
    uint64_t rightoffset;
    uint64_t devend;
    uint64_t devsize;
    uint     iter;
    uint     fail = 0;

    if (g_devend != 0)
        devend = g_devend;
    else
        devend = g_devsize;
    devsize = devend - g_devstart;

#define ZONES 31  // Must be a prime number to avoid duplicates

    devblocks = devsize / g_sector_size;
    /* Zone size is a whole number of transfers (bufsize need not be 2^n) */
    bytes_per_zone = (devsize / ZONES) / bufsize * bufsize;
    if (bytes_per_zone == 0) {
        printf("Device is not large enough for butterfly test with block "
               "size %u (needs at least %s bytes)\n",
               bufsize, llu_to_str((uint64_t) bufsize * ZONES));
        return (1);
    }

    if (g_verbose > 1) {
        printf("devsize=%s ", llu_to_str(devsize));
        printf("devblocks=%s ", llu_to_str(devblocks));
        printf("bpz=%s\n", llu_to_str(bytes_per_zone));
    }
    if (g_verbose == 1) {
        uint percent = zoneoffset * 1000 / bytes_per_zone;
        printf("  Zone sector %s (%u.%u%% of media check #%u) %s\n",
               llu_sector_to_str(zoneoffset), percent / 10, percent % 10,
               checknum + 1,
               bytes_to_human_str(bytes_per_zone * checknum + zoneoffset,
                                  ZONES * 2));
    }

    leftoffset = zoneoffset;
    for (iter = 0; iter < ZONES; iter++) {
        rightoffset = devsize - leftoffset - bufsize;

        /* Check for overlap */
        if (((leftoffset <= rightoffset) &&
             (leftoffset + bufsize > rightoffset)) ||
            ((leftoffset > rightoffset) &&
             (rightoffset + bufsize > leftoffset))) {
            /* Overlap detected */
            if (leftoffset < bufsize) {
#if 0
                printf("Butterfly offsets %s and ", llu_to_str(leftoffset));
                printf("%s overlap ", llu_to_str(rightoffset));
                printf("at sector %s.\n", llu_sector_to_str(leftoffset));
#endif
                printf("Device is not large enough to test with block "
                       "size %u\n", bufsize);
                fail++;
                goto fail_cleanup;
            }
            rightoffset = 0;
        }

        /*
         * Algorithm at each iteration:
         *   Save left
         *   Save right
         *   Write left
         *   Write right
         *   Verify left
         *   Verify right
         *   Restore left
         *   Restore right
         */
        if (g_verbose > 1) {
            printf("%12s  ", llu_sector_to_str(leftoffset));
            printf("%12s\n", llu_sector_to_str(rightoffset));
        }
        if (flag_destructive < 2) {
            /* Save a copies of the original data */
            rc = do_read_cmd(tio, leftoffset, bufsize, g_ibuf[4], g_has_nsd);
            if (rc != 0) {
                printf("Read failed at %s\n", llu_sector_to_str(leftoffset));
                break;
            }
            rc = do_read_cmd(tio, rightoffset, bufsize, g_ibuf[5], g_has_nsd);
            if (rc != 0) {
                printf("Read failed at %s\n", llu_sector_to_str(rightoffset));
                break;
            }
        }
        if (flag_destructive) {
            /* Write data patterns */
            rc = do_write_cmd(tio, leftoffset, bufsize, g_align[0], g_has_nsd);
            if (rc != 0) {
                printf("Write failed at %s\n", llu_sector_to_str(leftoffset));
                fail++;
                goto fail_cleanup;
            }
            rc = do_write_cmd(tio, rightoffset, bufsize, g_align[1], g_has_nsd);
            if (rc != 0) {
                printf("Write failed at %s\n", llu_sector_to_str(rightoffset));
                fail++;
                goto fail_cleanup;
            }
        }

        /* Handle the left data block */
        rc = do_read_cmd(tio, leftoffset, bufsize, g_align[2], g_has_nsd);
        if (rc != 0) {
            printf("Read failed at %s\n", llu_sector_to_str(leftoffset));
            fail++;
            goto fail_cleanup;
        }
        if ((flag_destructive &&
             (memcmp(g_align[0], g_align[2], bufsize) != 0)) ||
            (!flag_destructive &&
             (memcmp(g_ibuf[4], g_align[2], bufsize) != 0))) {
            /*
             * Read data either mismatches the newly written pattern
             * (destructive mode) or mismatches the previously read data
             * (read-only mode).
             */
            printf("Miscompare at %s\n", llu_sector_to_str(leftoffset));
            if (memcmp_const(g_align[2], bufsize, 0xa5) == 0) {
                printf("Read buffer was not updated\n");
            } else if (flag_destructive &&
                       (memcmp(g_align[1], g_align[2], bufsize) == 0)) {
                printf("Read buffer has previously written data: "
                       "read did not succeed?\n");
            } else if (flag_destructive) {
                show_diffs(g_align[0], g_align[2], bufsize, "expected");
            } else {
                show_diffs(g_ibuf[4], g_align[2], bufsize, "first read");
            }
            fail++;
        }
        if (fail) {
            /* Pattern the second read buffer */
            memset(g_align[3], 0xa5, bufsize);

            /* Re-read data */
            rc = do_read_cmd(tio, leftoffset, bufsize, g_align[3],
                             g_has_nsd);
            if (memcmp(g_align[2], g_align[3], bufsize) == 0) {
                if (flag_destructive) {
                    printf("Re-read of data matches what was read "
                           "(write failure?)\n");
                } else {
                    printf("Re-read of data matches what was read "
                           "the second time\n");
                }
            } else if (memcmp_const(g_align[3], bufsize, 0xa5) == 0) {
                printf("Re-read buffer was not updated\n");
            } else if (flag_destructive &&
                       (memcmp(g_align[0], g_align[3], bufsize) == 0)) {
                printf("Re-read of data matches what was written "
                       "(read failure?)\n");
            } else if (!flag_destructive &&
                       (memcmp(g_ibuf[4], g_align[3], bufsize) == 0)) {
                printf("Re-read of data matches first read "
                       "(read failure?)\n");
            } else {
                printf("Re-read of data differs (floating data?)\n");
                if (flag_destructive) {
                    show_diffs(g_align[0], g_align[3], bufsize, "expected");
                    show_diffs(g_align[2], g_align[3], bufsize, "first read");
                } else {
                    show_diffs(g_ibuf[4], g_align[3], bufsize, "first read");
                    show_diffs(g_align[2], g_align[3], bufsize, "second read");
                }
            }
            CacheClearU();
            if (flag_destructive) {
                if (memcmp(g_align[0], g_align[2], bufsize) == 0) {
                    printf("Initial read data now matches what was written "
                           "(CPU cache or memory failure?)\n");
                }
            } else {
                if (memcmp(g_ibuf[4], g_align[2], bufsize) == 0) {
                    printf("Initial read data now matches second read "
                           "(CPU cache or memory failure?)\n");
                }
            }
            goto fail_cleanup;
        }

        /* Handle the right data block */
        rc = do_read_cmd(tio, rightoffset, bufsize, g_align[2], g_has_nsd);
        if (rc != 0) {
            printf("Read failed at %s\n", llu_sector_to_str(rightoffset));
            fail++;
            goto fail_cleanup;
        }
        if ((flag_destructive &&
             (memcmp(g_align[1], g_align[2], bufsize) != 0)) ||
            (!flag_destructive &&
             (memcmp(g_ibuf[5], g_align[2], bufsize) != 0))) {
            /*
             * Read data either mismatches the newly written pattern
             * (destructive mode) or mismatches the previously read data
             * (read-only mode).
             */
            printf("Miscompare at %s\n", llu_sector_to_str(rightoffset));
            if (memcmp_const(g_align[2], bufsize, 0xa5) == 0) {
                printf("Read buffer was not updated\n");
            } else if (flag_destructive &&
                       (memcmp(g_align[1], g_align[2], bufsize) == 0)) {
                printf("Read buffer has previously written data: "
                       "read did not succeed?\n");
            } else if (flag_destructive) {
                show_diffs(g_align[1], g_align[2], bufsize, "expected");
            } else {
                show_diffs(g_ibuf[5], g_align[2], bufsize, "first read");
            }
            fail++;
        }
        if (fail) {
            /* Pattern the second read buffer */
            memset(g_align[3], 0xa5, bufsize);

            /* Re-read data */
            rc = do_read_cmd(tio, rightoffset, bufsize, g_align[3],
                             g_has_nsd);
            if (memcmp(g_align[2], g_align[3], bufsize) == 0) {
                if (flag_destructive) {
                    printf("Re-read of data matches what was read "
                           "(write failure?)\n");
                } else {
                    printf("Re-read of data matches what was read "
                           "the second time\n");
                }
            } else if (memcmp_const(g_align[3], bufsize, 0xa5) == 0) {
                printf("Re-read buffer was not updated\n");
            } else if (flag_destructive &&
                       (memcmp(g_align[1], g_align[3], bufsize) == 0)) {
                printf("Re-read of data matches what was written "
                       "(read failure?)\n");
            } else if (!flag_destructive &&
                       (memcmp(g_ibuf[5], g_align[3], bufsize) == 0)) {
                printf("Re-read of data matches first read "
                       "(read failure?)\n");
            } else {
                printf("Re-read of data differs (floating data?)\n");
                if (flag_destructive) {
                    show_diffs(g_align[1], g_align[3], bufsize, "expected");
                    show_diffs(g_align[2], g_align[3], bufsize, "first read");
                } else {
                    show_diffs(g_ibuf[5], g_align[3], bufsize, "first read");
                    show_diffs(g_align[2], g_align[3], bufsize, "second read");
                }
            }
            CacheClearU();
            if (flag_destructive) {
                if (memcmp(g_align[1], g_align[2], bufsize) == 0) {
                    printf("Initial read data now matches what was written "
                           "(CPU cache or memory failure?)\n");
                }
            } else {
                if (memcmp(g_ibuf[5], g_align[2], bufsize) == 0) {
                    printf("Initial read data now matches second read "
                           "(CPU cache or memory failure?)\n");
                }
            }
            goto fail_cleanup;
        }

fail_cleanup:
        if (flag_destructive == 1) {
            /* Restore original data */
            rc = do_write_cmd(tio, leftoffset, bufsize, g_ibuf[4], g_has_nsd);
            if (rc != 0) {
                /* Try again */
                printf("Write failed at %s", llu_sector_to_str(leftoffset));
                rc = do_write_cmd(tio, leftoffset, bufsize, g_ibuf[4],
                                  g_has_nsd);
                if (rc != 0)
                    printf("; data compromised");
                printf("\n");
                fail++;
            }
            rc = do_write_cmd(tio, rightoffset, bufsize, g_ibuf[5], g_has_nsd);
            if (rc != 0) {
                /* Try again */
                printf("Write failed at %s", llu_sector_to_str(rightoffset));
                rc = do_write_cmd(tio, rightoffset, bufsize, g_ibuf[5],
                                  g_has_nsd);
                if (rc != 0)
                    printf("; data compromised");
                printf("\n");
                fail++;
            }
        }
        if (fail) {
            /* A failure occurred */
            rc = 1;
            break;
        }

        leftoffset += bytes_per_zone;
        if (is_user_abort()) {
            printf("^C abort\n");
            rc = 1;
            break;
        }
    }
    zoneoffset += bufsize;
    if (zoneoffset + bufsize > bytes_per_zone) {
        zoneoffset = 0;
        checknum++;
    }

    return (rc);
}

static int
test_integrity(uint mode, uint pattern, uint32_t memtype, uint bufsize,
               uint align)
{
    struct IOExtTD *tio;
    struct MsgPort *mp;
    uint8_t chkcur = 0;
    uint    bnum;
    int     rc = 0;

    mp = CreatePort(0, 0);
    if (mp == NULL) {
        printf("Failed to create message port\n");
        return (1);
    }

    tio = (struct IOExtTD *) CreateExtIO(mp, sizeof (struct IOExtTD));
    if (tio == NULL) {
        printf("Failed to create tio struct\n");
        rc = 1;
        goto extio_fail;
    }

    if ((rc = open_device(tio)) != 0) {
        printf("Open %s Unit %u: ", g_devname, g_unitno);
        print_fail_nl(rc);
        rc = 1;
        goto opendev_fail;
    }

    if (g_ibuf[0] == NULL) {
        for (bnum = 0; bnum < ARRAY_SIZE(g_ibuf); bnum++) {
            uint32_t base;
            uint     cur;
            /*
             * g_ibuf is the collection of buffers for integrity testing.
             *     g_ialign is located within the buffer g_ibuf
             *
             * g_align[0] is the data pattern to write
             * g_align[1] is the alternate (inverted) data pattern to write
             * g_align[2] is read buffer; initially patterned with 0x5a, but
             *               not re-patterned at every read
             * g_align[3] is second read buffer (when miscompare happens
             *               on the first read). Re-patterned with 0xa5 at
             *               every re-read.
             * g_ibuf[4] is original data buffer 1, unless -dd specified
             * g_ibuf[5] is original data buffer 2, unless -dd specified
             */
            if (((bnum == 4) || (bnum == 5)) && (flag_destructive > 1))
                continue;
            g_ibuf[bnum] = AllocMemType(bufsize + align, memtype);
            if (g_ibuf[bnum] == NULL) {
                report_allocmem_fail(bufsize + align, memtype);
                rc = ENOMEM;
                goto integrity_fail;
            }

            /*
             * Create specific alignment
             * If the address is aligned at a lower order of alignment,
             * then need to increase the alignment by rounding up.
             *    Example: address = 0x0004  desired_alignment = 8
             *             address = 0x0008
             * If the address is already aligned at a higher order than
             * the requested alignment, need to reduce the alignment.
             *    Example: address = 0x0004  desired_alignment = 1
             *             address = 0x0005
             */
            base = (uintptr_t) g_ibuf[bnum];
            if (base & (align - 1)) {
                /* Lower order of alignment; round up */
                g_align[bnum] = (void *) ((base + align - 1) & ~(align - 1));
            } else {
                /* Higher order of alignment; force alignment */
                g_align[bnum] = (void *) (base + align);
            }
            if (g_verbose > 1)
                printf("Align %p\n", g_align[bnum]);

            if (bnum == 0) {
                switch (pattern) {
                    default: {
                        uint32_t *ptr = (uint32_t *) g_align[bnum];
                        srand32(time(NULL));
                        for (cur = 0; cur < bufsize / 4; cur++)
                            ptr[cur] = rand32();
                        break;
                    }
                    case 2:
                        for (cur = 0; cur < bufsize; cur++)
                            g_align[bnum][cur] = (uint8_t) cur;
                        break;
                    case 3: {
                        for (cur = 0; cur < bufsize; cur++) {
                            g_align[bnum][cur] = chkpat[chkcur++];
                            if (chkcur >= ARRAY_SIZE(chkpat))
                                chkcur = 0;
                        }
                        break;
                    }
                }
            } else if (bnum == 1) {
                for (cur = 0; cur < bufsize; cur++)
                    g_align[bnum][cur] = ~g_align[0][cur];
            } else if (bnum == 2) {
                /* Pattern the receive buffer */
                memset(g_align[bnum], 0xa5, bufsize);
            }
        }
    }

    if ((g_devsize == 0) && ((rc = get_devsize(tio)) != 0)) {
        printf("Failed to get device size\n");
        goto integrity_fail;
    }

    switch (mode) {
        case 0:  // Mode not specified with -k
        case 1:  // -k simple
        default:
            rc = test_integrity_simple(tio, bufsize);
            break;
        case 2:  // -k butterfly
            rc = test_integrity_butterfly(tio, bufsize);
            break;
    }
    g_turn_motor_off = 1;

integrity_fail:
    close_device(tio);
opendev_fail:
    DeleteExtIO((struct IORequest *) tio);
extio_fail:
    DeletePort(mp);

    return (rc);
}

static void
show_cmds(void)
{
    size_t bit;
    printf("  Name        Args                 Description\n"
           "  ----------- -------------------- --------------------------\n");
    for (bit = 0; bit < ARRAY_SIZE(test_cmds); bit++) {
        printf("  %-11s %-20s %s\n",
               test_cmds[bit].alias,
               test_cmds[bit].args ? test_cmds[bit].args : NULL,
               test_cmds[bit].desc);
    }
}

static void
usage_cmd(void)
{
    printf("-c <cmd>  tests a specific trackdisk command\n");
    show_cmds();
}

static void
show_arg_help(const char *str, uint cmd)
{
    const char *const arg_help = test_cmds[cmd].args;
    if (arg_help == NULL)
        printf("No arguments for this command\n");
    else
        printf("%s(%s)\n", str, arg_help);
}

/*
 * max_cmd_args() returns the number of arguments a -c command accepts,
 * which is the number of comma-separated names in its argument help.
 * Checking this while parsing the command line catches too many arguments
 * before the idle task is started (get_args_1/2/3 must not exit()).
 */
static uint
max_cmd_args(uint cmd)
{
    const char *str = test_cmds[cmd].args;
    uint count;

    if (str == NULL)
        return (0);
    for (count = 1; *str != '\0'; str++)
        if (*str == ',')
            count++;
    return (count);
}

/*
 * get_cmd_scsi_args() packs the -c scsi(rlen,cmd,...) arguments into
 * args->arg[] as bytes: arg[0] holds the opcode, CDB length, and reply
 * length; the remaining CDB bytes start at byte offset 4 (args->arg[1]).
 * The CDB bytes following the opcode must fit in scsi_generic_t.bytes.
 */
#define SCSI_ARG_MAX_POS (4 + sizeof (((scsi_generic_t *) 0)->bytes))

static void
get_cmd_scsi_args(args_t *args, const char *arg)
{
    int count;
    uint val;
    uint argpos = 0;
    if (arg == NULL) {
        printf("SCSI command requires len + cmd args (rlen,cb0,cb1,cb2...)\n"
               "   (0,0,0000000000)             - Test Unit Ready\n"
               "   (200,8,000000,1,0)           - Read(6) a 512 byte sector\n"
               "   (200,a,000000,1,0)           - Write(6) a 512 byte sector\n"
               "   (800,28,0,00000000,0,0001,0) - Read(10) a 2048 byte sector\n"
               "   (200,2a,0,00000000,0,0001,0) - Write(10) a 512 byte sector\n"
               "   (28,12,000000,28,0)          - Inquiry\n"
               "   (8,25,00,00000000,00000000)  - Read Capacity(10)\n"
               "   (8,9e,10,0)                  - Read Capacity(16)\n"
               "   (ff,1a,8,3f,0,ff,0)          - Mode Sense (all pages)\n"
               "   (18,1a,8,3,0,18,0)           - Get Mode Page 3\n"
               "");
        exit(RETURN_ERROR);
    }
    memset(args->arg, 0, sizeof (args->arg));
    while (sscanf(arg, "%x%n", &val, &count) == 1) {
        if (count != 0) {
            arg += count;
            if (*arg == ',')
                arg++;
        }
        switch (argpos) {
            case 0:  // reply length
                args->arg[0] = (uint16_t) val;
                argpos += 3;
                break;
            case 3:  // cmd
                args->arg[0] |= (val & 0xff) << 24;
                argpos += 1;
                break;
            default: {
                /*
                 * The number of hex digits determines the field width.
                 * Bytes are stored one at a time, big-endian, as the
                 * field may not be aligned (68000 faults on unaligned
                 * word and long access).
                 */
                uint nbytes;
                uint cur;
                if (count <= 2)
                    nbytes = 1;
                else if (count <= 4)
                    nbytes = 2;
                else if (count <= 6)
                    nbytes = 3;
                else if (count <= 8)
                    nbytes = 4;
                else
                    nbytes = 8;  // val is only 32 bits; upper bytes are 0
                if (argpos + nbytes > SCSI_ARG_MAX_POS) {
                    printf("SCSI command is too long (max %u bytes)\n",
                           SCSI_ARG_MAX_POS - 3);
                    exit(RETURN_ERROR);
                }
                for (cur = 0; cur < nbytes; cur++) {
                    uint shift = (nbytes - 1 - cur) * 8;
                    ((uint8_t *) args->arg)[argpos++] =
                        (shift < 32) ? (uint8_t) (val >> shift) : 0;
                }
                break;
            }
        }
    }
    if (argpos < 4) {
        printf("SCSI command requires len and cmd args\n");
        exit(RETURN_ERROR);
    }
    args->arg[0] |= ((argpos - 3) & 0xff) << 16;
    args->arg_count = argpos - 3;
}

static uint64_t
get_cmd(const char *str, args_t *args)
{
    size_t pos;
    uint   col;
    uint   cols = 6;
    uint   row;
    uint   rows;
    char  *arg = strchr(str, '(');

    args->arg_count = 0;
    if (arg != NULL)
        *(arg++) = '\0';

    for (pos = 0; pos < ARRAY_SIZE(test_cmds); pos++) {
        if (strcasecmp(test_cmds[pos].alias, str) == 0) {
            if (test_cmds[pos].mask == TEST_CMD_SCSI) {
                get_cmd_scsi_args(args, arg);
            } else if (arg != NULL) {
                int count;
                uint val;
                while (sscanf(arg, "%i%n", (int *) &val, &count) == 1) {
                    if (count != 0) {
                        arg += count;
                        if (*arg == ',')
                            arg++;
                    }
                    if (args->arg_count == ARRAY_SIZE(args->arg)) {
                        printf("Too many arguments to %s\n", str);
                        exit(RETURN_ERROR);
                    }
                    args->arg[args->arg_count++] = val;
                }
                if ((args->arg_count == 0) ||
                    (args->arg_count > max_cmd_args(pos))) {
                    if (args->arg_count != 0)
                        printf("Too many arguments to %s: ", str);
                    show_arg_help(str, pos);
                    exit(RETURN_ERROR);
                }
            }
            return (test_cmds[pos].mask);
        }
    }

    printf("Invalid test command \"%s\"\n", str);
    printf("Use one of:\n");
    rows = (ARRAY_SIZE(test_cmds) + cols - 1) / cols;
    for (row = 0; row < rows; row++) {
        printf("  ");
        for (col = 0; col < cols; col++) {
            pos = col * rows + row;
            if (pos >= ARRAY_SIZE(test_cmds))
                break;
            printf("%-13s", test_cmds[pos].alias);
        }
        printf("\n");
    }
    exit(RETURN_ERROR);
}

/*
 * destructive_cmd_name() returns the name of a -c command if that command
 * may modify data on the device (and so requires -d), otherwise NULL.
 * For the raw SCSI command, the opcode is checked against known
 * data-modifying SCSI commands.
 */
static const char *
destructive_cmd_name(uint64_t mask, const args_t *args)
{
    static const uint8_t scsi_modify_opcodes[] = {
        0x04,  // FORMAT UNIT
        0x07,  // REASSIGN BLOCKS
        0x0a,  // WRITE(6)
        0x15,  // MODE SELECT(6)
        0x2a,  // WRITE(10)
        0x2e,  // WRITE AND VERIFY(10)
        0x3b,  // WRITE BUFFER (may update firmware)
        0x3f,  // WRITE LONG(10)
        0x41,  // WRITE SAME(10)
        0x55,  // MODE SELECT(10)
        0x8a,  // WRITE(16)
        0x8e,  // WRITE AND VERIFY(16)
        0x93,  // WRITE SAME(16)
        0xaa,  // WRITE(12)
        0xae,  // WRITE AND VERIFY(12)
    };
    size_t pos;

    for (pos = 0; pos < ARRAY_SIZE(test_cmds); pos++) {
        if (test_cmds[pos].mask != mask)
            continue;
        if (mask == TEST_CMD_SCSI) {
            uint8_t opcode = (uint8_t) (args->arg[0] >> 24);
            size_t  op;
            for (op = 0; op < ARRAY_SIZE(scsi_modify_opcodes); op++)
                if (scsi_modify_opcodes[op] == opcode)
                    return (test_cmds[pos].alias);
            return (NULL);
        }
        return ((test_cmds[pos].flags == 1) ? test_cmds[pos].alias : NULL);
    }
    return (NULL);
}

static void
parse_tsize(const char *arg, uint *tsize, int *pos)
{
    *pos = 0;
    if ((sscanf(arg, "%i%n", (int *) tsize, pos) != 1) || (*pos == 0)) {
        printf("Invalid transfer size %s\n", arg);
        exit(RETURN_ERROR);
    }
    switch (arg[*pos]) {
        case '\0':
            break;
        case ',':
            break;
        case 'k':
        case 'K':
            *tsize <<= 10;
            (*pos)++;
            if ((arg[*pos] == 'b') || (arg[*pos] == 'B'))
                (*pos)++;
            break;
        case 'm':
        case 'M':
            *tsize <<= 20;
            (*pos)++;
            if ((arg[*pos] == 'b') || (arg[*pos] == 'B'))
                (*pos)++;
            break;
        default:
            printf("Invalid transfer size %s\n", arg);
            exit(RETURN_ERROR);
    }
}

static void
idle_task_ent(void)
{
    ReadEClock(&idle_stime);
}

static void
idle_task_exit(void)
{
    struct EClockVal idle_etime;
    ReadEClock(&idle_etime);
    idle_time += diff_e_clock(&idle_stime, &idle_etime);
}

/*
 * idle_task_func
 * --------------
 * This task is a spin loop which runs at a low priority.
 * The helper functions above execute every time the scheduler starts
 * and stops the idle task. The helper functions will accumulate the
 * number of EClock ticks which have elapsed during each tun of the
 * idle task. It is up to the caller to determine the percentage of
 * idle time over a given time period.
 *
 * The task is created at a priority above the main task, so it starts
 * immediately. It signals the main task before lowering its own priority:
 * once at priority -10, it may not run again for a long time if another
 * task is busy, and the main task must not be left waiting for it.
 * idle_task_end() similarly raises its priority so that it exits promptly.
 */
static void
idle_task_func(void)
{
    struct Task *task_ptr = (struct Task *) FindTask((char *)NULL);

    Forbid();
    task_ptr->tc_Launch = idle_task_ent;
    task_ptr->tc_Switch = idle_task_exit;
    task_ptr->tc_Flags |= TF_SWITCH | TF_LAUNCH;
    Permit();

    Signal(main_task, 1 << main_signal);  // Let main task resume
    SetTaskPri(task_ptr, -10);

    while (!idle_task_stop) {
        __asm("nop");
    }

    /*
     * Remain in Forbid() until the task has been removed (which ends the
     * Forbid), so that the main task can not see idle_task == NULL and
     * exit (unloading this code) while this task is still executing it.
     */
    Forbid();
    task_ptr->tc_Launch = NULL;
    task_ptr->tc_Switch = NULL;
    task_ptr->tc_Flags &= ~(TF_SWITCH | TF_LAUNCH);
    idle_task = NULL;
}

/*
 * idle_task_start() starts the idle task which measures CPU utilization.
 * Returns nonzero if the task could not be started.
 */
static int
idle_task_start(void)
{
    BYTE pri;
    ULONG sigs;

    main_signal = AllocSignal(-1);
    if (main_signal == -1) {
        printf("Failed to allocate signal\n");
        return (1);
    }
    main_task = (struct Task *) FindTask((char *)NULL);
    pri = main_task->tc_Node.ln_Pri;
    if (pri < 127)
        pri++;
    idle_task_stop = 0;
    idle_task = CreateTask("idle_task", pri, (void *)
                            (uintptr_t) idle_task_func, 4096);
    if (idle_task == NULL) {
        FreeSignal(main_signal);
        printf("Failed to create idle task\n");
        return (1);
    }

    sigs = Wait((1 << main_signal) | SIGBREAKF_CTRL_C);
    if ((sigs & (1 << main_signal)) == 0) {
        /* ^C before the idle task signaled; it may not have run yet */
        Forbid();
        if ((SetSignal(0, 0) & (1 << main_signal)) == 0) {
            /* Not started: it's safe to delete it (it holds nothing) */
            DeleteTask(idle_task);
            idle_task = NULL;
            Permit();
            FreeSignal(main_signal);
            printf("^C abort\n");
            return (1);
        }
        Permit();
        Wait(1 << main_signal);  // Signaled after all; consume it
    }
    FreeSignal(main_signal);
    return (0);
}

/*
 * idle_task_end() stops the idle task and waits for it to exit.
 * The task is raised above the main task's priority, so that it runs
 * (and exits) right away, even if other tasks are keeping the CPU busy.
 */
static void
idle_task_end(void)
{
    BYTE pri;

    if (idle_task == NULL)
        return;
    pri = main_task->tc_Node.ln_Pri;
    if (pri < 127)
        pri++;
    idle_task_stop = 1;
    SetTaskPri(idle_task, pri);
    while (idle_task != NULL)
        Delay(1);  // Normally already exited, as it has higher priority
}

int
main(int argc, char *argv[])
{
    int arg;
    int rc;
    int pos;
    uint bnum;
    uint loop = 0;
    uint loops = 1;
#define MAX_CMD_MASKS 32
    static uint test_cmd_count = 0;
    struct IOExtTD tio;
    uint flag_benchmark = 0;
    uint flag_geometry = 0;
    uint flag_integrity = 0;
    uint flag_openclose = 0;
    uint flag_probe = 0;
    uint flag_showmemlist = 0;
    uint flag_testpackets = 0;
    uint did_open = 0;
    uint tsize = BUFSIZE;
    uint talign = 16;
    uint test_mode = 0;
    char *unit = NULL;
    struct EClockVal dummy;

    memset(test_cmd_mask, 0, sizeof (test_cmd_mask));
#ifndef _DCC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds="
    SysBase = *(struct ExecBase **)4UL;
#pragma GCC diagnostic pop
#endif
    TimerBase = (struct Device *) FindName(&SysBase->DeviceList, TIMERNAME);
    g_e_freq = ReadEClock(&dummy);

    for (arg = 1; arg < argc; arg++) {
        char *ptr = argv[arg];
        if (*ptr == '-') {
            for (++ptr; *ptr != '\0'; ptr++) {
                switch (*ptr) {
                    case 'b':
                        flag_benchmark++;
                        break;
                    case 'B':
                        if (flag_benchmark == 0)
                            flag_benchmark++;
                        if (++arg >= argc) {
                            printf("-%s requires an argument\n", ptr);
                            printf("    Transfer size: 512 1M 64k etc\n"
                                   "    optional alignment: ,1 ,2 ,3 etc\n");
                            exit(RETURN_ERROR);
                        }
                        parse_tsize(argv[arg], (uint *)&user_perf_size, &pos);
                        if ((user_perf_size == 0) || (user_perf_size & 511)) {
                            printf("transfer size must be a non-zero "
                                   "multiple of 512 bytes\n");
                            exit(RETURN_ERROR);
                        }
                        if (argv[arg][pos] == ',') {
                            char *str = argv[arg] + pos + 1;
                            if ((sscanf(str, "%i%n", (int *) &user_num_tio,
                                        &pos) != 1) || (pos == 0) ||
                                (user_num_tio < 1) ||
                                (user_num_tio > MAX_NUM_TIO)) {
                                printf("Invalid TIO count %s\n", str);
                                exit(RETURN_ERROR);
                            }
                        }
                        break;
                    case 'c':
                        if (++arg < argc) {
                            if (test_cmd_count >= ARRAY_SIZE(test_cmd_mask)) {
                                printf("Too many -c commands (maximum %u)\n",
                                       (uint) ARRAY_SIZE(test_cmd_mask));
                                exit(RETURN_ERROR);
                            }
                            test_cmd_mask[test_cmd_count] |=
                                get_cmd(argv[arg],
                                        &test_cmd_args[test_cmd_count]);
                            test_cmd_count++;
                        } else {
                            usage_cmd();
                            exit(RETURN_ERROR);
                        }
                        break;
                    case 'C':
                        flag_cpu_utilization = 0;
                        break;
                    case 'd':
                        flag_destructive++;
                        break;
                    case 'g':
                        flag_geometry++;
                        break;
                    case 'h':
                        usage();
                        exit(RETURN_OK);
                    case 'i':
                        if (flag_integrity++ > 0)
                            break;
                        if (++arg < argc) {
                            parse_tsize(argv[arg], &tsize, &pos);
                            if (argv[arg][pos] == ',') {
                                char *str = argv[arg] + pos + 1;
                                if ((sscanf(str, "%i%n", (int *) &talign,
                                            &pos) != 1) || (pos == 0)) {
                                    printf("Invalid alignment %s\n", str);
                                    exit(RETURN_ERROR);
                                }
                                /*
                                 * An alignment of 0 would place the test
                                 * buffers at address 0 (see test_integrity).
                                 */
                                if ((talign == 0) || (talign > 0x10000) ||
                                    (talign & (talign - 1))) {
                                    printf("Invalid alignment %s; must be a "
                                           "power of 2 from 1 to 65536\n",
                                           str);
                                    exit(RETURN_ERROR);
                                }
                            }
                            if ((tsize == 0) || (tsize & 511)) {
                                printf("transfer size must be a non-zero "
                                       "multiple of 512 bytes\n");
                                exit(RETURN_ERROR);
                            }
                        } else {
                            printf("-%s requires an argument\n", ptr);
                            printf("    Transfer size: 512 1M 64k etc\n"
                                   "    optional alignment: ,1 ,2 ,4 etc\n");
                            exit(RETURN_ERROR);
                        }
                        break;
                    case 'k':
                        /* Integrity test mode required */
                        if (++arg < argc) {
                            int len = strlen(argv[arg]);
                            if (strncmp(argv[arg], "butterfly", len) == 0) {
                                test_mode = 2;
                            } else if (strncmp(argv[arg], "simple", len) == 0) {
                                test_mode = 1;
                            } else {
                                printf("Unknown integrity test mode %s\n",
                                       argv[arg]);
                                exit(RETURN_ERROR);
                            }
                        } else {
                            printf("-%s requires an argument\n", ptr);
                            printf("    One of: simple or butterfly\n");
                            exit(RETURN_ERROR);
                        }
                        break;
                    case 'l':
                        /* Loop count */
                        if (++arg < argc) {
                            int count = atoi(argv[arg]);
                            if (count < 1) {
                                printf("Invalid loop count %s; must be at "
                                       "least 1\n", argv[arg]);
                                exit(RETURN_ERROR);
                            }
                            loops = count;
                        } else {
                            printf("-%s requires an argument\n", ptr);
                            printf("    The number of loop iterations\n");
                            exit(RETURN_ERROR);
                        }
                        break;
                    case 'm':
                        if (memtype != MEMTYPE_ANY) {
                            if (memtype <= MEMTYPE_MAX) {
                                printf("Memory type already specified\n");
                                exit(RETURN_ERROR);
                            }
                            mem_skip_alloc++;
                        } else if (++arg < argc) {
                            if (strcmp(argv[arg], "-") == 0) {
                                flag_showmemlist = 1;
                            } else if (strncasecmp(argv[arg], "chip", 4) == 0) {
                                memtype = MEMTYPE_CHIP;
                            } else if (strncasecmp(argv[arg], "fast", 4) == 0) {
                                memtype = MEMTYPE_FAST;
                            } else if (strcasecmp(argv[arg], "24bit") == 0) {
                                memtype = MEMTYPE_24BIT;
                            } else if (strncasecmp(argv[arg],
                                                   "zorro", 5) == 0) {
                                if ((strcmp(argv[arg] + 5, "2") == 0) ||
                                    (strcasecmp(argv[arg] + 5, "ii") == 0)) {
                                    memtype = MEMTYPE_ZORRO2;
                                } else if ((strcmp(argv[arg] + 5, "3") == 0) ||
                                     (strcasecmp(argv[arg] + 5, "iii") == 0)) {
                                    memtype = MEMTYPE_ZORRO3;
                                } else {
                                    memtype = MEMTYPE_ZORRO;
                                }
                            } else if (strncasecmp(argv[arg], "copr", 3) == 0) {
                                memtype = MEMTYPE_COPROC;
                            } else if (strncasecmp(argv[arg], "acce", 4) == 0) {
                                memtype = MEMTYPE_ACCEL;
                            } else if ((strncasecmp(argv[arg],
                                                    "motherboard", 4) == 0) ||
                                       (strcasecmp(argv[arg], "mb") == 0)) {
                                memtype = MEMTYPE_MB;
                            } else {
                                unsigned int memtype_arg;

                                if (sscanf(argv[arg], "%x", &memtype_arg) != 1) {
                                    printf("invalid argument %s for %s\n",
                                           argv[arg], ptr);
                                    exit(RETURN_ERROR);
                                }
                                /*
                                 * Values up to MEMTYPE_MAX are memory type
                                 * codes, not addresses (and are not usable
                                 * addresses anyway).
                                 */
                                if (memtype_arg <= MEMTYPE_MAX) {
                                    printf("invalid memory address %s for "
                                           "%s\n", argv[arg], ptr);
                                    exit(RETURN_ERROR);
                                }
                                memtype = (uint32_t) memtype_arg;
                            }
                        } else {
                            printf("-%s requires an argument\n", ptr);
                            printf("    One of: chip, fast, 24bit, mb, "
                                   "zorro[2|3], accel, coproc, or <addr>\n");
                            exit(RETURN_ERROR);
                        }
                        break;
                    case 'o':
                        flag_openclose++;
                        break;
                    case 'p':
                        flag_probe++;
                        break;
                    case 't':
                        flag_testpackets++;
                        break;
                    case 'v':
                        g_verbose++;
                        break;
                    case 'y':
                        force_yes++;
                        break;
                    default:
                        printf("Unknown argument %s\n", ptr);
                        usage();
                        exit(RETURN_ERROR);
                }
            }
        } else if (g_devname == NULL) {
            g_devname = ptr;
        } else if (unit == NULL) {
            unit = ptr;
        } else {
            printf("Error: unknown argument %s\n", ptr);
            usage();
            exit(RETURN_ERROR);
        }
    }
    if (flag_showmemlist) {
        show_memlist();
        exit(RETURN_OK);
    }
    if (test_mode && !flag_integrity) {
        printf("You must specify -i <tsize> with -k\n");
        usage();
        exit(RETURN_ERROR);
    }
    if ((flag_benchmark || flag_geometry || flag_integrity || flag_openclose ||
         flag_testpackets || flag_probe || test_cmd_mask[0]) == 0) {
        printf("You must specify an operation to perform\n");
        usage();
        exit(RETURN_ERROR);
    }
    if (flag_destructive == 0) {
        uint cur;
        for (cur = 0; cur < test_cmd_count; cur++) {
            const char *name = destructive_cmd_name(test_cmd_mask[cur],
                                                    &test_cmd_args[cur]);
            if (name != NULL) {
                printf("-c %s can modify data on the device; "
                       "it requires -d\n", name);
                exit(RETURN_ERROR);
            }
        }
    }
    if (flag_destructive && (force_yes == 0) &&
        (are_you_sure("Destructive test") == FALSE)) {
        exit(RETURN_ERROR);
    }
    if ((g_devname != NULL) && (strchr(g_devname, ':') != 0)) {
        struct FileSysStartupMsg *startup = find_startup(g_devname);
        static char unitbuf[10];
        if (startup == NULL)
            exit(RETURN_ERROR);  // find_startup() reported why
        g_devname     = ((char *) BTOC(startup->fssm_Device)) + 1;
        g_unitno      = startup->fssm_Unit;
        g_envec       = (struct DosEnvec *) BTOC(startup->fssm_Environ);
        if (g_envec != NULL) {
            g_sector_size = g_envec->de_SizeBlock * 4;
            g_devstart = (uint64_t) g_envec->de_LowCyl * g_envec->de_Surfaces *
                         g_envec->de_BlocksPerTrack * g_sector_size;
            g_devend = (uint64_t) (g_envec->de_HighCyl + 1) *
                       g_envec->de_Surfaces * g_envec->de_BlocksPerTrack *
                       g_sector_size;
        }
        if (g_verbose)
            printf("Device %s %u\n", g_devname, g_unitno);
        sprintf(unitbuf, "%u", g_unitno);
        unit = unitbuf;
        goto got_unit;
    }
    if (unit == NULL) {
        if ((g_devname == NULL) || flag_benchmark || flag_geometry ||
            flag_integrity || flag_openclose || flag_testpackets ||
            test_cmd_mask[0]) {
            printf("You must specify a device name and unit number to open\n");
            usage();
            exit(RETURN_ERROR);
        }
    } else if (sscanf(unit, "%u", &g_unitno) != 1) {
        printf("Invalid device unit \"%s\"\n", unit);
        usage();
        exit(RETURN_ERROR);
    }

got_unit:
    memset(g_tbuf, 0, sizeof (g_tbuf));
    /*
     * The general test buffers are only needed by packet tests (-t, -c).
     * Not allocating them otherwise leaves a user-specified address
     * (-m <addr>) for the benchmark or integrity test buffers.
     */
    for (bnum = 0; (flag_testpackets || (test_cmd_count > 0)) &&
                   (bnum < ARRAY_SIZE(g_tbuf)); bnum++) {
        g_tbuf[bnum] = (uint8_t *) AllocMemType(BUFSIZE, memtype);
        if (g_tbuf[bnum] == NULL) {
            report_allocmem_fail(BUFSIZE, memtype);
            goto allocmem_fail;
        }
    }

    if (flag_cpu_utilization && idle_task_start())
        goto idle_fail;

    for (loop = 0; loop < loops; loop++) {
        uint stop_on_error = (loop != 0) || (loops == 1);
        if (loops > 1) {
            printf("Pass %u  ", loop + 1);
            print_time();
            if (flag_benchmark)
                printf("  ");
            else
                printf("\n");
        }
        if (flag_benchmark &&
            drive_benchmark(flag_destructive, memtype))
            break;

        if (flag_openclose) {
            if ((rc = open_device(&tio)) != 0) {
                printf("Open %s unit %d: ", g_devname, g_unitno);
                print_fail_nl(rc);
                if (stop_on_error)
                    break;
            } else {
                did_open = 1;
            }
        }
        if (flag_probe && scsi_probe(unit) && stop_on_error)
            break;
        if (flag_geometry && drive_geometry() && stop_on_error)
            break;
        if (flag_integrity &&
            test_integrity(test_mode, flag_integrity, memtype, tsize, talign)) {
            break;
        }
        if (flag_testpackets &&
            test_packets(flag_destructive, flag_testpackets, 0, NULL) &&
            stop_on_error) {
            break;
        }
        if ((test_cmd_count > 0) &&
            test_packets(0, 0, test_cmd_count, test_cmd_mask) &&
            stop_on_error) {
            break;
        }
        if ((flag_benchmark > 1) &&
            drive_latency(flag_destructive) &&
            stop_on_error) {
            break;
        }
        if (did_open) {
            did_open = 0;
            close_device(&tio);
        }
        if (is_user_abort())
            break;
    }
    if (did_open)
        close_device(&tio);

idle_fail:
allocmem_fail:
    for (bnum = 0; bnum < ARRAY_SIZE(g_tbuf); bnum++)
        if (g_tbuf[bnum] != NULL)
            FreeMemType(g_tbuf[bnum], BUFSIZE);

    for (bnum = 0; bnum < ARRAY_SIZE(g_ibuf); bnum++)
        if (g_ibuf[bnum] != NULL)
            FreeMemType(g_ibuf[bnum], tsize + talign);

    if (loops > 1) {
        if (loop < loops)
            printf("Stopped at pass %u of %u\n", loop + 1, loops);
        else
            printf("%u passes completed successfully\n", loops);
    }

    idle_task_end();

    if (loop < loops)
        exit(RETURN_ERROR);

    exit(RETURN_OK);
}
