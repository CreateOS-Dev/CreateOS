#ifndef CREATEOS_VERSION_H
#define CREATEOS_VERSION_H

#include <stdint.h>

#define CREATEOS_NAME "CreateOS"
#define CREATEOS_VERSION "0.0.1"
#define CREATEOS_CODENAME "Alpha"
#define CREATEOS_ARCH "x86_64"
#define CREATEOS_BUILD_DATE __DATE__ " " __TIME__
#define CREATEOS_KERNEL_VERSION CREATEOS_VERSION
#define CREATEOS_ROOTFS_VERSION CREATEOS_VERSION
#define CREATEOS_VERSION_ABI 1

typedef struct {
    uint32_t abi_version;
    char name[16];
    char release[16];
    char codename[16];
    char architecture[16];
    char build_date[32];
} createos_kernel_version_t;

#define OS_NAME CREATEOS_NAME
#define OS_VERSION CREATEOS_VERSION
#define OS_CODENAME CREATEOS_CODENAME
#define OS_ARCH CREATEOS_ARCH
#define OS_BUILD_DATE CREATEOS_BUILD_DATE
#define OS_KERNEL_VER CREATEOS_KERNEL_VERSION
#define OS_ROOTFS_VER CREATEOS_ROOTFS_VERSION
#define UNAME_SYSNAME OS_NAME
#define UNAME_RELEASE OS_VERSION
#define UNAME_VERSION OS_BUILD_DATE
#define UNAME_MACHINE OS_ARCH
#define UNAME_ROOTFS OS_ROOTFS_VER

#endif
