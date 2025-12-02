#!/bin/bash
sudo ../qemu/build/qemu-system-x86_64 \
        -cpu host \
        -m 8192 \
        -smp 16 \
        -enable-kvm \
        -drive file=vcrypto.qcow2,format=qcow2  \
        -nographic \
        -netdev user,id=mynet1 -device e1000,netdev=mynet1 \
        -chardev socket,id=chr1,path=/tmp/vhost_crypto_dst.sock \
        -object cryptodev-vhost-user,id=crypto0,chardev=chr1 \
        -device virtio-crypto-pci,cryptodev=crypto0 \
        -object memory-backend-file,id=mem,size=8G,mem-path=/dev/hugepages,share=on \
        -mem-prealloc \
        -numa node,memdev=mem \
        -virtfs local,path=/home/dyh/dpdk-share,mount_tag=host0,security_model=passthrough,id=host0 \
        -monitor tcp:0.0.0.0:4445,server,nowait \
        -incoming tcp:0.0.0.0:6666
