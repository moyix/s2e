extern "C"
{
    #include "hw/hw.h"
    #include "hw/pci.h"
    #include "hw/isa.h"
}

#include "SymbolicHardware.h"
#include <s2e/S2E.h>
#include <s2e/ConfigFile.h>
#include <s2e/Utils.h>

#include <sstream>


namespace s2e {
namespace plugins {

struct SymbolicPciDeviceState {
    PCIDevice dev;
    PciDeviceDescriptor *desc;
};

struct SymbolicIsaDeviceState {
    ISADevice dev;
    IsaDeviceDescriptor *desc;
    qemu_irq qirq;
};


extern "C" {
    static int pci_symbhw_init(PCIDevice *pci_dev);
    static int pci_symbhw_uninit(PCIDevice *pci_dev);
    static int isa_symbhw_init(ISADevice *dev);

    static uint32_t symbhw_read8(void *opaque, uint32_t address);
    static uint32_t symbhw_read16(void *opaque, uint32_t address);
    static uint32_t symbhw_read32(void *opaque, uint32_t address);
    static void symbhw_write8(void *opaque, uint32_t address, uint32_t data);
    static void symbhw_write16(void *opaque, uint32_t address, uint32_t data);
    static void symbhw_write32(void *opaque, uint32_t address, uint32_t data);

    static void symbhw_mmio_writeb(void *opaque, target_phys_addr_t addr, uint32_t val);
    static void symbhw_mmio_writew(void *opaque, target_phys_addr_t addr, uint32_t val);
    static void symbhw_mmio_writel(void *opaque, target_phys_addr_t addr, uint32_t val);
    static uint32_t symbhw_mmio_readb(void *opaque, target_phys_addr_t addr);
    static uint32_t symbhw_mmio_readw(void *opaque, target_phys_addr_t addr);
    static uint32_t symbhw_mmio_readl(void *opaque, target_phys_addr_t addr);
}


S2E_DEFINE_PLUGIN(SymbolicHardware, "Symbolic hardware plugin for PCI/ISA devices", "SymbolicHardware",);

void SymbolicHardware::initialize()
{
    ConfigFile *cfg = s2e()->getConfig();
    std::ostream &ws = s2e()->getWarningsStream();
    bool ok;

    s2e()->getMessagesStream() << "======= Initializing Symbolic Hardware =======" << std::endl;

    ConfigFile::string_list keys = cfg->getListKeys(getConfigKey(), &ok);
    if (!ok || keys.empty()) {
        ws << "No symbolic device descriptor specified in " << getConfigKey() << "." <<
                " S2E will start without symbolic hardware." << std::endl;
        return;
    }

    foreach2(it, keys.begin(), keys.end()) {
        std::stringstream ss;
        ss << getConfigKey() << "." << *it;
        DeviceDescriptor *dd = DeviceDescriptor::create(this, cfg, ss.str());
        if (!dd) {
            ws << "Failed to create a symbolic device for " << ss.str() << std::endl;
            exit(-1);
        }

        dd->print(s2e()->getMessagesStream());
        m_devices.insert(dd);
    }

    s2e()->getCorePlugin()->onDeviceRegistration.connect(
        sigc::mem_fun(*this, &SymbolicHardware::onDeviceRegistration)
    );
}

const DeviceDescriptor *SymbolicHardware::findDevice(const std::string &name) const
{
    DeviceDescriptor dd(name);
    DeviceDescriptors::const_iterator it = m_devices.find(&dd);
    if (it != m_devices.end()) {
        return *it;
    }
    return NULL;
}

void SymbolicHardware::onDeviceRegistration()
{
    s2e()->getMessagesStream() << "Registering symbolic devices with QEMU..." << std::endl;
    foreach2(it, m_devices.begin(), m_devices.end()) {
        (*it)->initializeQemuDevice();
    }
}


SymbolicHardware::~SymbolicHardware()
{
    foreach2(it, m_devices.begin(), m_devices.end()) {
        delete *it;
    }
}

DeviceDescriptor::DeviceDescriptor(const std::string &id){
    m_id = id;
}

DeviceDescriptor::~DeviceDescriptor()
{

}

DeviceDescriptor *DeviceDescriptor::create(SymbolicHardware *plg, ConfigFile *cfg, const std::string &key)
{
    bool ok;
    std::ostream &ws = plg->s2e()->getWarningsStream();

    std::string id = cfg->getString(key + ".id", "", &ok);
    if (!ok || id.empty()) {
        ws << "You must specifiy an id for " << key << ". " <<
                "This is required by QEMU for saving/restoring snapshots." << std::endl;
        return NULL;
    }

    //Check the type of device we want to create
    std::string devType = cfg->getString(key + ".type", "", &ok);
    if (!ok || (devType != "pci" && devType != "isa")) {
        ws << "You must define either an ISA or PCI device!" << std::endl;
        return NULL;
    }

    if (devType == "isa") {
        return IsaDeviceDescriptor::create(plg, cfg, key);
    }else if (devType == "pci") {
        return PciDeviceDescriptor::create(plg, cfg, key);
    }

    return NULL;
}

/////////////////////////////////////////////////////////////////////
IsaDeviceDescriptor::IsaDeviceDescriptor(const std::string &id, const IsaResource &res):DeviceDescriptor(id) {
    m_isaResource = res;
    m_isaInfo = NULL;
}

void IsaDeviceDescriptor::initializeQemuDevice()
{
    m_isaInfo = new ISADeviceInfo();
    m_isaProperties = new Property[1];

    m_isaInfo->qdev.name = m_id.c_str();
    m_isaInfo->qdev.size = sizeof(SymbolicIsaDeviceState);
    m_isaInfo->init = isa_symbhw_init;
    m_isaInfo->qdev.props = m_isaProperties;

    isa_qdev_register(m_isaInfo);
}

IsaDeviceDescriptor::~IsaDeviceDescriptor()
{
    if (m_isaInfo) {
        delete m_isaInfo;
    }

    if (m_isaProperties) {
        delete [] m_isaProperties;
    }
}

void IsaDeviceDescriptor::print(std::ostream &os) const
{
    os << "ISA Device Descriptor id=" << m_id << std::endl;
    os << std::hex << "Base=0x" << m_isaResource.portBase <<
            " Size=0x" << m_isaResource.portSize << std::endl;
    os << std::endl;
}

IsaDeviceDescriptor* IsaDeviceDescriptor::create(SymbolicHardware *plg, ConfigFile *cfg, const std::string &key)
{
    bool ok;
    std::ostream &ws = plg->s2e()->getWarningsStream();

    std::string id = cfg->getString(key + ".id", "", &ok);
    assert(ok);

    uint64_t start = cfg->getInt(key + ".start", 0, &ok);
    if (!ok || start > 0xFFFF) {
        ws << "The base address of an ISA device must be between 0x0 and 0xffff." << std::endl;
        return NULL;
    }

    uint16_t size = cfg->getInt(key + ".size", 0, &ok);
    if (!ok) {
        return NULL;
    }

    if (start + size > 0x10000) {
        ws << "An ISA address range must not exceed 0xffff." << std::endl;
        return NULL;
    }

    uint8_t irq =  cfg->getInt(key + ".irq", 0, &ok);
    if (!ok || irq > 15) {
        ws << "You must specify an IRQ between 0 and 15 for the ISA device." << std::endl;
        return NULL;
    }

    IsaResource r;
    r.portBase = start;
    r.portSize = size;
    r.irq = irq;

    return new IsaDeviceDescriptor(id, r);
}

/////////////////////////////////////////////////////////////////////

PciDeviceDescriptor* PciDeviceDescriptor::create(SymbolicHardware *plg, ConfigFile *cfg, const std::string &key)
{
    bool ok;
    std::ostream &ws = plg->s2e()->getWarningsStream();

    std::string id = cfg->getString(key + ".id", "", &ok);
    assert(ok);

    uint16_t vid = cfg->getInt(key + ".vid", 0, &ok);
    if (!ok) {
        ws << "You must specifiy a vendor id for a symbolic PCI device!" << std::endl;
        return NULL;
    }

    uint16_t pid = cfg->getInt(key + ".pid", 0, &ok);
    if (!ok) {
        ws << "You must specifiy a product id for a symbolic PCI device!" << std::endl;
        return NULL;
    }

    uint32_t classCode = cfg->getInt(key + ".classCode", 0, &ok);
    if (!ok || classCode > 0xffffff) {
        ws << "You must specifiy a valid class code for a symbolic PCI device!" << std::endl;
        return NULL;
    }

    uint8_t revisionId = cfg->getInt(key + ".revisionId", 0, &ok);
    if (!ok) {
        ws << "You must specifiy a revision id for a symbolic PCI device!" << std::endl;
        return NULL;
    }

    uint8_t interruptPin = cfg->getInt(key + ".interruptPin", 0, &ok);
    if (!ok || interruptPin > 4) {
        ws << "You must specifiy an interrupt pin (1-4, 0 for none) for " << key << "!" << std::endl;
        return NULL;
    }

    std::vector<PciResource> resources;

    //Reading the resource list
    ConfigFile::string_list resKeys = cfg->getListKeys(key + ".resources", &ok);
    if (!ok || resKeys.empty()) {
        ws << "You must specifiy at least one resource descriptor for a symbolic PCI device!" << std::endl;
        return NULL;
    }

    foreach2(it, resKeys.begin(), resKeys.end()) {
        std::stringstream ss;
        ss << key << ".resources." << *it;

        bool isIo = cfg->getBool(ss.str() + ".isIo", false, &ok);
        if (!ok) {
            ws << "You must specify whether the resource " << ss.str() << " is IO or MMIO!" << std::endl;
            return NULL;
        }

        bool isPrefetchable = cfg->getBool(ss.str() + ".isPrefetchable", false, &ok);
        if (!ok && !isIo) {
            ws << "You must specify whether the resource " << ss.str() << " is prefetchable!" << std::endl;
            return NULL;
        }

        uint32_t size = cfg->getInt(ss.str() + ".size", 0, &ok);
        if (!ok) {
            ws << "You must specify a size for the resource " << ss.str() << "!" << std::endl;
            return NULL;
        }

        PciResource res;
        res.isIo = isIo;
        res.prefetchable = isPrefetchable;
        res.size = size;
        resources.push_back(res);
    }

    if (resources.size() > 6) {
        ws << "A PCI device can have at most 6 resource descriptors!" << std::endl;
        return NULL;
    }

    PciDeviceDescriptor *ret = new PciDeviceDescriptor(id);
    ret->m_classCode = classCode;
    ret->m_pid = pid;
    ret->m_vid = vid;
    ret->m_revisionId = revisionId;
    ret->m_interruptPin = interruptPin;
    ret->m_resources = resources;

    return ret;
}


void PciDeviceDescriptor::initializeQemuDevice()
{
    m_pciInfo = new PCIDeviceInfo();
    m_pciInfo->qdev.name = m_id.c_str();
    m_pciInfo->qdev.size = sizeof(SymbolicPciDeviceState);
    m_pciInfo->qdev.vmsd = m_vmState;
    m_pciInfo->init = pci_symbhw_init;
    m_pciInfo->exit = pci_symbhw_uninit;

    m_pciInfoProperties = new Property[1];
    memset(m_pciInfoProperties, 0, sizeof(Property));

    m_pciInfo->qdev.props = m_pciInfoProperties;

    m_vmState = new VMStateDescription();
    m_vmState->name = m_id.c_str();
    m_vmState->version_id = 3,
    m_vmState->minimum_version_id = 3,
    m_vmState->minimum_version_id_old = 3,

    m_vmStateFields = new VMStateField[2];
    memset(m_vmStateFields, 0, sizeof(m_vmStateFields)*2);
    m_vmStateFields[0].name = "dev";
    m_vmStateFields[0].size = sizeof(PCIDevice);
    m_vmStateFields[0].vmsd = &vmstate_pci_device;
    m_vmStateFields[0].flags = VMS_STRUCT;
    m_vmStateFields[0].offset = vmstate_offset_value(SymbolicPciDeviceState, dev, PCIDevice);

    pci_qdev_register(m_pciInfo);
}

PciDeviceDescriptor::PciDeviceDescriptor(const std::string &id):DeviceDescriptor(id)
{
    m_pciInfo = NULL;
    m_pciInfoProperties = NULL;
    m_vmState = NULL;
}

PciDeviceDescriptor::~PciDeviceDescriptor()
{
    if (m_pciInfo) delete m_pciInfo;
    if (m_pciInfoProperties) delete [] m_pciInfoProperties;
    if (m_vmState) delete m_vmState;
}

void PciDeviceDescriptor::print(std::ostream &os) const
{
    os << "PCI Device Descriptor id=" << m_id << std::endl;
    os << std::hex << "VID=0x" << m_vid <<
            " PID=0x" << m_pid <<
            " RevID=0x" << (unsigned)m_revisionId << std::endl;

    os << "Class=0x" << (unsigned)m_classCode <<
            " INT=0x" << (unsigned)m_interruptPin << std::endl;

    unsigned i=0;
    foreach2(it, m_resources.begin(), m_resources.end()) {
        const PciResource &res = *it;
        os << "R[" << i << "]: " <<
                "Size=0x" << res.size << " IsIO=" << (int)res.isIo <<
                " IsPrefetchable=0x" << (int)res.prefetchable << std::endl;
    }
    os << std::endl;
}

/////////////////////////////////////////////////////////////////////
/* Dummy I/O functions for symbolic devices. Unused for now. */
static void symbhw_write8(void *opaque, uint32_t address, uint32_t data) {
    g_s2e->getDebugStream() << __FUNCTION__ << std::hex << " 0x" << address << " 0x" << data << std::endl;
}

static void symbhw_write16(void *opaque, uint32_t address, uint32_t data) {
    g_s2e->getDebugStream() << __FUNCTION__ << std::hex << " 0x" << address << " 0x" << data << std::endl;
}

static void symbhw_write32(void *opaque, uint32_t address, uint32_t data) {
    g_s2e->getDebugStream() << __FUNCTION__ << std::hex << " 0x" << address << " 0x" << data << std::endl;
}

/* These will never be called */
static uint32_t symbhw_read8(void *opaque, uint32_t address)
{
    g_s2e->getDebugStream() << __FUNCTION__ << std::hex << " 0x" << address << std::endl;
    return 0;
}

static uint32_t symbhw_read16(void *opaque, uint32_t address)
{
    g_s2e->getDebugStream() << __FUNCTION__ << std::hex << " 0x" << address << std::endl;
    return 0;
}

static uint32_t symbhw_read32(void *opaque, uint32_t address)
{
    g_s2e->getDebugStream() << __FUNCTION__ << std::hex << " 0x" << address << std::endl;
    return 0;
}

static void symbhw_mmio_writeb(void *opaque, target_phys_addr_t addr, uint32_t val)
{
    g_s2e->getDebugStream() << __FUNCTION__ << std::hex << " 0x" << addr << " 0x" << val << std::endl;
}

static void symbhw_mmio_writew(void *opaque, target_phys_addr_t addr, uint32_t val)
{
    g_s2e->getDebugStream() << __FUNCTION__ << std::hex << " 0x" << addr << " 0x" << val << std::endl;
}

static void symbhw_mmio_writel(void *opaque, target_phys_addr_t addr, uint32_t val)
{
    g_s2e->getDebugStream() << __FUNCTION__ << std::hex << " 0x" << addr << " 0x" << val << std::endl;
}

static uint32_t symbhw_mmio_readb(void *opaque, target_phys_addr_t addr)
{
    g_s2e->getDebugStream() << __FUNCTION__ << std::hex << " 0x" << addr << std::endl;
    return 0;
}

static uint32_t symbhw_mmio_readw(void *opaque, target_phys_addr_t addr)
{
    g_s2e->getDebugStream() << __FUNCTION__ << std::hex << " 0x" << addr << std::endl;
    return 0;
}

static uint32_t symbhw_mmio_readl(void *opaque, target_phys_addr_t addr)
{
    g_s2e->getDebugStream() << __FUNCTION__ << std::hex << " 0x" << addr << std::endl;
    return 0;
}

static CPUReadMemoryFunc * const symbhw_mmio_read[3] = {
    symbhw_mmio_readb,
    symbhw_mmio_readw,
    symbhw_mmio_readl,
};

static CPUWriteMemoryFunc * const symbhw_mmio_write[3] = {
    symbhw_mmio_writeb,
    symbhw_mmio_writew,
    symbhw_mmio_writel,
};

/////////////////////////////////////////////////////////////////////

static void pci_symbhw_map(PCIDevice *pci_dev, int region_num,
                       pcibus_t addr, pcibus_t size, int type)
{
    SymbolicPciDeviceState *s = DO_UPCAST(SymbolicPciDeviceState, dev, pci_dev);

    if (type & PCI_BASE_ADDRESS_SPACE_IO) {
        register_ioport_write(addr, size, 1, symbhw_write8, s);
        register_ioport_read(addr, size, 1, symbhw_read8, s);

        register_ioport_write(addr, size, 2, symbhw_write16, s);
        register_ioport_read(addr, size, 2, symbhw_read16, s);

        register_ioport_write(addr, size, 4, symbhw_write32, s);
        register_ioport_read(addr, size, 4, symbhw_read32, s);
    }

    if (type & PCI_BASE_ADDRESS_SPACE_MEMORY) {
        cpu_register_physical_memory(addr, size, s->desc->mmio_io_addr);
    }
}


static int isa_symbhw_init(ISADevice *dev)
{
    g_s2e->getDebugStream() << __FUNCTION__ << " called" << std::endl;

    SymbolicIsaDeviceState *isa = DO_UPCAST(SymbolicIsaDeviceState, dev, dev);
    IsaDeviceDescriptor *s = isa->desc;

    uint32_t size = s->getResource().portSize;
    uint32_t addr = s->getResource().portBase;
    uint32_t irq = s->getResource().irq;

    register_ioport_write(addr, size, 1, symbhw_write8, s);
    register_ioport_read(addr, size, 1, symbhw_read8, s);

    register_ioport_write(addr, size, 2, symbhw_write16, s);
    register_ioport_read(addr, size, 2, symbhw_read16, s);

    register_ioport_write(addr, size, 4, symbhw_write32, s);
    register_ioport_read(addr, size, 4, symbhw_read32, s);

    isa_init_irq(dev, &isa->qirq, irq);

    return 0;
}


static int pci_symbhw_init(PCIDevice *pci_dev)
{
    SymbolicPciDeviceState *d = DO_UPCAST(SymbolicPciDeviceState, dev, pci_dev);
    uint8_t *pci_conf;

    //Retrive the configuration
    SymbolicHardware *hw = (SymbolicHardware*)g_s2e->getPlugin("SymbolicHardware");
    assert(hw);

    PciDeviceDescriptor *dd = (PciDeviceDescriptor*)hw->findDevice(pci_dev->name);
    assert(dd);

    d->desc = dd;

    pci_conf = d->dev.config;
    pci_config_set_vendor_id(pci_conf, dd->getVid());
    pci_config_set_device_id(pci_conf, dd->getPid());
    pci_config_set_class(pci_conf, dd->getClassCode());
    pci_conf[PCI_HEADER_TYPE] = PCI_HEADER_TYPE_NORMAL; // header_type
    pci_conf[0x3d] = dd->getInterruptPin(); // interrupt pin 0

    const PciDeviceDescriptor::PciResources &resources =
            dd->getResources();

    unsigned i=0;
    foreach2(it, resources.begin(), resources.end()) {
        const PciDeviceDescriptor::PciResource &res = *it;
        int type = 0;

        type |= res.isIo ? PCI_BASE_ADDRESS_SPACE_IO : PCI_BASE_ADDRESS_SPACE_MEMORY;
        type |= res.prefetchable ? PCI_BASE_ADDRESS_MEM_PREFETCH : 0;

        pci_register_bar(&d->dev, i, res.size,
                                   type, pci_symbhw_map);

        ++i;
    }

    /* I/O handler for memory-mapped I/O */
    dd->mmio_io_addr =
    cpu_register_io_memory(symbhw_mmio_read, symbhw_mmio_write, d);


    return 0;
}

static int pci_symbhw_uninit(PCIDevice *pci_dev)
{
    SymbolicPciDeviceState *d = DO_UPCAST(SymbolicPciDeviceState, dev, pci_dev);

    cpu_unregister_io_memory(d->desc->mmio_io_addr);
    return 0;
}


} // namespace plugins
} // namespace s2e