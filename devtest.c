/*
 * devtest by Chris Hooper
 *
 * Utility to test AmigaOS block devices (trackdisk.device, scsi.device, etc).
 */
const char *version = "\0$VER: devtest 1.2 ("__DATE__") © Chris Hooper";

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
#ifdef _DCC
#define CDERR_BadDataType    36
#define CDERR_InvalidState   37
#else
#include <devices/cd.h>
#include <inline/exec.h>
#include <inline/dos.h>
struct ExecBase *SysBase;
struct ExecBase *DOSBase;
#endif

#include <inttypes.h>
/* ULONG has changed from NDK 3.9 to NDK 3.2.
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

/* Trackdisk-64 enhanced commands */
/* Check before defining. AmigaOS 3.2 NDK provides these in
 * trackdisk.h
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

#define BUFSIZE 8192

typedef struct NSDeviceQueryResult
{
    /*
    ** Standard information
    */
    ULONG   DevQueryFormat;         /* this is type 0               */
    ULONG   SizeAvailable;          /* bytes available              */

    /*
    ** Common information (READ ONLY!)
    */
    UWORD   DeviceType;             /* what the device does         */
    UWORD   DeviceSubType;          /* depends on the main type     */
    UWORD   *SupportedCommands;     /* 0 terminated list of cmd's   */

    /* May be extended in the future! Check SizeAvailable! */
} NSDeviceQueryResult_t;

typedef unsigned int uint;

int verbose = 0;
BOOL __check_abort_enabled = 0;     // Disable gcc clib2 ^C break handling
uint sector_size = 512;             // Updated when getting drive geometry
char *devname = NULL;               // Device name
uint unitno;                        // Device unit and LUN

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
    return (OpenDevice(devname, unitno, (struct IORequest *) tio, 0));
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
           "   -b           benchmark device performance [-bb tests latency]\n"
           "   -d           also do destructive operations (write)\n"
           "   -g           test drive geometry\n"
           "   -h           display help\n"
           "   -l <loops>   run multiple times\n"
           "   -m <addr>    use specific memory (<addr> Chip Fast 24Bit Zorro -=list)\n"
           "   -o           test open/close\n"
           "   -p           probe SCSI bus for devices\n"
           "   -t           test all packet types (basic, TD64, NSD)\n",
           version + 7);
}

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

#define READ_CAPACITY_16        0x9e    /* really SERVICE ACTION IN */
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
typedef struct scsi_rw_6 {
    uint8_t opcode;
    uint8_t addr[3];
    uint8_t length;
    uint8_t control;
} __packed scsi_rw_6_t;


UBYTE sense_data[255];

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
    scmd->scsi_Flags = SCSIF_READ | SCSIF_AUTOSENSE;
    // scmd.scsi_Status = 0;
    scmd->scsi_SenseData = sense_data;
    scmd->scsi_SenseLength = sizeof (sense_data);
    // scmd.scsi_SenseActual = 0;
}

static void *
do_scsidirect_cmd(struct IOExtTD *tio, scsi_generic_t *cmd, uint cmdlen,
                  uint reslen, int *rc)
{
    void *res = AllocMem(reslen, MEMF_PUBLIC | MEMF_CLEAR);
    if (res == NULL) {
        printf("AllocMem 0x%x fail", reslen);
        *rc = ENOMEM;
    } else {
        struct SCSICmd scmd;
        setup_scsidirect_cmd(&scmd, cmd, cmdlen, res, reslen);
        tio->iotd_Req.io_Command = HD_SCSICMD;
        tio->iotd_Req.io_Length  = sizeof (scmd);
        tio->iotd_Req.io_Data    = &scmd;

        if ((*rc = DoIO((struct IORequest *) tio)) != 0) {
            FreeMem(res, reslen);
            res = NULL;
        }
    }
    return (res);
}

static int
do_scsi_inquiry(struct IOExtTD *tio, uint lun, scsi_inquiry_data_t **inq)
{
    scsi_generic_t cmd;
    int rc;

    memset(&cmd, 0, sizeof (cmd));
    cmd.opcode = INQUIRY;
    cmd.bytes[0] = lun << 5;
    cmd.bytes[1] = 0;  // Page code
    cmd.bytes[2] = 0;
    cmd.bytes[3] = sizeof (scsi_inquiry_data_t);
    cmd.bytes[4] = 0;  // Control

    *inq = do_scsidirect_cmd(tio, &cmd, 6, sizeof (**inq), &rc);
    return (rc);
}

static scsi_read_capacity_10_data_t *
do_scsi_read_capacity_10(struct IOExtTD *tio, uint lun)
{
    int rc;
    scsi_generic_t cmd;
    uint len = sizeof (scsi_read_capacity_10_data_t);

    memset(&cmd, 0, sizeof (cmd));
    cmd.opcode = READ_CAPACITY_10;
    cmd.bytes[0] = lun << 5;

    return (do_scsidirect_cmd(tio, &cmd, 10, len, &rc));
}

#define	SRC16_SERVICE_ACTION	0x10
static scsi_read_capacity_16_data_t *
do_scsi_read_capacity_16(struct IOExtTD *tio, uint lun)
{
    int rc;
    scsi_generic_t cmd;
    uint len = sizeof (scsi_read_capacity_16_data_t);

    memset(&cmd, 0, sizeof (cmd));
    cmd.opcode = READ_CAPACITY_16;
    cmd.bytes[0] = SRC16_SERVICE_ACTION;
    *(uint32_t *)&cmd.bytes[8] = len;
    /*
     * XXX: If I use [9] above instead of [8] with SCSI2SD, the device
     *      will ignore the request and cause a phase error. The A4091
     *      driver previously didn't handle this correctly and would
     *      never time out / fail the request from the devtest utility.
     */
    return (do_scsidirect_cmd(tio, &cmd, 16, len, &rc));
}

#define	MODE_SENSE_6		0x1a
#define SCSI_MODE_PAGES_BUFSIZE 255

#define DISK_PGCODE 0x3F    /* only 6 bits valid */
typedef struct scsi_generic_mode_page {
    uint8_t pg_code;        /* page code */
    uint8_t pg_length;      /* page length in bytes */
    uint8_t pg_bytes[253];  /* this number of bytes or less */
} scsi_generic_mode_page_t;

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


static uint8_t *
scsi_read_mode_pages(struct IOExtTD *tio, uint unit)
{
    int            rc;
    scsi_generic_t cmd;

#define	SMS_PAGE_ALL_PAGES		0x3f
#define	SMS_PAGE_SUBPAGES		0x00
#define	SMS_PAGE_NO_SUBPAGES            0xff
#define SMS_DBD                         0x08 /* disable block descriptors */

    memset(&cmd, 0, sizeof (cmd));
    cmd.opcode = MODE_SENSE_6;
    cmd.bytes[0] = SMS_DBD;
    cmd.bytes[1] = SMS_PAGE_ALL_PAGES;
    cmd.bytes[2] = 0; // reserved
    cmd.bytes[3] = SCSI_MODE_PAGES_BUFSIZE; // length
    cmd.bytes[4] = 0; // control

    return (do_scsidirect_cmd(tio, &cmd, 6, SCSI_MODE_PAGES_BUFSIZE, &rc));
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
scsi_probe_unit(const char *devname, uint unit, struct IOExtTD *tio)
{
    int rc;
    int erc;
    scsi_inquiry_data_t *inq_res;

    rc = OpenDevice(devname, unit, (struct IORequest *) tio, 0);
    if (rc == 0) {
        printf("%3d", unit);
        erc = do_scsi_inquiry(tio, unit, &inq_res);
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
               devtype_str(inq_res->device & SID_TYPE));
            FreeMem(inq_res, sizeof (*inq_res));
        }
        scsi_read_capacity_10_data_t *cap10;
        cap10 = do_scsi_read_capacity_10(tio, unit);
        if (cap10 != NULL) {
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
            FreeMem(cap10, sizeof (*cap10));
        }
        printf("\n");
        close_device(tio);
    }
    return (rc);
}

static int
scsi_probe(const char *devname, char *unitstr)
{
    int rc = 0;
    int found = 0;
    int justunit = -1;
    uint target;
    uint lun;
    uint unit;
    struct IOExtTD *tio;
    struct MsgPort *mp;

    if ((unitstr != NULL) &&
        (sscanf(unitstr, "%u", &unit) == 1)) {
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
            if ((justunit != -1) && (unit != justunit))
                continue;
            rc = scsi_probe_unit(devname, unit, tio);
            if (rc == 0) {
                found++;
            } else {
                if (justunit != -1)
                    printf("Open %s Unit %u fail: %d\n", devname, justunit, rc);
                break;  // Stop probing at first failed lun of each target
            }
        }
    }
    DeleteExtIO((struct IORequest *) tio);
extio_fail:
    DeletePort(mp);
    if (found == 0) {
        if (justunit == -1)
            printf("Could not open %s: %d\n", devname, rc);
        rc = 1;
    }
    return (rc);
}

static int
drive_geometry(const char *devname, uint lun)
{
    int    rc;
    struct IOExtTD *tio;
    struct DriveGeometry dg;
    struct MsgPort *mp;
    uint8_t *pages;

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
        printf("Open fail: %d\n", rc);
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
        printf("%7c %12c %5c %5c %5c  %4c  -    Fail %d\n",
               '-', '-', '-', '-', '-', '-', rc);
    } else {
        printf("%7"PRIu32" %12"PRIu32" %5"PRIu32" %5"PRIu32" %5"PRIu32"  0x%02x  %s\n",
               dg.dg_SectorSize, dg.dg_TotalSectors, dg.dg_Cylinders,
               dg.dg_Heads, dg.dg_TrackSectors,
               dg.dg_DeviceType,
               (dg.dg_Flags & DGF_REMOVABLE) ? "Yes" : "No");
    }

    printf("Inquiry ");
    scsi_inquiry_data_t *inq_res;
    rc = do_scsi_inquiry(tio, lun, &inq_res);
    if (rc != 0) {
        printf("%51c  -    Fail\n", '-');
    } else {
        printf("%46s 0x%02x  %s\n", "",
               inq_res->device & SID_TYPE,
               (inq_res->dev_qual2 & SID_REMOVABLE) ? "Yes" : "No");
#if 0
        printf("V='%.*s' P='%.*s' R='%.*s' Qual=0x%x DT=0x%x %s\n",
                sizeof (inq_res->vendor), inq_res->vendor,
                sizeof (inq_res->product), inq_res->product,
                sizeof (inq_res->revision), inq_res->revision,
                inq_res->device & SID_QUAL,
                inq_res->device & SID_TYPE,
                (inq_res->dev_qual2 & SID_REMOVABLE) ? "Removable" : "");
#endif
        FreeMem(inq_res, sizeof (*inq_res));
    }

    printf("READ_CAPACITY_10 ");
    scsi_read_capacity_10_data_t *cap10;
    cap10 = do_scsi_read_capacity_10(tio, lun);
    if (cap10 == NULL) {
        printf("%5c %12c %34s\n", '-', '-', "Fail");
    } else {
        uint last_sector = *(uint32_t *) &cap10->addr;
        printf("%5u %12u\n", *(uint32_t *) &cap10->length, last_sector + 1);
        FreeMem(cap10, sizeof (*cap10));
    }

    printf("READ_CAPACITY_16 ");
    scsi_read_capacity_16_data_t *cap16;
    cap16 = do_scsi_read_capacity_16(tio, lun);
    if (cap16 == NULL) {
        printf("%5c %12c %34s\n", '-', '-', "Fail");
    } else {
        uint64_t last_sector = *(uint64_t *) &cap16->addr;
        printf("%5u %12llu\n", *(uint32_t *) &cap16->length, last_sector + 1);
        FreeMem(cap16, sizeof (*cap16));
    }

    pages = scsi_read_mode_pages(tio, lun);
    if (pages == NULL) {
        printf("Mode Pages%40sFail\n", "");
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
                    printf("%8u %12llu\n", ssize, blks);
                    break;
                }
                default:
                    printf("page 0x%02x len=%d\n", pages[pos], pages[pos + 1]);
                case 0x00:  // Vendor-specific
                case 0x01:  // Error recovery
                case 0x02:  // Disconnect-Reconnect
                case 0x08:  // Caching
                case 0x0a:  // Control
                case 0x30:  // Apple-specific
                    break;
            }
        }
#if 0
        int i = 0;
        for (i = 0; i < len; i++)
            printf(" %02x", pages[i]);
        printf("\n");
#endif
        FreeMem(pages, SCSI_MODE_PAGES_BUFSIZE);
    }

    close_device(tio);
opendev_fail:
    DeleteExtIO((struct IORequest *) tio);
extio_fail:
    DeletePort(mp);
    return (rc);
}

static void
print_time(void)
{
    struct tm *tm;
    time_t timet;
    time(&timet);
    tm = localtime(&timet);
    printf("%04d-%02d-%02d %02d:%02d:%02d",
           tm->tm_year + 1900, tm->tm_mon, tm->tm_mday,
           tm->tm_hour, tm->tm_min, tm->tm_sec);
}

static unsigned int
read_system_ticks(void)
{
    struct DateStamp ds;
    DateStamp(&ds);  /* Measured latency is ~250us on A3000 A3640 */
    return ((unsigned int) (ds.ds_Minute) * 60 * TICKS_PER_SECOND + ds.ds_Tick);
}

static unsigned int
read_system_ticks_wait(void)
{
    unsigned int stick = read_system_ticks();
    unsigned int tick;

    do {
        tick = read_system_ticks();
    } while (tick == stick);

    return (tick);
}

static void
print_perf(unsigned int ttime, unsigned int xfer_kb, int is_write,
           uint xfer_size)
{
    unsigned int tsec;
    unsigned int trem;
    uint rep = xfer_kb;
    char c1 = 'K';
    char c2 = 'K';

    if (rep >= 10000) {
        rep /= 1000;
        c1 = 'M';
    }
    if (ttime == 0)
        ttime = 1;
    tsec = ttime / TICKS_PER_SECOND;
    trem = ttime % TICKS_PER_SECOND;

    if ((xfer_kb / ttime) >= (100000 / TICKS_PER_SECOND)) {
        /* Transfer rate > 100 MB/sec */
        xfer_kb /= 1000;
        c2 = 'M';
    }
    if (xfer_kb < 10000000) {
        /* Transfer < 10 MB (or 10 GB) */
        xfer_kb = xfer_kb * TICKS_PER_SECOND / ttime;
    } else {
        /* Change math order to avoid 32-bit overflow */
        ttime /= TICKS_PER_SECOND;
        if (ttime == 0)
            ttime = 1;
        xfer_kb /= ttime;
    }

    if (verbose) {
        printf("%4u %cB %s in %2u.%02u sec: %3u KB xfer: %3u %cB/sec\n",
               rep, c1, is_write ? "write" : "read ",
               tsec, trem * 100 / TICKS_PER_SECOND, xfer_size / 1024,
               xfer_kb, c2);
    } else {
        printf("%s %3u KB xfers %13u %cB/sec\n",
               is_write ? "write" : "read ", xfer_size / 1024, xfer_kb, c2);
    }
}

static void
print_latency(uint ttime, uint iters, char endch)
{
    uint tusec;
    uint tmsec;
    if (iters == 0)
        iters = 1;
    tusec = ttime * (1000000 / TICKS_PER_SECOND) / iters;
    tmsec = tusec / 1000;
    tusec %= 1000;

    printf("%u.%03u ms%c", tmsec, tusec / 10, endch);
}

static int
latency_getgeometry(const char *devname, uint lun, struct IOExtTD **tio,
                    int max_iter)
{
    int iter;
    int rc = 0;
    int failcode;
    int iters;
    int num_iter = max_iter;
    unsigned int stime;
    unsigned int etime;
    unsigned int ttime;
    struct DriveGeometry dg;

    for (iters = 0; iters < max_iter; iters++) {
        if ((rc = open_device(tio[iters])) != 0) {
            printf("Open fail: %d\n", rc);
            break;
        }
    }
    num_iter = iters;
    if (iters == 0)
        return (1);

    printf("TD_GETGEOMETRY sequential   ");

    stime = read_system_ticks_wait();
    for (iter = 0; iter < num_iter; iter++) {
        tio[iter]->iotd_Req.io_Command = TD_GETGEOMETRY;
        tio[iter]->iotd_Req.io_Actual  = 0xa5;
        tio[iter]->iotd_Req.io_Offset  = 0;
        tio[iter]->iotd_Req.io_Length  = sizeof (dg);
        tio[iter]->iotd_Req.io_Data    = &dg;
        tio[iter]->iotd_Req.io_Flags   = 0;
        tio[iter]->iotd_Req.io_Error   = 0xa5;
        failcode = DoIO((struct IORequest *) tio[iter]);
        if (failcode != 0) {
            if (++rc < 10)
                printf("  Error %d\n", failcode);
        }

        etime = read_system_ticks();
        if (etime < stime)
            etime += 24 * 60 * 60 * TICKS_PER_SECOND;  /* Next day */
        ttime = etime - stime;
        if (ttime > TICKS_PER_SECOND * 2) {
            iter++;
            break;
        }
    }
    ttime = etime - stime;
    print_latency(ttime, iter, '\n');

    iters = iter;
    printf("TD_GETGEOMETRY parallel     ");
    stime = read_system_ticks_wait();
    for (iter = 0; iter < iters; iter++) {
        tio[iter]->iotd_Req.io_Command = TD_GETGEOMETRY;
        tio[iter]->iotd_Req.io_Actual  = 0xa5;
        tio[iter]->iotd_Req.io_Offset  = 0;
        tio[iter]->iotd_Req.io_Length  = sizeof (dg);
        tio[iter]->iotd_Req.io_Data    = &dg;
        tio[iter]->iotd_Req.io_Flags   = 0;
        tio[iter]->iotd_Req.io_Error   = 0xa5;
        SendIO((struct IORequest *) tio[iter]);
    }
    for (iter = 0; iter < iters; iter++) {
        failcode = WaitIO((struct IORequest *) tio[iter]);
        if (failcode != 0) {
            if (++rc < 10) {
                printf("  Error %d\n", failcode);
            }
        }
    }
    etime = read_system_ticks();
    if (etime < stime)
        etime += 24 * 60 * 60 * TICKS_PER_SECOND;  /* Next day */
    ttime = etime - stime;
    print_latency(ttime, iter, ' ');
    printf("(%u requests)\n", iters);

    for (iter = 0; iter < num_iter; iter++)
        close_device(tio[iter]);

    sector_size = dg.dg_SectorSize;

    return (0);
}

static int
latency_iocmd(UWORD iocmd, uint8_t *buf, int num_iter, struct IOExtTD **tio)
{
    int iter;
    int rc = 0;
    int failcode;
    unsigned int stime;
    unsigned int etime;

    if (iocmd == CMD_READ)
        printf("CMD_READ sequential         ");
    else
        printf("CMD_WRITE sequential        ");

    stime = read_system_ticks_wait();
    for (iter = 0; iter < num_iter; iter++) {
        tio[0]->iotd_Req.io_Command = iocmd;
        tio[0]->iotd_Req.io_Actual  = 0;
        tio[0]->iotd_Req.io_Offset  = 0;
        tio[0]->iotd_Req.io_Length  = BUFSIZE;
        tio[0]->iotd_Req.io_Data    = buf;
        tio[0]->iotd_Req.io_Flags   = 0;
        tio[0]->iotd_Req.io_Error   = 0xa5;

        failcode = DoIO((struct IORequest *) tio[0]);
        if (failcode != 0) {
            rc += failcode;
            if (++rc < 10)
                printf("  Error %d\n", failcode);
            break;
        }
    }
    etime = read_system_ticks();
    if (etime < stime)
        etime += 24 * 60 * 60 * TICKS_PER_SECOND;  /* Next day */
    print_latency(etime - stime, iter, '\n');


    if (iocmd == CMD_READ)
        printf("CMD_READ parallel           ");
    else
        printf("CMD_WRITE parallel          ");

    for (iter = 0; iter < num_iter; iter++) {
        tio[iter]->iotd_Req.io_Command = iocmd;
        tio[iter]->iotd_Req.io_Actual  = 0;
        tio[iter]->iotd_Req.io_Offset  = 0;
        tio[iter]->iotd_Req.io_Length  = BUFSIZE;
        tio[iter]->iotd_Req.io_Data    = buf;
        tio[iter]->iotd_Req.io_Flags   = 0;
        tio[iter]->iotd_Req.io_Error   = 0xa5;
    }

    stime = read_system_ticks_wait();
    for (iter = 0; iter < num_iter; iter++) {
        SendIO((struct IORequest *) tio[iter]);
    }
    for (iter = 0; iter < num_iter; iter++) {
        int failcode = WaitIO((struct IORequest *) tio[iter]);
        if (failcode != 0) {
            if (++rc < 10) {
                printf("  Error %d\n", failcode);
            }
        }
    }
    etime = read_system_ticks();
    if (etime < stime)
        etime += 24 * 60 * 60 * TICKS_PER_SECOND;  /* Next day */
    print_latency(etime - stime, iter, ' ');
    printf("(%u requests)\n", num_iter);
    return (rc);
}

static int
latency_scsidirect_iocmd(UWORD iocmd, uint8_t *buf, int num_iter,
                         struct IOExtTD **tio)
{
    int iter;
    int rc = 0;
    int failcode;
    unsigned int stime;
    unsigned int etime;
    scsi_rw_6_t cmd;
    struct SCSICmd *scmd;

    scmd = AllocMem(sizeof (*scmd) * num_iter, MEMF_PUBLIC);
    if (scmd == NULL) {
        printf("Allocmem failed\n");
        return (1);
    }

    if (iocmd == CMD_READ)
        printf("HD_SCSICMD read sequential  ");
    else
        printf("HD_SCSICMD write sequential ");

    memset(&cmd, 0, sizeof (cmd));
    if (iocmd == CMD_READ)
        cmd.opcode = SCSI_READ_6_COMMAND;
    else
        cmd.opcode = SCSI_WRITE_6_COMMAND;
    cmd.addr[0] = 0; // lun << 5;
    cmd.length = BUFSIZE / sector_size;

    for (iter = 0; iter < num_iter; iter++) {
        setup_scsidirect_cmd(scmd + iter, (scsi_generic_t *) &cmd, sizeof (cmd),
                             buf, BUFSIZE);
        tio[iter]->iotd_Req.io_Command = HD_SCSICMD;
        tio[iter]->iotd_Req.io_Length  = sizeof (*scmd);
        tio[iter]->iotd_Req.io_Data    = scmd + iter;
    }

    stime = read_system_ticks_wait();
    for (iter = 0; iter < num_iter; iter++) {
        failcode = DoIO((struct IORequest *) tio[iter]);
        if (failcode != 0) {
            rc += failcode;
            if (++rc < 10)
                printf("  Error %d\n", failcode);
            break;
        }
    }
    etime = read_system_ticks();
    if (etime < stime)
        etime += 24 * 60 * 60 * TICKS_PER_SECOND;  /* Next day */
    print_latency(etime - stime, iter, '\n');

    if (iocmd == CMD_READ)
        printf("HD_SCSICMD read parallel    ");
    else
        printf("HD_SCSICMD write parallel   ");

    for (iter = 0; iter < num_iter; iter++) {
        setup_scsidirect_cmd(scmd + iter, (scsi_generic_t *) &cmd, sizeof (cmd),
                             buf, BUFSIZE);
        tio[iter]->iotd_Req.io_Command = HD_SCSICMD;
        tio[iter]->iotd_Req.io_Length  = sizeof (*scmd);
        tio[iter]->iotd_Req.io_Data    = scmd + iter;
    }

    stime = read_system_ticks_wait();
    for (iter = 0; iter < num_iter; iter++) {
        SendIO((struct IORequest *) tio[iter]);
    }
    for (iter = 0; iter < num_iter; iter++) {
        int failcode = WaitIO((struct IORequest *) tio[iter]);
        if (failcode != 0) {
            if (++rc < 10) {
                printf("  Error %d\n", failcode);
            }
        }
    }
    etime = read_system_ticks();
    if (etime < stime)
        etime += 24 * 60 * 60 * TICKS_PER_SECOND;  /* Next day */
    print_latency(etime - stime, iter, ' ');
    printf("(%u requests)\n", num_iter);

    FreeMem(scmd, sizeof (*scmd) * num_iter);

    return (rc);
}

static int
latency_read(const char *devname, uint lun, struct IOExtTD **tio, int max_iter,
             int do_destructive)
{
    int rc = 0;
    int iter;
    int num_iter;
    uint8_t *buf;

    buf = AllocMem(BUFSIZE, MEMF_PUBLIC);
    if (buf == NULL) {
        printf("  AllocMem\n");
        return (1);
    }

    if (max_iter > 100)
        max_iter = 100;

    for (num_iter = 0; num_iter < max_iter; num_iter++) {
        if ((rc = open_device(tio[num_iter])) != 0) {
            printf("Open fail: %d\n", rc);
            break;
        }
    }
    if (num_iter == 0)
        return (1);

    if (latency_iocmd(CMD_READ, buf, num_iter, tio))
        rc++;

    if (latency_scsidirect_iocmd(CMD_READ, buf, num_iter, tio))
        rc++;

    if (do_destructive) {
        if (latency_iocmd(CMD_WRITE, buf, num_iter, tio))
            rc++;
        if (latency_scsidirect_iocmd(CMD_WRITE, buf, num_iter, tio))
            rc++;
    }

    FreeMem(buf, BUFSIZE);

    for (iter = 0; iter < num_iter; iter++)
        close_device(tio[iter]);

    return (rc);
}

static int
drive_latency(uint lun, int do_destructive)
{
    int iters;
    int i;
    int rc = 0;
    struct MsgPort *mp;
    struct IOExtTD *tio;
    struct IOExtTD **mtio;
    unsigned int stime;
    unsigned int etime;
    unsigned int ttime;

    mp = CreatePort(0, 0);
    if (mp == NULL) {
        printf("Failed to create message port\n");
        return (1);
    }

    printf("OpenDevice / CloseDevice    ");

    tio = (struct IOExtTD *) CreateExtIO(mp, sizeof (struct IOExtTD));
    if (tio == NULL) {
        printf("Failed to create tio struct\n");
        rc = 1;
        goto need_delete_port;
    }

#define OPENDEVICE_MAX 10000
    stime = read_system_ticks_wait();
    for (iters = 0; iters < OPENDEVICE_MAX; iters++) {
        if ((rc = open_device(tio)) != 0) {
            printf("Open fail: %d\n", rc);
            break;
        }
        close_device(tio);
        if ((iters & 7) == 0) {
            etime = read_system_ticks();
            if (etime < stime)
                etime += 24 * 60 * 60 * TICKS_PER_SECOND;  /* Next day */
            ttime = etime - stime;
            if (ttime > TICKS_PER_SECOND * 2) {
                iters++;
                break;
            }
        }
    }

    etime = read_system_ticks();
    if (etime < stime)
        etime += 24 * 60 * 60 * TICKS_PER_SECOND;  /* Next day */
    ttime = etime - stime;

    print_latency(ttime, iters, '\n');

    printf("OpenDevice multiple         ");

#define NUM_MTIO 1000
    mtio = AllocMem(sizeof (*mtio) * NUM_MTIO, MEMF_PUBLIC | MEMF_CLEAR);
    if (mtio == NULL) {
        printf("AllocMem\n");
        rc = 1;
        goto need_delete_tio;
    }

    for (i = 0; i < NUM_MTIO; i++) {
        mtio[i] = (struct IOExtTD *) CreateExtIO(mp, sizeof (struct IOExtTD));
        if (mtio[i] == NULL) {
            printf("Failed to create tio structs\n");
            rc = 1;
            goto need_delete_mtio;
        }
    }

    if ((rc = open_device(tio)) != 0) {
        printf("Open fail: %d\n", rc);
        rc = 1;
        goto need_delete_mtio;
    }
    /* Note that tio is left open, so driver is maintaining state here... */

    stime = read_system_ticks_wait();
    for (iters = 0; iters < NUM_MTIO; iters++) {
        if ((rc = open_device(mtio[iters])) != 0) {
            printf("Open fail: %d\n", rc);
            break;
        }
        if ((iters & 7) == 0) {
            etime = read_system_ticks();
            if (etime < stime)
                etime += 24 * 60 * 60 * TICKS_PER_SECOND;  /* Next day */
            ttime = etime - stime;
            if ((ttime > TICKS_PER_SECOND * 2) ||
                (iters > NUM_MTIO - 7)) {
                iters++;
                break;
            }
        }
    }
    etime = read_system_ticks();
    if (etime < stime)
        etime += 24 * 60 * 60 * TICKS_PER_SECOND;  /* Next day */
    ttime = etime - stime;
    print_latency(ttime, iters, '\n');

    printf("CloseDevice multiple        ");
    stime = read_system_ticks_wait();
    for (i = 0; i < iters; i++)
        close_device(mtio[i]);
    etime = read_system_ticks();
    if (etime < stime)
        etime += 24 * 60 * 60 * TICKS_PER_SECOND;  /* Next day */
    ttime = etime - stime;
    print_latency(ttime, iters, '\n');

    if ((latency_getgeometry(devname, lun, mtio, NUM_MTIO / 4)) ||
        (latency_read(devname, lun, mtio, NUM_MTIO, do_destructive))) {
        rc = 1;
    }

    close_device(tio);

need_delete_mtio:
    for (i = 0; i < NUM_MTIO; i++)
        if (mtio[i] != NULL)
            DeleteExtIO((struct IORequest *) mtio[i]);
    FreeMem(mtio, sizeof (*mtio) * NUM_MTIO);

need_delete_tio:
    DeleteExtIO((struct IORequest *) tio);

need_delete_port:
    DeletePort(mp);
    return (rc);
}

#define PERF_BUF_SIZE (512 << 10)

#define NUM_TIO 4
static int
run_bandwidth(UWORD iocmd, struct IOExtTD *tio[NUM_TIO], uint8_t *buf[NUM_TIO],
              uint32_t bufsize)
{
    int xfer;
    int i;
    int rc = 0;
    uint8_t issued[NUM_TIO];
    int cur = 0;
    uint32_t pos = 0;
    unsigned int stime;
    unsigned int etime;
    int rep;

    for (rep = 0; rep < 10; rep++) {
        memset(issued, 0, sizeof (issued));

        stime = read_system_ticks_wait();

        for (xfer = 0; xfer < 50; xfer++) {
            if (issued[cur]) {
                int failcode = WaitIO((struct IORequest *) tio[cur]);
                if (failcode != 0) {
                    issued[cur] = 0;
                    printf("  Error %d %sing at %"PRIu32"\n",
                           failcode, (iocmd == CMD_READ) ? "read" : "writ",
                           tio[cur]->iotd_Req.io_Offset);
                    rc++;
                    break;
                } else if (tio[cur]->iotd_Req.io_Error != 0) {
                    printf("Got io_Error %d\n", tio[cur]->iotd_Req.io_Error);
                }
                if ((xfer & 0x7) == 0) {
                    /* Cut out early if device is slow */
                    etime = read_system_ticks();
                    if (etime - stime > TICKS_PER_SECOND) {
                        if (etime < stime)
                            etime += 24 * 60 * 60 * TICKS_PER_SECOND;
                        if (etime - stime > TICKS_PER_SECOND)
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
                if (failcode != 0) {
                    printf("  Error %d %sing at %"PRIu32"\n",
                           failcode, (iocmd == CMD_READ) ? "read" : "writ",
                           tio[cur]->iotd_Req.io_Offset);
                    rc++;
                }
            } else if (tio[cur]->iotd_Req.io_Error != 0) {
                printf("Got io_Error %d\n", tio[cur]->iotd_Req.io_Error);
            }
            if (++cur >= NUM_TIO)
                cur = 0;
        }

        etime = read_system_ticks();
        if (etime < stime)
            etime += 24 * 60 * 60 * TICKS_PER_SECOND;  /* Next day */

        print_perf(etime - stime, bufsize / 1000 * xfer,
                   (iocmd == CMD_READ) ? 0 : 1, bufsize);
        bufsize >>= 2;
        if (bufsize < 16384)
            break;
    }

    return (rc);
}

#include <exec/execbase.h>
#include <exec/memory.h>
#include <libraries/configregs.h>

#define MEMTYPE_ANY   0
#define MEMTYPE_CHIP  1
#define MEMTYPE_FAST  2
#define MEMTYPE_24BIT 3
#define MEMTYPE_ZORRO 4
#define MEMTYPE_ACCEL 5
#define MEMTYPE_MAX   5

static const char *
memtype_str(uint32_t mem)
{
    const char *type;
    if (((mem > 0x1000) && (mem < 0x00200000)) || (mem == MEMTYPE_CHIP)) {
        type = "Chip";
    } else if ((mem >= 0x00c00000) && (mem < 0x00d80000)) {
        type = "Slow";
    } else if (((mem >= 0x04000000) && (mem < 0x08000000)) ||
               (mem == MEMTYPE_FAST))  {
        type = "Fast";
    } else if (mem == MEMTYPE_ZORRO) {
        type = "Zorro";
    } else if ((mem >= E_MEMORYBASE) && (mem < E_MEMORYBASE + E_MEMORYSIZE)) {
        type = "Zorro II";
    } else if ((mem >= 0x10000000) && (mem < 0x80000000)) {
        type = "Zorro III";
    } else if (((mem >= 0x80000000) && (mem < 0xe0000000)) ||
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
        uint32_t    size = (void *) mem->mh_Upper - (void *) mem;
        const char *type = memtype_str((uint32_t) mem);

        printf("%s RAM at %p size=0x%x\n", type, mem, size);

        for (chunk = mem->mh_First; chunk != NULL;
             chunk = chunk->mc_Next) {
            printf("  %p 0x%x\n", chunk, (uint) chunk->mc_Bytes);
        }
    }
    Permit();
}

APTR
AllocMemType(ULONG byteSize, uint32_t memtype)
{
    switch (memtype) {
        case 0:
            /* Highest priority (usually fast) memory */
            return (AllocMem(byteSize, MEMF_PUBLIC | MEMF_ANY));
        case MEMTYPE_CHIP:
            /* Chip memory */
            return (AllocMem(byteSize, MEMF_PUBLIC | MEMF_CHIP));
        case MEMTYPE_FAST:
            /* Fast memory */
            return (AllocMem(byteSize, MEMF_PUBLIC | MEMF_FAST));
        case MEMTYPE_24BIT:
            /* 24-bit memory */
            return (AllocMem(byteSize, MEMF_PUBLIC | MEMF_24BITDMA));
        case MEMTYPE_ZORRO:
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
            APTR              addr      = NULL;

            Forbid();
            for (mem = (struct MemHeader *)eb->MemList.lh_Head;
                 mem->mh_Node.ln_Succ != NULL;
                 mem = (struct MemHeader *)mem->mh_Node.ln_Succ) {
                uint32_t size = (void *) mem->mh_Upper - (void *) mem;

                if ((memtype == MEMTYPE_ZORRO) &&
                    ((((uint32_t) mem < E_MEMORYBASE) ||
                      ((uint32_t) mem > E_MEMORYBASE + E_MEMORYSIZE)) &&
                     (((uint32_t) mem < 0x10000000) ||    // EZ3_CONFIGAREA
                      ((uint32_t) mem > 0x80000000)))) {  // EZ3_CONFIGAREAEND
                    /* Not in Zorro II or Zorro III address range */
                    continue;
                }
                if ((memtype == MEMTYPE_ACCEL) &&
                     (((uint32_t) mem < 0x80000000) ||
                      ((uint32_t) mem > 0xe0000000))) {
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
            return (addr);
        }
        default:
            /* Allocate user-specified address */
            if (memtype > MEMTYPE_MAX)
                return (AllocAbs(byteSize, (APTR) memtype));
            return (NULL);
    }
}

static int
drive_benchmark(int do_destructive, uint32_t memtype)
{
    struct IOExtTD *tio[NUM_TIO];
    uint8_t *buf[NUM_TIO];
    uint8_t opened[NUM_TIO];
    uint32_t perf_buf_size = PERF_BUF_SIZE;
    struct MsgPort *mp;
    int i;
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
            printf("Open fail: %d\n", rc);
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
                int j;
                for (j = 0; j < i; j++) {
                    FreeMem(buf[j], perf_buf_size);
                    buf[j] = NULL;
                }
                perf_buf_size /= 2;
                goto try_again;
            }
            printf("Unable to allocate ");
            if (memtype != 0)
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
           devname, unitno, memtype_str((uint32_t) buf[0]));
    if (verbose) {
        for (i = 0; i < ARRAY_SIZE(buf); i++)
            printf(" %08x", (uint32_t) buf[i]);
    }
    printf("\n");

    rc += run_bandwidth(CMD_READ, tio, buf, perf_buf_size);

    if (do_destructive)
        rc += run_bandwidth(CMD_WRITE, tio, buf, perf_buf_size);

allocmem_fail:
    for (i = 0; i < ARRAY_SIZE(buf); i++)
        if (buf[i] != NULL)
            FreeMem(buf[i], perf_buf_size);

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

typedef struct {
    int errcode;
    const char *const errstr;
} err_to_str_t;
err_to_str_t err_to_str[] = {
    { IOERR_OPENFAIL, "IOERR_OPENFAIL" },
    { IOERR_ABORTED, "IOERR_ABORTED" },
    { IOERR_NOCMD, "IOERR_NOCMD (unsupported)" },
    { IOERR_BADLENGTH, "IOERR_BADLENGTH" },
    { IOERR_BADADDRESS, "IOERR_BADADDRESS" },
    { IOERR_UNITBUSY, "IOERR_UNITBUSY" },
    { IOERR_SELFTEST, "IOERR_SELFTEST" },
    { TDERR_NotSpecified, "TDERR_NotSpecified" },
    { TDERR_NoSecHdr, "TDERR_NoSecHdr" },
    { TDERR_BadSecPreamble, "TDERR_BadSecPreamble" },
    { TDERR_BadSecID, "TDERR_BadSecID" },
    { TDERR_BadHdrSum, "TDERR_BadHdrSum" },
    { TDERR_BadSecSum, "TDERR_BadSecSum" },
    { TDERR_TooFewSecs, "TDERR_TooFewSecs" },
    { TDERR_BadSecHdr, "TDERR_BadSecHdr" },
    { TDERR_WriteProt, "TDERR_WriteProt" },
    { TDERR_DiskChanged, "TDERR_DiskChanged" },
    { TDERR_SeekError, "TDERR_SeekError" },
    { TDERR_NoMem, "TDERR_NoMem" },
    { TDERR_BadUnitNum, "TDERR_BadUnitNum" },
    { TDERR_BadDriveType, "TDERR_BadDriveType" },
    { TDERR_DriveInUse, "TDERR_DriveInUse" },
    { TDERR_PostReset, "TDERR_PostReset" },
    { CDERR_BadDataType, "CDERR_BadDataType" },
    { CDERR_InvalidState, "CDERR_InvalidState" },
    { HFERR_SelfUnit, "HFERR_SelfUnit" },
    { HFERR_DMA, "HFERR_DMA" },
    { HFERR_Phase, "HFERR_Phase" },
    { HFERR_Parity, "HFERR_Parity" },
    { HFERR_SelTimeout, "HFERR_SelTimeout" },
    { HFERR_BadStatus, "HFERR_BadStatus" },
    { HFERR_NoBoard, "HFERR_NoBoard" },
    { ENOMEM, "ENOMEM" },
};

static void
print_fail(int rc)
{
    int i;
    printf("Fail %d", rc);
    for (i = 0; i < ARRAY_SIZE(err_to_str); i++)
        if (err_to_str[i].errcode == rc)
            printf(" %s", err_to_str[i].errstr);
}

static int
check_write(struct IOExtTD *tio, uint8_t *wbuf, uint8_t *rbuf, uint bufsize,
            uint64_t offset, int has_nsd)
{
    int rc;
    memset(rbuf, 0xa5, bufsize);
    rc = do_read_cmd(tio, offset, bufsize, rbuf, has_nsd);
    if (rc == 0) {
        if (memcmp(wbuf, rbuf, bufsize) == 0)
            printf("Success");
        else
            printf("Miscompare");
    } else {
        printf("V");
        print_fail(rc);
    }
    return (rc);
}

static int
test_packets(uint lun, int do_destructive)
{
    int    rc = 1;
    struct IOExtTD *tio;
    struct MsgPort *mp;
#define BUF_COUNT 6
    uint8_t *buf[BUF_COUNT];
    int i;
    int changenum = 0;
    int has_nsd = 0;
    struct DriveGeometry dg;
    uint64_t devsize = 0;

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
        printf("Open fail: %d\n", rc);
        rc = 1;
        goto opendev_fail;
    }

    memset(buf, 0, sizeof (buf));
    for (i = 0; i < ARRAY_SIZE(buf); i++) {
        buf[i] = (uint8_t *) AllocMem(BUFSIZE, MEMF_PUBLIC);
        if (buf[i] == NULL)
            goto allocmem_fail;
    }

    /* Geometry */
    tio->iotd_Req.io_Command = TD_GETGEOMETRY;
    tio->iotd_Req.io_Actual  = 0xa5;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = sizeof (dg);
    tio->iotd_Req.io_Data    = &dg;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    printf("TD_GETGEOMTRY\t   ");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        printf("Success %"PRIu32" x %"PRIu32"  C=%"PRIu32" H=%"PRIu32" S=%"PRIu32" Type=%u%s",
               dg.dg_TotalSectors, dg.dg_SectorSize, dg.dg_Cylinders,
               dg.dg_Heads, dg.dg_TrackSectors, dg.dg_DeviceType,
               (dg.dg_Flags & DGF_REMOVABLE) ? " Removable" : "");
        changenum = tio->iotd_Req.io_Actual;
        devsize = (uint64_t) dg.dg_TotalSectors * dg.dg_SectorSize;
    } else {
        print_fail(rc);
    }
    printf("\n");

    /* Miscellaneous packets */
    tio->iotd_Req.io_Command = TD_CHANGENUM;
    tio->iotd_Req.io_Actual  = 0xa5;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 0;
    tio->iotd_Req.io_Data    = NULL;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    printf("TD_CHANGENUM\t   ");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        changenum = tio->iotd_Req.io_Actual;
        printf("Success Count=%"PRIu32, tio->iotd_Req.io_Actual);
    } else {
        print_fail(rc);
    }
    printf("\n");

    tio->iotd_Req.io_Command = TD_CHANGESTATE;
    tio->iotd_Req.io_Actual  = 0xa5;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 0;
    tio->iotd_Req.io_Data    = NULL;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    printf("TD_CHANGESTATE\t   ");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        printf("Success ");
        if (tio->iotd_Req.io_Actual == 0)
            printf("Disk present");
        else
            printf("No disk present");
    } else {
        print_fail(rc);
    }
    printf("\n");

    tio->iotd_Req.io_Command = TD_PROTSTATUS;
    tio->iotd_Req.io_Actual  = 0xa5;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 0;
    tio->iotd_Req.io_Data    = 0;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    printf("TD_PROTSTATUS\t   ");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        printf("Success ");
        if (tio->iotd_Req.io_Actual == 0)
            printf("Unprotected");
        else
            printf("Protected");
    } else {
        print_fail(rc);
    }
    printf("\n");

    tio->iotd_Req.io_Command = TD_GETDRIVETYPE;
    tio->iotd_Req.io_Actual  = 0xa5;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 0;
    tio->iotd_Req.io_Data    = NULL;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    printf("TD_GETDRIVETYPE\t   ");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        printf("Success ");
        switch (tio->iotd_Req.io_Actual) {
            case DRIVE3_5:
                printf("3.5\"");
                break;
            case DRIVE5_25:
                printf("5.25\"");
                break;
            case DRIVE3_5_150RPM:
                printf("3.5\" 150RPM");
                break;
            default:
                printf("Type=%"PRIu32" ", tio->iotd_Req.io_Actual);
        }
    } else {
        print_fail(rc);
    }
    printf("\n");

    tio->iotd_Req.io_Command = TD_GETNUMTRACKS;
    tio->iotd_Req.io_Actual  = 0xa5;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 0;
    tio->iotd_Req.io_Data    = NULL;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    printf("TD_GETNUMTRACKS\t   ");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        printf("Success Tracks=%"PRIu32, tio->iotd_Req.io_Actual);
    } else {
        print_fail(rc);
    }
    printf("\n");

    /* HD_SCSICMD (SCSI direct) */
    scsi_inquiry_data_t *inq_res;
    rc = do_scsi_inquiry(tio, lun, &inq_res);
    printf("HD_SCSICMD\t   ");
    if (rc == 0) {
        printf("Success V='%.*s' P='%.*s' R='%.*s' DT=0x%x",
               sizeof (inq_res->vendor),
               trim_spaces(inq_res->vendor, sizeof (inq_res->vendor)),
               sizeof (inq_res->product),
               trim_spaces(inq_res->product, sizeof (inq_res->product)),
               sizeof (inq_res->revision),
               trim_spaces(inq_res->revision, sizeof (inq_res->revision)),
               inq_res->device & SID_TYPE);
        if (inq_res->dev_qual2 & SID_QUAL_LU_NOTPRESENT)
            printf(" NoLUNs");
        if (inq_res->dev_qual2 & SID_REMOVABLE)
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
        FreeMem(inq_res, sizeof (*inq_res));
    } else {
        print_fail(rc);
    }

    /* Read */
    memset(buf[0], 0x5a, BUFSIZE);
    printf("CMD_READ\t   ");
    rc = do_read_cmd(tio, 0, BUFSIZE, buf[0], has_nsd);
    if (rc == 0) {
        uint count;
        for (count = 0; count < BUFSIZE; count++)
            if (buf[0][count] != 0x5a)
                break;
        if (count == BUFSIZE) {
            printf("No data\n");
            goto read_done;
        } else {
            printf("Success\n");
        }
    } else {
        print_fail(rc);
        printf("\n");
        goto read_done;
    }
    if (do_destructive)
        memcpy(buf[2], buf[0], BUFSIZE);  // Keep a copy

    memset(buf[1], 0xa5, BUFSIZE);
    tio->iotd_Req.io_Command = ETD_READ;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = BUFSIZE;
    tio->iotd_Req.io_Data    = buf[1];
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    tio->iotd_Count          = changenum;
    printf("ETD_READ\t   ");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        if (memcmp(buf[0], buf[1], BUFSIZE) == 0)
            printf("Success");
        else
            printf("Miscompare");
    } else {
        print_fail(rc);
    }
    printf("\n");

    memset(buf[1], 0xa5, BUFSIZE);
    tio->iotd_Req.io_Command = TD_READ64;
    tio->iotd_Req.io_Actual  = 0;  // High 64 bits
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = BUFSIZE;
    tio->iotd_Req.io_Data    = buf[1];
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    printf("TD_READ64\t   ");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        if (memcmp(buf[0], buf[1], BUFSIZE) == 0)
            printf("Success");
        else
            printf("Miscompare");
    } else {
        print_fail(rc);
    }
    printf("\n");

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
    printf("NSCMD_DEVICEQUERY  ");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        if (nsd_r->DevQueryFormat != 0) {
            printf("Unexpected DevQueryFormat %"PRIx32, nsd_r->DevQueryFormat);
        } else if (nsd_r->DeviceType != NSDEVTYPE_TRACKDISK) {
            printf("Unexpected DeviceType %x", nsd_r->DeviceType);
        } else {
            printf("Success");
            has_nsd++;
        }

        memset(buf[1], 0xa5, BUFSIZE);
    } else {
        print_fail(rc);
    }
    printf("\n");

    if (has_nsd) {
        memset(buf[1], 0xa5, BUFSIZE);
        tio->iotd_Req.io_Command = NSCMD_TD_READ64;
        tio->iotd_Req.io_Actual  = 0;  // High 64 bits
        tio->iotd_Req.io_Offset  = 0;
        tio->iotd_Req.io_Length  = BUFSIZE;
        tio->iotd_Req.io_Data    = buf[1];
        tio->iotd_Req.io_Flags   = 0;
        tio->iotd_Req.io_Error   = 0xa5;
        printf("NSCMD_TD_READ64\t   ");
        rc = DoIO((struct IORequest *) tio);
        if (rc == 0) {
            if (memcmp(buf[0], buf[1], BUFSIZE) == 0)
                printf("Success");
            else
                printf("Miscompare");
        } else {
            print_fail(rc);
        }
        printf("\n");
    }

read_done:
    /* Seek */
    tio->iotd_Req.io_Command = TD_SEEK;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 0;
    tio->iotd_Req.io_Data    = NULL;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    memset(buf[1], 0xa5, BUFSIZE);
    printf("TD_SEEK \t   ");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        printf("Success");
    } else {
        /* Commodore scsi.device requires a buffer for TD_SEEK? */
        tio->iotd_Req.io_Command = TD_SEEK;
        tio->iotd_Req.io_Offset  = 0;
        tio->iotd_Req.io_Length  = BUFSIZE;
        tio->iotd_Req.io_Data    = buf[1];
        tio->iotd_Req.io_Flags   = 0;
        tio->iotd_Req.io_Error   = 0xa5;
        memset(buf[1], 0xa5, BUFSIZE);
        rc = DoIO((struct IORequest *) tio);
        if (rc == 0) {
            printf("Success (Bug: requires io_Length)");
        } else {
            print_fail(rc);
        }
    }
    printf("\n");

    tio->iotd_Req.io_Command = ETD_SEEK;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 0;
    tio->iotd_Req.io_Data    = NULL;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    tio->iotd_Count          = changenum;
    printf("ETD_SEEK\t   ");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0)
        printf("Success");
    else
        print_fail(rc);
    printf("\n");

    tio->iotd_Req.io_Command = TD_SEEK64;
    tio->iotd_Req.io_Actual  = 0;  // High 64 bits
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 0;
    tio->iotd_Req.io_Data    = NULL;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    printf("TD_SEEK64\t   ");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0)
        printf("Success");
    else
        print_fail(rc);
    printf("\n");

    if (has_nsd) {
        tio->iotd_Req.io_Command = NSCMD_TD_SEEK64;
        tio->iotd_Req.io_Actual  = 0;  // High 64 bits
        tio->iotd_Req.io_Offset  = 0;
        tio->iotd_Req.io_Length  = 0;
        tio->iotd_Req.io_Data    = NULL;
        tio->iotd_Req.io_Flags   = 0;
        tio->iotd_Req.io_Error   = 0xa5;
        printf("NSCMD_TD_SEEK64\t   ");
        rc = DoIO((struct IORequest *) tio);
        if (rc == 0)
            printf("Success");
        else
            print_fail(rc);
        printf("\n");
    }

    /* Write */
    if (do_destructive == 0)
        goto write_done;

    /* Capture data overwritten by format commands */
    do_read_cmd(tio, BUFSIZE, BUFSIZE, buf[3], has_nsd);

    /* Device is large enough to test 4GB boundary */
    if (devsize >= ((1ULL << 32) + BUFSIZE * 2)) {
        do_read_cmd(tio, 1ULL << 32, BUFSIZE, buf[4], has_nsd);
        do_read_cmd(tio, (1ULL << 32) + BUFSIZE, BUFSIZE, buf[5], has_nsd);
    }

    memset(buf[0], 0xdb, BUFSIZE);
    tio->iotd_Req.io_Command = CMD_WRITE;
    tio->iotd_Req.io_Actual  = 0;  // Unused
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = BUFSIZE;
    tio->iotd_Req.io_Data    = buf[0];
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    printf("CMD_WRITE\t   ");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        check_write(tio, buf[0], buf[1], BUFSIZE, 0, has_nsd);
    } else {
        print_fail(rc);
    }
    printf("\n");

    memset(buf[0], 0xc9, BUFSIZE);
    tio->iotd_Req.io_Command = ETD_WRITE;
    tio->iotd_Req.io_Actual  = 0;  // Unused
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = BUFSIZE;
    tio->iotd_Req.io_Data    = buf[0];
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    tio->iotd_Count          = changenum;
    printf("ETD_WRITE\t   ");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        check_write(tio, buf[0], buf[1], BUFSIZE, 0, has_nsd);
    } else {
        print_fail(rc);
    }
    printf("\n");

    memset(buf[0], 0xd6, BUFSIZE);
    tio->iotd_Req.io_Command = TD_WRITE64;
    tio->iotd_Req.io_Actual  = 0;  // High 64 bits
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = BUFSIZE;
    tio->iotd_Req.io_Data    = buf[0];
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    printf("TD_WRITE64\t   ");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        if (check_write(tio, buf[0], buf[1], BUFSIZE, 0, has_nsd) == 0) {
            if (devsize >= ((1ULL << 32) + BUFSIZE * 2)) {
                printf(" 4GB:");
                memset(buf[0], 0xd7, BUFSIZE);
                tio->iotd_Req.io_Command = TD_WRITE64;
                tio->iotd_Req.io_Actual  = 1;  // High 64 bits
                tio->iotd_Req.io_Offset  = 0;
                tio->iotd_Req.io_Length  = BUFSIZE;
                tio->iotd_Req.io_Data    = buf[0];
                tio->iotd_Req.io_Flags   = 0;
                tio->iotd_Req.io_Error   = 0xa5;
                rc = DoIO((struct IORequest *) tio);
                if (rc == 0) {
                    check_write(tio, buf[0], buf[1], BUFSIZE,
                                1ULL << 32, has_nsd);
                } else {
                    print_fail(rc);
                }
            }
        }
    } else {
        print_fail(rc);
    }
    printf("\n");

    if (has_nsd) {
        memset(buf[0], 0xe5, BUFSIZE);
        tio->iotd_Req.io_Command = NSCMD_TD_WRITE64;
        tio->iotd_Req.io_Actual  = 0;  // High 64 bits
        tio->iotd_Req.io_Offset  = 0;
        tio->iotd_Req.io_Length  = BUFSIZE;
        tio->iotd_Req.io_Data    = buf[0];
        tio->iotd_Req.io_Flags   = 0;
        tio->iotd_Req.io_Error   = 0xa5;
        printf("NSCMD_TD_WRITE64   ");
        rc = DoIO((struct IORequest *) tio);
        if (rc == 0) {
            if (check_write(tio, buf[0], buf[1], BUFSIZE, 0, has_nsd) == 0) {
                if (devsize >= ((1ULL << 32) + BUFSIZE * 2)) {
                    /* Device is large enough to test 4GB boundary */
                    printf(" 4GB:");
                    memset(buf[0], 0xe6, BUFSIZE);
                    tio->iotd_Req.io_Command = NSCMD_TD_WRITE64;
                    tio->iotd_Req.io_Actual  = 1;  // High 64 bits
                    tio->iotd_Req.io_Offset  = 0;
                    tio->iotd_Req.io_Length  = BUFSIZE;
                    tio->iotd_Req.io_Data    = buf[0];
                    tio->iotd_Req.io_Flags   = 0;
                    tio->iotd_Req.io_Error   = 0xa5;
                    rc = DoIO((struct IORequest *) tio);
                    if (rc == 0) {
                        check_write(tio, buf[0], buf[1], BUFSIZE,
                                    1ULL << 32, has_nsd);
                    } else {
                        print_fail(rc);
                    }
                }
            }
        } else {
            print_fail(rc);
        }
        printf("\n");
    }

    /*
     * To do:
     * TD_MOTOR
     * TD_REMOVE
     * TD_RAWREAD
     * TD_RAWWRITE
     * TD_ADDCHANGEINT
     * TD_REMCHANGEINT
     * TD_EJECT
     */

    /* Format (acts the same same as write) */
    memset(buf[0], 0xdb, BUFSIZE);
    tio->iotd_Req.io_Command = TD_FORMAT;
    tio->iotd_Req.io_Actual  = 0;  // Unused
    tio->iotd_Req.io_Offset  = BUFSIZE;
    tio->iotd_Req.io_Length  = BUFSIZE;
    tio->iotd_Req.io_Data    = buf[0];
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    printf("TD_FORMAT\t   ");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        check_write(tio, buf[0], buf[1], BUFSIZE, BUFSIZE, has_nsd);
    } else {
        print_fail(rc);
    }
    printf("\n");

    memset(buf[0], 0xc9, BUFSIZE);
    tio->iotd_Req.io_Command = ETD_FORMAT;
    tio->iotd_Req.io_Actual  = 0;  // Unused
    tio->iotd_Req.io_Offset  = BUFSIZE;
    tio->iotd_Req.io_Length  = BUFSIZE;
    tio->iotd_Req.io_Data    = buf[0];
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    tio->iotd_Count          = changenum;
    printf("ETD_FORMAT\t   ");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        check_write(tio, buf[0], buf[1], BUFSIZE, BUFSIZE, has_nsd);
    } else {
        print_fail(rc);
    }
    printf("\n");

    memset(buf[0], 0xf4, BUFSIZE);
    tio->iotd_Req.io_Command = TD_FORMAT64;
    tio->iotd_Req.io_Actual  = 0;  // High 64 bits
    tio->iotd_Req.io_Offset  = BUFSIZE;
    tio->iotd_Req.io_Length  = BUFSIZE;
    tio->iotd_Req.io_Data    = buf[0];
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    printf("TD_FORMAT64\t   ");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        if (check_write(tio, buf[0], buf[1], BUFSIZE, BUFSIZE, has_nsd) == 0) {
            if (devsize >= ((1ULL << 32) + BUFSIZE * 2)) {
                /* Device is large enough to test 4GB boundary */
                printf(" 4GB:");
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
                                (1ULL << 32) + BUFSIZE, has_nsd);
                } else {
                    print_fail(rc);
                }
            }
        }
    } else {
        print_fail(rc);
    }
    printf("\n");

    if (has_nsd) {
        memset(buf[0], 0x1e, BUFSIZE);
        tio->iotd_Req.io_Command = NSCMD_TD_FORMAT64;
        tio->iotd_Req.io_Actual  = 0;  // High 64 bits
        tio->iotd_Req.io_Offset  = BUFSIZE;
        tio->iotd_Req.io_Length  = BUFSIZE;
        tio->iotd_Req.io_Data    = buf[0];
        tio->iotd_Req.io_Flags   = 0;
        tio->iotd_Req.io_Error   = 0xa5;
        printf("NSCMD_TD_FORMAT64  ");
        rc = DoIO((struct IORequest *) tio);
        if (rc == 0) {
            if (check_write(tio, buf[0], buf[1], BUFSIZE, BUFSIZE,
                            has_nsd) == 0) {
                if (devsize >= ((1ULL << 32) + BUFSIZE * 2)) {
                    /* Device is large enough to test 4GB boundary */
                    printf(" 4GB:");
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
                                    (1ULL << 32) + BUFSIZE, has_nsd);
                    } else {
                        print_fail(rc);
                    }
                }
            }
        } else {
            print_fail(rc);
        }
        printf("\n");
    }

    /* Restore overwritten data */
    do_write_cmd(tio, 0, BUFSIZE, buf[2], has_nsd);
    do_write_cmd(tio, BUFSIZE, BUFSIZE, buf[3], has_nsd);
    if (devsize >= ((1ULL << 32) + BUFSIZE * 2)) {
        /* Addresses above 4GB were tested */
        do_write_cmd(tio, 1ULL << 32, BUFSIZE, buf[4], has_nsd);
        do_write_cmd(tio, (1ULL << 32) + BUFSIZE, BUFSIZE, buf[5], has_nsd);
    }

    /* Stop device */
    memset(buf[0], 0xf4, BUFSIZE);
    tio->iotd_Req.io_Command = CMD_STOP;
    tio->iotd_Req.io_Actual  = 0;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 0;
    tio->iotd_Req.io_Data    = NULL;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    printf("CMD_STOP\t   ");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        printf("Success");
    } else {
        print_fail(rc);
    }
    printf("\n");

    /* Start device */
    memset(buf[0], 0xf4, BUFSIZE);
    tio->iotd_Req.io_Command = CMD_START;
    tio->iotd_Req.io_Actual  = 0;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = 0;
    tio->iotd_Req.io_Data    = NULL;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0xa5;
    printf("CMD_START\t   ");
    rc = DoIO((struct IORequest *) tio);
    if (rc == 0) {
        printf("Success");
    } else {
        print_fail(rc);
    }
    printf("\n");


write_done:

allocmem_fail:
    for (i = 0; i < ARRAY_SIZE(buf); i++)
        if (buf[i] != NULL)
            FreeMem(buf[i], BUFSIZE);
    if (tio != NULL)
        close_device(tio);
opendev_fail:
    if (tio != NULL)
        DeleteExtIO((struct IORequest *) tio);
extio_fail:
    DeletePort(mp);
    return (rc);
}

int
main(int argc, char *argv[])
{
    int arg;
    int rc;
    uint loop;
    uint loops = 1;
    uint32_t memtype = MEMTYPE_ANY;
    struct IOExtTD tio;
    uint flag_benchmark = 0;
    uint flag_destructive = 0;
    uint flag_geometry = 0;
    uint flag_openclose = 0;
    uint flag_probe = 0;
    uint flag_testpackets = 0;
    char *unit = NULL;

#ifndef _DCC
    SysBase = *(struct ExecBase **)4UL;
#endif

    for (arg = 1; arg < argc; arg++) {
        char *ptr = argv[arg];
        if (*ptr == '-') {
            for (++ptr; *ptr != '\0'; ptr++) {
                switch (*ptr) {
                    case 'b':
                        flag_benchmark++;
                        break;
                    case 'h':
                        usage();
                        exit(0);
                    case 'd':
                        flag_destructive++;
                        break;
                    case 'g':
                        flag_geometry++;
                        break;
                    case 'l':
                        /* Loop count */
                        if (++arg < argc) {
                            loops = atoi(argv[arg]);
                        } else {
                            printf("%s requires an argument\n", ptr);
                            exit(1);
                        }
                        break;
                    case 'm':
                        if (++arg < argc) {
                            if (strcmp(argv[arg], "-") == 0) {
                                show_memlist();
                                exit(0);
                            } else if (strcasecmp(argv[arg], "chip") == 0) {
                                memtype = MEMTYPE_CHIP;
                            } else if (strcasecmp(argv[arg], "fast") == 0) {
                                memtype = MEMTYPE_FAST;
                            } else if (strcasecmp(argv[arg], "24bit") == 0) {
                                memtype = MEMTYPE_24BIT;
                            } else if (strcasecmp(argv[arg], "zorro") == 0) {
                                memtype = MEMTYPE_ZORRO;
                            } else if (strncasecmp(argv[arg],
                                                   "accel", 5) == 0) {
                                memtype = MEMTYPE_ACCEL;
                            } else if (sscanf(argv[arg], "%x", &memtype) != 1) {
                                printf("invalid argument %s for %s\n",
                                       argv[arg], ptr);
                                exit(1);
                            }
                        } else {
                            printf("%s requires an argument\n", ptr);
                            exit(1);
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
                        verbose++;
                        break;
                    default:
                        printf("Unknown argument %s\n", ptr);
                        usage();
                        exit(1);
                }
            }
        } else if (devname == NULL) {
            devname = ptr;
        } else if (unit == NULL) {
            unit = ptr;
        } else {
            printf("Error: unknown argument %s\n", ptr);
            usage();
            exit(1);
        }
    }
    if ((flag_benchmark || flag_geometry || flag_openclose ||
         flag_testpackets || flag_probe) == 0) {
        printf("You must specify an operation to perform\n");
        usage();
        exit(1);
    }
    if (unit == NULL) {
        if (flag_benchmark || flag_geometry || flag_openclose ||
            flag_testpackets) {
            printf("You must specify a device name and unit number to open\n");
            usage();
            exit(1);
        }
    } else if (sscanf(unit, "%u", &unitno) != 1) {
        printf("Invalid device unit \"%s\"\n", unit);
        usage();
        exit(1);
    }

    for (loop = 0; loop < loops; loop++) {
        if (loops > 1) {
            printf("Pass %u  ", loop + 1);
            print_time();
            printf("  ");
        }
        if (flag_benchmark &&
            drive_benchmark(flag_destructive, memtype) &&
            (loop == 0)) {
            break;
        }
        if (flag_openclose && ((rc = open_device(&tio)) != 0)) {
            printf("Failed to open %s unit %d: %d\n", devname, unitno, rc);
            if (loop == 0)
                break;
        }
        if (flag_probe && scsi_probe(devname, unit) && (loop == 0))
            break;
        if (flag_geometry && drive_geometry(devname, unitno) && (loop == 0))
            break;
        if (flag_testpackets && test_packets(unitno, flag_destructive) &&
            (loop == 0))
            break;
        if ((flag_benchmark > 1) &&
            drive_latency(unitno, flag_destructive) &&
            (loop == 0)) {
            break;
        }
        if (flag_openclose)
            close_device(&tio);
        if (is_user_abort())
            break;
    }
    exit(0);
}
