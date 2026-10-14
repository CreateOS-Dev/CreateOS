// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "drivers/nvme.h"
#include "drivers/pci.h"
#include "memory/pmm.h"
#include <stdint.h>

#define NVME_CLASS 0x01
#define NVME_SUBCLASS 0x08
#define NVME_PROGIF 0x02
#define NVME_CAP 0x00
#define NVME_VS 0x08
#define NVME_CC 0x14
#define NVME_CSTS 0x1C
#define NVME_AQA 0x24
#define NVME_ASQ 0x28
#define NVME_ACQ 0x30
#define NVME_CC_EN (1U << 0)
#define NVME_CSTS_RDY (1U << 0)
#define NVME_CSTS_FATAL (1U << 1)
#define NVME_ADMIN_ENTRIES 16
#define NVME_ADMIN_IDENTIFY 0x06
#define NVME_ADMIN_IDENTIFY_CONTROLLER 1
#define NVME_ADMIN_IDENTIFY_NAMESPACE 0
#define NVME_ADMIN_CREATE_SQ 0x01
#define NVME_ADMIN_CREATE_CQ 0x05
#define NVME_IO_READ 0x02
#define NVME_IO_WRITE 0x01
#define NVME_DB_BASE 0x1000
#define NVME_IO_ENTRIES 16

typedef struct {
    uint32_t opcode;
    uint32_t nsid;
    uint64_t reserved2;
    uint64_t metadata;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} nvme_command_t;

typedef struct {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t command_id;
    uint16_t status;
} nvme_completion_t;

static volatile uint8_t* controller;
static nvme_command_t admin_submission[NVME_ADMIN_ENTRIES]
    __attribute__((aligned(4096)));
static nvme_completion_t admin_completion[NVME_ADMIN_ENTRIES]
    __attribute__((aligned(4096)));
static int initialized;
static uint32_t namespace_count;
static char controller_serial[21];
static char controller_model[41];
static uint32_t doorbell_stride;
static uint16_t admin_tail;
static uint16_t admin_head;
static uint8_t admin_phase;
static uint16_t next_command_id;
static uint8_t identify_data[4096] __attribute__((aligned(4096)));
static nvme_command_t io_submission[NVME_IO_ENTRIES]
    __attribute__((aligned(4096)));
static nvme_completion_t io_completion[NVME_IO_ENTRIES]
    __attribute__((aligned(4096)));
static uint32_t namespace_block_size;
static uint64_t namespace_block_count;
static uint16_t io_tail;
static uint16_t io_head;
static uint8_t io_phase;
static uint16_t io_command_id;

static uint32_t nvme_read32(uint32_t offset)
{
    return *(volatile uint32_t*)(controller + offset);
}

static void nvme_write32(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t*)(controller + offset) = value;
}

static uint64_t nvme_read64(uint32_t offset)
{
    uint32_t low = nvme_read32(offset);
    uint32_t high = nvme_read32(offset + 4);
    return ((uint64_t)high << 32) | low;
}

static int nvme_wait_ready(int ready)
{
    for (uint32_t i = 0; i < 1000000; i++) {
        uint32_t status = nvme_read32(NVME_CSTS);
        if (status & NVME_CSTS_FATAL) return 0;
        if (((status & NVME_CSTS_RDY) != 0) == (ready != 0)) return 1;
    }
    return 0;
}

static void nvme_copy_field(char* out, uint32_t out_size,
                            const uint8_t* source, uint32_t source_size)
{
    uint32_t length = source_size < out_size - 1 ? source_size : out_size - 1;
    while (length > 0 && source[length - 1] == ' ') length--;
    for (uint32_t i = 0; i < length; i++) out[i] = (char)source[i];
    out[length] = 0;
}

static int nvme_admin_identify(uint32_t namespace_id)
{
    nvme_command_t* command = &admin_submission[admin_tail];
    uint32_t submission_doorbell = NVME_DB_BASE;
    uint32_t completion_doorbell = NVME_DB_BASE + (4U << doorbell_stride);
    uint16_t command_id = next_command_id++;

    for (uint32_t i = 0; i < sizeof(identify_data); i++) identify_data[i] = 0;
    for (uint32_t i = 0; i < sizeof(*command); i++)
        ((volatile uint8_t*)command)[i] = 0;
    command->opcode = NVME_ADMIN_IDENTIFY | ((uint32_t)command_id << 16);
    command->nsid = namespace_id;
    command->prp1 = (uint64_t)(uintptr_t)identify_data;
    command->cdw10 = namespace_id ? NVME_ADMIN_IDENTIFY_NAMESPACE :
                     NVME_ADMIN_IDENTIFY_CONTROLLER;

    admin_tail = (uint16_t)((admin_tail + 1) % NVME_ADMIN_ENTRIES);
    nvme_write32(submission_doorbell, admin_tail);
    for (uint32_t i = 0; i < 1000000; i++) {
        nvme_completion_t* completion = &admin_completion[admin_head];
        uint16_t status = completion->status;
        uint8_t phase = (uint8_t)(status & 1U);
        if (phase != admin_phase) continue;
        if (completion->command_id != command_id) continue;
        admin_head = (uint16_t)((admin_head + 1) % NVME_ADMIN_ENTRIES);
        if (admin_head == 0) admin_phase ^= 1;
        nvme_write32(completion_doorbell, admin_head);
        if (status & 0xFFFEU) return 0;
        if (!namespace_id) {
            namespace_count = *(uint32_t*)(identify_data + 516);
            nvme_copy_field(controller_serial, sizeof(controller_serial),
                            identify_data + 4, 20);
            nvme_copy_field(controller_model, sizeof(controller_model),
                            identify_data + 24, 40);
        }
        if (namespace_id) {
            uint8_t formatted = identify_data[26];
            uint32_t format_offset = 128U + (uint32_t)(formatted & 0x0F) * 4U;
            uint8_t exponent = identify_data[format_offset + 2];
            namespace_block_size = 1U << exponent;
            namespace_block_count = *(uint64_t*)identify_data;
        }
        return 1;
    }
    return 0;
}

static int nvme_admin_queue_command(uint8_t opcode, uint32_t cdw10,
                                    uint32_t cdw11, uint64_t prp1)
{
    nvme_command_t* command = &admin_submission[admin_tail];
    uint32_t submission_doorbell = NVME_DB_BASE;
    uint32_t completion_doorbell = NVME_DB_BASE + (4U << doorbell_stride);
    uint16_t command_id = next_command_id++;

    for (uint32_t i = 0; i < sizeof(*command); i++)
        ((volatile uint8_t*)command)[i] = 0;
    command->opcode = opcode | ((uint32_t)command_id << 16);
    command->prp1 = prp1;
    command->cdw10 = cdw10;
    command->cdw11 = cdw11;
    admin_tail = (uint16_t)((admin_tail + 1) % NVME_ADMIN_ENTRIES);
    nvme_write32(submission_doorbell, admin_tail);
    for (uint32_t i = 0; i < 1000000; i++) {
        nvme_completion_t* completion = &admin_completion[admin_head];
        uint16_t status = completion->status;
        if ((status & 1U) != admin_phase || completion->command_id != command_id)
            continue;
        admin_head = (uint16_t)((admin_head + 1) % NVME_ADMIN_ENTRIES);
        if (admin_head == 0) admin_phase ^= 1;
        nvme_write32(completion_doorbell, admin_head);
        return (status & 0xFFFEU) == 0;
    }
    return 0;
}

static int nvme_create_io_queue(void)
{
    for (uint32_t i = 0; i < sizeof(io_submission); i++)
        ((volatile uint8_t*)io_submission)[i] = 0;
    for (uint32_t i = 0; i < sizeof(io_completion); i++)
        ((volatile uint8_t*)io_completion)[i] = 0;
    if (!nvme_admin_queue_command(NVME_ADMIN_CREATE_CQ,
            (NVME_IO_ENTRIES - 1) | (1U << 16), 1,
            (uint64_t)(uintptr_t)io_completion))
        return 0;
    if (!nvme_admin_queue_command(NVME_ADMIN_CREATE_SQ,
            (NVME_IO_ENTRIES - 1) | (1U << 16), 1 | (1U << 16),
            (uint64_t)(uintptr_t)io_submission))
        return 0;
    io_tail = 0;
    io_head = 0;
    io_phase = 1;
    io_command_id = 1;
    return 1;
}

static int nvme_io_block(uint8_t opcode, uint64_t lba, void* buffer)
{
    nvme_command_t* command;
    uint32_t sq_doorbell = NVME_DB_BASE + (2U * 4U << doorbell_stride);
    uint32_t cq_doorbell = NVME_DB_BASE + (3U * 4U << doorbell_stride);
    uint16_t command_id = io_command_id++;
    if (!initialized || namespace_block_size != 512 ||
        lba >= namespace_block_count || !buffer ||
        ((uintptr_t)buffer & 0xFFFU)) return 0;
    command = &io_submission[io_tail];
    for (uint32_t i = 0; i < sizeof(*command); i++)
        ((volatile uint8_t*)command)[i] = 0;
    command->opcode = opcode | ((uint32_t)command_id << 16);
    command->nsid = 1;
    command->prp1 = (uint64_t)(uintptr_t)buffer;
    command->cdw10 = (uint32_t)lba;
    command->cdw11 = (uint32_t)(lba >> 32);
    io_tail = (uint16_t)((io_tail + 1) % NVME_IO_ENTRIES);
    nvme_write32(sq_doorbell, io_tail);
    for (uint32_t i = 0; i < 1000000; i++) {
        nvme_completion_t* completion = &io_completion[io_head];
        uint16_t status = completion->status;
        if ((status & 1U) != io_phase || completion->command_id != command_id)
            continue;
        io_head = (uint16_t)((io_head + 1) % NVME_IO_ENTRIES);
        if (io_head == 0) io_phase ^= 1;
        nvme_write32(cq_doorbell, io_head);
        return (status & 0xFFFEU) == 0;
    }
    return 0;
}

int nvme_init(void)
{
    pci_device_t device;
    const pci_bar_t* bar;
    uint64_t capability;
    uint32_t max_queue_entries;

    initialized = 0;
    namespace_count = 0;
    controller_serial[0] = 0;
    controller_model[0] = 0;
    namespace_block_size = 0;
    namespace_block_count = 0;
    if (!pci_find_class(NVME_CLASS, NVME_SUBCLASS, &device) ||
        device.prog_if != NVME_PROGIF || !pci_enable_device(&device, 1))
        return 0;
    bar = pci_bar_info(&device, 0);
    if (!bar || !bar->present || bar->type != 0 || !bar->base ||
        (bar->base >> 32) != 0) return 0;
    controller = (volatile uint8_t*)(uintptr_t)bar->base;
    capability = nvme_read64(NVME_CAP);
    max_queue_entries = (uint32_t)(capability & 0xFFFFU) + 1;
    if (max_queue_entries < NVME_ADMIN_ENTRIES) return 0;
    doorbell_stride = 4U << ((capability >> 32) & 0x0FU);
    nvme_write32(NVME_CC, nvme_read32(NVME_CC) & ~NVME_CC_EN);
    if (!nvme_wait_ready(0)) return 0;
    for (uint32_t i = 0; i < sizeof(admin_submission); i++)
        ((volatile uint8_t*)admin_submission)[i] = 0;
    for (uint32_t i = 0; i < sizeof(admin_completion); i++)
        ((volatile uint8_t*)admin_completion)[i] = 0;
    nvme_write32(NVME_AQA, ((NVME_ADMIN_ENTRIES - 1) << 16) |
                 (NVME_ADMIN_ENTRIES - 1));
    nvme_write32(NVME_ASQ, (uint32_t)(uintptr_t)admin_submission);
    nvme_write32(NVME_ASQ + 4, 0);
    nvme_write32(NVME_ACQ, (uint32_t)(uintptr_t)admin_completion);
    nvme_write32(NVME_ACQ + 4, 0);
    nvme_write32(NVME_CC, (6U << 20) | (4U << 16) | (0U << 4) | NVME_CC_EN);
    if (!nvme_wait_ready(1)) return 0;
    admin_tail = 0;
    admin_head = 0;
    admin_phase = 1;
    next_command_id = 1;
    if (!nvme_admin_identify(0)) return 0;
    if (namespace_count > 0 && !nvme_admin_identify(1)) return 0;
    if (namespace_count > 0 && !nvme_create_io_queue()) return 0;
    initialized = 1;
    return 1;
}

int nvme_available(void) { return initialized; }
uint32_t nvme_namespace_count(void) { return namespace_count; }
const char* nvme_serial(void) { return controller_serial; }
const char* nvme_model(void) { return controller_model; }
uint32_t nvme_namespace_block_size(void) { return namespace_block_size; }
uint64_t nvme_namespace_block_count(void) { return namespace_block_count; }
int nvme_read_block(uint64_t lba, void* buffer)
{
    return nvme_io_block(NVME_IO_READ, lba, buffer);
}
int nvme_write_block(uint64_t lba, const void* buffer)
{
    return nvme_io_block(NVME_IO_WRITE, lba, (void*)buffer);
}

int nvme_flush(void)
{
    nvme_command_t* command;
    uint32_t sq_doorbell = NVME_DB_BASE + (2U * 4U << doorbell_stride);
    uint32_t cq_doorbell = NVME_DB_BASE + (3U * 4U << doorbell_stride);
    uint16_t command_id;
    if (!initialized || !namespace_count) return 0;
    command_id = io_command_id++;
    command = &io_submission[io_tail];
    for (uint32_t i = 0; i < sizeof(*command); i++)
        ((volatile uint8_t*)command)[i] = 0;
    command->opcode = (uint32_t)command_id << 16;
    command->nsid = 1;
    io_tail = (uint16_t)((io_tail + 1) % NVME_IO_ENTRIES);
    nvme_write32(sq_doorbell, io_tail);
    for (uint32_t i = 0; i < 1000000; i++) {
        nvme_completion_t* completion = &io_completion[io_head];
        uint16_t status = completion->status;
        if ((status & 1U) != io_phase || completion->command_id != command_id)
            continue;
        io_head = (uint16_t)((io_head + 1) % NVME_IO_ENTRIES);
        if (io_head == 0) io_phase ^= 1;
        nvme_write32(cq_doorbell, io_head);
        return (status & 0xFFFEU) == 0;
    }
    return 0;
}