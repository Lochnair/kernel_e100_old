#!/bin/sh
make -j$(nproc) ARCH=mips CROSS_COMPILE=mips64-octeon-linux- vmlinux modules
make ARCH=mips CROSS_COMPILE=mips64-octeon-linux- INSTALL_MOD_PATH=target/ modules_install
cp vmlinux vmlinux.64
md5sum vmlinux.64 | cut -f 1 -d ' ' | tee vmlinux.64.md5
tar --owner=root --group=root -cjvf sfe-kernel.tar.bz2 vmlinux.64 vmlinux.64.md5
tar --owner=root --group=root -cjvf sfe-modules.tar.bz2 -C target lib/modules/3.10.20-UBNT/kernel/net/shortcut-fe \
								 lib/modules/3.10.20-UBNT/kernel/net/bridge/bridge.ko \
								 lib/modules/3.10.20-UBNT/kernel/net/netfilter/nf_conntrack.ko \
								 lib/modules/3.10.20-UBNT/kernel/net/netfilter/nf_conntrack_netlink.ko
rm -rf target
rm -f vmlinux.64*
