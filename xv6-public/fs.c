// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"
#include "shadow.h"
#include "spec.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
#define FSBLOCKLOCKS 16
static int itrunc(struct inode*);
// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb; 

static struct sleeplock fsblocklock[FSBLOCKLOCKS];
static struct sleeplock fsalloclock;

static struct {
  struct spinlock lock;
  int readers;
  int publishing;
  int waiting;
} fsgate;

void
fs_gate_init(void)
{
  initlock(&fsgate.lock, "fsgate");
  fsgate.readers = 0;
  fsgate.publishing = 0;
  fsgate.waiting = 0;
}

void
fs_gate_enter(void)
{
  if(myproc() != 0 && myproc()->fs_gate_depth > 0){
    myproc()->fs_gate_depth++;
    return;
  }
  acquire(&fsgate.lock);
  while(fsgate.publishing || fsgate.waiting)
    sleep(&fsgate, &fsgate.lock);
  fsgate.readers++;
  release(&fsgate.lock);
  if(myproc() != 0)
    myproc()->fs_gate_depth = 1;
}

void
fs_gate_leave(void)
{
  if(myproc() != 0 && myproc()->fs_gate_depth > 1){
    myproc()->fs_gate_depth--;
    return;
  }
  acquire(&fsgate.lock);
  if(fsgate.readers <= 0)
    panic("fs_gate_leave");
  fsgate.readers--;
  if(fsgate.readers == 0)
    wakeup(&fsgate);
  release(&fsgate.lock);
  if(myproc() != 0)
    myproc()->fs_gate_depth = 0;
}

void
fs_gate_publish_begin(void)
{
  acquire(&fsgate.lock);
  fsgate.waiting = 1;
  while(fsgate.readers != 0 || fsgate.publishing)
    sleep(&fsgate, &fsgate.lock);
  fsgate.publishing = 1;
  fsgate.waiting = 0;
  release(&fsgate.lock);
}

void
fs_gate_publish_end(void)
{
  acquire(&fsgate.lock);
  if(!fsgate.publishing)
    panic("fs_gate_publish_end");
  fsgate.publishing = 0;
  wakeup(&fsgate);
  release(&fsgate.lock);
}

static void
fs_note_failure(int spec_id)
{
  if(myproc() == 0)
    return;
  myproc()->fs_error = 1;
  if(spec_id > 0 && myproc()->spec_id == spec_id)
    myproc()->spec_fault = 1;
}

static uint
fs_lock_index(uint dev, uint blockno)
{
  return (dev * 16777619U + blockno) % FSBLOCKLOCKS;
}

// Read into a caller-owned image. Speculative callers never receive a shared
// buffer-cache buffer, and a shadow miss falls back to the committed base.
static int
fs_read_image(int spec_id, uint dev, uint blockno, char *dst)
{
  struct buf *bp;
  int r;

  fs_gate_enter();
  if(spec_id > 0){
    if(shadow_note_read(spec_id, dev, blockno) < 0){
      fs_note_failure(spec_id);
      fs_gate_leave();
      return -1;
    }
    r = shadow_read_image(spec_id, dev, blockno, dst);
    if(r != 0){
      if(r < 0)
        fs_note_failure(spec_id);
      fs_gate_leave();
      return r < 0 ? -1 : 0;
    }
  }
  bp = bread(dev, blockno);
  memmove(dst, bp->data, BSIZE);
  brelse(bp);
  fs_gate_leave();
  return 0;
}

static int
fs_read_base_image(uint dev, uint blockno, char *dst)
{
  struct buf *bp;

  fs_gate_enter();
  bp = bread(dev, blockno);
  memmove(dst, bp->data, BSIZE);
  brelse(bp);
  fs_gate_leave();
  return 0;
}

static int
fs_write_image(int spec_id, uint dev, uint blockno, const char *src)
{
  struct buf *bp;

  if(spec_id > 0){
    char base[BSIZE];

    fs_gate_enter();
    if(fs_read_base_image(dev, blockno, base) < 0){
      fs_gate_leave();
      return -1;
    }
    {
      int result = shadow_write_versioned(spec_id, dev, blockno,
                                          shadow_base_version(dev, blockno),
                                          base, src);
      fs_gate_leave();
      return result;
    }
  }
  fs_gate_enter();
  bp = bread(dev, blockno);
  memmove(bp->data, src, BSIZE);
  log_write(bp);
  brelse(bp);
  shadow_note_base_write(dev, blockno);
  fs_gate_leave();
  return 0;
}

// Serialize full-image read/modify/write operations for blocks shared by
// different inode locks (notably inode-table blocks). The lock is sleepable;
// no spinlock is held over buffer-cache I/O.
static int
fs_update_range(int spec_id, uint dev, uint blockno, uint off,
                const char *src, uint n)
{
  char image[BSIZE];
  struct sleeplock *lk;
  int r;

  if(off > BSIZE || n > BSIZE - off || (n > 0 && src == 0))
    return -1;
  lk = &fsblocklock[fs_lock_index(dev, blockno)];
  acquiresleep(lk);
  r = fs_read_image(spec_id, dev, blockno, image);
  if(r == 0){
    memmove(image + off, src, n);
    r = fs_write_image(spec_id, dev, blockno, image);
  }
  releasesleep(lk);
  if(r < 0)
    fs_note_failure(spec_id);
  return r;
}

// Read the super block.
void
readsb(int dev, struct superblock *sb)
{
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Zero a block.
static int
bzero(int dev, int bno, int spec_id)
{
  char zero[BSIZE];

  memset(zero, 0, sizeof(zero));
  return fs_update_range(spec_id, dev, bno, 0, zero, BSIZE);
}

// Blocks.

// Allocate a zeroed disk block.
static uint
balloc(uint dev, int spec_id)
{
  int b, bi, m;
  char image[BSIZE];
  char base[BSIZE];
  struct spec_reservation_token token;
  uchar value;

  acquiresleep(&fsalloclock);
  for(b = 0; b < sb.size; b += BPB){
    if(fs_read_image(spec_id, dev, BBLOCK(b, sb), image) < 0){
      releasesleep(&fsalloclock);
      return 0;
    }
    for(bi = 0; bi < BPB && b + bi < sb.size; bi++){
      m = 1 << (bi % 8);
      if((image[bi/8] & m) == 0){  // Is block free in this view?
        if(spec_id > 0){
          if(fs_read_base_image(dev, BBLOCK(b, sb), base) < 0){
            releasesleep(&fsalloclock);
            return 0;
          }
          if((base[bi/8] & m) != 0 &&
             spec_reservation_state(spec_id, SPEC_RES_BLOCK, dev,
                                   b + bi) != SPEC_RES_DEFERRED_FREE)
            continue;
        }
        if(spec_reservation_claim(spec_id, SPEC_RES_BLOCK, dev, b + bi,
                                  SPEC_RES_ALLOCATED, &token) < 0)
          continue;
        value = image[bi/8] | m;
        if(fs_update_range(spec_id, dev, BBLOCK(b, sb), bi/8,
                           (char*)&value, sizeof(value)) < 0){
          spec_reservation_cancel(&token);
          releasesleep(&fsalloclock);
          return 0;
        }
        if(bzero(dev, b + bi, spec_id) < 0){
          value = image[bi/8] & ~m;
          if(fs_update_range(spec_id, dev, BBLOCK(b, sb), bi/8,
                             (char*)&value, sizeof(value)) < 0)
            fs_note_failure(spec_id);
          spec_reservation_cancel(&token);
          releasesleep(&fsalloclock);
          return 0;
        }
        spec_reservation_finish(&token);
        releasesleep(&fsalloclock);
        return b + bi;
      }
    }
  }
  releasesleep(&fsalloclock);
  return 0;
}

// Free a disk block while fsalloclock is held.
static int
bfree_locked(int dev, uint b, int spec_id)
{
  char image[BSIZE];
  int bi, m, result;
  uchar value;
  struct spec_reservation_token token;

  if(fs_read_image(spec_id, dev, BBLOCK(b, sb), image) < 0){
    return -1;
  }
  bi = b % BPB;
  m = 1 << (bi % 8);
  if((image[bi/8] & m) == 0){
    panic("freeing free block");
  }
  if(spec_reservation_claim(spec_id, SPEC_RES_BLOCK, dev, b,
                            SPEC_RES_DEFERRED_FREE, &token) < 0){
    return -1;
  }
  value = image[bi/8] & ~m;
  result = fs_update_range(spec_id, dev, BBLOCK(b, sb), bi/8,
                           (char*)&value, sizeof(value));
  if(result < 0)
    spec_reservation_cancel(&token);
  else
    spec_reservation_finish(&token);
  return result;
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at
// sb.startinode. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a cache of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The cached
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in cache: an entry in the inode cache
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a cache entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   cache entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays cached and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The icache.lock spin-lock protects the allocation of icache
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold icache.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} icache;

void
iinit(int dev)
{
  int i = 0;
  
  initlock(&icache.lock, "icache");
  for(i = 0; i < NINODE; i++) {
    initsleeplock(&icache.inode[i].lock, "inode");
  }
  for(i = 0; i < FSBLOCKLOCKS; i++)
    initsleeplock(&fsblocklock[i], "fsblock");
  initsleeplock(&fsalloclock, "fsalloc");

  readsb(dev, &sb);
  cprintf("sb: size %d nblocks %d ninodes %d nlog %d logstart %d\
 inodestart %d bmap start %d\n", sb.size, sb.nblocks,
          sb.ninodes, sb.nlog, sb.logstart, sb.inodestart,
          sb.bmapstart);
}

static struct inode* iget(uint dev, uint inum, int spec_id);

int
fs_inode_free_allowed(struct inode *ip)
{
  int i, allowed;
  char image[BSIZE];
  uint a[NINDIRECT];

  if(ip == 0)
    return 0;
  memset(a, 0, sizeof(a));
  acquiresleep(&fsalloclock);
  if(ip->addrs[NDIRECT] &&
     fs_read_image(ip->spec_id, ip->dev, ip->addrs[NDIRECT], image) < 0){
    releasesleep(&fsalloclock);
    return 0;
  }
  if(ip->addrs[NDIRECT])
    memmove(a, image, sizeof(a));
  allowed = 1;
  for(i = 0; i < NDIRECT; i++)
    if(ip->addrs[i] && !spec_reservation_check(ip->spec_id,
         SPEC_RES_BLOCK, ip->dev, ip->addrs[i]))
      allowed = 0;
  for(i = 0; i < NINDIRECT; i++)
    if(a[i] && !spec_reservation_check(ip->spec_id,
         SPEC_RES_BLOCK, ip->dev, a[i]))
      allowed = 0;
  if(ip->addrs[NDIRECT] && !spec_reservation_check(ip->spec_id,
       SPEC_RES_BLOCK, ip->dev, ip->addrs[NDIRECT]))
    allowed = 0;
  releasesleep(&fsalloclock);
  return allowed;
}

static int
base_inode_referenced_locked(uint dev, uint inum)
{
  struct inode *ip;

  for(ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++)
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum &&
       ip->spec_id == 0)
      return 1;
  return 0;
}

//PAGEBREAK!
// Allocate an inode on device dev.
// Mark it as allocated by  giving it type type.
// Returns an unlocked but allocated and referenced inode.
struct inode*
ialloc(uint dev, short type)
{
  int inum;
  int spec_id;
  char image[BSIZE];
  char base[BSIZE];
  struct dinode *dip;
  struct dinode newdi;
  struct spec_reservation_token token;

  spec_id = myproc()->spec_id;
  acquiresleep(&fsalloclock);
  for(inum = 1; inum < sb.ninodes; inum++){
    if(fs_read_image(spec_id, dev, IBLOCK(inum, sb), image) < 0){
      releasesleep(&fsalloclock);
      return 0;
    }
    dip = (struct dinode*)image + inum%IPB;
    if(dip->type == 0){  // a free inode
      if(spec_id > 0){
        if(fs_read_base_image(dev, IBLOCK(inum, sb), base) < 0){
          releasesleep(&fsalloclock);
          return 0;
        }
        dip = (struct dinode*)base + inum%IPB;
        if(dip->type != 0 &&
           spec_reservation_state(spec_id, SPEC_RES_INODE, dev, inum) !=
             SPEC_RES_DEFERRED_FREE)
          continue;
      }
      if(spec_reservation_claim(spec_id, SPEC_RES_INODE, dev, inum,
                                SPEC_RES_ALLOCATED, &token) < 0)
        continue;
      memset(&newdi, 0, sizeof(newdi));
      newdi.type = type;
      if(fs_update_range(spec_id, dev, IBLOCK(inum, sb),
                         (inum % IPB) * sizeof(newdi),
                         (char*)&newdi, sizeof(newdi)) < 0){
        spec_reservation_cancel(&token);
        releasesleep(&fsalloclock);
        return 0;
      }
      spec_reservation_finish(&token);
      releasesleep(&fsalloclock);
      return iget(dev, inum, spec_id);
    }
  }
  releasesleep(&fsalloclock);
  return 0;
}

// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk, since i-node cache is write-through.
// Caller must hold ip->lock.
int
iupdate(struct inode *ip)
{
  struct dinode di;
  struct spec_reservation_token token;
  int reserved, result;

  memset(&di, 0, sizeof(di));
  di.type = ip->type;
  di.major = ip->major;
  di.minor = ip->minor;
  di.nlink = ip->nlink;
  di.size = ip->size;
  memmove(di.addrs, ip->addrs, sizeof(ip->addrs));
  reserved = 0;
  if(ip->type == 0){
    acquiresleep(&fsalloclock);
    if(spec_reservation_claim(ip->spec_id, SPEC_RES_INODE, ip->dev,
                              ip->inum, SPEC_RES_DEFERRED_FREE,
                              &token) < 0){
      fs_note_failure(ip->spec_id);
      releasesleep(&fsalloclock);
      return -1;
    }
    reserved = 1;
  }
  result = fs_update_range(ip->spec_id, ip->dev, IBLOCK(ip->inum, sb),
                           (ip->inum % IPB) * sizeof(di),
                           (char*)&di, sizeof(di));
  if(result < 0){
    fs_note_failure(ip->spec_id);
    if(reserved)
      spec_reservation_cancel(&token);
  } else if(reserved){
    spec_reservation_finish(&token);
  }
  if(reserved)
    releasesleep(&fsalloclock);
  return result;
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
static struct inode*
iget(uint dev, uint inum, int spec_id)
{
  struct inode *ip, *empty;

  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for(ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum &&
       ip->spec_id == spec_id){
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if(empty == 0 && ip->ref == 0)    // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode cache entry.
  if(empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->spec_id = spec_id;
  ip->spec_dead = 0;
  ip->ref = 1;
  ip->valid = 0;
  release(&icache.lock);

  return ip;
}

struct inode*
iget_context(uint dev, uint inum, int spec_id)
{
  return iget(dev, inum, spec_id);
}

void
fs_spec_discard(int spec_id)
{
  struct inode *ip;
  int i;

  for(i = 0; i < NINODE; i++){
    ip = &icache.inode[i];
    acquire(&icache.lock);
    if(ip->ref > 0 && ip->spec_id == spec_id)
      ip->ref++;
    else
      ip = 0;
    release(&icache.lock);
    if(ip != 0){
      acquiresleep(&ip->lock);
      ip->spec_dead = 1;
      releasesleep(&ip->lock);
      acquire(&icache.lock);
      ip->ref--;
      release(&icache.lock);
    }
  }
}

// The journal has installed real inode-table blocks. Drop speculative views
// and invalidate cached base inodes so ordinary lookups reload new metadata.
void
fs_spec_publish(int spec_id)
{
  struct inode *ip;
  int i;

  fs_spec_discard(spec_id);
  for(i = 0; i < NINODE; i++){
    ip = &icache.inode[i];
    acquire(&icache.lock);
    if(ip->ref > 0 && ip->spec_id == 0)
      ip->ref++;
    else
      ip = 0;
    release(&icache.lock);
    if(ip != 0){
      acquiresleep(&ip->lock);
      ip->valid = 0;
      releasesleep(&ip->lock);
      acquire(&icache.lock);
      ip->ref--;
      release(&icache.lock);
    }
  }
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode*
idup(struct inode *ip)
{
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void
ilock(struct inode *ip)
{
  char image[BSIZE];
  struct dinode *dip;

  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  acquiresleep(&ip->lock);

  if(ip->spec_dead){
    releasesleep(&ip->lock);
    panic("ilock: discarded view");
  }

  if(ip->valid == 0){
    if(fs_read_image(ip->spec_id, ip->dev, IBLOCK(ip->inum, sb), image) < 0){
      releasesleep(&ip->lock);
      panic("ilock: block view failed");
    }
    dip = (struct dinode*)image + ip->inum%IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    ip->valid = 1;
    if(ip->type == 0)
      panic("ilock: no type");
  }
}

// Unlock the given inode.
void
iunlock(struct inode *ip)
{
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");

  releasesleep(&ip->lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode cache entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
void
iput(struct inode *ip)
{
  int r, baseheld;
  short oldtype;

  acquiresleep(&ip->lock);
  if(!ip->spec_dead && ip->valid && ip->nlink == 0){
    acquire(&icache.lock);
    r = ip->ref;
    baseheld = ip->spec_id > 0 &&
               base_inode_referenced_locked(ip->dev, ip->inum);
    release(&icache.lock);
    if(r == 1 && !baseheld){
      // inode has no links and no other references: truncate and free.
      if(itrunc(ip) == 0){
        oldtype = ip->type;
        ip->type = 0;
        if(iupdate(ip) == 0)
          ip->valid = 0;
        else
          ip->type = oldtype;
      }
    }
  }
  releasesleep(&ip->lock);

  acquire(&icache.lock);
  ip->ref--;
  release(&icache.lock);
}

// Common idiom: unlock, then put.
void
iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

//PAGEBREAK!
// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  char image[BSIZE];

  if(bn < NDIRECT){
    if((addr = ip->addrs[bn]) == 0){
      ip->addrs[bn] = addr = balloc(ip->dev, ip->spec_id);
      if(addr == 0)
        fs_note_failure(ip->spec_id);
    }
    return addr;
  }
  bn -= NDIRECT;

  if(bn < NINDIRECT){
    // Load indirect block, allocating if necessary.
    if((addr = ip->addrs[NDIRECT]) == 0){
      ip->addrs[NDIRECT] = addr = balloc(ip->dev, ip->spec_id);
      if(addr == 0)
        fs_note_failure(ip->spec_id);
    }
    if(addr == 0 || fs_read_image(ip->spec_id, ip->dev, addr, image) < 0)
      return 0;
    a = (uint*)image;
    if((addr = a[bn]) == 0){
      a[bn] = addr = balloc(ip->dev, ip->spec_id);
      if(addr == 0){
        fs_note_failure(ip->spec_id);
        return 0;
      }
      if(fs_update_range(ip->spec_id, ip->dev,
                                    ip->addrs[NDIRECT], bn*sizeof(uint),
                                    (char*)&addr, sizeof(addr)) < 0)
        return 0;
    }
    return addr;
  }

  panic("bmap: out of range");
}

// Truncate inode (discard contents).
// Only called when the inode has no links
// to it (no directory entries referring to it)
// and has no in-memory reference to it (is
// not an open file or current directory).
static int
itrunc(struct inode *ip)
{
  int i, j;
  char image[BSIZE];
  uint a[NINDIRECT];

  memset(a, 0, sizeof(a));
  acquiresleep(&fsalloclock);
  if(ip->addrs[NDIRECT] &&
     fs_read_image(ip->spec_id, ip->dev, ip->addrs[NDIRECT], image) < 0){
    fs_note_failure(ip->spec_id);
    releasesleep(&fsalloclock);
    return -1;
  }
  if(ip->addrs[NDIRECT])
    memmove(a, image, sizeof(a));

  // Preflight all frees before changing any bitmap image. This prevents an
  // ordinary or other-context truncation from partially freeing a set when
  // one resource is already reserved elsewhere.
  for(i = 0; i < NDIRECT; i++)
    if(ip->addrs[i] && !spec_reservation_check(ip->spec_id, SPEC_RES_BLOCK,
                                                ip->dev, ip->addrs[i])){
      fs_note_failure(ip->spec_id);
      releasesleep(&fsalloclock);
      return -1;
    }
  for(j = 0; j < NINDIRECT; j++)
    if(a[j] && !spec_reservation_check(ip->spec_id, SPEC_RES_BLOCK,
                                       ip->dev, a[j])){
      fs_note_failure(ip->spec_id);
      releasesleep(&fsalloclock);
      return -1;
    }
  if(ip->addrs[NDIRECT] &&
     !spec_reservation_check(ip->spec_id, SPEC_RES_BLOCK, ip->dev,
                             ip->addrs[NDIRECT])){
    fs_note_failure(ip->spec_id);
    releasesleep(&fsalloclock);
    return -1;
  }

  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      if(bfree_locked(ip->dev, ip->addrs[i], ip->spec_id) < 0){
        fs_note_failure(ip->spec_id);
        releasesleep(&fsalloclock);
        return -1;
      }
      ip->addrs[i] = 0;
    }
  }

  if(ip->addrs[NDIRECT]){
    for(j = 0; j < NINDIRECT; j++){
      if(a[j]){
        if(bfree_locked(ip->dev, a[j], ip->spec_id) < 0){
          fs_note_failure(ip->spec_id);
          releasesleep(&fsalloclock);
          return -1;
        }
      }
    }
    if(bfree_locked(ip->dev, ip->addrs[NDIRECT], ip->spec_id) < 0){
      fs_note_failure(ip->spec_id);
      releasesleep(&fsalloclock);
      return -1;
    }
    ip->addrs[NDIRECT] = 0;
  }
  releasesleep(&fsalloclock);

  ip->size = 0;
  if(iupdate(ip) < 0)
    return -1;
  return 0;
}

// Copy stat information from inode.
// Caller must hold ip->lock.
void
stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

//PAGEBREAK!
// Read data from inode.
// Caller must hold ip->lock.
int
readi(struct inode *ip, char *dst, uint off, uint n)
{
  uint tot, m;
  char image[BSIZE];
  uint addr;

  if(ip->type == T_DEV){
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].read)
      return -1;
    return devsw[ip->major].read(ip, dst, n);
  }

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > ip->size)
    n = ip->size - off;

  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    addr = bmap(ip, off/BSIZE);
    if(addr == 0 || fs_read_image(ip->spec_id, ip->dev, addr, image) < 0){
      fs_note_failure(ip->spec_id);
      return -1;
    }
    m = min(n - tot, BSIZE - off%BSIZE);
    memmove(dst, image + off%BSIZE, m);
  }
  return n;
}

// PAGEBREAK!
// Write data to inode.
// Caller must hold ip->lock.
int
writei(struct inode *ip, char *src, uint off, uint n)
{
  uint tot, m;
  uint addr;

  if(ip->type == T_DEV){
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].write)
      return -1;
    return devsw[ip->major].write(ip, src, n);
  }

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > MAXFILE*BSIZE)
    return -1;

  for(tot=0; tot<n; tot+=m, off+=m, src+=m){
    addr = bmap(ip, off/BSIZE);
    if(addr == 0){
      fs_note_failure(ip->spec_id);
      return -1;
    }
    m = min(n - tot, BSIZE - off%BSIZE);
    if(fs_update_range(ip->spec_id, ip->dev, addr, off%BSIZE,
                       src, m) < 0)
      return -1;
  }

  if(n > 0 && off > ip->size){
    ip->size = off;
    iupdate(ip);
  }
  return n;
}

//PAGEBREAK!
// Directories

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode*
dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de;

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");

  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      return 0;
    if(de.inum == 0)
      continue;
    if(namecmp(name, de.name) == 0){
      // entry matches path element
      if(poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum, dp->spec_id);
    }
  }

  return 0;
}

// Write a new directory entry (name, inum) into the directory dp.
int
dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  // Check that name is not present.
  if((ip = dirlookup(dp, name, 0)) != 0){
    iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      return -1;
    if(de.inum == 0)
      break;
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    return -1;

  return 0;
}

//PAGEBREAK!
// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char*
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;
  struct proc *p;
  int spec_id;

  p = myproc();
  spec_id = p == 0 ? 0 : p->spec_id;

  if(*path == '/')
    ip = iget(ROOTDEV, ROOTINO, spec_id);
  else
    ip = iget(myproc()->cwd->dev, myproc()->cwd->inum,
              spec_id);

  while((path = skipelem(path, name)) != 0){
    ilock(ip);
    if(ip->type != T_DIR){
      iunlockput(ip);
      return 0;
    }
    if(nameiparent && *path == '\0'){
      // Stop one level early.
      iunlock(ip);
      return ip;
    }
    if((next = dirlookup(ip, name, 0)) == 0){
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = next;
  }
  if(nameiparent){
    iput(ip);
    return 0;
  }
  return ip;
}

struct inode*
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode*
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}
