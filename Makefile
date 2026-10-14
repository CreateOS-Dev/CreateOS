.DEFAULT_GOAL := all

TARGET ?= x86_64-elf
TOOLCHAIN_PREFIX ?= $(CURDIR)/build/toolchain/$(TARGET)
TCC_PREFIX ?= $(CURDIR)/build/toolchain/tcc-host
JOBS ?= 2
override CROSS_COMPILE := $(TOOLCHAIN_PREFIX)/bin/$(TARGET)-
override CC := $(CROSS_COMPILE)gcc
override LD := $(CROSS_COMPILE)ld
AS := nasm
HOSTCC ?= cc
PREFIX ?= /usr/local
DESTDIR ?=
BUILD_DIR := build
-include $(BUILD_DIR)/config.mk
ISO_IMAGE := os.iso
ISO_ROOT := $(BUILD_DIR)/isodir
COMPRESSED_RELEASE_DIR := $(BUILD_DIR)/compressed-release
COMPRESSED_RELEASE_PACKAGE := $(BUILD_DIR)/CreateOS-x86_64-compressed.tar
KERNEL_XZ := CreateOS-kernel.elf.xz
USERLAND_XZ := CreateOS-userland.img.xz
SERIAL_ISO_IMAGE := $(BUILD_DIR)/os-serial.iso
SERIAL_ISO_ROOT := $(BUILD_DIR)/serial-isodir
QEMU ?= qemu-system-x86_64
QEMU_CPU ?= max
QEMU_NET_ARGS ?= -netdev user,id=createos-net,restrict=off -device e1000,netdev=createos-net
QEMU_LOGS ?= 0
QEMU_SERIAL ?= none
QEMU_DISPLAY ?= cocoa,zoom-to-fit=on
QEMU_FULLSCREEN ?= 0
QEMU_RTC ?= base=localtime
CONFIG_CONSOLE ?= tty1
CONFIG_LOGLEVEL ?= info
CONFIG_GRUB_TIMEOUT ?= 0
KERNEL_CMDLINE ?= console=$(CONFIG_CONSOLE),115200 loglevel=$(CONFIG_LOGLEVEL)

ifeq ($(QEMU_LOGS),2)
QEMU_DEBUG_ARGS := -d cpu_reset,int,guest_errors,unimp,pcall,mmu -D $(BUILD_DIR)/qemu.log
else ifeq ($(QEMU_LOGS),1)
QEMU_DEBUG_ARGS := -d cpu_reset,int,guest_errors -D $(BUILD_DIR)/qemu.log
else
QEMU_DEBUG_ARGS :=
endif

ifeq ($(QEMU_SERIAL),none)
QEMU_SERIAL_ARG := -serial null
else
QEMU_SERIAL_ARG := -serial $(QEMU_SERIAL)
endif

ifeq ($(QEMU_FULLSCREEN),1)
QEMU_DISPLAY_ARG := -display $(QEMU_DISPLAY) -full-screen
else
QEMU_DISPLAY_ARG := -display $(QEMU_DISPLAY)
endif

CFLAGS := -m64 -ffreestanding -fno-pic -fstack-protector-strong -mno-red-zone -Ikernel/header -Ikernel/ -Wall -Wextra -D_FORTIFY_SOURCE=2 -MMD -MP
LDFLAGS := -z noexecstack -T linker.ld

LWIP_DIR := kernel/drivers/network/lwip
LWIP_SRCS := \
	$(LWIP_DIR)/src/core/def.c \
	$(LWIP_DIR)/src/core/dns.c \
	$(LWIP_DIR)/src/core/inet_chksum.c \
	$(LWIP_DIR)/src/core/init.c \
	$(LWIP_DIR)/src/core/ip.c \
	$(LWIP_DIR)/src/core/mem.c \
	$(LWIP_DIR)/src/core/memp.c \
	$(LWIP_DIR)/src/core/netif.c \
	$(LWIP_DIR)/src/core/pbuf.c \
	$(LWIP_DIR)/src/core/raw.c \
	$(LWIP_DIR)/src/core/stats.c \
	$(LWIP_DIR)/src/core/tcp.c \
	$(LWIP_DIR)/src/core/tcp_in.c \
	$(LWIP_DIR)/src/core/tcp_out.c \
	$(LWIP_DIR)/src/core/timeouts.c \
	$(LWIP_DIR)/src/core/udp.c \
	$(LWIP_DIR)/src/core/ipv4/acd.c \
	$(LWIP_DIR)/src/core/ipv4/autoip.c \
	$(LWIP_DIR)/src/core/ipv4/dhcp.c \
	$(LWIP_DIR)/src/core/ipv4/etharp.c \
	$(LWIP_DIR)/src/core/ipv4/icmp.c \
	$(LWIP_DIR)/src/core/ipv4/igmp.c \
	$(LWIP_DIR)/src/core/ipv4/ip4.c \
	$(LWIP_DIR)/src/core/ipv4/ip4_addr.c \
	$(LWIP_DIR)/src/core/ipv4/ip4_frag.c \
	$(LWIP_DIR)/src/core/ipv6/ethip6.c \
	$(LWIP_DIR)/src/core/ipv6/icmp6.c \
	$(LWIP_DIR)/src/core/ipv6/inet6.c \
	$(LWIP_DIR)/src/core/ipv6/ip6.c \
	$(LWIP_DIR)/src/core/ipv6/ip6_addr.c \
	$(LWIP_DIR)/src/core/ipv6/ip6_frag.c \
	$(LWIP_DIR)/src/core/ipv6/mld6.c \
	$(LWIP_DIR)/src/core/ipv6/nd6.c \
	$(LWIP_DIR)/src/netif/ethernet.c
KERNEL_C_SRCS := $(filter-out kernel/fs/mkfs_cfs5.c \
	kernel/arch/x86_64/interrupts.c kernel/lib/third_party/% \
	$(LWIP_DIR)/src/%,$(shell find kernel -name '*.c' -print))
KERNEL_C_SRCS += $(LWIP_SRCS) \
	kernel/lib/third_party/libschrift/schrift.c \
	kernel/lib/third_party/libschrift/createos_alloc.c \
	kernel/lib/third_party/tinygl/src/api.c \
	kernel/lib/third_party/tinygl/src/arrays.c \
	kernel/lib/third_party/tinygl/src/clear.c \
	kernel/lib/third_party/tinygl/src/clip.c \
	kernel/lib/third_party/tinygl/src/error.c \
	kernel/lib/third_party/tinygl/src/image_util.c \
	kernel/lib/third_party/tinygl/src/init.c \
	kernel/lib/third_party/tinygl/src/light.c \
	kernel/lib/third_party/tinygl/src/list.c \
	kernel/lib/third_party/tinygl/src/matrix.c \
	kernel/lib/third_party/tinygl/src/memory.c \
	kernel/lib/third_party/tinygl/src/misc.c \
	kernel/lib/third_party/tinygl/src/msghandling.c \
	kernel/lib/third_party/tinygl/src/select.c \
	kernel/lib/third_party/tinygl/src/specbuf.c \
	kernel/lib/third_party/tinygl/src/texture.c \
	kernel/lib/third_party/tinygl/src/vertex.c \
	kernel/lib/third_party/tinygl/src/zbuffer.c \
	kernel/lib/third_party/tinygl/src/zdither.c \
	kernel/lib/third_party/tinygl/src/zline.c \
	kernel/lib/third_party/tinygl/src/zmath.c \
	kernel/lib/third_party/tinygl/src/ztriangle.c \
	kernel/lib/third_party/tinygl/compat/createos_compat.c \
	kernel/lib/third_party/tinygl/compat/createos_math.c
KERNEL_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(KERNEL_C_SRCS))
LWIP_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(LWIP_SRCS))
LWIP_CFLAGS := -I$(LWIP_DIR) -I$(LWIP_DIR)/src/include -Ikernel/header/lib
SCHRIFT_CFLAGS := -DCREATEOS_KERNEL
TINYGL_CFLAGS := -DCREATEOS_KERNEL \
	-Ikernel/lib/third_party/tinygl/compat \
	-Ikernel/lib/third_party/tinygl/include \
	-Ikernel/lib/third_party/tinygl/src \
	-Ikernel/lib/third_party/libschrift
$(LWIP_OBJS) $(BUILD_DIR)/kernel/drivers/network/lwip_port.o \
$(BUILD_DIR)/kernel/drivers/network/netstack.o: CFLAGS += $(LWIP_CFLAGS)
$(LWIP_OBJS) $(BUILD_DIR)/kernel/drivers/network/lwip_port.o \
$(BUILD_DIR)/kernel/drivers/network/netstack.o: $(LWIP_DIR)/lwipopts.h $(LWIP_DIR)/arch/cc.h
$(BUILD_DIR)/kernel/lib/third_party/libschrift/schrift.o \
$(BUILD_DIR)/kernel/lib/third_party/libschrift/createos_alloc.o: CFLAGS += $(SCHRIFT_CFLAGS)
$(filter $(BUILD_DIR)/kernel/lib/third_party/tinygl/%,$(KERNEL_OBJS)): CFLAGS += $(TINYGL_CFLAGS)
$(BUILD_DIR)/kernel/drivers/console/tinygl.o: CFLAGS += $(TINYGL_CFLAGS)
OBJ := $(BUILD_DIR)/boot/boot.o $(BUILD_DIR)/kernel/header/fonts/noto_sans.o $(BUILD_DIR)/kernel/arch/x86_64/syscall.o $(BUILD_DIR)/kernel/arch/x86_64/interrupts.o $(BUILD_DIR)/kernel/arch/x86_64/context_switch.o $(BUILD_DIR)/kernel/arch/x86_64/s3.o $(BUILD_DIR)/kernel/arch/x86_64/interrupts_c.o $(KERNEL_OBJS)
KERNEL_DEPS := $(KERNEL_OBJS:.o=.d) $(BUILD_DIR)/kernel/arch/x86_64/interrupts_c.d

-include $(KERNEL_DEPS)

GRUB_MKRESCUE ?= $(shell command -v i686-elf-grub-mkrescue 2>/dev/null || command -v grub-mkrescue 2>/dev/null || command -v x86_64-elf-grub-mkrescue 2>/dev/null || echo /opt/homebrew/bin/i686-elf-grub-mkrescue)

all: prepare-components check-toolchain check-build-tools verify-toolchain $(ISO_IMAGE)

.PHONY: menuconfig prepare-components
menuconfig:
	python3 scripts/menuconfig.py

prepare-components:
	@if [ -e rootfs_maker/.createos-public-assembled ] || \
		[ -e sdk/.createos-public-assembled ] || \
		[ -e docs/.createos-public-assembled ] || \
		{ git -C "$(CURDIR)" remote get-url origin 2>/dev/null | \
			grep -q 'CreateOS-Dev/CreateOS'; }; then \
		./scripts/assemble-project.sh; \
	elif [ -d rootfs_maker ] && [ -d sdk ] && [ -d docs ] && \
		[ ! -e rootfs_maker/.git ] && [ ! -e sdk/.git ] && \
		[ ! -e docs/.git ]; then \
		test -f rootfs_maker/Makefile && \
		test -f rootfs_maker/lib/user_syscall.h && \
		test -f sdk/Makefile && \
		test -f sdk/app.mk && \
		test -f docs/kernel/overview.md && \
		test -f docs/userland/userland.md && \
		test -f docs/sdk/app-development.md; \
	else \
		./scripts/assemble-project.sh; \
	fi

$(BUILD_DIR)/config.mk: configs/default.config scripts/menuconfig.py \
		$(wildcard .config)
	@mkdir -p $(@D)
	@python3 scripts/menuconfig.py --export $@

clean:
	rm -rf $(BUILD_DIR)/boot $(BUILD_DIR)/kernel \
		$(ISO_ROOT) $(SERIAL_ISO_ROOT) \
		$(BUILD_DIR)/rootfs $(BUILD_DIR)/rootfs_maker \
		$(BUILD_DIR)/toolchain-check \
		$(COMPRESSED_RELEASE_DIR) $(COMPRESSED_RELEASE_PACKAGE) \
		$(ISO_IMAGE) $(SERIAL_ISO_IMAGE) \
		$(BUILD_DIR)/debug-disk.img \
		$(KERNEL_XZ) $(USERLAND_XZ) \
		$(BUILD_DIR)/qemu.log $(BUILD_DIR)/qemu-debug.log \
		$(BUILD_DIR)/qemu-serial.log \
		$(BUILD_DIR)/disk.img
	@if [ -d rootfs_maker ]; then $(MAKE) -C rootfs_maker clean; fi

$(BUILD_DIR)/boot/boot.o: boot/boot.S | check-toolchain
	mkdir -p $(@D)
	$(AS) -f elf64 $< -o $@

$(BUILD_DIR)/kernel/arch/x86_64/syscall.o: kernel/arch/x86_64/syscall.asm | check-toolchain
	mkdir -p $(@D)
	$(AS) -f elf64 $< -o $@

$(BUILD_DIR)/kernel/arch/x86_64/interrupts.o: kernel/arch/x86_64/interrupts.asm | check-toolchain
	mkdir -p $(@D)
	$(AS) -f elf64 $< -o $@

$(BUILD_DIR)/kernel/arch/x86_64/context_switch.o: kernel/arch/x86_64/context_switch.asm | check-toolchain
	mkdir -p $(@D)
	$(AS) -f elf64 $< -o $@

$(BUILD_DIR)/kernel/arch/x86_64/s3.o: kernel/arch/x86_64/s3.asm | check-toolchain
	mkdir -p $(@D)
	$(AS) -f elf64 $< -o $@

$(BUILD_DIR)/kernel/arch/x86_64/interrupts_c.o: kernel/arch/x86_64/interrupts.c | check-toolchain
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel/header/fonts/noto_sans.o: kernel/header/fonts/noto_sans.S rootfs_maker/root/assets/fonts/NotoSans.ttf | check-toolchain
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.c | check-toolchain
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/CreateOS: $(OBJ) | verify-toolchain
	$(LD) $(LDFLAGS) $^ -o $@

$(ISO_IMAGE): prepare-components refresh-system-apps check-build-tools $(BUILD_DIR)/CreateOS boot/grub/grub.cfg $(BUILD_DIR)/disk.img
	@mkdir -p $(ISO_ROOT)/boot/grub
	@cp $(BUILD_DIR)/CreateOS $(ISO_ROOT)/boot/kernel.elf
	@sed -e 's|^set timeout=.*|set timeout=$(CONFIG_GRUB_TIMEOUT)|' \
		-e 's|^    multiboot2 /boot/kernel.elf.*|    multiboot2 /boot/kernel.elf $(KERNEL_CMDLINE)|' \
		boot/grub/grub.cfg > $(ISO_ROOT)/boot/grub/grub.cfg
	@cp $(BUILD_DIR)/disk.img $(ISO_ROOT)/boot/disk.img
	$(GRUB_MKRESCUE) -o $@ $(ISO_ROOT)

$(SERIAL_ISO_IMAGE): prepare-components refresh-system-apps check-build-tools $(BUILD_DIR)/CreateOS boot/grub/grub-serial.cfg $(BUILD_DIR)/disk.img
	@mkdir -p $(SERIAL_ISO_ROOT)/boot/grub
	@cp $(BUILD_DIR)/CreateOS $(SERIAL_ISO_ROOT)/boot/kernel.elf
	@cp boot/grub/grub-serial.cfg $(SERIAL_ISO_ROOT)/boot/grub/grub.cfg
	@cp $(BUILD_DIR)/disk.img $(SERIAL_ISO_ROOT)/boot/disk.img
	$(GRUB_MKRESCUE) -o $@ $(SERIAL_ISO_ROOT)

.PHONY: compressed-release
compressed-release: $(ISO_IMAGE)
	@command -v xz >/dev/null 2>&1 || { \
		printf 'Missing compression tool: xz\n' >&2; exit 1; \
	}
	mkdir -p "$(COMPRESSED_RELEASE_DIR)"
	xz -c "$(ISO_ROOT)/boot/kernel.elf" > "$(KERNEL_XZ)"
	xz -c "$(ISO_ROOT)/boot/disk.img" > "$(USERLAND_XZ)"
	cp "$(KERNEL_XZ)" "$(COMPRESSED_RELEASE_DIR)/kernel.elf.xz"
	cp "$(USERLAND_XZ)" "$(COMPRESSED_RELEASE_DIR)/rootfs-disk.img.xz"
	cp "$(ISO_ROOT)/boot/grub/grub.cfg" \
		"$(COMPRESSED_RELEASE_DIR)/grub.cfg"
	xz -t "$(KERNEL_XZ)"
	xz -t "$(USERLAND_XZ)"
	xz -t "$(COMPRESSED_RELEASE_DIR)/kernel.elf.xz"
	xz -t "$(COMPRESSED_RELEASE_DIR)/rootfs-disk.img.xz"
	tar -cf "$(COMPRESSED_RELEASE_PACKAGE)" \
		-C "$(COMPRESSED_RELEASE_DIR)" \
		kernel.elf.xz rootfs-disk.img.xz grub.cfg
	tar -tf "$(COMPRESSED_RELEASE_PACKAGE)" >/dev/null
	@printf 'Compressed release package created: %s\n' \
		"$(COMPRESSED_RELEASE_PACKAGE)"

userapps: prepare-components
	$(MAKE) -C rootfs_maker populate-rootfs \
		CROSS_COMPILE="$(CROSS_COMPILE)" \
		TOOLCHAIN_PREFIX="$(TOOLCHAIN_PREFIX)"

toolchain: check-toolchain-deps
	TOOLCHAIN_PREFIX="$(TOOLCHAIN_PREFIX)" \
		TARGET="$(TARGET)" JOBS="$(JOBS)" \
		./scripts/build-toolchain.sh
	$(MAKE) verify-toolchain

toolchain-tcc:
	TCC_PREFIX="$(TCC_PREFIX)" \
		./scripts/build-tcc-host.sh

install-deps:
	./scripts/install-deps.sh install

check-deps:
	./scripts/install-deps.sh check-all

check-toolchain-deps:
	./scripts/install-deps.sh check-toolchain

delete-toolchain:
	@test "$(CONFIRM)" = "YES" || { \
		printf 'Refusing deletion. Re-run with CONFIRM=YES.\n' >&2; exit 2; \
	}
	@test "$(TOOLCHAIN_PREFIX)" = "$(CURDIR)/build/toolchain/$(TARGET)" || { \
		printf 'Refusing deletion outside the repository toolchain prefix.\n' >&2; exit 2; \
	}
	@if [ -d "$(TOOLCHAIN_PREFIX)" ]; then \
		rm -rf -- "$(TOOLCHAIN_PREFIX)"; \
		printf 'Removed toolchain install: %s\n' "$(TOOLCHAIN_PREFIX)"; \
	else \
		printf 'No installed toolchain at %s\n' "$(TOOLCHAIN_PREFIX)"; \
	fi

delete-toolchain-sources:
	@test "$(CONFIRM)" = "YES" || { \
		printf 'Refusing deletion. Re-run with CONFIRM=YES.\n' >&2; exit 2; \
	}
	@if [ -d "$(CURDIR)/build/toolchain/src" ]; then \
		rm -rf -- "$(CURDIR)/build/toolchain/src"; \
		printf 'Removed cached toolchain sources and build files.\n'; \
	else \
		printf 'No cached toolchain sources found.\n'; \
	fi

host-tools: prepare-components
	$(MAKE) -C rootfs_maker host-tools HOSTCC="$(HOSTCC)"

install-host-tools:
	$(MAKE) prepare-components
	$(MAKE) -C rootfs_maker install-host-tools HOSTCC="$(HOSTCC)" \
		PREFIX="$(PREFIX)" DESTDIR="$(DESTDIR)"

uninstall-host-tools:
	$(MAKE) prepare-components
	$(MAKE) -C rootfs_maker uninstall-host-tools \
		PREFIX="$(PREFIX)" DESTDIR="$(DESTDIR)"

$(BUILD_DIR)/disk.img: | userapps
	$(MAKE) -C rootfs_maker disk-image-from-stage

refresh-system-apps: userapps $(BUILD_DIR)/disk.img
	$(MAKE) -C rootfs_maker host-tools HOSTCC="$(HOSTCC)"
	$(BUILD_DIR)/rootfs_maker/host/cfs5tool check $(BUILD_DIR)/disk.img
	$(BUILD_DIR)/rootfs_maker/host/copy_to_cfs5 $(BUILD_DIR)/disk.img \
		$(BUILD_DIR)/rootfs/app/system/bin/settings.APPL \
		/app/system/bin/settings.APPL
	$(BUILD_DIR)/rootfs_maker/host/copy_to_cfs5 $(BUILD_DIR)/disk.img \
		$(BUILD_DIR)/rootfs/app/system/bin/textedit.APPL \
		/app/system/bin/textedit.APPL
	$(BUILD_DIR)/rootfs_maker/host/copy_to_cfs5 $(BUILD_DIR)/disk.img \
		$(BUILD_DIR)/rootfs/app/system/bin/explorer.APPL \
		/app/system/bin/explorer.APPL
	$(BUILD_DIR)/rootfs_maker/host/copy_to_cfs5 $(BUILD_DIR)/disk.img \
		$(BUILD_DIR)/rootfs/app/system/bin/lua.APPL \
		/app/system/bin/lua.APPL
	$(BUILD_DIR)/rootfs_maker/host/copy_to_cfs5 $(BUILD_DIR)/disk.img \
		$(BUILD_DIR)/rootfs/bin/lua.bin \
		/bin/lua.bin
	$(BUILD_DIR)/rootfs_maker/host/copy_to_cfs5 $(BUILD_DIR)/disk.img \
		rootfs_maker/root/app/system/lua.link \
		/app/system/lua.link
	$(BUILD_DIR)/rootfs_maker/host/copy_to_cfs5 $(BUILD_DIR)/disk.img \
		$(BUILD_DIR)/rootfs/app/system/bin/luac.APPL \
		/app/system/bin/luac.APPL
	$(BUILD_DIR)/rootfs_maker/host/copy_to_cfs5 $(BUILD_DIR)/disk.img \
		$(BUILD_DIR)/rootfs/bin/luac.bin \
		/bin/luac.bin
	$(BUILD_DIR)/rootfs_maker/host/copy_to_cfs5 $(BUILD_DIR)/disk.img \
		rootfs_maker/root/app/system/luac.link \
		/app/system/luac.link
	$(BUILD_DIR)/rootfs_maker/host/copy_to_cfs5 $(BUILD_DIR)/disk.img \
		$(BUILD_DIR)/rootfs/app/system/bin/tcc.APPL \
		/app/system/bin/tcc.APPL
	$(BUILD_DIR)/rootfs_maker/host/copy_to_cfs5 $(BUILD_DIR)/disk.img \
		$(BUILD_DIR)/rootfs/bin/tcc.bin /bin/tcc.bin
	$(BUILD_DIR)/rootfs_maker/host/copy_to_cfs5 $(BUILD_DIR)/disk.img \
		$(BUILD_DIR)/rootfs/usr/lib/tcc /usr/lib/tcc
	$(BUILD_DIR)/rootfs_maker/host/copy_to_cfs5 $(BUILD_DIR)/disk.img \
		$(BUILD_DIR)/rootfs/usr/include /usr/include
	$(BUILD_DIR)/rootfs_maker/host/copy_to_cfs5 $(BUILD_DIR)/disk.img \
		$(BUILD_DIR)/rootfs/app/custom /app/custom
	$(BUILD_DIR)/rootfs_maker/host/copy_to_cfs5 $(BUILD_DIR)/disk.img \
		$(BUILD_DIR)/rootfs/bin/lspci.bin \
		/bin/lspci.bin
	$(BUILD_DIR)/rootfs_maker/host/copy_to_cfs5 $(BUILD_DIR)/disk.img \
		$(BUILD_DIR)/rootfs/bin/clock.bin \
		/bin/clock.bin
	$(BUILD_DIR)/rootfs_maker/host/copy_to_cfs5 $(BUILD_DIR)/disk.img \
		rootfs_maker/root/etc/keyboards/fr-azerty.lyt \
		/etc/keyboards/fr-azerty.lyt
	$(BUILD_DIR)/rootfs_maker/host/cfs5tool check $(BUILD_DIR)/disk.img

refresh-settings: refresh-system-apps

disk-image:
	$(MAKE) prepare-components
	$(MAKE) -C rootfs_maker disk-image

edit:
	$(MAKE) prepare-components
	$(MAKE) -C rootfs_maker disk-image
	$(MAKE) os.iso

.PHONY: run serial debug debug-serial debug-monitor debug-gdb os.iso clean all userapps host-tools install-host-tools \
	uninstall-host-tools disk-image edit refresh-system-apps refresh-settings \
	check-build-tools check-qemu-tools \
	toolchain toolchain-tcc \
	verify-toolchain check-toolchain check-toolchain-deps delete-toolchain delete-toolchain-sources \
	install-deps check-deps

.NOTPARALLEL: all

check-toolchain:
	@for tool in "$(CC)" "$(LD)" "$(CROSS_COMPILE)nm" "$(CROSS_COMPILE)objcopy"; do \
		if [ ! -x "$$tool" ]; then \
			printf 'Required CreateOS toolchain executable is missing: %s\n' \
				"$$tool" >&2; \
			printf 'Build the pinned local toolchain first with: make toolchain\n' >&2; \
			exit 1; \
		fi; \
	done
	@target=`"$(CC)" -dumpmachine` || exit $$?; \
	if [ "$$target" != "$(TARGET)" ]; then \
		printf 'Local toolchain target mismatch: expected %s, got %s\n' \
			"$(TARGET)" "$$target" >&2; exit 1; \
	fi

check-build-tools:
	@missing=0; \
	for tool in "$(CC)" "$(LD)" "$(AS)" "$(CROSS_COMPILE)objcopy" \
		"$(GRUB_MKRESCUE)" xorriso "$(HOSTCC)" gcc make; do \
		if ! command -v "$$tool" >/dev/null 2>&1; then \
			printf 'Missing build dependency: %s\n' "$$tool" >&2; \
			missing=1; \
		fi; \
	done; \
	if [ "$$missing" -ne 0 ]; then exit 1; fi

verify-toolchain: check-toolchain
	@set -eu; \
	mkdir -p "$(BUILD_DIR)/toolchain-check"; \
	target=`"$(CC)" -dumpmachine`; \
	if [ "$$target" != "$(TARGET)" ]; then \
		printf 'Wrong compiler target: expected %s, got %s\n' \
			"$(TARGET)" "$$target" >&2; exit 1; \
	fi; \
	printf '%s\n' 'int createos_toolchain_probe(void) { return 42; }' | \
		"$(CC)" -ffreestanding -fno-stack-protector -x c -c - \
			-o "$(BUILD_DIR)/toolchain-check/probe.o"; \
	"$(LD)" -r "$(BUILD_DIR)/toolchain-check/probe.o" \
		-o "$(BUILD_DIR)/toolchain-check/probe-linked.o"; \
	"$(CROSS_COMPILE)nm" "$(BUILD_DIR)/toolchain-check/probe-linked.o" | \
		grep -q 'createos_toolchain_probe'; \
	printf 'Verified %s compiler, linker and symbol output.\n' "$(TARGET)"

check-qemu-tools:
	@command -v "$(QEMU)" >/dev/null 2>&1 || { \
		printf 'Missing QEMU executable: %s\n' "$(QEMU)" >&2; exit 1; \
	}

run: check-qemu-tools $(ISO_IMAGE) $(BUILD_DIR)/disk.img
	$(QEMU) -cpu $(QEMU_CPU) $(QEMU_DEBUG_ARGS) $(QEMU_NET_ARGS) \
		-drive file=$(ISO_IMAGE),format=raw,if=ide,index=0,media=cdrom \
		-drive file=$(BUILD_DIR)/disk.img,format=raw,if=ide,index=1,media=disk \
		-boot order=d \
		-rtc $(QEMU_RTC) \
		$(QEMU_SERIAL_ARG) \
		$(QEMU_DISPLAY_ARG)

serial: check-qemu-tools $(SERIAL_ISO_IMAGE) $(BUILD_DIR)/disk.img
	$(QEMU) -cpu $(QEMU_CPU) $(QEMU_DEBUG_ARGS) $(QEMU_NET_ARGS) \
		-drive file=$(SERIAL_ISO_IMAGE),format=raw,if=ide,index=0,media=cdrom \
		-drive file=$(BUILD_DIR)/disk.img,format=raw,if=ide,index=1,media=disk \
		-boot order=d \
		-rtc $(QEMU_RTC) \
		-serial stdio \
		-display none

define run_qemu_debug
	@set -eu; \
		debug_disk="$(BUILD_DIR)/debug-disk.img"; \
		cp $(BUILD_DIR)/disk.img "$$debug_disk"; \
		trap 'rm -f "$$debug_disk"' EXIT INT TERM; \
		$(QEMU) -cpu $(QEMU_CPU) \
			-d cpu_reset,int,guest_errors,unimp,pcall,mmu \
			-D $(BUILD_DIR)/qemu-debug.log $(QEMU_NET_ARGS) \
			-drive file=$(1),format=raw,if=ide,index=0,media=cdrom \
			-drive file="$$debug_disk",format=raw,if=ide,index=1,media=disk \
			-boot order=d -rtc $(QEMU_RTC) $(2) $(3) $(4)
endef

debug: check-qemu-tools $(ISO_IMAGE) $(BUILD_DIR)/disk.img
	$(call run_qemu_debug,$(ISO_IMAGE),-serial file:$(BUILD_DIR)/qemu-serial.log,-display $(QEMU_DISPLAY),-monitor none)

debug-serial: check-qemu-tools $(SERIAL_ISO_IMAGE) $(BUILD_DIR)/disk.img
	$(call run_qemu_debug,$(SERIAL_ISO_IMAGE),-serial stdio,-display none,-monitor none)

debug-monitor: check-qemu-tools $(SERIAL_ISO_IMAGE) $(BUILD_DIR)/disk.img
	$(call run_qemu_debug,$(SERIAL_ISO_IMAGE),-serial file:$(BUILD_DIR)/qemu-serial.log,-display none,-monitor stdio)

debug-gdb: check-qemu-tools $(SERIAL_ISO_IMAGE) $(BUILD_DIR)/disk.img
	$(call run_qemu_debug,$(SERIAL_ISO_IMAGE),-serial file:$(BUILD_DIR)/qemu-serial.log,-display none,-monitor none -S -gdb tcp::1234)
