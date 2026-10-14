# CreateOS

CreateOS is a small x86-64 operating system project. GRUB loads the kernel,
the kernel mounts its own CFS5 disk, and a collection of C programs provides
login, a command shell, and a few desktop-style tools.

The project is still growing. It is fun to explore in QEMU, but it is not a
Linux replacement and should not be used to protect important data.

## Build and try it

You will need Make, NASM, a host C compiler, GRUB's ISO tools, xorriso, and
QEMU. The guest kernel and programs use the project's pinned cross-toolchain.

On macOS or Debian/Ubuntu, check and install the host prerequisites, then
build that toolchain:

```sh
make check-deps
make install-deps
make toolchain
make verify-toolchain
```

Build the ISO and run it in QEMU:

```sh
make
make run
```

For a text-only session, run `make serial`. On first boot, CreateOS asks for
basic regional settings and an administrator account. Later boots go to the
login prompt.

`make menuconfig` changes the boot console, kernel log level, and GRUB menu
timeout. These settings are saved in the local `.config` file.

The [build guide](https://github.com/CreateOS-Dev/Docs/blob/main/kernel/build-and-run.md)
has more detail about the toolchain, QEMU and disk images.

## Get the source and assemble the project

Start with the kernel repository:

```sh
git clone https://github.com/CreateOS-Dev/CreateOS.git
cd CreateOS
```

This checkout contains the kernel, boot files, Makefile, and assembly script.
The userland, docs, and SDK are separate components; assemble them beside the
kernel with:

```sh
scripts/assemble-project.sh
```

The script clones missing components and fast-forwards clean component
checkouts. It will stop rather than overwrite local edits or unexpected
folders. Once assembled, set up dependencies and build:

```sh
make check-deps
make install-deps
make toolchain
make
make run
```

`make` also checks and prepares the components automatically. To fetch the
components and immediately build, use `scripts/assemble-project.sh --build`
after installing the host dependencies and toolchain. Use
`scripts/assemble-project.sh --check` to verify existing component checkouts
without fetching.

## Find your way around

- [`kernel/`](kernel): CPU setup, memory, drivers, filesystems, users, and
  syscalls.
- [`boot/`](boot): Multiboot2 entry code and GRUB menus.
- [`rootfs_maker/`](rootfs_maker): guest applications, libraries, filesystem
  tools, and root image builder.
- [`sdk/`](sdk): headers, libraries, application build rules, and examples.
- [`docs/`](docs): guides for users and developers.
- [`scripts/`](scripts): build helpers and source synchronization.
- [`build/`](build): generated files; not the source of record.

### Component repositories and documentation

The source is split across four repositories. The links below open the
component repositories or the matching page in the documentation repository.

| Component | Repository | Start with |
| --- | --- | --- |
| Kernel and boot | [CreateOS](https://github.com/CreateOS-Dev/CreateOS) | [System tour](https://github.com/CreateOS-Dev/Docs/blob/main/kernel/overview.md), [build guide](https://github.com/CreateOS-Dev/Docs/blob/main/kernel/build-and-run.md) |
| Userland and root filesystem | [rootfs_maker](https://github.com/CreateOS-Dev/rootfs_maker) | [Userland README](rootfs_maker/README.md), [userland guide](https://github.com/CreateOS-Dev/Docs/blob/main/userland/userland.md), [shell guide](https://github.com/CreateOS-Dev/Docs/blob/main/userland/shell.md) |
| SDK and application examples | [sdk](https://github.com/CreateOS-Dev/sdk) | [SDK README](sdk/README.md), [app development](https://github.com/CreateOS-Dev/Docs/blob/main/sdk/app-development.md), [language guides](https://github.com/CreateOS-Dev/Docs/blob/main/sdk/languages.md) |
| Guides and references | [Docs](https://github.com/CreateOS-Dev/Docs) | [Documentation index](https://github.com/CreateOS-Dev/Docs/blob/main/README.md) |

New here? Start with the
[system tour](https://github.com/CreateOS-Dev/Docs/blob/main/kernel/overview.md),
then take a look at the
[userland guide](https://github.com/CreateOS-Dev/Docs/blob/main/userland/userland.md) or the
[SDK guide](https://github.com/CreateOS-Dev/Docs/blob/main/sdk/app-development.md).
The [documentation index](https://github.com/CreateOS-Dev/Docs/blob/main/README.md)
links to the kernel, userland, language, UIKit, Lua, and assembly guides.

For the longer language manuals, start with
[C programming](https://github.com/CreateOS-Dev/Docs/blob/main/sdk/c.md),
[Lua](https://github.com/CreateOS-Dev/Docs/blob/main/sdk/lua.md),
[x86-64 assembly](https://github.com/CreateOS-Dev/Docs/blob/main/sdk/assembly.md), and
[CreateOS shell usage](https://github.com/CreateOS-Dev/Docs/blob/main/userland/shell.md). The
[UI Kit reference](https://github.com/CreateOS-Dev/Docs/blob/main/sdk/uikit.md) documents the console API used by apps.

## Working with the component repositories

The complete source tree keeps the kernel, userland, docs, and SDK together.
If you also work with the four component repositories, refresh their local
folders from the complete tree with:

```sh
scripts/sync-public.sh
```

By default, the script uses `../CreateOS-public/` and copies into `CreateOS/`,
`rootfs_maker/`, `Docs/`, and `sdk/`. It only copies local files: no GitHub
connection, commit, or push. Review and publish each project folder yourself
in GitHub Desktop.

To build from the kernel repository, run `scripts/assemble-project.sh`. It
uses Git to clone or fast-forward the userland, docs, and SDK into the
locations the kernel Makefile expects. The build runs this step when needed.

More on the layout and the two scripts is in the
[repository guide](https://github.com/CreateOS-Dev/Docs/blob/main/repository-layout.md).

## Make targets

Run these from the source-tree root. `make` is the normal build; `make help`
is not currently a target.

### Build, configure, and run

| Command | What it does |
| --- | --- |
| `make` or `make all` | Build userland, disk, kernel, and bootable `os.iso`. |
| `make prepare-components` | Validate the components, or assemble them in a kernel checkout. |
| `make os.iso` | Build the normal ISO target. |
| `make menuconfig` | Edit console, log level, and GRUB timeout settings. |
| `make toolchain` | Build and verify the pinned x86-64 cross-toolchain. |
| `make verify-toolchain` | Check the compiler target and compile/link a probe. |
| `make userapps` | Build and stage the guest userland. |
| `make disk-image` | Rebuild the CFS5 disk image from staged userland. |
| `make run` | Run the graphical ISO and guest disk in QEMU. |
| `make serial` | Run the serial-console ISO in QEMU. |
| `make debug` | Run QEMU with tracing and a disposable disk copy. |
| `make debug-serial` | Run the serial debug configuration. |
| `make debug-monitor` | Run with the QEMU monitor on the terminal. |
| `make debug-gdb` | Pause QEMU and open its GDB stub on port 1234. |
| `make edit` | Rebuild the disk image and normal ISO. |
| `make compressed-release` | Build `os.iso` and a tar of compressed boot inputs. |

`make disk-image`, `make edit`, and `make clean` can replace or remove
`build/disk.img`. Back it up first if it contains data you want to keep.
`make refresh-system-apps` updates selected files in the existing disk image
without rebuilding it from scratch; `make refresh-settings` is an alias for
that target.

### Host tools and dependencies

| Command | What it does |
| --- | --- |
| `make check-deps` | Check all host build dependencies. |
| `make install-deps` | Install supported host dependencies (Homebrew or apt). |
| `make check-toolchain-deps` | Check dependencies needed to build the cross-toolchain. |
| `make check-toolchain` | Check that the local cross-compiler and linker exist. |
| `make check-build-tools` | Check compiler, assembler, GRUB, xorriso, and host compiler. |
| `make check-qemu-tools` | Check that the configured QEMU executable exists. |
| `make toolchain-tcc` | Build the host TinyCC compiler used by the TCC app build. |
| `make host-tools` | Build the CFS5 host utilities. |
| `make install-host-tools` | Install CFS5 host utilities under `PREFIX` (default `/usr/local`). |
| `make uninstall-host-tools` | Remove the installed CFS5 host utilities. |
| `make clean` | Remove generated build outputs, including the disk image and ISO files. |
| `make delete-toolchain CONFIRM=YES` | Remove the installed local cross-toolchain. |
| `make delete-toolchain-sources CONFIRM=YES` | Remove cached cross-toolchain sources/build files. |

The userland Makefile also exposes these targets:

| Command | What it does |
| --- | --- |
| `make -C rootfs_maker all` | Build guest core programs, commands, apps, and `luac`. |
| `make -C rootfs_maker check-toolchain` | Check the cross-compiler and linker. |
| `make -C rootfs_maker populate-rootfs` | Stage built files under `build/rootfs/`. |
| `make -C rootfs_maker host-tools` | Build CFS5 image utilities. |
| `make -C rootfs_maker cfs5-to-fat32` | Display the CFS5-to-FAT32 converter's usage line. |
| `make -C rootfs_maker install-host-tools` | Install host utilities under `PREFIX`. |
| `make -C rootfs_maker uninstall-host-tools` | Remove installed host utilities. |
| `make -C rootfs_maker disk-image` | Stage userland and create a fresh CFS5 disk image. |
| `make -C rootfs_maker disk-image-from-stage` | Create a fresh disk image from the already staged rootfs. |
| `make -C rootfs_maker reset-firstboot` | Remove the first-boot completion marker from the staged rootfs. |
| `make -C rootfs_maker clean` | Remove generated rootfs-maker output. |

`make -C sdk all` (or simply `make -C sdk`) builds SDK libraries;
`make -C sdk install DESTDIR=/path/to/rootfs` stages public headers under
`usr/include/createos/` and libraries under `usr/lib/createos/` in that
destination. `make -C sdk clean` removes generated libraries. Individual apps have their own
Makefiles, for example `make -C rootfs_maker/root/app/lua`; that project also
provides `luac`, `clean-luac`, and `clean` targets. Other app targets vary
with the Makefile in each app directory. The
[build guide](https://github.com/CreateOS-Dev/Docs/blob/main/kernel/build-and-run.md) and
[userland README](rootfs_maker/README.md) explain image handling in more detail.

## Disk images

The guest disk lives at `build/disk.img`. It can contain accounts and files
you have created, so back it up before rebuilding it. `make clean` removes
generated files **including `build/disk.img`**. `make run` uses the current
disk; `make edit` rebuilds it from the staged root filesystem.

The bootable ISO is `os.iso`. `make compressed-release` creates a tar archive
of compressed release inputs; it is not itself a compressed bootable ISO.

## Getting involved

Hardware coverage, filesystem recovery, test automation, and guest
applications are all good areas to explore. The [work list](https://github.com/CreateOS-Dev/Docs/blob/main/TODO.md)
collects some open-ended ideas. Before changing a subsystem, read its guide
and note exactly what you tested.
