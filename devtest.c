/*
 * devtest by Chris Hooper
 *
 * Utility to test AmigaOS block devices (trackdisk.device, scsi.device, etc).
 */
const char *version = "\0$VER: devtest 1.0 ("__DATE__") © Chris Hooper";

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

/* Trackdisk-64 enhanced commands */
#define TD_READ64    24      // Read at 64-bit offset
#define TD_WRITE64   25      // Write at 64-bit offset
#define TD_SEEK64    26      // Seek to 64-bit offset
#define TD_FORMAT64  27      // Format (write) at 64-bit offset

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


BOOL __check_abort_enabled = 0;     // Disable gcc clib2 ^C break handling

static BOOL
is_user_abort(void)
{
    if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
        return (1);
    return (0);
}

static int
open_device(const char *devname, uint unit, struct IOExtTD *tio)
{
    if (OpenDevice(devname, unit, (struct IORequest *) tio, 0))
        return (1);
    else
        return (0);
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
           "   -b           benchmark device performance\n"
           "   -d           also do destructive operations (write)\n"
           "   -g           test drive geometry\n"
           "   -h           display help\n"
           "   -l <loops>   run multiple times\n"
           "   -o           test open/close\n"
           "   -p           test all packet types (basic, TD64, NSD)\n",
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

UBYTE sense_data[255];

static int
do_scsi_inquiry(struct IOExtTD *tio, uint lun, scsi_inquiry_data_t **inq)
{
    int rc;
    scsi_inquiry_data_t *res;
    scsi_generic_t cmd;
    struct SCSICmd scmd;

#define	SCSIPI_INQUIRY_LENGTH_SCSI2	36
    res = (scsi_inquiry_data_t *) AllocMem(sizeof (*res), MEMF_PUBLIC);
    if (res == NULL) {
        printf("AllocMem ");
        *inq = NULL;
        return (1);
    }

    memset(&cmd, 0, sizeof (cmd));
    cmd.opcode = INQUIRY;
    cmd.bytes[0] = lun << 5;
    cmd.bytes[1] = 0;  // Page code
    cmd.bytes[2] = 0;
    cmd.bytes[3] = sizeof (scsi_inquiry_data_t);
    cmd.bytes[4] = 0;  // Control

    memset(&scmd, 0, sizeof (scmd));
    scmd.scsi_Data = (UWORD *) res;
    scmd.scsi_Length = sizeof (*res);
    // scmd.scsi_Actual = 0;
    scmd.scsi_Command = (UBYTE *) &cmd;
    scmd.scsi_CmdLength = 6;
    // scmd.scsi_CmdActual = 0;
    scmd.scsi_Flags = SCSIF_READ | SCSIF_AUTOSENSE;
    // scmd.scsi_Status = 0;
    scmd.scsi_SenseData = sense_data;
    scmd.scsi_SenseLength = sizeof (sense_data);
    // scmd.scsi_SenseActual = 0;

    tio->iotd_Req.io_Command = HD_SCSICMD;
    tio->iotd_Req.io_Length  = sizeof (scmd);
    tio->iotd_Req.io_Data    = &scmd;

    if ((rc = DoIO((struct IORequest *) tio)) != 0) {
        FreeMem(res, sizeof (*res));
        res = NULL;
    }
    *inq = res;
    return (rc);
}

static int
do_scsidirect_cmd(struct IOExtTD *tio, scsi_generic_t *cmd, uint cmdlen,
               void *res, uint reslen)
{
    struct SCSICmd scmd;

    memset(&scmd, 0, sizeof (scmd));
    scmd.scsi_Data = (UWORD *) res;
    scmd.scsi_Length = reslen;
    // scmd.scsi_Actual = 0;
    scmd.scsi_Command = (UBYTE *) cmd;
    scmd.scsi_CmdLength = cmdlen;  // sizeof (cmd);
    // scmd.scsi_CmdActual = 0;
    scmd.scsi_Flags = SCSIF_READ | SCSIF_AUTOSENSE;
    // scmd.scsi_Status = 0;
    scmd.scsi_SenseData = sense_data;
    scmd.scsi_SenseLength = sizeof (sense_data);
    // scmd.scsi_SenseActual = 0;

    tio->iotd_Req.io_Command = HD_SCSICMD;
    tio->iotd_Req.io_Length  = sizeof (scmd);
    tio->iotd_Req.io_Data    = &scmd;

    return (DoIO((struct IORequest *) tio));
}

static void *
do_scsidirect_alloc(struct IOExtTD *tio, scsi_generic_t *cmd, uint cmdlen,
                    uint reslen)
{
    void *res = AllocMem(reslen, MEMF_PUBLIC | MEMF_CLEAR);
    if (res == NULL) {
        printf("AllocMem ");
    } else {
        if (do_scsidirect_cmd(tio, cmd, cmdlen, res, reslen)) {
            FreeMem(res, reslen);
            res = NULL;
        }
    }
    return (res);
}

static scsi_read_capacity_10_data_t *
do_scsi_read_capacity_10(struct IOExtTD *tio, uint lun)
{
    scsi_generic_t cmd;

    memset(&cmd, 0, sizeof (cmd));
    cmd.opcode = READ_CAPACITY_10;
    cmd.bytes[0] = lun << 5;

    return (do_scsidirect_alloc(tio, &cmd, 10,
                                sizeof (scsi_read_capacity_10_data_t)));
}

#define	SRC16_SERVICE_ACTION	0x10
static scsi_read_capacity_16_data_t *
do_scsi_read_capacity_16(struct IOExtTD *tio, uint lun)
{
    scsi_generic_t cmd;
    uint len = sizeof (scsi_read_capacity_16_data_t);

    memset(&cmd, 0, sizeof (cmd));
    cmd.opcode = READ_CAPACITY_16;
    cmd.bytes[0] = SRC16_SERVICE_ACTION;
    *(uint32_t *)&cmd.bytes[8] = len;
// XXX: If I use [9] above instead of [8], the device will ignore the
//      request and cause a phase error. The A4091 driver is not handling
//      this correctly and never times out / fails the request from the
//      devtest utility.

    return (do_scsidirect_alloc(tio, &cmd, 16, len));
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

    return (do_scsidirect_alloc(tio, &cmd, 6, SCSI_MODE_PAGES_BUFSIZE));
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
    if (OpenDevice(devname, lun, (struct IORequest *) tio, 0)) {
        printf("OpenDevice failed\n");
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
        printf("%7lu %12lu %5lu %5lu %5lu  0x%02x  %s\n",
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
        printf("Mode Pages%40sFail", "");
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

    CloseDevice((struct IORequest *) tio);
opendev_fail:
    DeleteExtIO((struct IORequest *) tio);
extio_fail:
    DeletePort(mp);
    return (rc);
}

static unsigned int
read_system_ticks(void)
{
    struct DateStamp ds;
    DateStamp(&ds);  /* Measured latency is ~250us on A3000 A3640 */
    return ((unsigned int) (ds.ds_Minute) * 60 * TICKS_PER_SECOND + ds.ds_Tick);
}

static void
print_perf(unsigned int ttime, unsigned int xfer_kb, int is_write)
{
    unsigned int tsec;
    unsigned int trem;
    uint rep = xfer_kb;
    char c1 = 'K';
    char c2 = 'K';

    if (rep > 10000) {
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

    printf("%u %cB %s in %u.%02u sec: %u %cB/sec\n",
           rep, c1, is_write ? "write" : "read",
           tsec, trem * 100 / TICKS_PER_SECOND, xfer_kb, c2);
}

static int
drive_benchmark(const char *devname, uint lun, int do_destructive)
{
#define PERF_BUF_SIZE (512 << 10)
#define NUM_TIO 4
    struct IOExtTD *tio[NUM_TIO];
    uint8_t *buf[NUM_TIO];
    uint8_t opened[NUM_TIO];
    uint8_t issued[NUM_TIO];
    uint32_t pos = 0;
    struct MsgPort *mp;
    unsigned int stime;
    unsigned int etime;
    int i;
    int xfer;
    int cur;
    int rc;

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
        if (OpenDevice(devname, lun, (struct IORequest *) tio[i], 0)) {
            printf("Opendevice failed\n");
            rc = 1;
            goto opendevice_fail;
        }
        opened[i] = 1;
    }

    memset(buf, 0, sizeof (buf));
    for (i = 0; i < ARRAY_SIZE(buf); i++) {
        buf[i] = (uint8_t *) AllocMem(PERF_BUF_SIZE, MEMF_PUBLIC);
        if (buf[i] == NULL)
            goto allocmem_fail;
    }


    stime = read_system_ticks();

    memset(issued, 0, sizeof (issued));
    cur = 0;
    for (xfer = 0; xfer < 50; xfer++) {
        if (issued[cur]) {
            rc = WaitIO((struct IORequest *) tio[cur]);
            if (rc != 0) {
                issued[cur] = 0;
                printf("Error %d reading at %lu\n",
                       rc, tio[cur]->iotd_Req.io_Offset);
                break;
            }
        }

        tio[cur]->iotd_Req.io_Command = CMD_READ;
        tio[cur]->iotd_Req.io_Actual = 0;
        tio[cur]->iotd_Req.io_Data = buf[cur];
        tio[cur]->iotd_Req.io_Length = PERF_BUF_SIZE;
        tio[cur]->iotd_Req.io_Offset = pos;
        SendIO((struct IORequest *) tio[cur]);
        issued[cur] = 1;
        pos += PERF_BUF_SIZE;
        if (++cur >= ARRAY_SIZE(tio))
            cur = 0;
    }
    for (i = 0; i < ARRAY_SIZE(tio); i++) {
        if (issued[cur]) {
            rc = WaitIO((struct IORequest *) tio[cur]);
            if (rc != 0) {
                printf("Error %d reading at %lu\n",
                       rc, tio[cur]->iotd_Req.io_Offset);
            }
        }
        if (++cur >= ARRAY_SIZE(tio))
            cur = 0;
    }

    etime = read_system_ticks();
    if (etime < stime)
        etime += 24 * 60 * 60 * TICKS_PER_SECOND;  /* Next day */

    print_perf(etime - stime, PERF_BUF_SIZE / 1000 * xfer, 0);

    if (do_destructive) {
        memset(issued, 0, sizeof (issued));
        stime = read_system_ticks();

        memset(issued, 0, sizeof (issued));
        cur = 0;
        for (xfer = 0; xfer < 50; xfer++) {
            if (issued[cur]) {
                rc = WaitIO((struct IORequest *) tio[cur]);
                if (rc != 0) {
                    issued[cur] = 0;
                    printf("Error %d reading at %lu\n",
                           rc, tio[cur]->iotd_Req.io_Offset);
                    break;
                }
            }

            tio[cur]->iotd_Req.io_Command = CMD_WRITE;
            tio[cur]->iotd_Req.io_Actual = 0;
            tio[cur]->iotd_Req.io_Data = buf[cur];
            tio[cur]->iotd_Req.io_Length = PERF_BUF_SIZE;
            tio[cur]->iotd_Req.io_Offset = pos;
            SendIO((struct IORequest *) tio[cur]);
            issued[cur] = 1;
            pos += PERF_BUF_SIZE;
            if (++cur >= ARRAY_SIZE(tio))
                cur = 0;
        }
        for (i = 0; i < ARRAY_SIZE(tio); i++) {
            if (issued[cur]) {
                rc = WaitIO((struct IORequest *) tio[cur]);
                if (rc != 0) {
                    printf("Error %d reading at %lu\n",
                           rc, tio[cur]->iotd_Req.io_Offset);
                }
            }
            if (++cur >= ARRAY_SIZE(tio))
                cur = 0;
        }

        etime = read_system_ticks();
        if (etime < stime)
            etime += 24 * 60 * 60 * TICKS_PER_SECOND;  /* Next day */

        print_perf(etime - stime, PERF_BUF_SIZE / 1000 * xfer, 1);
    }

allocmem_fail:
    for (i = 0; i < ARRAY_SIZE(buf); i++)
        if (buf[i] != NULL)
            FreeMem(buf[i], PERF_BUF_SIZE);

opendevice_fail:
    for (i = 0; i < ARRAY_SIZE(tio); i++)
        if (opened[i] != 0)
            CloseDevice((struct IORequest *) tio[i]);

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

static int
test_packets(const char *devname, uint lun, int do_destructive)
{
    int    rc;
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
    if (OpenDevice(devname, lun, (struct IORequest *) tio, 0)) {
        printf("Opendevice failed\n");
        rc = 1;
        goto opendev_fail;
    }

#define BUFSIZE 8192
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
        printf("%lu sectors x %lu  C=%lu H=%lu S=%lu  Type=%u%s",
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
        printf("Success Count=%lu", tio->iotd_Req.io_Actual);
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
                printf("Type=%lu ", tio->iotd_Req.io_Actual);
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
        printf("Success Tracks=%lu", tio->iotd_Req.io_Actual);
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

    NSDeviceQueryResult_t *nsd = (NSDeviceQueryResult_t *) buf[1];
    memset(buf[1], 0xa5, BUFSIZE);
    nsd->DevQueryFormat = 0;
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
        NSDeviceQueryResult_t *nsd = (NSDeviceQueryResult_t *) buf[1];
        if (nsd->DevQueryFormat != 0) {
            printf("Unexpected DevQueryFormat %lx", nsd->DevQueryFormat);
        } else if (nsd->DeviceType != NSDEVTYPE_TRACKDISK) {
            printf("Unexpected DeviceType %x", nsd->DeviceType);
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

    if (nsd) {
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

    if (nsd) {
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
        CloseDevice((struct IORequest *) tio);
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
    char *devname = NULL;
    char *unit = NULL;
    uint lun;
    uint loops = 1;
    struct IOExtTD tio;
    uint flag_benchmark = 0;
    uint flag_destructive = 0;
    uint flag_geometry = 0;
    uint flag_openclose = 0;
    uint flag_testpackets = 0;

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
                    case 'o':
                        flag_openclose++;
                        break;
                    case 'p':
                        flag_testpackets++;
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
    if ((flag_benchmark | flag_geometry | flag_openclose |
         flag_testpackets) == 0) {
        printf("You must specify an operation to perform\n");
        usage();
        exit(1);
    }
    if (unit == NULL) {
        printf("You must specify a device name and unit number to open\n");
        usage();
        exit(1);
    }
    if (sscanf(unit, "%u", &lun) != 1) {
        printf("Invalid device unit \"%s\"\n", unit);
        usage();
        exit(1);
    }

    while (loops-- > 0) {
        if (flag_openclose) {
            if (open_device(devname, lun, &tio)) {
                printf("Failed to open %s unit %d\n", devname, lun);
            }
        }
        if (flag_geometry)
            drive_geometry(devname, lun);
        if (flag_testpackets)
            test_packets(devname, lun, flag_destructive);
        if (flag_benchmark)
            drive_benchmark(devname, lun, flag_destructive);
        if (flag_openclose)
            close_device(&tio);
        if (is_user_abort())
            break;
    }
    exit(0);
}
