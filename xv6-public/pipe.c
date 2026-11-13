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

#define PIPESIZE 512

struct pipe {
  struct spinlock lock;
  char data[PIPESIZE];
  int spec[PIPESIZE];
  uint nread;     // number of bytes read
  uint nwrite;    // number of bytes written
  int readopen;   // read fd is still open
  int writeopen;  // write fd is still open
  struct pipe *next;
};

static struct {
  struct spinlock lock;
  struct pipe *head;
} pipes;

void
pipeinit(void)
{
  initlock(&pipes.lock, "pipes");
  pipes.head = 0;
}

int
pipealloc(struct file **f0, struct file **f1)
{
  struct pipe *p;

  p = 0;
  *f0 = *f1 = 0;
  if((*f0 = filealloc()) == 0 || (*f1 = filealloc()) == 0)
    goto bad;
  if((p = (struct pipe*)kalloc()) == 0)
    goto bad;
  p->readopen = 1;
  p->writeopen = 1;
  p->nwrite = 0;
  p->nread = 0;
  memset(p->spec, 0, sizeof(p->spec));
  initlock(&p->lock, "pipe");
  acquire(&pipes.lock);
  p->next = pipes.head;
  pipes.head = p;
  release(&pipes.lock);
  (*f0)->type = FD_PIPE;
  (*f0)->readable = 1;
  (*f0)->writable = 0;
  (*f0)->pipe = p;
  (*f1)->type = FD_PIPE;
  (*f1)->readable = 0;
  (*f1)->writable = 1;
  (*f1)->pipe = p;
  return 0;

//PAGEBREAK: 20
 bad:
  if(p)
    kfree((char*)p);
  if(*f0)
    fileclose(*f0);
  if(*f1)
    fileclose(*f1);
  return -1;
}

void
pipeclose(struct pipe *p, int writable)
{
  struct pipe **link;
  int freepipe;

  acquire(&pipes.lock);
  acquire(&p->lock);
  if(writable){
    p->writeopen = 0;
    wakeup(&p->nread);
  } else {
    p->readopen = 0;
    wakeup(&p->nwrite);
  }
  if(p->readopen == 0 && p->writeopen == 0){
    release(&p->lock);
    for(link = &pipes.head; *link && *link != p; link = &(*link)->next)
      ;
    if(*link == p)
      *link = p->next;
    freepipe = 1;
  } else
    freepipe = 0;
  if(!freepipe)
    release(&p->lock);
  release(&pipes.lock);
  if(freepipe)
    kfree((char*)p);
}

//PAGEBREAK: 40
int
pipewrite(struct pipe *p, char *addr, int n)
{
  int i;
  int specid;

  specid = myproc()->spec_id;
  acquire(&p->lock);
  for(i = 0; i < n; i++){
    // Do not accept bytes after the final reader has closed.  In a
    // speculative write, return the short count so the caller can abort
    // without publishing output to a pipe nobody can consume.
    if(p->readopen == 0 || myproc()->killed){
      if(i > 0)
        wakeup(&p->nread);
      release(&p->lock);
      return specid != 0 ? i : -1;
    }
    while(p->nwrite == p->nread + PIPESIZE){  //DOC: pipewrite-full
      if(p->readopen == 0 || myproc()->killed){
        if(i > 0)
          wakeup(&p->nread);
        release(&p->lock);
        return specid != 0 ? i : -1;
      }
      if(specid != 0){
        if(i > 0)
          wakeup(&p->nread);
        release(&p->lock);
        return i;
      }
      wakeup(&p->nread);
      sleep(&p->nwrite, &p->lock);  //DOC: pipewrite-sleep
    }
    p->data[p->nwrite % PIPESIZE] = addr[i];
    p->spec[p->nwrite++ % PIPESIZE] = specid;
  }
  wakeup(&p->nread);  //DOC: pipewrite-wakeup1
  release(&p->lock);
  return n;
}

int
piperead(struct pipe *p, char *addr, int n)
{
  int i;
  int specid;
  int tag;

  specid = myproc()->spec_id;
  acquire(&p->lock);
  for(;;){
    if(p->nread != p->nwrite){
      tag = p->spec[p->nread % PIPESIZE];
      if(tag == 0 || tag == specid)
        break;
    } else if(!p->writeopen){
      break;
    }
    if(myproc()->killed){
      release(&p->lock);
      return -1;
    }
    // A terminal lifecycle transition must be able to drain this operation
    // reference even if its producer remains open and no data will arrive.
    if(specid != 0 && spec_context_state(specid) != SPEC_ACTIVE){
      release(&p->lock);
      return -1;
    }
    sleep(&p->nread, &p->lock); //DOC: piperead-sleep
  }
  for(i = 0; i < n; i++){  //DOC: piperead-copy
    if(p->nread == p->nwrite ||
       (p->spec[p->nread % PIPESIZE] != 0 &&
        p->spec[p->nread % PIPESIZE] != specid))
      break;
    addr[i] = p->data[p->nread++ % PIPESIZE];
  }
  wakeup(&p->nwrite);  //DOC: piperead-wakeup
  if(i > 0)
    wakeup(&p->nread);
  release(&p->lock);
  return i;
}

static void
pipe_spec_finish(int specid, int publish)
{
  struct pipe *p;
  uint pos, kept, dst;

  acquire(&pipes.lock);
  for(p = pipes.head; p; p = p->next){
    acquire(&p->lock);
    kept = 0;
    for(pos = p->nread; pos != p->nwrite; pos++){
      uint src = pos % PIPESIZE;
      int tag = p->spec[src];
      if(tag == specid){
        if(!publish)
          continue;
        p->spec[src] = 0;
      }
      dst = (p->nread + kept) % PIPESIZE;
      if(dst != src){
        p->data[dst] = p->data[src];
        p->spec[dst] = p->spec[src];
      }
      kept++;
    }
    p->nwrite = p->nread + kept;
    wakeup(&p->nread);
    wakeup(&p->nwrite);
    release(&p->lock);
  }
  release(&pipes.lock);
}

void
pipe_output_publish(int specid)
{
  pipe_spec_finish(specid, 1);
}

void
pipe_output_discard(int specid)
{
  pipe_spec_finish(specid, 0);
}

void
pipe_output_merge(int childid, int parentid)
{
  struct pipe *p;
  uint pos;

  acquire(&pipes.lock);
  for(p = pipes.head; p; p = p->next){
    acquire(&p->lock);
    for(pos = p->nread; pos != p->nwrite; pos++)
      if(p->spec[pos % PIPESIZE] == childid)
        p->spec[pos % PIPESIZE] = parentid;
    wakeup(&p->nread);
    release(&p->lock);
  }
  release(&pipes.lock);
}

// Wake potentially blocked speculative readers before commit/abort drains
// their operation references. Readers recheck their context after waking.
void
pipe_context_wakeup(int specid)
{
  struct pipe *p;

  (void)specid;
  acquire(&pipes.lock);
  for(p = pipes.head; p; p = p->next){
    acquire(&p->lock);
    wakeup(&p->nread);
    release(&p->lock);
  }
  release(&pipes.lock);
}
