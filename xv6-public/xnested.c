#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

static void
fail(char *message)
{
  // Failure diagnostics must escape any active speculative console buffer.
  while(xabort() == 0)
    ;
  printf(2, "XNESTED FAIL: %s\n", message);
  exit();
}

static int
write_byte(char *path, char value)
{
  int fd, result;

  fd = open(path, O_CREATE|O_RDWR);
  if(fd < 0)
    return -1;
  result = write(fd, &value, 1);
  if(close(fd) < 0 || result != 1)
    return -1;
  return 0;
}

static int
read_byte(char *path, char *value)
{
  int fd, result;

  fd = open(path, O_RDONLY);
  if(fd < 0)
    return -1;
  result = read(fd, value, 1);
  if(close(fd) < 0 || result != 1)
    return -1;
  return 0;
}

int
main(void)
{
  int fd, pid;
  char value;

  // A grandchild merge must remain private to the outer transaction, and
  // aborting its parent must discard both nested levels.
  if(xbegin() <= 0 || write_byte("xnested-data", 'A') < 0 ||
     xbegin() <= 0 || write_byte("xnested-data", 'B') < 0 ||
     xbegin() <= 0 || write_byte("xnested-data", 'C') < 0 ||
     xcommit() < 0 || read_byte("xnested-data", &value) < 0 || value != 'C' ||
     xabort() < 0 || read_byte("xnested-data", &value) < 0 || value != 'A' ||
     xcommit() < 0 || read_byte("xnested-data", &value) < 0 || value != 'A')
    fail("nested commit and selective abort");
  if(unlink("xnested-data") < 0)
    fail("cleanup data file");

  // A nested commit merges into its parent; only the outer commit makes the
  // merged image visible to the base filesystem.
  if(xbegin() <= 0 || write_byte("xnested-merge", 'A') < 0 ||
     xbegin() <= 0 || write_byte("xnested-merge", 'B') < 0 ||
     xcommit() < 0 || read_byte("xnested-merge", &value) < 0 || value != 'B' ||
     xcommit() < 0 || read_byte("xnested-merge", &value) < 0 || value != 'B')
    fail("nested merge publication");
  if(unlink("xnested-merge") < 0)
    fail("cleanup merged file");

  // Child-created namespace entries and their allocation reservations move
  // into the parent together with the child shadow blocks.
  if(xbegin() <= 0 || xbegin() <= 0 ||
     write_byte("xnested-created", 'N') < 0 || xcommit() < 0 ||
     read_byte("xnested-created", &value) < 0 || value != 'N' ||
     xcommit() < 0 || read_byte("xnested-created", &value) < 0 || value != 'N')
    fail("nested create reservation merge");
  if(unlink("xnested-created") < 0)
    fail("cleanup nested create");

  // Forked members of a nested scope share its overlay; their writes merge
  // with the nested context when the parent commits it.
  if(xbegin() <= 0 || write_byte("xnested-fork", 'A') < 0 || xbegin() <= 0)
    fail("fork setup");
  pid = fork();
  if(pid < 0)
    fail("fork");
  if(pid == 0){
    if(write_byte("xnested-fork", 'F') < 0)
      exit();
    exit();
  }
  if(wait() != pid || read_byte("xnested-fork", &value) < 0 || value != 'F' ||
     xcommit() < 0 || xcommit() < 0 ||
     read_byte("xnested-fork", &value) < 0 || value != 'F')
    fail("forked nested participant");
  if(unlink("xnested-fork") < 0)
    fail("cleanup fork file");

  // If the owner of an inner scope exits without ending it, the inner scope
  // aborts and the still-running parent transaction remains usable.
  if(xbegin() <= 0 || write_byte("xnested-owner-exit", 'P') < 0)
    fail("inner owner exit setup");
  pid = fork();
  if(pid < 0)
    fail("inner owner exit fork");
  if(pid == 0){
    if(xbegin() <= 0 || write_byte("xnested-owner-exit", 'X') < 0)
      exit();
    exit();
  }
  if(wait() != pid || read_byte("xnested-owner-exit", &value) < 0 ||
     value != 'P' || xcommit() < 0 ||
     read_byte("xnested-owner-exit", &value) < 0 || value != 'P')
    fail("inner owner exit cleanup");
  if(unlink("xnested-owner-exit") < 0)
    fail("cleanup inner owner exit");

  // An aborted child scope must not leak a create or overwrite into parent.
  if(xbegin() <= 0 || write_byte("xnested-abort", 'P') < 0 ||
     xbegin() <= 0 || write_byte("xnested-abort", 'X') < 0 ||
     write_byte("xnested-private", 'X') < 0 || xabort() < 0 ||
     read_byte("xnested-abort", &value) < 0 || value != 'P' ||
     open("xnested-private", O_RDONLY) >= 0 || xcommit() < 0 ||
     read_byte("xnested-abort", &value) < 0 || value != 'P')
    fail("nested abort isolation");
  if(unlink("xnested-abort") < 0)
    fail("cleanup aborted file");

  // Commit/abort calls at the wrong nesting level remain invalid.
  if(xcommit() >= 0 || xabort() >= 0)
    fail("invalid terminal calls");
  fd = open("xnested-private", O_RDONLY);
  if(fd >= 0){
    close(fd);
    fail("aborted create escaped");
  }
  printf(1, "XNESTED PASS (nested merge and abort)\n");
  exit();
}
