#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/qdev-properties.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-bus.h"
#include "hw/virtio/virtio-pci.h"
#include "hw/virtio/virtio-comp.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qom/object.h"

typedef struct VirtIOCompressPCI VirtIOCompressPCI;

/*
 * virtio-comp-pci: This extends VirtioPCIProxy.
 */
#define TYPE_VIRTIO_COMPRESS_PCI "virtio-comp-pci"
DECLARE_INSTANCE_CHECKER(VirtIOCompressPCI, VIRTIO_COMPRESS_PCI,
                         TYPE_VIRTIO_COMPRESS_PCI)

struct VirtIOCompressPCI {
    VirtIOPCIProxy parent_obj;
    VirtIOCompress vdev;
};

static const Property virtio_compress_pci_properties[] = {
    DEFINE_PROP_BIT("ioeventfd", VirtIOPCIProxy, flags,
                    VIRTIO_PCI_FLAG_USE_IOEVENTFD_BIT, true),
    DEFINE_PROP_UINT32("vectors", VirtIOPCIProxy, nvectors, 2),
};

static void virtio_compress_pci_realize(VirtIOPCIProxy *vpci_dev, Error **errp)
{
    VirtIOCompressPCI *vcompress = VIRTIO_COMPRESS_PCI(vpci_dev);
    DeviceState *vdev = DEVICE(&vcompress->vdev);

    if (vcompress->vdev.conf.compressdev == NULL) {
        error_setg(errp, "'compressdev' parameter expects a valid object");
        return;
    }

    virtio_pci_force_virtio_1(vpci_dev);
    if (!qdev_realize(vdev, BUS(&vpci_dev->bus), errp)) {
        return;
    }
}

static void virtio_compress_pci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioPCIClass *k = VIRTIO_PCI_CLASS(klass);
    PCIDeviceClass *pcidev_k = PCI_DEVICE_CLASS(klass);

    k->realize = virtio_compress_pci_realize;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    device_class_set_props(dc, virtio_compress_pci_properties);
    pcidev_k->class_id = PCI_CLASS_OTHERS;
}

static void virtio_compress_initfn(Object *obj)
{
    VirtIOCompressPCI *dev = VIRTIO_COMPRESS_PCI(obj);

    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_VIRTIO_COMP);
}

static const VirtioPCIDeviceTypeInfo virtio_compress_pci_info = {
    .generic_name  = TYPE_VIRTIO_COMPRESS_PCI,
    .instance_size = sizeof(VirtIOCompressPCI),
    .instance_init = virtio_compress_initfn,
    .class_init    = virtio_compress_pci_class_init,
};

static void virtio_compress_pci_register_types(void)
{
    virtio_pci_types_register(&virtio_compress_pci_info);
}
type_init(virtio_compress_pci_register_types)
