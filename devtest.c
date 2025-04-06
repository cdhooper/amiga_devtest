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
struct ExecBase *DOSBase;
struct Device   *TimerBase;
#endif

/*
 * ULONG has changed from NDK 3.9 to NDK 3.2.
 * However, PRI*32 did not. What is the right way to implement this?
 */
#if INCLUDE_VERSION < 47
#undef PRIu32
#define PRIu32 "lu"
#undef PRId32
#define PRId32 "ld"
#undef PRIx32
#define PRIx32 "lx"
#endif

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

#define BUFSIZE    8192
#define RAWBUFSIZE 16384

#define MEMTYPE_ANY    0
#define MEMTYPE_CHIP   1
#define MEMTYPE_FAST   2
#define MEMTYPE_24BIT  3
#define MEMTYPE_ZORRO  4
#define MEMTYPE_ACCEL  5
#define MEMTYPE_COPROC 6
#define MEMTYPE_MB     7
#define MEMTYPE_MAX    7

// memory areas
#define MEMTYPE_CHIP_START      0x00001000u  // chip memory start
#define MEMTYPE_SIZE_SIZE       0x001FF000u  // max. ~2MB chip memory 
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
#define SCSI_WRITE_10_COMMAND           0x2a
#define SCSI_WRITE_12_COMMAND           0xaa
#define SCSI_WRITE_16_COMMAND           0x8a
typedef struct scsi_rw_6 {
    uint8_t opcode;
    uint8_t addr[3];
    uint8_t length;
    uint8_t control;
} __packed scsi_rw_6_t;

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
    char               name[24];
    const char * const desc;
    const char * const arg_help;
} test_cmds_t;

typedef struct {
    uint8_t  arg_count;
    uint32_t arg[4];
} args_t;

static int do_read_cmd(struct IOExtTD *tio, uint64_t offset, uint len,
                       void *buf, int nsd);

#define TEST_MAX_CMDS 32

BOOL __check_abort_enabled = 0;       // Disable gcc clib2 ^C break handling

static int       g_verbose = 0;       // Verbose output
static int       g_changenum = 0;     // Current device media change number
static uint      g_sector_size = 512; // Updated when getting drive geometry
static uint64_t  g_devsize = 0;       // Device total size in bytes
static uint      g_lun = 0;           // Target LUN to access
static uint      g_has_nsd = 0;       // Device driver supports NSD
static uint8_t **g_buf;               // Saved data buffers for certain tests
static char     *g_devname = NULL;    // Device name
static uint      g_unitno;            // Device unit and LUN
static uint      g_e_freq;            // Frequency of eclock (715909 tps)
static UWORD     g_sense_length;      // Length of returned sense data (if any)
static UBYTE     g_sense_data[255];   // Sense data buffer
static uint8_t  *g_ibuf[5];           // Integrity test buffers
static uint8_t  *g_align[5];          // Integrity test buffers (aligned)
static UBYTE     mem_skip_alloc = 0;  // Skip memory allocate
static uint32_t  memtype = MEMTYPE_ANY; // Memory type
static uint64_t  test_cmd_mask[32];
static args_t    test_cmd_args[TEST_MAX_CMDS];
static args_t   *cur_test_args = NULL;
static uint      flag_destructive = 0;

static BOOL
is_user_abort(void)
{
    if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
        return (1);
    return (0);
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
    CloseDevice((struct IORequest *)tio);
}

static void
usage(void)
{
    printf("%s\n\n"
           "usage: devtest <options> <x.device> <unit>\n"
           "   -b                    benchmark device performance "
                    "[-bb tests latency]\n"
           "   -c <cmd>              test a specific device driver request\n"
           "   -d                    also do destructive operations (write)\n"
// Undocumented: -dd skips save/restore of data with -i integrity test
           "   -g                    test drive geometry\n"
           "   -h                    display help\n"
           "   -i <tsize>[,<align>]  data integrity test (destructive) "
                          "[-i=rand -ii=addr -iii=patt]\n"
           "   -l <loops>            run multiple times\n"
           "   -m <addr>             "
                    "use specific memory (Chip Fast Zorro MB Accel -=list)\n"
           "   -mm <addr>            "
                    "use specific address without allocation by OS\n"
           "   -o                    test open/close\n"
           "   -p                    probe SCSI bus for devices\n"
           "   -t                    test all packet types (basic, TD64, NSD) "
                    "[-tt = more]\n",
           version + 7);
}

typedef struct {
    int errcode;
    const char *const errstr;
} err_to_str_t;

static const err_to_str_t err_to_str[] = {
    { IOERR_OPENFAIL,       "IOERR_OPENFAIL" },             // -1
    { IOERR_ABORTED,        "IOERR_ABORTED" },              // -2
    { IOERR_NOCMD,          "IOERR_NOCMD (unsupported)" },  // -3
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
    { 52,                   "ERROR_SENSE_CODE" },           // 52
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
    for (i = 0; i < ARRAY_SIZE(err_to_str); i++) {
        if (err_to_str[i].errcode == rc) {
            printf(" %s", err_to_str[i].errstr);
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
                high, (unsigned int) (value - high * 1000000));
    } else {
        sprintf(buf, "%u", (unsigned int) value);
    }
    return (buf);
}

APTR
AllocMemType(ULONG byteSize, uint32_t memtype)
{
    APTR addr = NULL;

    /* Don't bother with memory allocate */
    if (mem_skip_alloc)
        return ((APTR) memtype);

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
                addr = AllocAbs(byteSize, (APTR) memtype);
            else
                addr = NULL;
            break;
    }
    if (g_verbose)
        printf("Alloc %p\n", addr);
    return (addr);
}

void
FreeMemType(APTR addr, ULONG byteSize)
{
    if (mem_skip_alloc)
        return;  /* Memory was not allocated through the OS */
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
    struct SCSICmd scmd;

    if (reslen > 0) {
        res = AllocMemType(reslen, memtype);
        if (res == NULL) {
            printf("  AllocMem %x (%x) fail\n", reslen, memtype);
            g_sense_length = 0;
            return (ENOMEM);
        }
        memset(res, 0, reslen);
    } else {
        res = NULL;
    }
    setup_scsidirect_cmd(&scmd, cmd, cmdlen, res, reslen);
    tio->iotd_Req.io_Command = HD_SCSICMD;
    tio->iotd_Req.io_Length  = sizeof (scmd);
    tio->iotd_Req.io_Data    = &scmd;

    rc = DoIO((struct IORequest *) tio);
    if (rc != 0) {
        if (reslen != 0) {
            FreeMemType(res, reslen);
            res = NULL;
        }
    }
    g_sense_length = scmd.scsi_SenseActual;
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
do_scsi_read_capacity_10(struct IOExtTD *tio, uint lun,
                         scsi_read_capacity_10_data_t **cap)
{
    scsi_generic_t cmd;
    uint len = sizeof (scsi_read_capacity_10_data_t);

    memset(&cmd, 0, sizeof (cmd));
    cmd.opcode = READ_CAPACITY_10;
    cmd.bytes[0] = lun << 5;

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
    uint64_t  min_offset = 0;
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
        printf("  AllocMem %x (%x) fail\n", g_sector_size, memtype);
        return (1);
    }

    *sectors = 0;

    while (incdec >= g_sector_size / 2) {
        rc = do_read_cmd(tio, offset, g_sector_size, buf, g_has_nsd);
        if (rc == 0) {
            min_offset = offset;
            if (double_mode) {
                offset *= 2;
                incdec = offset / 2;
            } else {
                if (offset + incdec >= max_offset)
                    incdec /= 2;
                offset += incdec;
            }
        } else {
            double_mode = 0;
            max_offset = offset;
            if (offset - incdec <= min_offset)
                incdec /= 2;
            offset -= incdec;
        }
    }
    offset &= ~(g_sector_size - 1);

    FreeMemType(buf, g_sector_size);
    *sectors = min_offset / g_sector_size;
    return (0);
}

#if 0
static uint32_t
_2btol(const uint8_t *bytes)
{
    return ((bytes[0] << 8) |
            bytes[1]);
}
#endif

static uint32_t
_3btol(const uint8_t *bytes)
{
    return ((bytes[0] << 16) |
            (bytes[1] << 8) |
            bytes[2]);
}

#if 0
static uint32_t
_4btol(const uint8_t *bytes)
{
    return (((uint32_t)bytes[0] << 24) |
            ((uint32_t)bytes[1] << 16) |
            ((uint32_t)bytes[2] << 8) |
            (uint32_t)bytes[3]);
}
#endif

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
scsi_probe_unit(uint unit, struct IOExtTD *tio)
{
    int rc;
    int erc;
    scsi_inquiry_data_t *inq_res;

    rc = OpenDevice(g_devname, unit, (struct IORequest *) tio, 0);
    if (rc == 0) {
        printf("%3d", unit);
        erc = do_scsi_inquiry(tio, unit, &inq_res);
        if (erc == ENOMEM)
            return (erc);
        if (erc == 0) {
            printf(" %-*.*s %-*.*s %-*.*s %-7s",
               sizeof (inq_res->vendor),
               sizeof (inq_res->vendor),
               trim_spaces(inq_res->vendor, sizeof (inq_res->vendor)),
               sizeof (inq_res->product),
               sizeof (inq_res->product),
               trim_spaces(inq_res->product, sizeof (inq_res->product)),
               sizeof (inq_res->revision),
               sizeof (inq_res->revision),
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
        erc = do_scsi_read_capacity_10(tio, unit, &cap10);
        if ((erc == 0) && (cap10 != NULL)) {
            uint ssize = *(uint32_t *) &cap10->length;
            uint cap   = (*(uint32_t *) &cap10->addr + 1) / 1000;
            uint cap_c = 0;  // KMGTPEZY
            if (cap > 100000) {
                cap /= 1000;
                cap_c++;
            }
            cap *= ssize;
            while (cap > 9999) {
                cap /= 1000;
                cap_c++;
            }
            printf("%5u %5u %cB", ssize, cap, "KMGTPEZY"[cap_c]);
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

static int
drive_geometry(void)
{
    int    rc;
    struct IOExtTD *tio;
    struct DriveGeometry dg;
    struct MsgPort *mp;
    uint8_t *pages;
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
    printf("                 SSize TotalSectors   Cyl  Head  Sect  DType "
           "Removable\n");
    printf("TD_GETGEOMETRY ");
    if ((rc = DoIO((struct IORequest *) tio)) != 0) {
        printf("%7c %12c %5c %5c %5c  ",
               '-', '-', '-', '-', '-');
        print_fail_nl(rc);
    } else {
        printf("%7"PRIu32" %12"PRIu32" %5"PRIu32" %5"PRIu32" "
               "%5"PRIu32"  0x%02x  %s\n",
               dg.dg_SectorSize, dg.dg_TotalSectors, dg.dg_Cylinders,
               dg.dg_Heads, dg.dg_TrackSectors, dg.dg_DeviceType,
               (dg.dg_Flags & DGF_REMOVABLE) ? "Yes" : "No");
        g_devsize = (uint64_t) dg.dg_TotalSectors * dg.dg_SectorSize;
        g_sector_size = dg.dg_SectorSize;
    }

    printf("Inquiry ");
    scsi_inquiry_data_t *inq_res;
    rc = do_scsi_inquiry(tio, g_unitno, &inq_res);
    if (rc != 0) {
        printf("%51c  -    Fail\n", '-');
    } else {
        printf("%46s 0x%02x  %s", "",
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
    rc = do_scsi_read_capacity_10(tio, g_unitno, &cap10);
    if (cap10 == NULL) {
        printf("%5c %12c %19s", '-', '-', "");
        print_fail_nl(rc);
    } else {
        uint last_sector = *(uint32_t *) &cap10->addr;
        printf("%5u %12u\n", *(uint32_t *) &cap10->length, last_sector + 1);
        FreeMemType(cap10, sizeof (*cap10));
        if (g_devsize == 0) {
            g_devsize = (uint64_t) (*(uint32_t *) cap10->length) *
                        (last_sector + 1);
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
        printf("%5c %12s\n", ' ', llu_to_str(last_sector + 1));
        FreeMemType(cap16, sizeof (*cap16));
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
        printf("Mode Pages%40s", "");
        print_fail_nl(rc);
    } else {
        uint pos = 4;
        uint len = pages[0];
        for (pos = 4; pos < len; pos += pages[pos + 1] + 2) {
            uint page = pages[pos] & DISK_PGCODE;
            switch (page) {
                case 0x03: { // Disk Format
                    uint nsec  = *(uint16_t *)(&pages[pos + 10]);
                    uint ssize = *(uint16_t *)(&pages[pos + 12]);
                    printf("Mode Page 0x%02x", page);
                    printf("%8u %30u\n", ssize, nsec);
                    break;
                }
                case 0x04: { // Rigid Geometry
                    uint ncyl  = _3btol(&pages[pos + 2]);
                    uint nhead = pages[pos + 5];
                    printf("Mode Page 0x%02x", page);
                    printf("%27u %5u\n", ncyl, nhead);
                    break;
                }
                case 0x05: { // Flexible Geometry
                    uint nhead = pages[pos + 4];
                    uint nsec  = pages[pos + 5];
                    uint ssize = *(uint16_t *)(&pages[pos + 6]);
                    uint ncyl  = *(uint16_t *)(&pages[pos + 8]);
                    printf("Mode Page 0x%02x", page);
                    printf("%8u %18u %5u %5u\n", ssize, ncyl, nhead, nsec);
                    break;
                }
                case 0x06: { // Reduced Block Commands Parameters
                    uint     ssize = *(uint16_t *)(&pages[pos + 3]);
                    uint64_t blks  = _5btol(&pages[pos + 5]);
                    printf("Mode Page 0x%02x", page);
                    printf("%8u %12s\n", ssize, llu_to_str(blks));
                    break;
                }
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
                    printf(" len=%u\n", pages[pos + 1]);
                    break;
            }
            if (g_verbose && (pages[pos + 1] > 0)) {
                int cur;
                int len = pages[pos + 1];
                printf("   ");
                for (cur = 0; cur < len; cur++)
                    printf(" %02x", pages[pos + 2 + cur]);
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
    tm = localtime(&timet);
    printf("%04d-%02d-%02d %02d:%02d:%02d",
           tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
           tm->tm_hour, tm->tm_min, tm->tm_sec);
}

static void
print_perf_type(int is_write, uint xfer_size)
{
    if (g_verbose == 0) {
        printf("%s %3u KB xfers ", is_write ? "write" : "read ",
               xfer_size / 1024);
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
    tusec = (uint64_t) ttime * 1000000 / g_e_freq / iters;
    tmsec = tusec / 1000;
    tusec %= 1000;

    printf("%u.%03u ms%c", tmsec, (tusec + 5) / 10, endch);
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
    unsigned int ttime;
    struct DriveGeometry dg;

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
    for (iter = 0; iter < iters; iter++) {
        failcode = WaitIO((struct IORequest *) tio[iter]);
        if (failcode != 0) {
            if (++rc < 5) {
                printf(" ");
                print_fail(failcode);
                printf(" ");
            } else if (rc == 6) {
                printf(" ... ");
            }
        }
    }
    ReadEClock(&etime);
    ttime = diff_e_clock(&stime, &etime);
    print_latency(ttime, iter, '\n');

finish_fail:
    for (iter = 0; iter < num_iter; iter++)
        close_device(tio[iter]);

    g_devsize = (uint64_t) dg.dg_TotalSectors * dg.dg_SectorSize;
    g_sector_size = dg.dg_SectorSize;

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
    unsigned int step;
    struct EClockVal stime;
    struct EClockVal etime;
    uint64_t pos = 0;

    if (g_sector_size == 0)
        g_sector_size = 512;

    if (g_devsize == 0)
        g_devsize = 720 << 10;  // At least 720K

    if ((g_devsize >> 32) != 0) {
        if (iocmd == CMD_READ) {
            iocmd = g_has_nsd ? NSCMD_TD_READ64 : TD_READ64;
        } else if (iocmd == CMD_WRITE) {
            iocmd = g_has_nsd ? NSCMD_TD_WRITE64 : TD_WRITE64;
        } else {
            printf("Unknown iocmd %d\n", iocmd);
            return (1);
        }
    }
    step = g_devsize / num_iter;
    if (mode == BUTTERFLY_MODE_FAR)
        step /= 4;

    if (step < g_sector_size) {
        step = g_sector_size;
        num_iter /= 4;
    }

    for (iter = 0; iter < num_iter; iter++) {
        tio[0]->iotd_Req.io_Command = iocmd;
        tio[0]->iotd_Req.io_Actual  = pos >> 32;
        tio[0]->iotd_Req.io_Offset  = (uint32_t) pos;
        tio[0]->iotd_Req.io_Length  = g_sector_size;
        tio[0]->iotd_Req.io_Data    = buf;
        tio[0]->iotd_Req.io_Flags   = 0;
        tio[0]->iotd_Req.io_Error   = 0xa5;
        switch (mode) {
            case BUTTERFLY_MODE_AVG:
            case BUTTERFLY_MODE_FAR:
                if ((iter & 1) == 0)
                    pos = step * iter / 2;              // left --> right
                else
                    pos = g_devsize - step * iter / 2;  // left <-- right
                break;
            case BUTTERFLY_MODE_CONST:
                if ((iter & 1) == 0)
                    pos += g_devsize / 2;               // left --> right
                else
                    pos -= (g_devsize / 2 - step);      // left <-- right
                break;
        }
    }

    ReadEClock(&stime);
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

    if (iocmd & CMD_FLAG_NOT_QUICK) {
        iocmd &= ~CMD_FLAG_NOT_QUICK;
        flags = 0;
    }

    for (iter = 0; iter < num_iter; iter++) {
        tio[0]->iotd_Req.io_Command = iocmd;
        tio[0]->iotd_Req.io_Actual  = 0;
        tio[0]->iotd_Req.io_Offset  = 0;
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
    int failcode;
    struct EClockVal stime;
    struct EClockVal etime;

    for (iter = 0; iter < num_iter; iter++) {
        tio[iter]->iotd_Req.io_Command = iocmd;
        tio[iter]->iotd_Req.io_Actual  = 0;
        tio[iter]->iotd_Req.io_Offset  = 0;
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
        failcode = WaitIO((struct IORequest *) tio[iter]);
        if (failcode != 0) {
            if (++rc < 10) {
                printf("  ");
                print_fail(failcode);
            }
        }
    }
    ReadEClock(&etime);
    print_latency(diff_e_clock(&stime, &etime), iter, '\n');
    return (rc);
}

static int
latency_scsidirect_cmd_seq(uint8_t iocmd, uint8_t *buf, int num_iter,
                           struct IOExtTD **tio)
{
    int iter;
    int rc = 0;
    int failcode;
    scsi_rw_6_t cmd;
    struct SCSICmd *scmd;
    struct EClockVal stime;
    struct EClockVal etime;

    scmd = AllocMemType(sizeof (*scmd) * num_iter, memtype);
    if (scmd == NULL) {
        printf("  AllocMem %x (%x) fail\n", sizeof (*scmd) * num_iter, memtype);
        return (1);
    }

    memset(&cmd, 0, sizeof (cmd));
    cmd.opcode = iocmd;
    cmd.addr[0] = 0; // lun << 5;
    cmd.length = BUFSIZE / g_sector_size;

    for (iter = 0; iter < num_iter; iter++) {
        setup_scsidirect_cmd(scmd + iter, (scsi_generic_t *) &cmd, sizeof (cmd),
                             buf, BUFSIZE);
        tio[iter]->iotd_Req.io_Command = HD_SCSICMD;
        tio[iter]->iotd_Req.io_Length  = sizeof (*scmd);
        tio[iter]->iotd_Req.io_Data    = scmd + iter;
    }

    ReadEClock(&stime);
    for (iter = 0; iter < num_iter; iter++) {
        failcode = DoIO((struct IORequest *) tio[iter]);
        if (failcode != 0) {
            rc += failcode;
            if (++rc < 10) {
                printf("  ");
                print_fail(failcode);
            }
            break;
        }
    }
    ReadEClock(&etime);
    print_latency(diff_e_clock(&stime, &etime), iter, '\n');

    FreeMemType(scmd, sizeof (*scmd) * num_iter);

    return (rc);
}

static int
latency_scsidirect_cmd_par(uint8_t iocmd, uint8_t *buf, int num_iter,
                           struct IOExtTD **tio)
{
    int iter;
    int rc = 0;
    struct EClockVal stime;
    struct EClockVal etime;
    scsi_rw_6_t cmd;
    struct SCSICmd *scmd;

    scmd = AllocMemType(sizeof (*scmd) * num_iter, memtype);
    if (scmd == NULL) {
        printf("  AllocMem %x (%x) fail\n", sizeof (*scmd) * num_iter, memtype);
        return (1);
    }

    memset(&cmd, 0, sizeof (cmd));
    cmd.opcode = iocmd;
    cmd.addr[0] = 0; // lun << 5;
    cmd.length = BUFSIZE / g_sector_size;

    for (iter = 0; iter < num_iter; iter++) {
        setup_scsidirect_cmd(scmd + iter, (scsi_generic_t *) &cmd, sizeof (cmd),
                             buf, BUFSIZE);
        tio[iter]->iotd_Req.io_Command = HD_SCSICMD;
        tio[iter]->iotd_Req.io_Length  = sizeof (*scmd);
        tio[iter]->iotd_Req.io_Data    = scmd + iter;
    }

    ReadEClock(&stime);
    for (iter = 0; iter < num_iter; iter++) {
        SendIO((struct IORequest *) tio[iter]);
    }
    for (iter = 0; iter < num_iter; iter++) {
        int failcode = WaitIO((struct IORequest *) tio[iter]);
        if (failcode != 0) {
            if (++rc < 10) {
                printf("  ");
                print_fail(failcode);
            }
        }
    }
    ReadEClock(&etime);
    print_latency(diff_e_clock(&stime, &etime), iter, '\n');

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
        printf("  AllocMem %x (%x) fail\n", BUFSIZE, memtype);
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
        printf("  AllocMem %x (%x) fail\n", BUFSIZE, memtype);
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
    if (num_iter == 0)
        return (1);

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
            if (ttime > TICKS_PER_SECOND * 2) {
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
        printf("  AllocMem %x (%x) fail\n", sizeof (*mtio) * NUM_MTIO, memtype);
        rc = 1;
        goto need_delete_tio;
    }
    memset(mtio, 0, sizeof (*mtio));

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
            if ((ttime > TICKS_PER_SECOND * 2) ||
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
#define NUM_TIO 4

static void
print_perf(uint ttime, uint freq, uint xfer_kb, int is_write, uint xfer_size)
{
    uint tsec;
    uint trem;
    uint rep = xfer_kb;
    char c1 = 'K';
    char c2 = 'K';

    if (rep >= 10000) {
        rep /= 1000;
        c1 = 'M';
    }
    if (ttime == 0)
        ttime = 1;
    tsec = ttime / freq;
    trem = ttime % freq;

    if ((xfer_kb * (freq / 1000) / ttime) >= 100) {
        /* Transfer rate > about 100 MB/sec */
        xfer_kb /= 1000;
        c2 = 'M';
    }
    xfer_kb = (uint64_t) xfer_kb * (uint64_t) freq / (uint64_t) ttime;

    if (g_verbose) {
        printf("%4u %cB %s in %2u.%02u sec: %3u KB xfer: %3u %cB/sec\n",
               rep, c1, is_write ? "write" : "read ",
               tsec, trem * 100 / freq, xfer_size / 1024,
               xfer_kb, c2);
    } else {
        printf("%13u %cB/sec\n", xfer_kb, c2);
    }
}

static int
run_bandwidth(UWORD iocmd, struct IOExtTD *tio[NUM_TIO], uint8_t *buf[NUM_TIO],
              uint32_t bufsize)
{
    int xfer;
    int xfer_good;
    int i;
    int rc = 0;
    uint8_t issued[NUM_TIO];
    int cur = 0;
    uint32_t pos;
    struct EClockVal stime;
    struct EClockVal etime;
    uint32_t freq;
    uint32_t diff_ticks;

    int rep;

    for (rep = 0; rep < 10; rep++) {
        pos = 0;
        memset(issued, 0, sizeof (issued));

        ReadEClock(&stime);

        print_perf_type((iocmd == CMD_READ) ? 0 : 1, bufsize);
        xfer_good = 0;
        for (xfer = 0; xfer < 50; xfer++) {
            if (issued[cur]) {
                int failcode = WaitIO((struct IORequest *) tio[cur]);
                if (failcode == 0)
                    failcode = tio[cur]->iotd_Req.io_Error;
                issued[cur] = 0;
                if (failcode == 0) {
                    xfer_good++;
                } else {
                    printf("  %s ", (iocmd == CMD_READ) ? "Read" : "Write");
                    print_fail(failcode);
                    printf(" at 0x%x\n", tio[cur]->iotd_Req.io_Offset);
                    rc++;
                    break;
                }
                if ((xfer & 0x7) == 0) {
                    /* Cut out early if device is slow (> 1 second) */
                    ReadEClock(&etime);
                    if (diff_e_clock(&stime, &etime) > g_e_freq) {
                        break;
                    }
                }
            }

            tio[cur]->iotd_Req.io_Command = iocmd;
            tio[cur]->iotd_Req.io_Actual = 0;
            tio[cur]->iotd_Req.io_Data = buf[cur];
            tio[cur]->iotd_Req.io_Length = bufsize;
            tio[cur]->iotd_Req.io_Offset = pos;
            SendIO((struct IORequest *) tio[cur]);
            issued[cur] = 1;
            pos += bufsize;
            if (++cur >= NUM_TIO)
                cur = 0;
        }
        for (i = 0; i < NUM_TIO; i++) {
            if (issued[cur]) {
                int failcode = WaitIO((struct IORequest *) tio[cur]);
                if (failcode == 0)
                    failcode = tio[cur]->iotd_Req.io_Error;
                issued[cur] = 0;
                if (failcode == 0) {
                    xfer_good++;
                } else {
                    printf("  %s ", (iocmd == CMD_READ) ? "Read" : "Write");
                    print_fail(failcode);
                    printf(" at 0x%x\n", tio[cur]->iotd_Req.io_Offset);
                    rc++;
                }
            }
            if (++cur >= NUM_TIO)
                cur = 0;
        }

        freq = ReadEClock(&etime);
        diff_ticks = diff_e_clock(&stime, &etime);

        print_perf(diff_ticks, freq, bufsize / 1000 * xfer_good,
                   (iocmd == CMD_READ) ? 0 : 1, bufsize);
        bufsize >>= 2;
        if (bufsize < 16384)
            break;

        if (is_user_abort()) {
            printf("^C abort\n");
            rc++;
            break;
        }
    }

    return (rc);
}

static const char *
memtype_str(uint32_t mem)
{
    const char *type;
    if (((mem > MEMTYPE_CHIP_START) && (mem < MEMTYPE_CHIP_START + MEMTYPE_SIZE_SIZE)) || (mem == MEMTYPE_CHIP)) {
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
    } else if ((mem >= E_MEMORYBASE) && (mem < E_MEMORYBASE + E_MEMORYSIZE)) {
        type = "Zorro II";
    } else if ((mem >= MEMTYPE_ZORRO3_START) && (mem < MEMTYPE_ZORRO3_START + MEMTYPE_ZORRO3_SIZE)) {
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
        const char *type = memtype_str((uint32_t) mem);

        printf("%s RAM at %p size=0x%x\n", type, (void *) mem, size);

        for (chunk = mem->mh_First; chunk != NULL;
             chunk = chunk->mc_Next) {
            uint bytes = chunk->mc_Bytes;
            if (g_verbose || (chunk->mc_Bytes >= 512)) {
                printf("  %p 0x%x", (void *) chunk, bytes);
                if ((uintptr_t) chunk + bytes > upper)
                    printf(" ** CORRUPT: 0x%x is maximum size",
                           upper - (uintptr_t) chunk);
                printf("\n");
            }
            if ((uintptr_t) chunk + bytes >= upper)
                break;  // Corrupt memory list?
        }
    }
    Permit();
}

static int
drive_benchmark(int do_destructive, uint32_t memtype)
{
    struct IOExtTD *tio[NUM_TIO];
    uint8_t *buf[NUM_TIO];
    uint8_t opened[NUM_TIO];
    uint32_t perf_buf_size = PERF_BUF_SIZE;
    struct MsgPort *mp;
    size_t i;
    int rc = 0;

    mp = CreatePort(0, 0);
    if (mp == NULL) {
        printf("Failed to create message port\n");
        return (1);
    }

    memset(tio, 0, sizeof (tio));
    for (i = 0; i < ARRAY_SIZE(tio); i++) {
        tio[i] = (struct IOExtTD *) CreateExtIO(mp, sizeof (struct IOExtTD));
        if (tio[i] == NULL) {
            printf("Failed to create tio struct\n");
            rc = 1;
            goto create_tio_fail;
        }
    }

    memset(opened, 0, sizeof (opened));
    for (i = 0; i < ARRAY_SIZE(tio); i++) {
        if ((rc = open_device(tio[i])) != 0) {
            printf("Open %s Unit %u: ", g_devname, g_unitno);
            print_fail_nl(rc);
            rc = 1;
            goto opendevice_fail;
        }
        opened[i] = 1;
    }

    memset(buf, 0, sizeof (buf));
try_again:
    for (i = 0; i < ARRAY_SIZE(buf); i++) {
        uint32_t amemtype = memtype;
        if (amemtype > 0x10000)
            amemtype += perf_buf_size * i;
        buf[i] = (uint8_t *) AllocMemType(perf_buf_size, amemtype);
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
                printf(" at 0x%08x", memtype);
            printf("\n");
            rc = 1;
            goto allocmem_fail;
        }
    }
    printf("Test %s %u with %s RAM",
           g_devname, g_unitno, memtype_str((uint32_t) buf[0]));
    if (g_verbose) {
        for (i = 0; i < ARRAY_SIZE(buf); i++)
            printf(" %08x", (uint32_t) buf[i]);
    }
    printf("\n");

    rc += run_bandwidth(CMD_READ, tio, buf, perf_buf_size);

    if (do_destructive && (rc == 0))
        rc += run_bandwidth(CMD_WRITE, tio, buf, perf_buf_size);

allocmem_fail:
    for (i = 0; i < ARRAY_SIZE(buf); i++)
        if (buf[i] != NULL)
            FreeMemType(buf[i], perf_buf_size);

opendevice_fail:
    for (i = 0; i < ARRAY_SIZE(tio); i++)
        if (opened[i] != 0)
            close_device(tio[i]);

create_tio_fail:
    for (i = 0; i < ARRAY_SIZE(tio); i++)
        if (tio[i] != NULL)
            DeleteExtIO((struct IORequest *) tio[i]);

    DeletePort(mp);

    return (rc);
}

static int
do_read_cmd(struct IOExtTD *tio, uint64_t offset, uint len, void *buf, int nsd)
{
    tio->iotd_Req.io_Command = CMD_READ;
    tio->iotd_Req.io_Actual  = 0;
    tio->iotd_Req.io_Offset  = offset;
    tio->iotd_Req.io_Length  = len;
    tio->iotd_Req.io_Data    = buf;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;

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
    tio->iotd_Req.io_Command = CMD_WRITE;
    tio->iotd_Req.io_Actual  = 0;
    tio->iotd_Req.io_Offset  = offset;
    tio->iotd_Req.io_Length  = len;
    tio->iotd_Req.io_Data    = buf;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;

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

    /* Geometry */
    tio->iotd_Req.io_Command = TD_GETGEOMETRY;
    tio->iotd_Req.io_Actual  = 0xa5;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = sizeof (dg);
    tio->iotd_Req.io_Data    = &dg;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("TD_GETGEOMETRY");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        printf("Success  %"PRIu32" x %"PRIu32"  C=%"PRIu32" H=%"PRIu32" "
               "S=%"PRIu32" Type=%u%s\n",
               dg.dg_TotalSectors, dg.dg_SectorSize, dg.dg_Cylinders,
               dg.dg_Heads, dg.dg_TrackSectors, dg.dg_DeviceType,
               (dg.dg_Flags & DGF_REMOVABLE) ? " Removable" : "");
        g_devsize = (uint64_t) dg.dg_TotalSectors * dg.dg_SectorSize;
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
        printf("Success  Count=%"PRIu32"\n", tio->iotd_Req.io_Actual);
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
        printf("Success  Type=%"PRIu32" %s\n",
               tio->iotd_Req.io_Actual,
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
        printf("Success  Tracks=%"PRIu32"\n", tio->iotd_Req.io_Actual);
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
    uint lun = g_lun;

    scsi_inquiry_data_t *inq_res;
    rc = do_scsi_inquiry(tio, lun, &inq_res);
    print_test_name("SCSICMD Inquiry");
    if (rc == 0) {
        printf("Success  V='%.*s' P='%.*s' R='%.*s' DT=0x%x",
               sizeof (inq_res->vendor),
               trim_spaces(inq_res->vendor, sizeof (inq_res->vendor)),
               sizeof (inq_res->product),
               trim_spaces(inq_res->product, sizeof (inq_res->product)),
               sizeof (inq_res->revision),
               trim_spaces(inq_res->revision, sizeof (inq_res->revision)),
               inq_res->device & SID_TYPE);
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
        if (key == SKEY_NOT_READY) {
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
            printf(" Sense Key %x (ASC=%02x ASCQ=%02x)\n",
                    SSD_SENSE_KEY(g_sense_data),
                    SSD_SENSE_ASC(g_sense_data),
                    SSD_SENSE_ASCQ(g_sense_data));
        }
    }

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

static int
test_etd_command(struct IOExtTD *tio, UWORD cmd, const char *cmd_name,
                 uint len, void *buf, uint io_actual, uint io_offset)
{
    int rc;

    tio->iotd_Req.io_Command = cmd;
    tio->iotd_Req.io_Offset  = io_offset;
    tio->iotd_Req.io_Actual  = io_actual;
    tio->iotd_Req.io_Length  = len;
    tio->iotd_Req.io_Data    = buf;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    tio->iotd_Count          = 0;  /* Should cause TDERR_DiskChanged */
    if (cmd_name != NULL)
        print_test_name(cmd_name);
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
    tio->iotd_Req.io_Offset  = 0;
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

static void
get_args_1(uint *arg1)
{
    if (cur_test_args == NULL)
        return;

    if (cur_test_args->arg_count > 0)
        *arg1 = cur_test_args->arg[0];

    if (cur_test_args->arg_count > 1) {
        printf("Too many args for this command\n");
        exit(RETURN_ERROR);
    }
}

static void
get_args_2(uint *arg1, uint *arg2)
{
    if (cur_test_args == NULL)
        return;

    if (cur_test_args->arg_count > 0)
        *arg1 = cur_test_args->arg[0];

    if (cur_test_args->arg_count > 1)
        *arg2 = cur_test_args->arg[1];

    if (cur_test_args->arg_count > 2) {
        printf("Too many args for this command\n");
        exit(RETURN_ERROR);
    }
}

static void
get_args_3(uint *arg1, uint *arg2, uint *arg3)
{
    if (cur_test_args == NULL)
        return;

    if (cur_test_args->arg_count > 0)
        *arg1 = cur_test_args->arg[0];

    if (cur_test_args->arg_count > 1)
        *arg2 = cur_test_args->arg[1];

    if (cur_test_args->arg_count > 2)
        *arg3 = cur_test_args->arg[2];

    if (cur_test_args->arg_count > 3) {
        printf("Too many args for this command\n");
        exit(RETURN_ERROR);
    }
}

#define BUF_COUNT 6
static int
test_cmd_read(struct IOExtTD *tio)
{
    int rc;
    uint8_t **buf = g_buf;
    uint readoffset = 0;
    uint bufsize = BUFSIZE;

    get_args_2(&bufsize, &readoffset);
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
    uint8_t **buf = g_buf;
    uint readoffset;
    uint bufsize = BUFSIZE;

    get_args_2(&bufsize, &readoffset);
    if (bufsize > BUFSIZE)
        bufsize = BUFSIZE;

    memset(buf[1], 0xa5, bufsize);
    rc = test_etd_command(tio, ETD_READ, "ETD_READ", bufsize, buf[1], 0,
                          readoffset);
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
    uint8_t **buf = g_buf;
    uint bufsize = BUFSIZE;
    uint readoffset = 0;
    uint readoffsethi = 0;

    get_args_3(&bufsize, &readoffset, &readoffsethi);
    if (bufsize > BUFSIZE)
        bufsize = BUFSIZE;

    memset(buf[1], 0xa5, bufsize);
    tio->iotd_Req.io_Command = TD_READ64;
    tio->iotd_Req.io_Actual  = readoffsethi;  // High 64 bits
    tio->iotd_Req.io_Offset  = readoffset;
    tio->iotd_Req.io_Length  = bufsize;
    tio->iotd_Req.io_Data    = buf[1];
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("TD_READ64");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        rc = do_read_cmd(tio, 0, bufsize, buf[0], 0);
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
    uint8_t **buf = g_buf;

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
            printf("Unexpected DeviceType %x", nsd_r->DeviceType);
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
    uint8_t **buf = g_buf;
    uint bufsize = BUFSIZE;
    uint readoffset = 0;
    uint readoffsethi = 0;

    get_args_3(&bufsize, &readoffset, &readoffsethi);
    if (bufsize > BUFSIZE)
        bufsize = BUFSIZE;

    memset(buf[1], 0xa5, bufsize);
    tio->iotd_Req.io_Command = NSCMD_TD_READ64;
    tio->iotd_Req.io_Actual  = readoffsethi;  // High 64 bits
    tio->iotd_Req.io_Offset  = readoffset;
    tio->iotd_Req.io_Length  = bufsize;
    tio->iotd_Req.io_Data    = buf[1];
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("NSCMD_TD_READ64");
    rc = DoIO((struct IORequest *) tio);
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
    } else {
        print_fail_nl(rc);
    }
    return (rc);
}

static int
test_nscmd_etd_read64(struct IOExtTD *tio)
{
    int rc;
    uint8_t **buf = g_buf;
    uint bufsize = BUFSIZE;
    uint readoffset = 0;
    uint readoffsethi = 0;

    get_args_3(&bufsize, &readoffset, &readoffsethi);
    if (bufsize > BUFSIZE)
        bufsize = BUFSIZE;

    memset(buf[1], 0xa6, bufsize);
    rc = test_etd_command(tio, NSCMD_ETD_READ64, "NSCMD_ETD_READ64",
                          bufsize, buf[1], readoffsethi, readoffset);
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
test_td_seek(struct IOExtTD *tio)
{
    int rc;
    uint seekoffset = 0;

    get_args_1(&seekoffset);

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
        uint8_t **buf = g_buf;
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

    get_args_1(&seekoffset);

    rc = test_etd_command(tio, ETD_SEEK, "ETD_SEEK", BUFSIZE, g_buf[1],
                          0, seekoffset);
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

    get_args_2(&seekoffset, &seekoffsethi);

    tio->iotd_Req.io_Command = TD_SEEK64;
    tio->iotd_Req.io_Actual  = seekoffsethi;  // High 64 bits
    tio->iotd_Req.io_Offset  = seekoffset;
    tio->iotd_Req.io_Length  = BUFSIZE;
    tio->iotd_Req.io_Data    = g_buf[1];
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

    get_args_2(&seekoffset, &seekoffsethi);

    tio->iotd_Req.io_Command = NSCMD_TD_SEEK64;
    tio->iotd_Req.io_Actual  = seekoffsethi;  // High 64 bits
    tio->iotd_Req.io_Offset  = seekoffset;
    tio->iotd_Req.io_Length  = BUFSIZE;
    tio->iotd_Req.io_Data    = g_buf[1];
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

    get_args_2(&seekoffset, &seekoffsethi);

    rc = test_etd_command(tio, NSCMD_ETD_SEEK64, "NSCMD_ETD_SEEK64",
                          BUFSIZE, g_buf[1], seekoffsethi, seekoffset);
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

    /* Spin down device's motor */
    tio->iotd_Req.io_Command = TD_MOTOR;
    tio->iotd_Req.io_Actual  = 0;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 0;
    tio->iotd_Req.io_Data    = NULL;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("TD_MOTOR OFF");
    rc = DoIO((struct IORequest *) tio);
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

static int
test_addremchangeint(struct IOExtTD *tio)
{
    struct Interrupt int_addrem;
    struct Interrupt int_remove;
    volatile uint32_t int_count_addrem = 0;
    volatile uint32_t int_count_remove = 0;
    struct MsgPort *mp;
    struct IOExtTD *itio;
    int rc;
    int rc2;
    int changestate;
    int have_td_remove = 0;
    int have_td_addrem = 0;
    uint tick;

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
    } else {
        print_fail_nl(rc2);
    }
    if (rc == 0)
        rc = rc2;
    if (rc2 == 0)
        have_td_addrem = 1;

    if (int_count_remove != 0) {
        print_test_name("TD_REMOVE");
        printf("Fail - Premature interrupt: %u\n", int_count_remove);
        if (rc == 0)
            rc = 1;
        int_count_remove = 0;
    }
    if (int_count_addrem != 0) {
        print_test_name("TD_ADDCHANGEINT");
        printf("Fail - Premature interrupt: %u\n", int_count_addrem);
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
    if (have_td_addrem) {
        itio->iotd_Req.io_Command = TD_REMCHANGEINT;
        itio->iotd_Req.io_Flags |= IOF_QUICK;
        print_test_name("TD_REMCHANGEINT");
        BeginIO((struct IORequest *) itio);
        rc2 = itio->iotd_Req.io_Error;
        print_fail_nl(rc2);
        if (rc == 0)
            rc = rc2;
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
    DeletePort(mp);

    return (rc);
}

static void
save_overwritten_data(struct IOExtTD *tio, uint8_t **buf)
{
    if (flag_destructive > 1)  // Do not save or restore
        return;

    /* Save data which might be overwritten into a RAM buffer */
    do_read_cmd(tio, 0, BUFSIZE, buf[2], 0);
    do_read_cmd(tio, BUFSIZE, BUFSIZE, buf[3], 0);

    if (g_devsize >= ((1ULL << 32) + BUFSIZE * 2)) {
        /* Device is large enough to test 4GB boundary */
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
    if (high && (g_devsize >= ((1ULL << 32) + BUFSIZE * 2))) {
        /* Addresses above 4GB were tested */
        do_write_cmd(tio, 1ULL << 32, BUFSIZE, buf[4], has_nsd);
        do_write_cmd(tio, (1ULL << 32) + BUFSIZE, BUFSIZE, buf[5], has_nsd);
    }
}

static int
test_cmd_write(struct IOExtTD *tio)
{
    int rc;
    uint8_t **buf = g_buf;
    uint writeoffset = 0;
    uint bufsize = BUFSIZE;

    get_args_2(&bufsize, &writeoffset);
    if (bufsize > BUFSIZE)
        bufsize = BUFSIZE;

    memset(buf[0], 0xdb, bufsize);
    tio->iotd_Req.io_Command = CMD_WRITE;
    tio->iotd_Req.io_Actual  = 0;  // Unused
    tio->iotd_Req.io_Offset  = writeoffset;
    tio->iotd_Req.io_Length  = bufsize;
    tio->iotd_Req.io_Data    = buf[0];
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("CMD_WRITE");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        check_write(tio, buf[0], buf[1], bufsize, writeoffset, 0);
    } else {
        print_fail(rc);
    }
    printf("\n");
    restore_overwritten_data(tio, buf, 0);

    return (rc);
}

static int
test_etd_write(struct IOExtTD *tio)
{
    int rc;
    uint8_t **buf = g_buf;
    uint writeoffset = 0;
    uint bufsize = BUFSIZE;

    get_args_2(&bufsize, &writeoffset);
    if (bufsize > BUFSIZE)
        bufsize = BUFSIZE;

    memset(buf[0], 0xc9, bufsize);
    rc = test_etd_command(tio, ETD_WRITE, "ETD_WRITE", bufsize, buf[0], 0,
                          writeoffset);
    if (rc == 0) {
        rc = check_write(tio, buf[0], buf[1], bufsize, writeoffset, 0);
        printf("\n");
    }
    restore_overwritten_data(tio, buf, 0);

    return (rc);
}

static int
test_td_write64(struct IOExtTD *tio)
{
    int rc;
    uint8_t **buf = g_buf;
    uint bufsize = BUFSIZE;
    uint writeoffset = 0;
    uint writeoffsethi = 0;

    get_args_3(&bufsize, &writeoffset, &writeoffsethi);
    if (bufsize > BUFSIZE)
        bufsize = BUFSIZE;

    memset(buf[0], 0xd6, bufsize);
    tio->iotd_Req.io_Command = TD_WRITE64;
    tio->iotd_Req.io_Actual  = writeoffsethi;  // High 64 bits
    tio->iotd_Req.io_Offset  = writeoffset;
    tio->iotd_Req.io_Length  = bufsize;
    tio->iotd_Req.io_Data    = buf[0];
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("TD_WRITE64");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        uint64_t offset64 = ((uint64_t) writeoffsethi << 32) | writeoffset;
        if (check_write(tio, buf[0], buf[1], bufsize, offset64, 0) == 0) {
            if ((g_devsize >= ((1ULL << 32) + bufsize * 2)) &&
                (offset64 == 0)) {
                printf("  4GB:");
                memset(buf[0], 0xd7, bufsize);
                tio->iotd_Req.io_Command = TD_WRITE64;
                tio->iotd_Req.io_Actual  = 1;  // High 64 bits
                tio->iotd_Req.io_Offset  = 0;
                tio->iotd_Req.io_Length  = bufsize;
                tio->iotd_Req.io_Data    = buf[0];
                tio->iotd_Req.io_Flags   = 0;
                tio->iotd_Req.io_Error   = 0xa5;
                rc = DoIO((struct IORequest *) tio);
                if (rc == 0) {
                    check_write(tio, buf[0], buf[1], bufsize, 1ULL << 32, 0);
                } else {
                    print_fail(rc);
                }
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
test_nscmd_td_write64(struct IOExtTD *tio)
{
    int rc;
    uint8_t **buf = g_buf;
    uint bufsize = BUFSIZE;
    uint writeoffset = 0;
    uint writeoffsethi = 0;

    get_args_3(&bufsize, &writeoffset, &writeoffsethi);
    if (bufsize > BUFSIZE)
        bufsize = BUFSIZE;

    memset(buf[0], 0xe5, bufsize);
    tio->iotd_Req.io_Command = NSCMD_TD_WRITE64;
    tio->iotd_Req.io_Actual  = writeoffsethi;  // High 64 bits
    tio->iotd_Req.io_Offset  = writeoffset;
    tio->iotd_Req.io_Length  = bufsize;
    tio->iotd_Req.io_Data    = buf[0];
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("NSCMD_TD_WRITE64");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        uint64_t offset64 = ((uint64_t) writeoffsethi << 32) | writeoffset;
        if (check_write(tio, buf[0], buf[1], bufsize, offset64, 1) == 0) {
            if ((g_devsize >= ((1ULL << 32) + bufsize * 2)) &&
                (offset64 == 0)) {
                /* Device is large enough to test 4GB boundary */
                printf("  4GB:");
                memset(buf[0], 0xe6, bufsize);
                tio->iotd_Req.io_Command = NSCMD_TD_WRITE64;
                tio->iotd_Req.io_Actual  = 1;  // High 64 bits
                tio->iotd_Req.io_Offset  = 0;
                tio->iotd_Req.io_Length  = bufsize;
                tio->iotd_Req.io_Data    = buf[0];
                tio->iotd_Req.io_Flags   = 0;
                tio->iotd_Req.io_Error   = 0xa5;
                rc = DoIO((struct IORequest *) tio);
                if (rc == 0) {
                    check_write(tio, buf[0], buf[1], bufsize, 1ULL << 32, 1);
                } else {
                    print_fail(rc);
                }
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
test_nscmd_etd_write64(struct IOExtTD *tio)
{
    int rc;
    uint8_t **buf = g_buf;

    memset(buf[0], 0xe5, BUFSIZE);
    rc = test_etd_command(tio, NSCMD_ETD_WRITE64, "NSCMD_ETD_WRITE64",
                          BUFSIZE, buf[0], 0, 0);
    if (rc == 0) {
        if ((rc = check_write(tio, buf[0], buf[1], BUFSIZE, 0, 0)) == 0) {
            if (g_devsize >= ((1ULL << 32) + BUFSIZE * 2)) {
                /* Device is large enough to test 4GB boundary */
                printf("  4GB:");
                memset(buf[0], 0xe6, BUFSIZE);
                rc = test_etd_command(tio, NSCMD_ETD_WRITE64,
                                      NULL, BUFSIZE, buf[0], 1, 0);
                if (rc == 0) {
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

static int
test_td_format(struct IOExtTD *tio)
{
    int rc;
    uint8_t **buf = g_buf;

    /* Format (acts the same same as write) */
    memset(buf[0], 0xdb, BUFSIZE);
    tio->iotd_Req.io_Command = TD_FORMAT;
    tio->iotd_Req.io_Actual  = 0;  // Unused
    tio->iotd_Req.io_Offset  = BUFSIZE;
    tio->iotd_Req.io_Length  = BUFSIZE;
    tio->iotd_Req.io_Data    = buf[0];
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("TD_FORMAT");
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
    uint8_t **buf = g_buf;

    memset(buf[0], 0xca, BUFSIZE);
    rc = test_etd_command(tio, ETD_FORMAT, "ETD_FORMAT", BUFSIZE, buf[0], 0, 0);
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
    uint8_t **buf = g_buf;

    memset(buf[0], 0xf4, BUFSIZE);
    tio->iotd_Req.io_Command = TD_FORMAT64;
    tio->iotd_Req.io_Actual  = 0;  // High 64 bits
    tio->iotd_Req.io_Offset  = BUFSIZE;
    tio->iotd_Req.io_Length  = BUFSIZE;
    tio->iotd_Req.io_Data    = buf[0];
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("TD_FORMAT64");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        if (check_write(tio, buf[0], buf[1], BUFSIZE, BUFSIZE, 0) == 0) {
            if (g_devsize >= ((1ULL << 32) + BUFSIZE * 2)) {
                /* Device is large enough to test 4GB boundary */
                printf("  4GB:");
                memset(buf[0], 0xf5, BUFSIZE);
                tio->iotd_Req.io_Command = TD_FORMAT64;
                tio->iotd_Req.io_Actual  = 1;  // High 64 bits
                tio->iotd_Req.io_Offset  = BUFSIZE;
                tio->iotd_Req.io_Length  = BUFSIZE;
                tio->iotd_Req.io_Data    = buf[0];
                tio->iotd_Req.io_Flags   = 0;
                tio->iotd_Req.io_Error   = 0xa5;
                rc = DoIO((struct IORequest *) tio);
                if (rc == 0) {
                    check_write(tio, buf[0], buf[1], BUFSIZE,
                                (1ULL << 32) + BUFSIZE, 0);
                } else {
                    print_fail(rc);
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
    uint8_t **buf = g_buf;

    memset(buf[0], 0x1e, BUFSIZE);
    tio->iotd_Req.io_Command = NSCMD_TD_FORMAT64;
    tio->iotd_Req.io_Actual  = 0;  // High 64 bits
    tio->iotd_Req.io_Offset  = BUFSIZE;
    tio->iotd_Req.io_Length  = BUFSIZE;
    tio->iotd_Req.io_Data    = buf[0];
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("NSCMD_TD_FORMAT64");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        if (check_write(tio, buf[0], buf[1], BUFSIZE, BUFSIZE, 1) == 0) {
            if (g_devsize >= ((1ULL << 32) + BUFSIZE * 2)) {
                /* Device is large enough to test 4GB boundary */
                printf("  4GB:");
                memset(buf[0], 0x1d, BUFSIZE);
                tio->iotd_Req.io_Command = NSCMD_TD_FORMAT64;
                tio->iotd_Req.io_Actual  = 1;  // High 64 bits
                tio->iotd_Req.io_Offset  = BUFSIZE;
                tio->iotd_Req.io_Length  = BUFSIZE;
                tio->iotd_Req.io_Data    = buf[0];
                tio->iotd_Req.io_Flags   = 0;
                tio->iotd_Req.io_Error   = 0xa5;
                rc = DoIO((struct IORequest *) tio);
                if (rc == 0) {
                    check_write(tio, buf[0], buf[1], BUFSIZE,
                                (1ULL << 32) + BUFSIZE, 1);
                } else {
                    print_fail(rc);
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
    uint8_t **buf = g_buf;

    memset(buf[0], 0xe5, BUFSIZE);
    rc = test_etd_command(tio, NSCMD_ETD_FORMAT64, "NSCMD_ETD_FORMAT64",
                          BUFSIZE, buf[0], 0, 0);
    if (rc == 0) {
        if ((rc = check_write(tio, buf[0], buf[1], BUFSIZE, 0, 0)) == 0) {
            if (g_devsize >= ((1ULL << 32) + BUFSIZE * 2)) {
                /* Device is large enough to test 4GB boundary */
                printf("  4GB:");
                memset(buf[0], 0xe6, BUFSIZE);
                rc = test_etd_command(tio, NSCMD_ETD_FORMAT64,
                                      NULL, BUFSIZE, buf[0], 1, 0);
                if (rc == 0) {
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

static int
test_td_rawread(struct IOExtTD *tio)
{
    int      rc;
    uint8_t *buf;

    /* TD_RAWREAD and TD_RAWWRITE must be in Chip RAM */
    buf = (uint8_t *) AllocMem(RAWBUFSIZE, MEMF_PUBLIC | MEMF_CHIP);
    if (buf == NULL) {
        printf("Unable to allocate %d byte\n", RAWBUFSIZE);
        return (1);
    }

    memset(buf, 0x1e, RAWBUFSIZE);
    tio->iotd_Req.io_Command = TD_RAWREAD;
    tio->iotd_Req.io_Actual  = 0;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = RAWBUFSIZE;
    tio->iotd_Req.io_Data    = buf;
    tio->iotd_Req.io_Flags   = IOTDF_WORDSYNC;   // Sync to 0x4489 pattern
//    tio->iotd_Req.io_Flags   = IOTDF_INDEXSYNC;  // Sync to index pulse
    tio->iotd_Req.io_Error   = 0xa5;
    print_test_name("TD_RAWREAD");
    rc = DoIO((struct IORequest *) tio);
    print_fail_nl(rc);
#if 0
    if (rc == 0) {
        int i;
        printf("0: ");
        for (i = 0; i < 16384; i++) {
            if ((i & 15) == 0)
                printf("\n");
            printf(" %02x", buf[i]);
        }
        printf("\n");
    }
#endif

    /* TD_RAWREAD and TD_RAWWRITE must be in Chip RAM */
    FreeMem(buf, RAWBUFSIZE);
    return (rc);
}

static int
test_td_rawwrite(struct IOExtTD *tio)
{
    int rc;
    size_t i;
    uint8_t *buf[3];

    /*
     * Three buffers:
     *   [0] = Original data
     *   [1] = Replacement data -- does it need to be MFM?
     *   [2] = Read buffer to verify write was successful
     */
    memset(buf, 0, sizeof (buf));
    for (i = 0; i < ARRAY_SIZE(buf); i++) {
        /* TD_RAWREAD and TD_RAWWRITE must be in Chip RAM */
        buf[i] = (uint8_t *) AllocMem(RAWBUFSIZE, MEMF_PUBLIC | MEMF_CHIP);
        if (buf[i] == NULL) {
            printf("Unable to allocate %d bytes\n", RAWBUFSIZE);
            goto buf_alloc_failed;
        }
    }

    tio->iotd_Req.io_Command = TD_RAWREAD;
    tio->iotd_Req.io_Actual  = 0;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = RAWBUFSIZE;
    tio->iotd_Req.io_Data    = buf[0];
    tio->iotd_Req.io_Flags   = IOTDF_WORDSYNC;   // Sync to 0x4489 pattern
//    tio->iotd_Req.io_Flags   = IOTDF_INDEXSYNC;  // Sync to index pulse
    tio->iotd_Req.io_Error   = 0xa5;
    if (rc != 0) {
        print_test_name("TD_RAWREAD");
        rc = DoIO((struct IORequest *) tio);
    }
    print_test_name("TD_RAWWRITE");
    printf("not implemented yet\n");

buf_alloc_failed:
    for (i = 0; i < ARRAY_SIZE(buf); i++) {
        /* TD_RAWREAD and TD_RAWWRITE must be in Chip RAM */
        FreeMem(buf[i], RAWBUFSIZE);
    }
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
#define TEST_TD_MOTOR_OFF       UBIT(28)
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

static const test_cmds_t test_cmds[] = {
    { "CHANGEINT",   2, TEST_ADDREMCHANGEINT,
                        "TD_ADDREMCHANGEINT", "Test change interrupt",
                        NULL },
    { "CHANGENUM",   0, TEST_TD_CHANGENUM,
                        "CMD_CHANGENUM", "Get media change count",
                        NULL },
    { "CHANGESTATE", 0, TEST_TD_CHANGESTATE,
                        "CMD_CHANGESTATE", "Get media change state",
                        NULL },
    { "DRIVETYPE",   0, TEST_TD_GETDRIVETYPE,
                        "CMD_GETDRIVETYPE", "Get drive type",
                        NULL },
    { "GEOMETRY",    0, TEST_CMD_GETGEOMETRY,
                        "CMD_GETGEOMETRY", "Get device geometry",
                        NULL },
    { "NSD",         0, TEST_NSD_DEVICEQUERY,
                        "CMD_NSD_DEVICEQUERY", "Query for NSD",
                        NULL },
    { "NUMTRACKS",   0, TEST_TD_GETNUMTRACKS,
                        "TD_GETNUMTRACKS", "Get track count",
                        NULL },
    { "INQUIRY",     0, TEST_HD_SCSICMD_INQ,
                        "HD_SCSICMD_INQ", "SCSI Inquiry command",
                        NULL },
    { "PROTSTATUS",  0, TEST_TD_PROTSTATUS,
                        "CMD_PROTSTATUS", "Get protected state",
                        NULL },
    { "TUR",         0, TEST_HD_SCSICMD_TUR,
                        "HD_SCSICMD_TUR", "SCSI Test Unit Ready command",
                        NULL },
    { "RAWREAD",     0, TEST_TD_RAWREAD,
                        "TD_RAWREAD", "Read raw track from (floppy) device",
                        NULL },
#if 0
    /* These are not implemented yet */
    { "RAWWRITE",    0, TEST_TD_RAWWRITE,
                        "TD_RAWWRITE", "Write raw track to (floppy) device",
                        NULL },
    { "ERAWREAD",    0, TEST_ETD_RAWREAD,
                        "ETD_RAWREAD",
                        "Extended read raw track from (floppy) device",
                        NULL },
    { "ERAWWRITE",   0, TEST_ETD_RAWWRITE,
                        "ETD_RAWWRITE",
                        "Extended write raw track to (floppy) device",
                        NULL },
#endif
    { "READ",        0, TEST_CMD_READ,
                        "CMD_READ", "Read from device",
                        "size,offset" },
    { "EREAD",       0, TEST_ETD_READ,
                        "ETD_READ", "Extended read from device",
                        "size,offset" },
    { "READ64",      0, TEST_TD_READ64,
                        "TD_READ64", "TD64 read from device",
                        "size,offset,offsethi" },
    { "NSDREAD",     0, TEST_NSCMD_TD_READ64,
                        "NSCMD_TD_READ64", "NSD Read from device",
                        "size,offset,offsethi" },
    { "NSDEREAD",    0, TEST_NSCMD_ETD_READ64,
                        "NSCMD_ETD_READ64", "NSD extended read from device",
                        "size,offset" },
    { "SEEK",        0, TEST_TD_SEEK,
                        "TD_SEEK", "Seek to offset",
                        "offset" },
    { "ESEEK",       0, TEST_ETD_SEEK,
                        "ETD_SEEK", "Extended seek to offset",
                        "offset" },
    { "SEEK64",      0, TEST_TD_SEEK64,
                        "TD_SEEK64", "TD64 seek to offset",
                        "offset,offsethi" },
    { "NSDSEEK",     0, TEST_NSCMD_TD_SEEK64,
                        "NSCMD_TD_SEEK64", "NSD seek to offset",
                        "offset,offsethi" },
    { "NSDESEEK",    0, TEST_NSCMD_ETD_SEEK64,
                        "NSCMD_ETD_SEEK64", "NSD extended seek from device",
                        "offset,offsethi" },
    { "WRITE",       1, TEST_CMD_WRITE,
                        "CMD_WRITE", "Write to device",
                        "size,offset" },
    { "EWRITE",      1, TEST_ETD_WRITE,
                        "ETD_WRITE", "Extended write to device",
                        "size,offset" },
    { "WRITE64",     1, TEST_TD_WRITE64,
                        "TD_WRITE64", "TD64 write to device",
                        "size,offset,offsethi" },
    { "NSDWRITE",    1, TEST_NSCMD_TD_WRITE64,
                        "NSCMD_TD_WRITE64", "NSD write to device",
                        "size,offset,offsethi" },
    { "NSDEWRITE",   0, TEST_NSCMD_ETD_WRITE64,
                        "NSCMD_ETD_WRITE64", "NSD extended write to device",
                        "size,offset,offsethi" },
    { "FORMAT",      1, TEST_TD_FORMAT,
                        "TD_FORMAT", "Format device",
                        NULL },
    { "EFORMAT",     1, TEST_ETD_FORMAT,
                        "ETD_FORMAT", "Extended format device",
                        NULL },
    { "FORMAT64",    1, TEST_TD_FORMAT64,
                        "TD_FORMAT64", "TD64 format device",
                        NULL },
    { "NSDFORMAT",   1, TEST_NSCMD_TD_FORMAT64,
                        "NSCMD_TD_FORMAT64", "NSD format device",
                        NULL },
    { "NSDEFORMAT",  0, TEST_NSCMD_ETD_FORMAT64,
                        "NSCMD_ETD_FORMAT64", "NSD extended format to device",
                        NULL },
    { "MOTOROFF",    2, TEST_TD_MOTOR_OFF,
                        "TD_MOTOR OFF", "Stop motor (spin down)",
                        NULL },
    { "MOTORON",     2, TEST_TD_MOTOR_ON,
                        "TD_MOTOR ON", "Start motor (spin up)",
                        NULL },
    { "START",       2, TEST_CMD_START,
                        "CMD_START", "Start device (spin up)",
                        NULL },
    { "STOP",        2, TEST_CMD_STOP,
                        "CMD_STOP", "Stop device (spin down)",
                        NULL },
    { "EJECT",       2, TEST_TD_EJECT,
                        "TD_EJECT", "Eject device",
                        NULL },
    { "LOAD",        2, TEST_TD_LOAD,
                        "TD_LOAD", "Load device (insert media)",
                        NULL },
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
    if (0 && (test_mask & TEST_TD_RAWWRITE) && test_td_rawwrite(tio))
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
        save_overwritten_data(tio, g_buf);

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
    uint64_t test_mask;
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
            printf("  AllocMem %x (%x) fail\n", BUFSIZE, memtype);
            goto allocmem_fail;
        }
    }
    g_lun = lun;
    g_buf = buf;

    if (test_count == 0) {
        /* Run all available tests */
        test_mask = 0xffffffffffffffff;
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
                           TEST_TD_RAWWRITE);
        }
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

    if (test_mask == 0) {
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
            if ((miscompares++ < 9) || g_verbose)
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
test_integrity(uint pattern, uint32_t memtype, uint bufsize, uint align)
{
    int       rc = 0;
    uint      bnum;
    uint      cur;
    uint      memtypex = memtype;
    struct IOExtTD *tio;
    struct MsgPort *mp;
    static uint    pos = 0;
    static uint8_t curbuf = 0;
    static uint8_t chkcur = 0;

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

    if (g_sector_size == 0)
        g_sector_size = 512;

    if (g_devsize == 0)
        g_devsize = 720 << 10;  // At least 720K

    for (bnum = 0; bnum < ARRAY_SIZE(g_ibuf); bnum++) {
        /*
         * g_ibuf is the collection of buffers for integrity testing.
         *     g_ialign is located within the buffer g_ibuf
         *
         * g_align[0] is the data pattern to write
         * g_align[1] is the alternatte (inverted) data pattern to write
         * g_align[2] is read buffer; initially patterened with 0x5a, but
         *               not re-patterned at every read
         * g_align[3] is second read buffer (when miscompare happens
         *               on the first read). Re-patterned with 0xa5 at
         *               every re-read.
         * g_ibuf[4] is the original data buffer, unless -dd specified
         */
        if (g_ibuf[bnum] == NULL) {
            uint32_t base;
            if ((bnum == 5) && (flag_destructive > 1))
                continue;
            g_ibuf[bnum] = AllocMemType(bufsize + align, memtypex);
            if (g_ibuf[bnum] == NULL) {
                printf("  AllocMem %x (%x) fail\n", bufsize + align, memtypex);
                rc = ENOMEM;
                goto integrity_fail;
            }
            /*
             * Create specific alignment
             * If the address is already aligned at a higher order than
             * the requested alignment, need to reduce the alignment.
             * If the address is aligned at a lower order of alignment,
             * then need to increase the alignment.
             */
            base = (uintptr_t) g_ibuf[bnum];
            if (base & (align - 1)) {
                /* Lower order of alignment; round up */
                g_align[bnum] = (void *) ((base + align - 1) & ~(align - 1));
            } else {
                /* Lower order of alignment; force alignment */
                g_align[bnum] = (void *) (base + align);
            }
            if (g_verbose)
                printf("Align %p\n", g_align[bnum]);

            if (memtypex > MEMTYPE_MAX)
                memtypex += bufsize;
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
                            if (chkcur >= ARRAY_SIZE(chkpat))
                                chkcur = 0;
                            g_align[bnum][cur] = chkpat[chkcur++];
                        }
                        break;
                    }
                }
            } else if (bnum == 1) {
                for (cur = 0; cur < bufsize / 4; cur++)
                    g_align[bnum][cur] = ~g_align[0][cur];
            } else if (bnum == 2) {
                /* Pattern the receive buffer */
                memset(g_align[bnum], 0xa5, bufsize);
            }
        }
    }
    if (pos + bufsize > g_devsize)
        pos = 0;

    if (flag_destructive == 1)
        rc = do_read_cmd(tio, pos, bufsize, g_buf[4], g_has_nsd);

    rc = do_write_cmd(tio, pos, bufsize, g_align[curbuf], g_has_nsd);
    if (rc != 0) {
        printf("write failed at 0x%x\n", pos);
        goto integrity_fail;
    }
    rc = do_read_cmd(tio, pos, bufsize, g_align[2], g_has_nsd);
    if (rc != 0) {
        printf("read failed at 0x%x\n", pos);
        goto integrity_fail;
    }
    if (memcmp(g_align[curbuf], g_align[2], bufsize) != 0) {
        printf("Miscompare at 0x%x\n", pos);
        if (memcmp_const(g_align[2], bufsize, 0xa5) == 0) {
            printf("Read buffer was not updated\n");
        } else {
            show_diffs(g_align[curbuf], g_align[2], bufsize, "expected");
        }

        /* Pattern the second receive buffer */
        memset(g_align[3], 0x5a, bufsize);

        rc = do_read_cmd(tio, pos, bufsize, g_align[3], g_has_nsd);
        if (rc != 0) {
            printf("Re-read failed at 0x%x\n", pos);
            goto integrity_fail;
        }
        if (memcmp(g_align[curbuf], g_align[3], bufsize) == 0) {
            printf("Re-read of data matches what was written "
                   "(read failure?)\n");
        } else if (memcmp_const(g_align[3], bufsize, 0x5a) == 0) {
            printf("Re-read buffer was not updated\n");
        } else if (memcmp(g_align[2], g_align[3], bufsize) == 0) {
            printf("Re-read of data matches what was read "
                   "(write failure?)\n");
        } else {
            printf("Re-read of data differs (floating data?)\n");
            show_diffs(g_align[curbuf], g_align[3], bufsize, "expected");
            show_diffs(g_align[2], g_align[3], bufsize, "first read");
        }
        CacheClearU();
        if (memcmp(g_align[curbuf], g_align[2], bufsize) == 0) {
            printf("Initial read data now matches what was written "
                   "(CPU cache or memory failure?)\n");
        }
        rc = 1;
        goto integrity_fail;
    }

integrity_fail:
    if (flag_destructive == 1) {
        int rc2 = do_write_cmd(tio, pos, bufsize, g_ibuf[4], g_has_nsd);
        if (rc2 != 0) {
            /* Bad day: you may have lost data */
            printf("restore of original data failed at 0x%x\n", pos);
            if (rc == 0)
                rc = rc2;
        }
    }
    if (rc == 0) {
        pos += bufsize;
        curbuf ^= 1;
    }

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
    printf("  Name        Command             Description\n"
           "  ----------- ------------------- --------------------------\n");
    for (bit = 0; bit < ARRAY_SIZE(test_cmds); bit++)
        printf("  %-11s %-19s %s\n",
               test_cmds[bit].alias, test_cmds[bit].name, test_cmds[bit].desc);
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
    const char *const arg_help = test_cmds[cmd].arg_help;
    if (arg_help == NULL)
        printf("No arguments for this command\n");
    else
        printf("%s(%s)\n", str, arg_help);
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
        if ((strcasecmp(test_cmds[pos].alias, str) == 0) ||
            (strcasecmp(test_cmds[pos].name, str) == 0)) {
            if (arg != NULL) {
                int count;
                uint val;
                while (sscanf(arg, "%i%n", (int *) &val, &count) == 1) {
                    if (count != 0) {
                        arg += count;
                        if (*arg == ',')
                            arg++;
                    }
                    if (args->arg_count == 4) {
                        printf("Too many arguments to %s\n", str);
                        exit(RETURN_ERROR);
                    }
                    args->arg[args->arg_count++] = val;
                }
                if (args->arg_count == 0) {
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

int
main(int argc, char *argv[])
{
    int arg;
    int rc;
    uint bnum;
    uint loop;
    uint loops = 1;
#define MAX_CMD_MASKS 32
    static uint test_cmd_count = 0;
    struct IOExtTD tio;
    uint flag_benchmark = 0;
    uint flag_geometry = 0;
    uint flag_integrity = 0;
    uint flag_openclose = 0;
    uint flag_probe = 0;
    uint flag_testpackets = 0;
    uint did_open = 0;
    uint tsize = BUFSIZE;
    uint talign = 16;
    char *unit = NULL;
    struct EClockVal dummy;

    memset(test_cmd_mask, 0, sizeof (test_cmd_mask));
#ifndef _DCC
    SysBase = *(struct ExecBase **)4UL;
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
                    case 'c':
                        if (++arg < argc) {
                            test_cmd_mask[test_cmd_count] |=
                                get_cmd(argv[arg],
                                        &test_cmd_args[test_cmd_count]);
                            if (++test_cmd_count >= ARRAY_SIZE(test_cmd_mask))
                                test_cmd_count--;
                        } else {
                            usage_cmd();
                            exit(RETURN_ERROR);
                        }
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
                            int pos = 0;
                            if ((sscanf(argv[arg], "%i%n", (int *) &tsize,
                                        &pos) != 1) || (pos == 0)) {
                                printf("Invalid transfer size %s\n", argv[arg]);
                                exit(RETURN_ERROR);
                            }
                            switch (argv[arg][pos]) {
                                case '\0':
                                    break;
                                case ',':
                                    break;
                                case 'k':
                                case 'K':
                                    tsize <<= 10;
                                    pos++;
                                    if ((argv[arg][pos] == 'b') ||
                                        (argv[arg][pos] == 'B'))
                                        pos++;
                                    break;
                                case 'm':
                                case 'M':
                                    tsize <<= 20;
                                    pos++;
                                    if ((argv[arg][pos] == 'b') ||
                                        (argv[arg][pos] == 'B'))
                                        pos++;
                                    break;
                                default:
                                    printf("Invalid transfer size %s\n",
                                           argv[arg]);
                                    exit(RETURN_ERROR);
                            }
                            if (argv[arg][pos] == ',') {
                                char *str = argv[arg] + pos + 1;
                                if ((sscanf(str, "%i%n", (int *) &talign,
                                            &pos) != 1) || (pos == 0)) {
                                    printf("Invalid alignment %s\n", str);
                                    exit(RETURN_ERROR);
                                }
                                if (talign & (talign - 1)) {
                                    printf("Invalid alignment %s; must be a "
                                           "power of 2\n", str);
                                    exit(RETURN_ERROR);
                                }
                            }
                            if (tsize & 511) {
                                printf("transfer size must be a multiple "
                                       "of 512 bytes\n");
                                exit(RETURN_ERROR);
                            }
                        } else {
                            printf("%s requires an argument\n", ptr);
                            exit(RETURN_ERROR);
                        }
                        break;
                    case 'l':
                        /* Loop count */
                        if (++arg < argc) {
                            loops = atoi(argv[arg]);
                        } else {
                            printf("%s requires an argument\n", ptr);
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
                                show_memlist();
                                exit(RETURN_OK);
                            } else if (strncasecmp(argv[arg], "chip", 4) == 0) {
                                memtype = MEMTYPE_CHIP;
                            } else if (strncasecmp(argv[arg], "fast", 4) == 0) {
                                memtype = MEMTYPE_FAST;
                            } else if (strcasecmp(argv[arg], "24bit") == 0) {
                                memtype = MEMTYPE_24BIT;
                            } else if (strncasecmp(argv[arg],
                                                   "zorro", 5) == 0) {
                                memtype = MEMTYPE_ZORRO;
                            } else if (strncasecmp(argv[arg], "copr", 3) == 0) {
                                memtype = MEMTYPE_COPROC;
                            } else if (strncasecmp(argv[arg], "acce", 4) == 0) {
                                memtype = MEMTYPE_ACCEL;
                            } else if ((strncasecmp(argv[arg],
                                                    "motherboard", 4) == 0) ||
                                       (strcasecmp(argv[arg], "mb") == 0)) {
                                memtype = MEMTYPE_MB;
                            } else if (sscanf(argv[arg], "%x", &memtype) != 1) {
                                printf("invalid argument %s for %s\n",
                                       argv[arg], ptr);
                                exit(RETURN_ERROR);
                            }
                        } else {
                            printf("%s requires an argument\n"
                                   "    One of: chip, fast, 24bit, zorro, "
                                   "accel, coproc, or <addr>\n", ptr);
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
    if (flag_integrity && !flag_destructive) {
        printf("Integrity test requires -d (destructive) flag\n");
        exit(RETURN_ERROR);
    }
    if ((flag_benchmark || flag_geometry || flag_integrity || flag_openclose ||
         flag_testpackets || flag_probe || test_cmd_mask[0]) == 0) {
        printf("You must specify an operation to perform\n");
        usage();
        exit(RETURN_ERROR);
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
            test_integrity(flag_integrity, memtype, tsize, talign))
            break;
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

    for (bnum = 0; bnum < ARRAY_SIZE(g_ibuf); bnum++)
        if (g_ibuf[bnum] != NULL)
            FreeMemType(g_ibuf[bnum], tsize + talign);

    if (loops > 1) {
        if (loop < loops)
            printf("Stopped at pass %u of %u\n", loop + 1, loops);
        else
            printf("%u passes completed successfully\n", loops);
    }
    if (loop < loops)
        exit(RETURN_ERROR);

    exit(RETURN_OK);
}
