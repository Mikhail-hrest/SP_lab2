#include <linux/module.h>
#include <linux/pci.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/device.h>
#include <linux/minmax.h>
#include <linux/atomic.h>
#include <linux/ioctl.h>

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
    u32 count;
    u32 sizes[8];
};

static struct pci_device_id blk_id_table[] = {
        { PCI_DEVICE(BLK_VENDOR_ID, BLK_PRODUCT_ID)     },
        { 0,									        }
};
MODULE_DEVICE_TABLE(pci, blk_id_table);

struct blk_dev {
    struct pci_dev *pdev;       // используется для вызова pci api

    void __iomem *bar;          // Это память BAR устройства. void,
    resource_size_t bar_len;    // тк могут понадобиться различные размеры доступа памяти

    struct miscdevice miscdev;
    struct mutex lock;

    u64 addr;
    u32 block_size;
    u32 data_len;
    u32 timeout_ms;
    u32 poll_interval_us;

    u64 storage_base;
    u64 storage_size;
    u32 supported_blocks;
    u32 device_flags;
};

// функции для чтения и записи регистров
static inline u32 blk_read32(struct blk_dev *d, u32 reg)
{
    return ioread32(d->bar + reg);
}
static inline void blk_write32(struct blk_dev *d, u32 reg, u32 value)
{
    return iowrite32(value, d->bar + reg);
}

static u64 blk_read64(struct blk_dev *d, u32 reg)   // считываю поотдельности первые 32 бита и после вторые 32,
                                                    // тк не все архитектуры могут поддерживать 64 битную версию
{                                                   // без этой предостороженности можно было бы обойтись readq()
    u64 lower = blk_read32(d, reg);
    u64 higher = blk_read32(d, reg + 4);
    return lower | (higher << 32);
}

static void blk_write64(struct blk_dev *d, u32 reg, u64 val)
{
    blk_write32(d, reg, lower_32_bits(val)); // lower_32_bits - из файла  wordpart.h
    blk_write32(d, reg + 4, upper_32_bits(val));
}


static int blk_open(struct inode *inode, struct file *pfile);
static int blk_release(struct inode *inode, struct file *pfile);
static ssize_t blk_read(struct file *pfile, char __user *buf, size_t len, loff_t *ppos);         // чтение блока из устройства
static ssize_t blk_write(struct file *pfile, const char __user *buf, size_t len, loff_t *ppos);  // запись блока в устройство
static long blk_ioctl(struct file *pfile, unsigned int cmd, unsigned long arg);                  // управление устройством

static loff_t blk_llseek(struct file *filp, loff_t off, int whence);                             // отвечает за изменение позиции чтения.записи в файле устройства

static const struct file_operations blkdev_ops = {
        .owner          = THIS_MODULE,
        .open           = blk_open,
        .release        = blk_release,
        .unlocked_ioctl = blk_ioctl,
        .read           = blk_read,
        .write          = blk_write,
        .llseek         = blk_llseek
};



static void blk_remove(struct pci_dev *pdev)
{
    struct blk_dev *d = pci_get_drvdata(pdev);
    if (d)
    {
        misc_deregister(&d->miscdev);
    }
}

static int blk_mask_to_caps(u32 mask, struct blk_block_capacity * caps)
{
    memset(caps, 0, sizeof(*caps));
    for (int i = 0; i < 32; ++i)
    {
        if (mask & (1u << i))
        {
            if (caps->count >= ARRAY_SIZE(caps->sizes))
            {
                break;
            }
            caps->sizes[caps->count++] = 1024 << i;
        }
    }
    return caps->count;
}

static bool blk_is_supported_block(u32 mask, u32 size)
{
    // <!!> преобразует любое ненулевое значение в 1
    switch (size)
    {
        case 1024:
            return !!(mask & 1u);
        case 2048:
            return !!(mask & 2u);
        case 4096:
            return !!(mask & 4u);
        case 8192:
            return !!(mask & 8u);
        default:
            return false;
    }
}

// проверка на валидность запросов на запись или чтение
static int blk_validate_request(struct blk_dev *d, u64 addr, u32 len, bool is_write)
{
    if (!blk_is_supported_block(d->supported_blocks, d->block_size))
        return -EINVAL;
    if (len == 0)
        return -EINVAL;
    if (addr % d->block_size)
        return -EINVAL;
    if (len % d->block_size)
        return -EINVAL;
    if (BLK_DATA_BUFFER_OFFSET + len > d->bar_len)
        return -EMSGSIZE;
    if (addr < d->storage_base)
        return -ERANGE;
    if (addr + len < addr)
        return -EOVERFLOW;
    if (addr + len > d->storage_base + d->storage_size)
        return -ERANGE;
    if (is_write && (d->device_flags & 1u))
        return -EROFS;
    return 0;
}

// ожидание выполнения записи
static int blk_wait_complete(struct blk_dev *d)
{
    unsigned long timeout_j = jiffies + msecs_to_jiffies(d->timeout_ms); //todo: дописать что это
    while(true)
    {
        u32 status = blk_read32(d, BLK_REG_STATUS);
        u32 err;

        switch (status)
        {
            case BLK_STATUS_DONE:
                err = blk_read32(d, BLK_REG_ERROR);
                blk_write32(d, BLK_REG_STATUS, BLK_STATUS_EMPTY);
                blk_write32(d, BLK_REG_CMD, BLK_CMD_NOP);
                if(err == BLK_ERR_NONE)
                {
                    return 0;
                }
                switch(err)
                {
                    case BLK_ERR_ALIGN:
                        return -EINVAL;
                    case BLK_ERR_RANGE:
                        return -ERANGE;
                    case BLK_ERR_READONLY:
                        return -EROFS;
                    case BLK_ERR_UNSUPPORTED:
                        return -EOPNOTSUPP;
                    case BLK_ERR_IO:
                    default:
                        return -EIO;
                }

                return 0;
            case BLK_STATUS_ERROR:
                return -EIO;
            default:
                break;
        }

        if(time_after(jiffies, timeout_j))
        {
            blk_write32(d, BLK_REG_ERROR, BLK_ERR_TIMEOUT);
            return -ETIMEDOUT;
        }
        usleep_range(d->poll_interval_us, d->poll_interval_us + 50);
    }
}


// используется для запуска операции устройства после того,
// как драйвер подготовил параметры запроса.
static int blk_kick_locked(struct blk_dev *d, u32 cmd)
{
    blk_write64(d, BLK_REG_CUR_ADDR_LO, d->addr);
    blk_write32(d, BLK_REG_BLOCK_SIZE, d->block_size);
    blk_write32(d, BLK_REG_DATA_LEN, d->data_len);
    blk_write32(d, BLK_REG_ERROR, BLK_ERR_NONE);
    blk_write32(d, BLK_REG_STATUS, BLK_STATUS_BUSY);
    blk_write32(d, BLK_REG_ACK, 0);
    blk_write32(d, BLK_REG_CMD, cmd);
    return blk_wait_complete(d);
}

//--------------------------------------------------

static int blk_open(struct inode *inode, struct file *pfile)
{
    struct miscdevice *m = pfile->private_data;
    struct blk_dev* d = container_of(m, struct blk_dev, miscdev);   // нужна чтобы получить указатель на структуру,
                                                                    // содержащую некоторое заданное поле, если известен адрес поля
    pfile->private_data = d;
    return 0;
}

static int blk_release(struct inode *inode, struct file *pfile)
{
    return 0; // тк нет выделения памяти - то и чистить ничего не нужно, вызывается при закрытии устройства
}

static ssize_t blk_read(struct file *pfile, char __user *buf,
        size_t len, loff_t *ppos)
{
    struct blk_dev *d = pfile->private_data;
    ssize_t res;
    void *buff;

    mutex_lock(&d->lock); // блокируем доступ к файлу на время выполнения операции

    if (len < d->data_len)
    {
        mutex_unlock(&d->lock);
        return -EINVAL;
    }

    res = blk_validate_request(d, d->addr, d->data_len, false);
    if (res)
    {
        mutex_unlock(&d->lock);
        return res;
    }

    res = blk_kick_locked(d, BLK_CMD_READ);
    if (res)
    {
        mutex_unlock(&d->lock);
        return res;
    }

    buff = kmalloc(d->data_len, GFP_KERNEL); //выделение буфера в аддресном пространстве ядра, промежуточный этап
    if (!buff)
    {
        mutex_unlock(&d->lock);
        return -ENOMEM;
    }

    // копирование данных из памяти устройства в адрес буфера данных устройства внутри BAR.
    // нельзя просто memcpy тк bar - это MMIO память устройства и такие функции позволяют читать из устройства
    memcpy_fromio(buff, d->bar + BLK_DATA_BUFFER_OFFSET, d->data_len);
    if (copy_to_user(buf, buff, d->data_len))
    {
        kfree(buff);
        mutex_unlock(&d->lock);
        return -EFAULT;
    }

    blk_write32(d, BLK_REG_ACK, BLK_ACK_DRV_READ_INPUT);
    *ppos = d->addr - d->storage_base + d->data_len; // todo:
    d->addr += d->data_len;

    kfree(buff);
    mutex_unlock(&d->lock);
    return res;
}

static ssize_t blk_write(struct file *pfile, const char __user *buf, size_t len, loff_t *ppos)
{
    struct blk_dev *d = pfile->private_data;
    ssize_t res;

    void *buff;

    mutex_lock(&d->lock); // блокируем доступ к файлу на время выполнения операции

    if (len != d->data_len)
    {
        mutex_unlock(&d->lock);
        return -EINVAL;
    }

    res = blk_validate_request(d, d->addr, d->data_len, false);
    if (res)
    {
        mutex_unlock(&d->lock);
        return res;
    }

    buff = memdup_user(buf, len);
    if (IS_ERR(buff))
    {
        buff = NULL;
        mutex_unlock(&d->lock);
        return PTR_ERR(buff);
    }

    memcpy_toio(d->bar + BLK_DATA_BUFFER_OFFSET, buff, d->data_len);
    blk_write32(d, BLK_REG_ACK, BLK_ACK_DRV_WRITE_OUTPUT);
    res = blk_kick_locked(d, BLK_CMD_WRITE);
    if(res)
    {
        kfree(buff);
        mutex_unlock(&d->lock);
        return res;
    }

    *ppos = d->addr - d->storage_base + d->data_len;
    d->addr += d->data_len;
    res = d->data_len;
    kfree(buff);
    mutex_unlock(&d->lock);
    return res;
}

static loff_t blk_llseek(struct file *pfile, loff_t off, int whence)
{
    struct blk_dev *d = pfile->private_data;
    loff_t newpos;

    mutex_lock(&d->lock);
    switch (whence)
    {
        case SEEK_SET:
            newpos = off;
            break;
        case SEEK_CUR:
            newpos = pfile->f_pos + off;
            break;
        case SEEK_END:
            newpos = d->storage_size + off;
            break;
        default:
            mutex_unlock(&d->lock);
            return -EINVAL;
    }

    if (newpos < 0 || (u64)newpos > d->storage_size)
    {
        mutex_unlock(&d->lock);
        return -EINVAL;
    }
    if ((u64)newpos % d->block_size)
    {
        mutex_unlock(&d->lock);
        return -EINVAL;
    }

    pfile->f_pos = newpos;
    d->addr = d->storage_base + newpos;
    mutex_unlock(&d->lock);
    return newpos;
}

static long blk_ioctl(struct file *pfile, unsigned int cmd, unsigned long arg)
{
    struct blk_dev *d = pfile->private_data;
    u64 var64;
    u32 var32;
    struct blk_block_capacity capacity;
    int res = 0;

    mutex_lock(&d->lock);
    switch(cmd)
    {
        case BLK_IOC_GET_ADDR:
            var64 = d->addr;
            res = copy_to_user((void __user*)arg, &var64, sizeof(var64)) ? -EFAULT : 0;
            break;
        case BLK_IOC_SET_ADDR:
            res = copy_from_user(&var64, (void __user *)arg, sizeof(var64)) ? -EINVAL : 0;
            if (!res)
            {
                if (blk_validate_request(d, var64, d->data_len, false))
                {
                    res = -EINVAL;
                }
                else
                {
                    d->addr = var64;
                    pfile->f_pos = var64 - d->storage_base;
                }
            }
            break;
        case BLK_IOC_GET_BLOK_SIZE:
            var32 = d->block_size;
            res = copy_to_user((void __user * )arg, &var32, sizeof(var32)) ? -EFAULT : 0;
            break;
        case BLK_IOC_SET_BLOCK_SIZE:
            res = copy_from_user(&var32, (void __user *)arg, sizeof(var32)) ? -EFAULT : 0;
            if (!res)
            {
                if (!blk_is_supported_block(d->supported_blocks, var32))
                {
                    res = -EINVAL;
                }
                else if (d->addr % var32 || d->data_len % var32)
                {
                    res = -EINVAL;
                }
                else
                {
                    d->block_size = var32;
                }
            }
            break;
        case BLK_IOC_GET_DATA_LEN:
            var32 = d->data_len;
            res = copy_to_user((void __user *)arg, &var32,  sizeof(var32)) ? -EFAULT : 0;
            break;
        case BLK_IOC_SET_DATA_LEN:
            res = copy_from_user(&var32, (void __user *)arg, sizeof(var32)) ? -EFAULT : 0;
            if (!res)
            {
                if (blk_validate_request(d, d->addr, var32, false))
                {
                    res = -EINVAL;
                }
                else
                {
                    d->data_len = var32;
                }
            }
            break;
        case BLK_IOC_GET_STORAGE_SIZE:
            var64 = d->storage_size;
            res = copy_to_user((void __user *)arg, &var64, sizeof(var64)) ? -EFAULT : 0;
            break;
        case BLK_IOC_GET_STORAGE_BASE:
            var64 = d->storage_base;
            res = copy_to_user((void __user *)arg, &var64, sizeof(var64)) ? -EFAULT : 0;
            break;
        case BLK_IOC_GET_BLOCK_CAPACITY:
            blk_mask_to_caps(d->supported_blocks, &capacity);
            res = copy_to_user((void __user *)arg, &capacity, sizeof(capacity)) ? -EFAULT : 0;
            break;
        case BLK_IOC_GET_STATUS:
            var32 = blk_read32(d, BLK_REG_STATUS);
            res = copy_to_user((void __user *)arg, &var32, sizeof(var32)) ? -EFAULT : 0;
            break;
        case BLK_IOC_WAIT_IDLE:
            res = blk_wait_complete(d);
            break;
        default:
            res = -ENOTTY;
            break;
    }
    mutex_unlock(&d->lock);
    return res;
}

// функции sysfs


static ssize_t addr_show(struct device *dev, struct device_attribute *attr, char *buff)
{
    struct blk_dev *d = dev_get_drvdata(dev);
    return sysfs_emit(buff, "%llu\n", d->addr);
}
static ssize_t addr_store(struct device *dev, struct device_attribute *attr, const char *buff, size_t count)
{
    struct blk_dev *d = dev_get_drvdata(dev);
    u64 var64;
    int res;

    res = kstrtou64(buff, 0, &var64);
    if (res)
    {
        return res;
    }

    mutex_lock(&d->lock);
    if (blk_validate_request(d, var64, d->data_len, false))
    {
        res = -EINVAL;
    }
    else
    {
        d->addr = var64;
        res = count;
    }
    mutex_unlock(&d->lock);
    return res;
}
static DEVICE_ATTR_RW(addr);

static ssize_t block_size_show(struct device *dev, struct device_attribute * attr, char *buff)
{
    struct blk_dev *d = dev_get_drvdata(dev);
    return sysfs_emit(buff, "%u\n", d->block_size);
}
static ssize_t block_size_store(struct device *dev, struct device_attribute * attr, const char *buff, size_t count)
{
    struct blk_dev *d = dev_get_drvdata(dev);
    u32 var32;
    int res = kstrtou32(buff, 0, &var32);

    if (res)
    {
        return res;
    }
    mutex_lock(&d->lock);
    if (!blk_is_supported_block(d->supported_blocks, var32) || d->addr % var32 || d->data_len % var32)
    {
        res = -EINVAL;
    }
    else
    {
        d->block_size = var32;
        res = count;
    }
    mutex_unlock(&d->lock);
    return res;
}
static DEVICE_ATTR_RW(block_size);

static ssize_t data_len_show(struct device *dev, struct device_attribute * attr, char *buff)
{
    struct blk_dev *d = dev_get_drvdata(dev);
    return sysfs_emit(buff, "%u\n", d->data_len);
}
static ssize_t data_len_store(struct device *dev, struct device_attribute * attr, const char *buff, size_t count)
{
    struct blk_dev *d = dev_get_drvdata(dev);
    u32 var32;
    int res = kstrtou32(buff, 0, &var32);
    if (res)
    {
        return res;
    }
    mutex_lock(&d->lock);
    if (blk_validate_request(d, d->addr, var32, false))
    {
        res = -EINVAL;
    }
    else
    {
        d->data_len = var32;
        res = count;
    }
    mutex_unlock(&d->lock);
    return res;
}
static DEVICE_ATTR_RW(data_len);

static ssize_t storage_size_show(struct device *dev, struct device_attribute * attr, char *buff)
{
    struct blk_dev *d= dev_get_drvdata(dev);
    return sysfs_emit(buff, "%llu\n", d->storage_size);
}
static DEVICE_ATTR_RO(storage_size);

static ssize_t storage_base_show(struct device* dev, struct device_attribute * attr, char *buff)
{
    struct blk_dev *d = dev_get_drvdata(dev);
    return sysfs_emit(buff, "%llu\n", d->storage_base);
}
static DEVICE_ATTR_RO(storage_base);

static ssize_t supported_block_size_show(struct device* dev, struct device_attribute * attr, char *buff)
{
    struct blk_dev *d = dev_get_drvdata(dev);
    struct blk_block_capacity capacity;
    int pos = 0;

    blk_mask_to_caps(d->supported_blocks, &capacity);
    for (int i = 0; i < capacity.count; ++i)
    {
        pos += sysfs_emit_at(buff, pos, "%u%s", capacity.sizes[i], i == capacity.count-1 ? "\n" : " ");
    }
    return pos;
}
static DEVICE_ATTR_RO(supported_block_size);

static ssize_t status_show(struct device* dev, struct device_attribute * attr, char *buff)
{
    struct blk_dev *d = dev_get_drvdata(dev);
    return sysfs_emit(buff, "%u\n", blk_read32(d, BLK_REG_STATUS));
}
static DEVICE_ATTR_RO(status);

static ssize_t timeout_ms_show(struct device* dev, struct device_attribute * attr, char *buff)
{
    struct blk_dev *d = dev_get_drvdata(dev);
    return sysfs_emit(buff, "%u\n", d->timeout_ms);
}
static ssize_t timeout_ms_store(struct device* dev, struct device_attribute * attr, const char *buff, size_t count)
{
    struct blk_dev *d = dev_get_drvdata(dev);
    u32 var32;
    int res = kstrtou32(buff, 0, &var32);
    if (res)
    {
        return res;
    }
    d->timeout_ms = var32;
    return count;
}
static DEVICE_ATTR_RW(timeout_ms);

static ssize_t poll_interval_us_show(struct device* dev, struct device_attribute * attr, char *buff)
{
    struct blk_dev *d = dev_get_drvdata(dev);
    return sysfs_emit(buff, "%u\n", d->poll_interval_us);
}
static ssize_t poll_interval_us_store(struct device* dev, struct device_attribute * attr, const char *buff, size_t count)
{
    struct blk_dev *d = dev_get_drvdata(dev);
    u32 var32;
    int res = kstrtou32(buff, 0, &var32);
    if (res)
    {
        return res;
    }
    d->poll_interval_us = var32;
    return count;
}
static DEVICE_ATTR_RW(poll_interval_us);


static struct attribute *blk_attrs[] = {
        &dev_attr_addr.attr,
        &dev_attr_block_size.attr,
        &dev_attr_data_len.attr,
        &dev_attr_storage_size.attr,
        &dev_attr_storage_base.attr,
        &dev_attr_supported_block_size.attr,
        &dev_attr_status.attr,
        &dev_attr_timeout_ms.attr,
        &dev_attr_poll_interval_us.attr,
        NULL,
};
ATTRIBUTE_GROUPS(blk);

static int blk_probe(struct pci_dev* pdev, const struct pci_device_id *id)
{
    struct blk_dev *d;
    int err = pcim_enable_device(pdev);                         // включает устройство, настраивает BAR, разрешает доступ к памяти/IO
                                                                // ресурсы автоматически освобождаются
                                                                // когда устройство удаляется, в отличие от pci_en...
                                                                // упрощается реализация remove()
    if (err)
    {
        return err;
    }

    pci_set_master(pdev);                                       // разрешает PCI устройству самостоятельно инициировать операции на PCI шине.

    d = devm_kzalloc(&pdev->dev, sizeof(*d), GFP_KERNEL);       // выделяет память и привязывает к устройству pdev
    if (!d)
    {
        return -ENOMEM;                                         // если неудалось выделить память - возвращаю out of memory
    }

    d->bar = pcim_iomap_region(pdev, BLK_BAR_NUM, DRV_NAME);
    if (IS_ERR(d->bar))
    {
        return PTR_ERR(d->bar);
    }

    d->bar_len = pci_resource_len(pdev, BLK_BAR_NUM);
    if (d->bar_len < BLK_DATA_BUFFER_OFFSET + 1024)
    {
        return -ENODEV;
    }

    d->pdev = pdev;
    mutex_init(&d->lock);
    d->timeout_ms = BLK_DEFAULT_TIMEOUT;
    d->poll_interval_us = BLK_DEFAULT_POLL_US;

    if(blk_read32(d,BLK_REG_MAGIC) != BLK_MAGIC_VALUE)
    {
        dev_warn(&pdev->dev, "enexpecetd magic value, mb uninitialized\n");
    }

    d->storage_base = blk_read64(d, BLK_REG_STORAGE_BASE_LO);
    d->storage_size = blk_read64(d, BLK_REG_STORAGE_SIZE_LO);
    d->supported_blocks = blk_read32(d, BLK_REG_SUPPORTED_BLOCKS);
    d->device_flags = blk_read32(d, BLK_REG_DEVICE_FLAGS);
    d->block_size = blk_read32(d, BLK_REG_BLOCK_SIZE);
    if (!blk_is_supported_block(d->supported_blocks, d->block_size))
    {
        d->block_size = BLK_DEFAULT_BLK_SIZE;
    }
    d->data_len = d->block_size;
    d->addr = d->storage_base;

    d->miscdev.minor = MISC_DYNAMIC_MINOR;
    d->miscdev.name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "blk_dev%d", pdev->devfn);
    if(!d->miscdev.name)
    {
        return -ENOMEM;
    }
    d->miscdev.fops = &blkdev_ops;
    d->miscdev.parent = &pdev->dev;
    d->miscdev.groups = blk_groups;

    err = misc_register(&d->miscdev);
    if (err)
    {
        return err;
    }

    dev_set_drvdata(d->miscdev.this_device, d);
    pci_set_drvdata(pdev, d);
    dev_info(&pdev->dev,"registered /dev/%s, storage=%llu bytes, default block=%u\n" , d->miscdev.name , d->storage_size, d->block_size);
    return 0;
}



static struct pci_driver blkdev = {
        .name = DRV_NAME,
        .id_table = blk_id_table,
        .probe = blk_probe,
        .remove = blk_remove
};

module_pci_driver(blkdev);          // автоматически создает module_init и module_exit

MODULE_AUTHOR("Khristoforov Mikhail, B23_513");
MODULE_DESCRIPTION("lab2, 8-th var <Block device>");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");
