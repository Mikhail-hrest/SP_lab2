#include <stdio.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define DRV_NAME    "my_pci_driver"
//QEMU PCI Test Device
#define BLK_VENDOR_ID   0x1B36
#define BLK_PRODUCT_ID  0x0005

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

#define BLK_ACK_DRV_READ_INPUT      2u
#define BLK_ACK_DRV_WRITE_OUTPUT    4u

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
#define BLK_DEFAULT_POLL_US     1000u

struct blk_block_capacity {
    uint32_t count;
    uint32_t sizes[8];
};

// то же что и в my_pci_driver.c

int my_getlen(const char *str)
{
    int count = 0;
    while (str && str[count] != '\0')
        count++;
    return count;
}

uint64_t get_u64(const char* str)
{
    if (str == NULL) return 0;
    char* endptr = NULL;
    errno = 0;
    uint64_t val = strtoull(str, &endptr, 10);

    if (errno == ERANGE || *endptr != '\0')
    {
        fprintf(stderr, "uncorrect value: %s", str);
        exit(EXIT_FAILURE);
    }
    return (uint64_t)val;
}

void help()
{
    fprintf(stderr, "HOW TO USE"
                    "./blk_ioctl <device> info\n"
                    "./blk_ioctl <device> set_addr <value>\n"
                    "./blk_ioctl <device> set_block <1024|2048|4096|8192?>\n"
                    "./blk_ioctl <device> set_len <value>\n"
                    "./blk_ioctl <device> read <file>\n"
                    "./blk_ioctl <device> write <file>\n"
                    );
    exit(EXIT_FAILURE);
}
void fail(const char *msg)
{
    perror(msg);
    perror("\n");
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "error count of arguments");
        help();
    }
    int descriptor = open(argv[1], O_RDWR);
    if (descriptor < 0)
    {
        fail("can`t open argv[1]");
    }

    uint32_t len;
    uint32_t val32;
    uint64_t val64;
    ssize_t n;

    int rw_fd; //файловый дескриптор для операций чтения и записи
    void *buf = NULL; //буфер для операций чтения и записи

    if (!strncmp(argv[2], "info", my_getlen(argv[2])))
    {
        uint64_t addr, storage_size, storage_base;
        uint32_t block_size, data_len, status;
        struct blk_block_capacity capacity;
        if (ioctl(descriptor, BLK_IOC_GET_ADDR, &addr) < 0)
        {
            fail("fail in getting addr");
        }
        else if (ioctl(descriptor, BLK_IOC_GET_BLOK_SIZE, &block_size) < 0)
        {
            fail("fail in getting block_size");
        }
        else if (ioctl(descriptor, BLK_IOC_GET_DATA_LEN, &data_len) < 0)
        {
            fail("fail in getting data_len");
        }
        else if (ioctl(descriptor, BLK_IOC_GET_STORAGE_SIZE, &storage_size) < 0)
        {
            fail("fail in getting storage_size");
        }
        else if (ioctl(descriptor, BLK_IOC_GET_STORAGE_BASE, &storage_base) < 0)
        {
            fail("fail in getting storage_base");
        }
        else if (ioctl(descriptor, BLK_IOC_GET_STATUS, &status) < 0)
        {
            fail("fail in getting status");
        }
        else if (ioctl(descriptor, BLK_IOC_GET_BLOCK_CAPACITY, &capacity) < 0)
        {
            fail("fail in getting capacity");
        }
        printf("addr = %lu\n"
               "block size = %u\n"
               "data len = %u\n"
               "storage base = %lu\n"
               "storage size = %lu\n"
               "status = %u\n",
               addr, block_size, data_len, storage_base, storage_size, status);
        printf("supported blocks = < " );
        for (uint32_t i = 0; i < capacity.count; ++i)
        {
            printf("%u ", capacity.sizes[i]);
        }
        printf(">\n");
    }
    else
    {
        if (argc != 4)
        {
            help();
        }
    }
    if (!strncmp(argv[2], "set_addr", my_getlen(argv[2])))
    {
        val64 = get_u64(argv[3]);
        if(ioctl(descriptor, BLK_IOC_SET_ADDR, &val64) < 0)
        {
            fail("fail in set addr");
        }
    }
    else if(!strncmp(argv[2], "set_block", my_getlen(argv[2])))
    {
        val32 = (uint32_t) get_u64(argv[3]);
        if(ioctl(descriptor, BLK_IOC_SET_BLOCK_SIZE, &val32) < 0)
        {
            fail("fail in set block size");
        }
    }
    else if(!strncmp(argv[2], "set_len", my_getlen(argv[2])))
    {
        val32 = (uint32_t) get_u64(argv[3]);
        if(ioctl(descriptor, BLK_IOC_SET_DATA_LEN, &val32) < 0)
        {
            fail("fail in set data length");
        }
    }
    else if(!strncmp(argv[2], "read", my_getlen(argv[2])))
    {
        if (ioctl(descriptor, BLK_IOC_GET_DATA_LEN, &len) < 0)
        {
            fail("read: get data length");
        }
        buf = malloc(len);
        if (!buf)
        {
            fail("read: malloc failed");
        }

        n = read(descriptor, buf, len);
        if (n != (ssize_t)len)
        {
            fail("read can`t read data");
        }
        rw_fd = open(argv[3], O_CREAT | O_WRONLY | O_TRUNC, 0644); // создать если файла нет, только для записи и очистить данные если файл не пуст
        if(rw_fd < 0)
        {
            fail("read: cant open file for writing data");
        }
        if (write(rw_fd, buf, n) != n)
        {
            fail("read: error in writing data");
        }
        close(rw_fd);
        free(buf);
    }
    else if(!strncmp(argv[2], "write", my_getlen(argv[2])))
    {
        struct stat st;
        if (ioctl(descriptor, BLK_IOC_GET_DATA_LEN, &len) < 0)
        {
            fail("write: get data length");
        }

        rw_fd = open(argv[3], O_RDONLY);
        if (rw_fd < 0)
        {
            fail("write: cant open file for reading data");
        }
        if (fstat(rw_fd, &st) < 0)
        {
            fail("write: error in fstat"); //если не смогли получить информацию о файле
        }
        if((uint32_t)st.st_size != len)
        {
            fail("write: input size must be equal data_len");
        }

        buf = malloc(len);
        if (!buf)
        {
            fail("write: malloc failed");
        }
        n = read(rw_fd, buf, len);
        if (n != (ssize_t)len)
        {
            fail("write: error in reading file");
        }
        if(write(descriptor, buf, len) != (ssize_t)len)
        {
            fail("write: eeror in write on device");
        }
        else
        {
            help();
        }
        close(rw_fd);
        free(buf);
    }

    close(descriptor);
    return 0;
}
