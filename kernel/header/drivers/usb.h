// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef USB_H
#define USB_H

#include <stdint.h>

typedef struct {
    int controller_count;
    int controller_ready;
    int transfer_ready;
    int port_count;
    int connected;
    char controller_name[8];
    char storage_name[16];
    char partition_name[16];
    int partition_count;
    uint64_t storage_sector_count;
    uint64_t partition_first_lba;
    uint64_t partition_sector_count;
} usb_info_t;

int usb_init(void);
int usb_controller_present(void);
const char* usb_controller_name(void);
int usb_controller_count(void);
const char* usb_controller_name_at(int index);
int usb_controller_ready_at(int index);
int usb_storage_ready_at(int index);
uint64_t usb_storage_sector_count_at(int index);
const char* usb_storage_name_at(int index);
int usb_partition_count_at(int index);
int usb_partition_info_at(int index, int partition, char name[16],
                          uint64_t* first_lba, uint64_t* sector_count);
int usb_port_count(int controller);
int usb_port_connected(int controller, int port);
int usb_port_reset(int controller, int port);
void usb_poll_hotplug(void);

#endif
