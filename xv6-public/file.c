//
// File descriptors
//

#include "types.h"
#include "defs.h"
#include "param.h"
#include "mmu.h"
#include "proc.h"
#include "spec.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"

struct devsw devsw[NDEV];
struct {
  struct spinlock lock;
  struct file file[NFILE];
} ftable;

void
fileinit(void)
{
  initlock(&ftable.lock, "ftable");
}

// Allocate a file structure.
struct file*
filealloc(void)
{
  struct file *f;

  acquire(&ftable.lock);
  for(f = ftable.file; f < ftable.file + NFILE; f++){
    if(f->ref == 0){
      f->ref = 1;
      f->spec_id = 0;
      release(&ftable.lock);
      return f;
    }
  }
  release(&ftable.lock);
  return 0;
}

// Increment ref count for file f.
struct file*
filedup(struct file *f)
{
  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("filedup");
  f->ref++;
  release(&ftable.lock);
  return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void
fileclose(struct file *f)
{
  struct file ff;

  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("fileclose");
  if(--f->ref > 0){
    release(&ftable.lock);
    return;
  }
  ff = *f;
  f->ref = 0;
  f->type = FD_NONE;
  f->spec_id = 0;
  release(&ftable.lock);

  if(ff.type == FD_PIPE)
    pipeclose(ff.pipe, ff.writable);
  else if(ff.type == FD_INODE){
    begin_op();
    iput(ff.ip);
    end_op();
  }
  if(ff.spec_id > 0)
    spec_file_detach(ff.spec_id);
}

// Retag open file objects when a nested context merges into its parent.
// file_refs count allocated file objects, not descriptor duplicates.
int
file_context_merge(int from_id, int to_id)
{
  struct file *f;
  int moved;

  moved = 0;
  acquire(&ftable.lock);
  for(f = ftable.file; f < ftable.file + NFILE; f++)
    if(f->ref > 0 && f->spec_id == from_id){
      f->spec_id = to_id;
      moved++;
    }
  release(&ftable.lock);
  return moved;
}

static int
file_context_valid(struct file *f, int spec_id)
{
  if(f->spec_id == 0 || f->spec_id == spec_id)
    return 1;
  if(spec_context_visible(f->spec_id, spec_id))
    return 1;
  // Empty-context commits may detach a handle into the identical base view.
  // Aborted or still-active handles cannot be used from another context.
  return spec_context_state(f->spec_id) == SPEC_COMMITTED;
}

// Get metadata about file f.
int
filestat(struct file *f, struct stat *st)
{
  return filestat_context(f, st, myproc()->spec_id);
}

int
filestat_context(struct file *f, struct stat *st, int spec_id)
{
  struct inode *ip;

  if(f->type == FD_INODE){
    if(!file_context_valid(f, spec_id))
      return -1;
    ip = f->ip;
    if(ip->spec_id != spec_id)
      ip = iget_context(ip->dev, ip->inum, spec_id);
    if(ip == 0)
      return -1;
    ilock(ip);
    stati(ip, st);
    iunlock(ip);
    if(ip != f->ip)
      iput(ip);
    return 0;
  }
  return -1;
}

// Read from file f.
int
fileread(struct file *f, char *addr, int n)
{
  return fileread_context(f, addr, n, myproc()->spec_id);
}

int
fileread_context(struct file *f, char *addr, int n, int spec_id)
{
  int r;
  struct inode *ip;

  if(f->readable == 0)
    return -1;
  if(f->type == FD_PIPE)
    return piperead(f->pipe, addr, n);
  if(f->type == FD_INODE){
    if(!file_context_valid(f, spec_id))
      return -1;
    ip = f->ip;
    if(ip->spec_id != spec_id)
      ip = iget_context(ip->dev, ip->inum, spec_id);
    if(ip == 0)
      return -1;
    ilock(ip);
    if((r = readi(ip, addr, f->off, n)) > 0)
      f->off += r;
    iunlock(ip);
    if(ip != f->ip)
      iput(ip);
    return r;
  }
  panic("fileread");
}

//PAGEBREAK!
// Write to file f.
int
filewrite(struct file *f, char *addr, int n)
{
  return filewrite_context(f, addr, n, myproc()->spec_id);
}

int
filewrite_context(struct file *f, char *addr, int n, int spec_id)
{
  int r;
  struct inode *ip;

  if(f->writable == 0)
    return -1;
  if(f->type == FD_PIPE)
    return pipewrite(f->pipe, addr, n);
  if(f->type == FD_INODE){
    if(!file_context_valid(f, spec_id))
      return -1;
    ip = f->ip;
    if(ip->spec_id != spec_id)
      ip = iget_context(ip->dev, ip->inum, spec_id);
    if(ip == 0)
      return -1;
    // write a few blocks at a time to avoid exceeding
    // the maximum log transaction size, including
    // i-node, indirect block, allocation blocks,
    // and 2 blocks of slop for non-aligned writes.
    // this really belongs lower down, since writei()
    // might be writing a device like the console.
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * 512;
    int i = 0;
    while(i < n){
      int n1 = n - i;
      if(n1 > max)
        n1 = max;

      if(spec_id == 0)
        begin_op();
      ilock(ip);
      if ((r = writei(ip, addr + i, f->off, n1)) > 0)
        f->off += r;
      iunlock(ip);
      if(spec_id == 0)
        end_op();

      if(r < 0){
        if(spec_id > 0)
          myproc()->spec_fault = 1;
        break;
      }
      if(r != n1)
        panic("short filewrite");
      i += r;
    }
    if(ip != f->ip)
      iput(ip);
    return i == n ? n : -1;
  }
  panic("filewrite");
}

