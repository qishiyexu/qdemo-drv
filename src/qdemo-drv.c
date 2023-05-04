#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/ioctl.h>
#include <linux/sched.h>
#include <linux/wait.h>


#define DRV_NAME 		"qdemo"
#define DRV_VERSION		"0.1"
#define DOMAIN          "[qdemo]"


static dev_t g_qdemo_devno;
static int  g_qdemo_major;
static struct class *g_qdemo_class;

#define PCI_VENDOR_ID_QEMU              0x1234
#define QDEMO_DEVICE_ID                 0x12e8

struct qdemo_private {
    struct pci_dev      *dev;
    struct cdev         cdev;
    int                 minor;

    u8                  revision;
    u32                 ivposition;

    u8 __iomem          *base_addr;
    u8 __iomem          *regs_addr;

    resource_size_t          bar0_addr;
    unsigned int             bar0_len;
    resource_size_t          bar1_addr;
    unsigned int             bar1_len;
    resource_size_t          bar2_addr;
    unsigned int             bar2_len;

    char                (*msix_names)[256];
    struct msix_entry   *msix_entries;
    int                 nvectors;
};

static struct qdemo_private* g_qdemo_dev;
static int g_max_devices = 1;

static struct pci_device_id qdemo_id_table[] = {
    { PCI_VENDOR_ID_QEMU, QDEMO_DEVICE_ID, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},
    { 0 },
};

static int ivpci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    int ret;
    struct ivpci_private *ivpci_dev;
    dev_t devno;

    dev_info(&pdev->dev, PFX "probing for device: %s\n", pci_name(pdev));

    if (g_ivpci_count >= g_max_devices) {
        dev_err(&pdev->dev, PFX "reach the maxinum number of devices, " \
                "please adapt the `g_max_devices` value, reload the driver\n");
        ret = -1;
        goto out;
    }

    // Active pci device by setting some registers.
    ret = pci_enable_device(pdev);
    if (ret < 0) {
        dev_err(&pdev->dev, PFX "unable to enable device: %d\n", ret);
        goto out;
    }

    /* Reserved PCI I/O and memory resources for this device */
    ret = pci_request_regions(pdev, DRV_NAME);
    if (ret < 0) {
        dev_err(&pdev->dev, PFX "unable to reserve resources: %d\n", ret);
        goto disable_device;
    }

    ivpci_dev = ivpci_get_private();
    BUG_ON(ivpci_dev == NULL);

    // Get revision id
    pci_read_config_byte(pdev, PCI_REVISION_ID, &ivpci_dev->revision);

    dev_info(&pdev->dev, PFX "device %d:%d, revision: %d\n", g_ivpci_major,
            ivpci_dev->minor, ivpci_dev->revision);

    /* Pysical address of BAR0, BAR1, BAR2 */
    ivpci_dev->bar0_addr = pci_resource_start(pdev, 0);
    ivpci_dev->bar0_len = pci_resource_len(pdev, 0);
    ivpci_dev->bar1_addr = pci_resource_start(pdev, 1);
    ivpci_dev->bar1_len = pci_resource_len(pdev, 1);
    ivpci_dev->bar2_addr = pci_resource_start(pdev, 2);
    ivpci_dev->bar2_len = pci_resource_len(pdev, 2);

    // TODO:: print with 32 bit/64 bit?
    dev_info(&pdev->dev, PFX "BAR0: 0x%0llx, %d\n", ivpci_dev->bar0_addr,
            ivpci_dev->bar0_len);
    dev_info(&pdev->dev, PFX "BAR1: 0x%0llx, %d\n", ivpci_dev->bar1_addr,
            ivpci_dev->bar1_len);
    dev_info(&pdev->dev, PFX "BAR2: 0x%0llx, %d\n", ivpci_dev->bar2_addr,
            ivpci_dev->bar2_len);

    // Map bar0(register) from physical address to virtual address.
    ivpci_dev->regs_addr = ioremap(ivpci_dev->bar0_addr, ivpci_dev->bar0_len);
    if (!ivpci_dev->regs_addr) {
        dev_err(&pdev->dev, PFX "unable to ioremap bar0, size: %d\n",
                ivpci_dev->bar0_len);
        goto release_regions;
    }

    // Bar2 is the share memory address
    ivpci_dev->base_addr = ioremap(ivpci_dev->bar2_addr, ivpci_dev->bar2_len);
    if (!ivpci_dev->base_addr) {
        dev_err(&pdev->dev, PFX "unable to ioremap bar2, size: %d\n",
                ivpci_dev->bar2_len);
        goto iounmap_bar0;
    }
    dev_info(&pdev->dev, PFX "BAR2 map: %p\n", ivpci_dev->base_addr);

    /*
     * Create character device file.
     */
    cdev_init(&ivpci_dev->cdev, &ivpci_ops);
    ivpci_dev->cdev.owner = THIS_MODULE;

    devno = MKDEV(g_ivpci_major, ivpci_dev->minor);
    ret = cdev_add(&ivpci_dev->cdev, devno, 1);
    if (ret < 0) {
        dev_err(&pdev->dev, PFX "unable to add chrdev %d:%d to system: %d\n",
                g_ivpci_major, ivpci_dev->minor, ret);
        goto iounmap_bar2;
    }

    if (device_create(g_ivpci_class, NULL, devno, NULL, DRV_FILE_FMT,
                ivpci_dev->minor) == NULL)
    {
        dev_err(&pdev->dev, PFX "unable to create device file: %d:%d\n",
                g_ivpci_major, ivpci_dev->minor);
        goto delete_chrdev;
    }

    ivpci_dev->dev = pdev;
    pci_set_drvdata(pdev, ivpci_dev);

    if (ivpci_dev->revision == 1) {
        /* Only process the MSI-X interrupt. */
        ivpci_dev->ivposition = ioread32(ivpci_dev->regs_addr + IVPOSITION_OFF);

        dev_info(&pdev->dev, PFX "device ivposition: %u, MSI-X: %s\n",
                ivpci_dev->ivposition,
                (ivpci_dev->ivposition == 0) ? "no": "yes");

        if (ivpci_dev->ivposition != 0) {
            ret = ivpci_request_msix_vectors(ivpci_dev, 4);
            if (ret != 0) {
                goto destroy_device;
            }
        }
    }

    g_ivpci_count++;
    dev_info(&pdev->dev, PFX "device probed: %s\n", pci_name(pdev));
    return 0;

destroy_device:
    devno = MKDEV(g_ivpci_major, ivpci_dev->minor);
    device_destroy(g_ivpci_class, devno);
    ivpci_dev->dev = NULL;

delete_chrdev:
    cdev_del(&ivpci_dev->cdev);

iounmap_bar2:
    iounmap(ivpci_dev->base_addr);

iounmap_bar0:
    iounmap(ivpci_dev->regs_addr);

release_regions:
    pci_release_regions(pdev);

disable_device:
    pci_disable_device(pdev);

out:
    pci_set_drvdata(pdev, NULL);
    return ret;
}


static void ivpci_remove(struct pci_dev *pdev)
{
    int devno;
    struct ivpci_private *ivpci_dev;

    dev_info(&pdev->dev, PFX "removing ivshmem device: %s\n", pci_name(pdev));

    ivpci_dev = pci_get_drvdata(pdev);
    BUG_ON(ivpci_dev == NULL);

    ivpci_free_msix_vectors(ivpci_dev);

    ivpci_dev->dev = NULL;

    devno = MKDEV(g_ivpci_major, ivpci_dev->minor);
    device_destroy(g_ivpci_class, devno);

    cdev_del(&ivpci_dev->cdev);

    iounmap(ivpci_dev->base_addr);
    iounmap(ivpci_dev->regs_addr);

    pci_release_regions(pdev);
    pci_disable_device(pdev);
    pci_set_drvdata(pdev, NULL);
}

static struct pci_driver qdemo_driver = {
    .name       = DRV_NAME,
    .id_table   = qdemo_id_table,
    .probe      = qdemo_probe,
    .remove     = qdemo_remove,
};

static int __init qdemo_init(void)
{
    int ret, i, minor;

    pr_info( DOMAIN "qdemo init\n");
    ret = alloc_chrdev_region(&g_qdemo_devno, 0, g_max_devices, DRV_NAME);
    if (ret < 0) {
        pr_err(PFX "unable to allocate major number: %d\n", ret);
        goto out;
    }

    g_qdemo_dev = kzalloc(sizeof(struct g_qdemo_dev),
            GFP_KERNEL);
    if (g_qdemo_dev == NULL) {
        goto unregister_chrdev;
    }

    g_qdemo_dev->minor = MINOR(g_qdemo_devno);

    g_qdemo_class = class_create(THIS_MODULE, DRV_NAME);
    if (g_qdemo_class == NULL) {
        pr_err(PFX "unable to create the struct class\n");
        goto free_dev;
    }

    g_qdemo_major = MAJOR(g_qdemo_devno);
    pr_info(PFX "major: %d, minor: %d\n", g_qdemo_major, MINOR(g_qdemo_devno));

    ret = pci_register_driver(&qdemo_driver);
    if (ret < 0) {
        pr_err(PFX "unable to register driver: %d\n", ret);
        goto destroy_class;
    }

    return 0;

destroy_class:
    class_destroy(g_qdemo_class);

free_dev:
    kfree(g_qdemo_dev);

unregister_chrdev:
    unregister_chrdev_region(g_ivpci_devno, g_max_devices);

out:
    return -1;
}


static void __exit qdemo_exit(void)
{

    pci_unregister_driver(&qdemo_driver);
    class_destroy(g_qdemo_class);
    kfree(g_qdemo_dev);
    unregister_chrdev_region(g_qdemo_devno, 1);

    pr_info(DOMAIN "qdemo exit\n");
}


module_init(hello_3_init);
module_exit(hello_3_exit);


MODULE_AUTHOR("qishiyexu2@126.com");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("qdemo driver");
MODULE_VERSION(DRV_VERSION);