#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>

#define BLK_BAR_NUM 2

#define BLK_MAGIC_VALUE             0x4b4c4244
// layout inside BAR2
#define BLK_REG_STORAGE_BASE_LO     0x0000
#define BLK_REG_STORAGE_BASE_HI     0x0004
#define BLK_REG_STORAGE_SIZE_LO     0x0008
#define BLK_REG_STORAGE_SIZE_HI     0x000c
#define BLK_REG_CUR_ADDR_LO         0x0010
#define BLK_REG_CUR_ADDR_HI         0x0014
#define BLK_REG_BLOCK_SIZE          0x0018
#define BLK_REG_SUPPORTED_BLOCKS    0x001c
#define BLK_REG_DATA_LEN            0x0020
#define BLK_REG_STATUS              0x0024
#define BLK_REG_ACK                 0x0028
#define BLK_REG_CMD                 0x002c
#define BLK_REG_ERROR               0x0030
#define BLK_REG_DEVICE_FLAGS        0x0034
#define BLK_REG_MAGIC               0x0038
#define BLK_DATA_BUFFER_OFFSET      0x1000

#define BLK_STATUS_EMPTY    0u
#define BLK_STATUS_BUSY     1u
#define BLK_STATUS_DONE     2u
#define BLK_STATUS_ERROR    3u

#define BLK_CMD_NOP         0u
#define BLK_CMD_READ        1u
#define BLK_CMD_WRITE       2u

#define BLK_ERR_NONE            0u
#define BLK_ERR_INVAL           1u
#define BLK_ERR_ALIGN           2u
#define BLK_ERR_RANGE           3u
#define BLK_ERR_IO              4u
#define BLK_ERR_UNSUPPORTED     5u
#define BLK_ERR_READONLY        6u
#define BLK_ERR_TIMEOUT         7u

// для регистра подтверждения, сигналы о том что операция на каком либо этапе
#define BLK_ACK_HOST_NEW_DATA       1u
#define BLK_ACK_DRV_READ_INPUT      2u
#define BLK_ACK_DRV_WRITE_OUTPUT    4u
#define BLK_ACK_HOST_READ_OUTPUT    8u

#define BLK_IOC_MAGIC  'b'

#define BLK_IOC_GET_ADDR            _IOR(BLK_IOC_MAGIC, 0x01, uint64_t)
#define BLK_IOC_SET_ADDR            _IOW(BLK_IOC_MAGIC, 0x02, uint64_t)
#define BLK_IOC_GET_BLOK_SIZE       _IOR(BLK_IOC_MAGIC, 0x03, uint32_t)
#define BLK_IOC_SET_BLOCK_SIZE      _IOW(BLK_IOC_MAGIC, 0x04, uint32_t)
#define BLK_IOC_GET_DATA_LEN        _IOR(BLK_IOC_MAGIC, 0x05, uint32_t)
#define BLK_IOC_SET_DATA_LEN        _IOW(BLK_IOC_MAGIC, 0x06, uint32_t)
#define BLK_IOC_GET_STORAGE_SIZE    _IOR(BLK_IOC_MAGIC, 0x07, uint64_t)
#define BLK_IOC_GET_STORAGE_BASE    _IOR(BLK_IOC_MAGIC, 0x08, uint64_t)
#define BLK_IOC_GET_BLOCK_CAPACITY  _IOR(BLK_IOC_MAGIC, 0x09, struct blk_block_capacity)
#define BLK_IOC_GET_STATUS          _IOR(BLK_IOC_MAGIC, 0x0a, uint32_t)
#define BLK_IOC_WAIT_IDLE           _IO(BLK_IOC_MAGIC, 0x0b)

#define BLK_DEFAULT_BLK_SIZE    4096u
#define BLK_DEFAULT_DATALEN     4096u
#define BLK_DEFAULT_TIMEOUT     1000u
// poll_us = время опроса девайса в микросекундах
#define BLK_DEFAULT_POLL_US     1000u



void fail(const char *msg)
{
    perror(msg);
    perror("\n");
    exit(EXIT_FAILURE);
}


// напоминание
/*
 * storage.bin - играет роль диска в котором также есть место под адресное пространство, за которое
 *  как раз таки и отвечает buffer offset
 */
struct BlkEmu
{
    int device_fd;          // bar2.bin
    size_t device_size;     // размер bar2.bin
    int storage_fd;         // storage.bin
    size_t storage_size;    // размер storage.bin
    uint8_t *bar;           // указатель на бласть в памяти bar2.bin для записи и чтения регистров
    uint64_t storage_base;  // начальный адрес области хранения данных устройства
};

static uint32_t read32(const struct BlkEmu* emu, uint32_t offset)
{
    uint32_t var32;
    memcpy(&var32, emu->bar + offset, sizeof(var32));
    return var32;
}

static void write32(const struct BlkEmu* emu, uint32_t offset, uint32_t var32)
{
    memcpy(emu->bar + offset, &var32, sizeof(var32));
}

static uint64_t read64(const struct BlkEmu* emu, uint32_t offset) //todo: проверить
{
    uint64_t lo= read32(emu, offset);
    uint64_t hi= read32(emu, offset + 4);
    return lo | (hi << 32);
}

static void write64(const struct BlkEmu* emu, uint32_t offset, uint64_t var64)
{
    write32(emu, offset, (uint32_t)var64);
    write32(emu, offset+4, (uint32_t)(var64 >> 32));
}

static bool is_supported_block(uint32_t mask, uint32_t size)
{
    switch (size)
    {
        case 1024:
            return !!(mask & 1u);
        case 2048:
            return !!(mask & 2u);
        case 4096:
            return!!(mask & 3u);
        case 8192:
            return !!(mask & 4u);
        default:
            return false;
    }
}

static void delay(long microsec)
{
    struct timespec time;
    time.tv_sec = microsec / 1000000L;
    time.tv_nsec = (microsec % 1000000L) * 1000L;
    nanosleep(&time,NULL);
}

static void set_err(struct BlkEmu *emu, uint32_t err)
{
    write32(emu, BLK_REG_ERROR, err);
    write32(emu, BLK_REG_STATUS, BLK_STATUS_ERROR);
}

static int pread_all_len(int fd, void *buf, size_t len, off_t offset)
{
    uint8_t *p = (uint8_t *)buf;
    size_t bytes_readed = 0;
    while(bytes_readed < len)
    {
        ssize_t readed_char = pread(fd, p + bytes_readed, len-bytes_readed, offset + (off_t)bytes_readed);
        if (readed_char < 0)
        {
            if(errno == EINTR) //сигнал для повтора операции
            {
                continue;
            }
            return -1;
        }
        if (readed_char == 0)
        {
            return -1;
        }
        bytes_readed += (ssize_t)readed_char;
    }
    return 0;
}

static int pwrite_all_len(int fd, void *buf, size_t len, off_t offset)
{
    uint8_t *p = (uint8_t*)buf;
    size_t bytes_readed = 0;
    while(bytes_readed < len)
    {
        ssize_t writed_char = pwrite(fd, p + bytes_readed, len-bytes_readed, offset + (off_t)bytes_readed);
        if (writed_char < 0)
        {
            if(errno == EINTR) //сигнал для повтора операции
            {
                continue;
            }
            return -1;
        }
        if (writed_char == 0)
        {
            return -1;
        }
        bytes_readed += (ssize_t)writed_char;
    }
    return 0;
}

static void init_bar(struct BlkEmu *emu)
{
    memset(emu->bar, 0, emu->device_size);

    write32(emu, BLK_REG_MAGIC , BLK_MAGIC_VALUE);
    write64(emu, BLK_REG_STORAGE_SIZE_LO , (uint64_t)emu->storage_size);
    write64(emu, BLK_REG_STORAGE_BASE_LO , emu->storage_base);
    write32(emu, BLK_REG_BLOCK_SIZE , BLK_DEFAULT_BLK_SIZE);
    write32(emu, BLK_REG_SUPPORTED_BLOCKS , 0x0Fu);
    write32(emu, BLK_REG_DATA_LEN , BLK_DEFAULT_BLK_SIZE);
    write32(emu, BLK_REG_STATUS , BLK_STATUS_EMPTY);
    write32(emu, BLK_REG_ACK , 0);
    write32(emu, BLK_REG_CMD , BLK_CMD_NOP);
    write32(emu, BLK_REG_ERROR , BLK_ERR_NONE);
}


static int validate_request(struct BlkEmu * emu, uint64_t addr, uint32_t blk_size, uint8_t len)
{
    if (!is_supported_block(0x0Fu, blk_size))
    {
        return BLK_ERR_UNSUPPORTED;
    }
    if (len == 0)
    {
        return BLK_ERR_INVAL;
    }
    if (addr % blk_size || len % blk_size)
    {
        return BLK_ERR_ALIGN;
    }
    if (emu->device_size < (size_t)BLK_DATA_BUFFER_OFFSET ||
        addr < emu->storage_base)
    {
        return BLK_ERR_RANGE;
    }

    uint64_t off_in_strg = addr - emu->storage_base; //относительное смещение, для проверки выхода за границы storage.bin
    if (off_in_strg > emu->storage_size || len > emu->storage_size - off_in_strg)
    {
        return BLK_ERR_RANGE;
    }

    return BLK_ERR_NONE;
}

static void blk_read(struct BlkEmu *emu)
{
    uint64_t addr = read64(emu, BLK_REG_CUR_ADDR_LO);
    uint32_t block_size = read32(emu, BLK_REG_BLOCK_SIZE);
    uint32_t len = read32(emu, BLK_REG_DATA_LEN);

    int err = validate_request(emu, addr, block_size, len);

    if (err != BLK_ERR_NONE)
    {
        set_err(emu, (uint32_t)err);
        return ;
    }

    off_t strg_offset = (off_t)(addr - emu->storage_base);

    if (pread_all_len(emu->storage_fd, emu->bar + BLK_DATA_BUFFER_OFFSET, len, strg_offset) < 0)
    {
        set_err(emu, BLK_ERR_IO);
        return ;
    }

    write32(emu, BLK_REG_ACK, BLK_ACK_HOST_NEW_DATA); //ack_host_read_output
    write32(emu, BLK_REG_ERROR, BLK_ERR_NONE);
    write32(emu, BLK_REG_STATUS, BLK_STATUS_DONE);
}

static void blk_write(struct BlkEmu *emu)
{
    uint64_t addr = read64(emu, BLK_REG_CUR_ADDR_LO);
    uint32_t block_size = read32(emu, BLK_REG_BLOCK_SIZE);
    uint32_t len = read32(emu, BLK_REG_DATA_LEN);

    int err = validate_request(emu, addr, block_size, len);

    if (err != BLK_ERR_NONE)
    {
        set_err(emu, (uint32_t)err);
        return ;
    }

    off_t strg_offset = (off_t)(addr - emu->storage_base);

    if (pwrite_all_len(emu->storage_fd, emu->bar + BLK_DATA_BUFFER_OFFSET, len, strg_offset) < 0)
    {
        set_err(emu, BLK_ERR_IO);
        return;
    }

    if (fsync(emu->storage_fd) < 0)
    {
        set_err(emu, BLK_ERR_IO);
        return;
    }

    write32(emu, BLK_REG_ACK, BLK_ACK_HOST_READ_OUTPUT);
    write32(emu, BLK_REG_ERROR, BLK_ERR_NONE);
    write32(emu, BLK_REG_STATUS, BLK_STATUS_DONE);
}

static void help(const char * prg)
{
    fprintf(stderr, "\nUsage: %s --dev-mem <FILE> --storage <FILE> [--storage-base <N>] [--poll <N>]\n", prg);
}

int main(int argc, char **argv)
{
    const char *dev_mem_pth = NULL;
    const char *storage_pth = NULL;
    uint64_t storage_base = 0;
    uint64_t poll_us = BLK_DEFAULT_POLL_US;

    struct option long_opts[] = {
            {"dev-mem", required_argument, 0, 'd'},
            {"storage", required_argument, 0, 's'},
            {"storage-base", required_argument, 0, 'b'},
            {"poll", required_argument, 0, 'p'},
            {0,0,0,0}
    };

    int opt;
    int opt_index = 0;

    while ((opt = getopt_long(argc, argv, "d:s:b:p", long_opts, &opt_index)))
    {
        switch (opt)
        {
            case 'd':
                dev_mem_pth = optarg;
                break;
            case 's':
                storage_pth = optarg;
                break;
            case 'b':
            {
                char *end = NULL;
                storage_base = strtoull(optarg, &end, 0);
                if (!end || *end)
                {
                    fprintf(stderr, "invalid storage-base\n");
                    return EXIT_FAILURE;
                }
                break;
            }
            case 'p':
            {
                char *end = NULL;
                poll_us = strtol(optarg, &end, 0);
                if (!end || *end)
                {
                    fprintf(stderr, "invalid poll\n");
                    return EXIT_FAILURE;
                }
                break;
            }
            default:
                help(argv[0]);
                return EXIT_FAILURE;
        }
    }

    if ( !dev_mem_pth || !storage_base )
    {
       help(argv[0]);
       return EXIT_FAILURE;
    }

    struct BlkEmu emu;
    memset(&emu, 0, sizeof(emu));
    emu.storage_base = storage_base;
    emu.device_fd = open(dev_mem_pth, O_RDWR);
    if (emu.device_fd < 0)
    {
        fail("can't open dev_mem (bar)");
    }

    emu.storage_fd = open(storage_pth, O_RDWR);
    if (emu.storage_fd < 0)
    {
       fail("can`t open storage");
    }

    struct stat st;

    if (fstat(emu.device_fd, &st) < 0)
    {
        fail("fstat(bar)");
    }
    emu.device_size = (size_t)st.st_size;

    if (fstat(emu.storage_fd, &st) < 0)
    {
        fail("fstat(storage)");
    }
    emu.storage_size = (size_t)st.st_size;

    emu.bar = mmap(NULL, emu.device_size, PROT_READ | PROT_WRITE,
                   MAP_SHARED, emu.device_fd, 0);
    if (emu.bar == MAP_FAILED)
    {
        fail("mmap(device memory)");
    }

    init_bar(&emu);

    fprintf(stderr, "[emulator] started:device_mem=%s storage=%s dev_size=%zu storage_size=%zu\n",
            dev_mem_pth, storage_pth, emu.device_size, emu.storage_size);

    while (true)
    {
        uint32_t status = read32(&emu, BLK_REG_STATUS);
        uint32_t cmd = read32(&emu, BLK_REG_CMD);
        if(status == BLK_STATUS_BUSY)
        {
            if (cmd == BLK_CMD_READ)
            {
                blk_read(&emu);
                write32(&emu, BLK_REG_CMD, BLK_CMD_NOP);
            }
            else if (cmd == BLK_CMD_WRITE)
            {
                blk_write(&emu);
                write32(&emu, BLK_REG_CMD, BLK_CMD_NOP);
            }
            else if (cmd != BLK_CMD_NOP)
            {
                set_err(&emu, BLK_ERR_INVAL);
                write32(&emu, BLK_REG_CMD, BLK_CMD_NOP);
            }
        }
        delay(poll_us);
    }

    return 0;
}
