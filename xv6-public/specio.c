#include "types.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "specio.h"

#define SPEC_CONSOLE_CAP 2048

static struct {
  struct spinlock lock;
  int id[16];
  uint len[16];
  char data[16][SPEC_CONSOLE_CAP];
} output;

void
specioinit(void)
{
  memset(&output, 0, sizeof(output));
  initlock(&output.lock, "spec-output");
}

int
spec_console_write(int id, const char *data, int n)
{
  int i, free_slot;

  if(id <= 0 || n < 0 || n > SPEC_CONSOLE_CAP)
    return -1;
  acquire(&output.lock);
  free_slot = -1;
  for(i = 0; i < NELEM(output.id); i++){
    if(output.id[i] == id)
      break;
    if(output.id[i] == 0 && free_slot < 0)
      free_slot = i;
  }
  if(i == NELEM(output.id))
    i = free_slot < 0 ? NELEM(output.id) : free_slot;
  if(i == NELEM(output.id) || output.len[i] + n > SPEC_CONSOLE_CAP){
    release(&output.lock);
    return -1;
  }
  output.id[i] = id;
  memmove(output.data[i] + output.len[i], data, n);
  output.len[i] += n;
  release(&output.lock);
  return n;
}

void
spec_output_publish(int id)
{
  char data[SPEC_CONSOLE_CAP];
  int i, n;

  acquire(&output.lock);
  for(i = 0; i < NELEM(output.id); i++)
    if(output.id[i] == id)
      break;
  n = i == NELEM(output.id) ? 0 : output.len[i];
  if(n)
    memmove(data, output.data[i], n);
  if(i != NELEM(output.id)){
    output.id[i] = 0;
    output.len[i] = 0;
  }
  release(&output.lock);

  if(n)
    console_output_publish(data, n);
  pipe_output_publish(id);
}

void
spec_output_discard(int id)
{
  int i;

  acquire(&output.lock);
  for(i = 0; i < NELEM(output.id); i++)
    if(output.id[i] == id){
      output.id[i] = 0;
      output.len[i] = 0;
      break;
    }
  release(&output.lock);
  pipe_output_discard(id);
}
