// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "drivers/audio.h"
#include "drivers/pci.h"
#include "arch/x86_64/io.h"

#define PCI_CLASS_MULTIMEDIA 0x04
#define PCI_SUBCLASS_AUDIO    0x01
#define AC97_PROGIF           0x00
#define AC97_MIXER_RESET      0x00
#define AC97_GLOBAL_STATUS    0x30
#define AC97_CODEC_READY      0x00000001U

static int audio_present;
static int ac97_ready;
static uint16_t ac97_mixer;
static uint16_t ac97_bus_master;

static int ac97_init(const pci_device_t* device)
{
    const pci_bar_t* mixer_bar;
    const pci_bar_t* bus_master_bar;
    if (!device || device->prog_if != AC97_PROGIF) return 0;
    mixer_bar = pci_bar_info(device, 0);
    bus_master_bar = pci_bar_info(device, 1);
    if (!mixer_bar || !bus_master_bar || !mixer_bar->present ||
        !bus_master_bar->present || mixer_bar->type != 1 ||
        bus_master_bar->type != 1 || mixer_bar->base > 0xFFFF ||
        bus_master_bar->base > 0xFFFF)
        return 0;
    ac97_mixer = (uint16_t)mixer_bar->base;
    ac97_bus_master = (uint16_t)bus_master_bar->base;
    outw(ac97_mixer + AC97_MIXER_RESET, 0);
    for (uint32_t i = 0; i < 100000; i++) {
        if (inl(ac97_bus_master + AC97_GLOBAL_STATUS) & AC97_CODEC_READY)
            return 1;
    }
    return 0;
}

int audio_init(void)
{
    pci_device_t device;
    ac97_ready = 0;
    ac97_mixer = 0;
    ac97_bus_master = 0;
    audio_present = pci_find_class(PCI_CLASS_MULTIMEDIA,
                                   PCI_SUBCLASS_AUDIO, &device);
    if (audio_present) {
        audio_present = pci_enable_device(&device, 1);
        if (audio_present) ac97_ready = ac97_init(&device);
    }
    return audio_present;
}

int audio_device_present(void)
{
    return audio_present;
}

const char* audio_backend_name(void)
{
    if (ac97_ready) return "AC97";
    if (audio_present) return "PCI audio";
    return "PC speaker";
}

int audio_codec_ready(void) { return ac97_ready; }

void audio_beep(uint32_t frequency, uint32_t milliseconds)
{
    uint32_t divisor;
    if (frequency < 20 || frequency > 20000) frequency = 880;
    if (milliseconds == 0 || milliseconds > 5000) milliseconds = 80;
    divisor = 1193180U / frequency;
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t)(divisor & 0xFF));
    outb(0x42, (uint8_t)(divisor >> 8));
    uint8_t gate = inb(0x61);
    outb(0x61, gate | 0x03);
    for (volatile uint32_t i = 0; i < milliseconds * 1000; i++)
        __asm__ volatile("pause");
    outb(0x61, gate & (uint8_t)~0x03);
}
