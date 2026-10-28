#!/usr/bin/env python3
"""Read-only VFIO checks; never bind devices or elevate privileges."""
import argparse
import os
from pathlib import Path
import re
import resource


def memory_bytes(value):
    # QEMU 的 -m 参数不带单位时以 MiB 表示，而不是字节。
    match = re.fullmatch(r'([1-9][0-9]*)([KMGTPE]?)', value, re.I)
    if not match:
        raise ValueError('无法预检 QEMU 内存大小：' + value)
    unit = match[2].upper() or 'M'
    return int(match[1]) * 1024 ** ('KMGTPE'.index(unit) + 1)


def check(memory, devices, sysfs=Path('/sys/bus/pci/devices'),
          devfs=Path('/dev/vfio'), limit=None, access=os.access):
    required = memory_bytes(memory)
    for address in devices:
        if not re.fullmatch(r'(?:[0-9a-fA-F]{4}:)?[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]', address):
            raise ValueError('无效 PCI 地址：' + address)
        address = address.lower()
        if address.count(':') == 1:
            address = '0000:' + address
        device = sysfs/address
        if not (device/'driver').is_symlink() or (device/'driver').resolve().name != 'vfio-pci':
            raise ValueError(address + ' 尚未绑定 vfio-pci；不会自动解绑宿主驱动')
        group = device/'iommu_group'
        if not group.is_symlink() or not group.resolve().name.isdecimal():
            raise ValueError(address + ' 缺少有效 IOMMU 分组')
        for node in (devfs/'vfio', devfs/group.resolve().name):
            if not access(node, os.R_OK | os.W_OK):
                raise ValueError('当前账号无权读写 ' + str(node))
    if limit is None:
        limit = resource.getrlimit(resource.RLIMIT_MEMLOCK)[0]
    if limit != resource.RLIM_INFINITY and limit < required:
        raise ValueError(f'VFIO 锁页额度不足：当前 {limit} 字节，'
                         f'来宾内存至少需要 {required} 字节；'
                         '这是进程 memlock 限制，不代表物理内存耗尽')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--memory', required=True)
    parser.add_argument('devices', nargs='+')
    args = parser.parse_args()
    try:
        check(args.memory, args.devices)
    except ValueError as error:
        parser.exit(1, str(error) + '\n未构建或替换磁盘镜像。\n')
