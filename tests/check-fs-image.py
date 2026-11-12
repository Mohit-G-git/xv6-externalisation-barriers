#!/usr/bin/env python3
"""Check xv6 x86 inode references and allocation bitmap consistency."""

import struct
import sys
from pathlib import Path

BSIZE = 512
NDIRECT = 12
NINDIRECT = BSIZE // 4
DINODE_SIZE = 64
IPB = BSIZE // DINODE_SIZE
T_DIR = 1
T_FILE = 2
T_DEV = 3


class ImageError(Exception):
    pass


def fail(message: str) -> None:
    raise ImageError(message)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check-fs-image.py FS_IMAGE", file=sys.stderr)
        return 2

    image = Path(sys.argv[1])
    try:
        data = image.read_bytes()
        if len(data) % BSIZE:
            fail("image size is not block-aligned")
        blocks = len(data) // BSIZE
        if blocks < 2:
            fail("image is too small to contain a superblock")

        sb = struct.unpack_from("<7I", data, BSIZE)
        size, _nblocks, ninodes, nlog, logstart, inodestart, bmapstart = sb
        if size > blocks or size <= 0 or ninodes <= 1 or nlog <= 0:
            fail("invalid superblock dimensions")
        if not (2 <= logstart < inodestart < bmapstart < size):
            fail("invalid superblock region ordering")

        def block_bytes(block: int) -> bytes:
            if block < 0 or block >= size:
                fail(f"block address out of range: {block}")
            start = block * BSIZE
            return data[start : start + BSIZE]

        def read_inode(inum: int):
            if inum < 0 or inum >= ninodes:
                fail(f"inode number out of range: {inum}")
            block = inodestart + inum // IPB
            offset = (inum % IPB) * DINODE_SIZE
            kind, _major, _minor, nlink, file_size = struct.unpack_from(
                "<4hI", block_bytes(block), offset
            )
            addresses = struct.unpack_from(
                f"<{NDIRECT + 1}I", block_bytes(block), offset + 12
            )
            return kind, nlink, file_size, addresses

        metadata = {0, 1}
        metadata.update(range(logstart, logstart + nlog))
        metadata.update(range(inodestart, bmapstart))
        bitmap_blocks = (size + BSIZE * 8 - 1) // (BSIZE * 8)
        metadata.update(range(bmapstart, bmapstart + bitmap_blocks))
        if max(metadata) >= size:
            fail("filesystem metadata extends past the declared image size")

        referenced = set()
        inode_blocks = {}
        allocated_inodes = []

        def add_block(block: int, owner: int) -> None:
            if block == 0:
                return
            if block >= size:
                fail(f"inode {owner} references out-of-range block {block}")
            if block in referenced:
                fail(f"duplicate data-block reference to block {block}")
            referenced.add(block)

        for inum in range(1, ninodes):
            kind, nlink, file_size, addresses = read_inode(inum)
            if kind == 0:
                if nlink != 0 or file_size != 0 or any(addresses):
                    fail(f"free inode {inum} retains metadata or block addresses")
                continue
            if kind not in (T_DIR, T_FILE, T_DEV):
                fail(f"inode {inum} has unknown type {kind}")
            allocated_inodes.append((inum, kind, nlink, file_size))
            file_blocks = []
            for block in addresses[:NDIRECT]:
                if block:
                    add_block(block, inum)
                    file_blocks.append(block)
            indirect = addresses[NDIRECT]
            if indirect:
                add_block(indirect, inum)
                for block in struct.unpack(f"<{NINDIRECT}I", block_bytes(indirect)):
                    if block:
                        add_block(block, inum)
                        file_blocks.append(block)
            inode_blocks[inum] = file_blocks

        allocated = metadata | referenced
        for block in range(size):
            bitmap = block_bytes(bmapstart + block // (BSIZE * 8))
            is_set = bool(
                bitmap[(block % (BSIZE * 8)) // 8] & (1 << (block % 8))
            )
            if is_set != (block in allocated):
                state = "set for unreferenced" if is_set else "clear for referenced"
                fail(f"bitmap {state} block {block}")

        if read_inode(1)[0] != T_DIR:
            fail("root inode is missing or is not a directory")
        link_references = {}
        for inum, kind, _nlink, file_size in allocated_inodes:
            if kind != T_DIR:
                continue
            if file_size % 16:
                fail(f"directory inode {inum} has a partial dirent")
            directory_data = b"".join(
                block_bytes(block) for block in inode_blocks[inum]
            )
            for offset in range(0, min(len(directory_data), file_size), 16):
                entry_inum = struct.unpack_from("<H", directory_data, offset)[0]
                if entry_inum == 0:
                    continue
                if entry_inum >= ninodes or read_inode(entry_inum)[0] == 0:
                    fail(f"directory inode {inum} references free inode {entry_inum}")
                target_kind = read_inode(entry_inum)[0]
                if target_kind != T_DIR:
                    link_references[entry_inum] = link_references.get(entry_inum, 0) + 1
            if file_size > len(directory_data):
                fail(f"directory inode {inum} has missing data blocks")

        for inum, kind, nlink, _file_size in allocated_inodes:
            if kind != T_DIR and link_references.get(inum, 0) != nlink:
                fail(
                    f"inode {inum} link count mismatch: dinode={nlink}, "
                    f"directory references={link_references.get(inum, 0)}"
                )

    except (OSError, struct.error, ImageError) as error:
        print(f"filesystem consistency check failed: {error}", file=sys.stderr)
        return 1

    print(
        f"filesystem consistency PASS: {len(allocated_inodes)} allocated inodes, "
        f"{len(referenced)} referenced blocks, bitmap consistent"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
