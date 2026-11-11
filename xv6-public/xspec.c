#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

static char bulk[20000];

static int
is_priv(char *data)
{
  return data[0] == 'p' && data[1] == 'r' &&
         data[2] == 'i' && data[3] == 'v';
}

static int
is_spec(char *data)
{
  return data[0] == 's' && data[1] == 'p' &&
         data[2] == 'e' && data[3] == 'c';
}

static void
fail(char *what)
{
  // Make cleanup possible before reporting a failure from an active context.
  xabort();
  printf(2, "XSPEC FAIL: %s\n", what);
  exit();
}

static int
probe_start(char *path, int *result)
{
  int results[2], pid, fd, exists;

  if(pipe(results) < 0)
    return -1;
  pid = fork();
  if(pid < 0)
    return -1;
  if(pid == 0){
    close(results[0]);
    sleep(10); // Remain outside while the parent performs the private change.
    fd = open(path, O_RDONLY);
    exists = fd >= 0;
    if(fd >= 0)
      close(fd);
    write(results[1], &exists, sizeof(exists));
    exit();
  }
  close(results[1]);
  *result = results[0];
  return 0;
}

static int
probe_finish(int result, int should_exist)
{
  int exists;

  if(read(result, &exists, sizeof(exists)) != sizeof(exists))
    return -1;
  close(result);
  wait();
  return exists == should_exist ? 0 : -1;
}

static int
test_create_visibility(void)
{
  int fd, result;
  char data[4];

  if(probe_start("xspec-private", &result) < 0 || xbegin() <= 0)
    return -1;
  fd = open("xspec-private", O_CREATE|O_RDWR);
  if(fd < 0 || write(fd, "priv", 4) != 4 || close(fd) < 0)
    return -1;
  if(probe_finish(result, 0) < 0)
    return -1;
  fd = open("xspec-private", O_RDONLY);
  if(fd < 0 || read(fd, data, 4) != 4 || !is_priv(data))
    return -1;
  close(fd);

  // Nonempty publication is unavailable: it must fail, preserve the private
  // view and reservations, and leave the context abortable.
  if(xcommit() >= 0 || xbegin() >= 0)
    return -1;
  fd = open("xspec-private", O_RDONLY);
  if(fd < 0 || read(fd, data, 4) != 4 || !is_priv(data))
    return -1;
  close(fd);
  if(xabort() < 0)
    return -1;
  return 0;
}

static int
test_unlink_visibility(void)
{
  int fd, result;

  fd = open("xspec-unlink", O_CREATE|O_RDWR);
  if(fd < 0 || write(fd, "base", 4) != 4 || close(fd) < 0)
    return -1;
  if(probe_start("xspec-unlink", &result) < 0)
    return -1;
  if(xbegin() <= 0 || unlink("xspec-unlink") < 0)
    return -1;
  if(open("xspec-unlink", O_RDONLY) >= 0 ||
     probe_finish(result, 1) < 0)
    return -1;
  if(xcommit() >= 0 || xabort() < 0)
    return -1;
  fd = open("xspec-unlink", O_RDONLY);
  if(fd < 0)
    return -1;
  close(fd);
  return 0;
}

static int
test_write_visibility(void)
{
  int fd, result[2], pid;
  char value, data[4];

  fd = open("xspec-data", O_CREATE|O_RDWR);
  if(fd < 0 || write(fd, "base", 4) != 4 || close(fd) < 0 ||
     pipe(result) < 0)
    return -1;
  pid = fork();
  if(pid < 0)
    return -1;
  if(pid == 0){
    close(result[0]);
    sleep(10);
    fd = open("xspec-data", O_RDONLY);
    if(fd < 0 || read(fd, &value, 1) != 1)
      exit();
    write(result[1], &value, 1);
    close(fd);
    exit();
  }
  close(result[1]);
  if(xbegin() <= 0)
    return -1;
  fd = open("xspec-data", O_RDWR);
  if(fd < 0 || write(fd, "spec", 4) != 4 || close(fd) < 0)
    return -1;
  fd = open("xspec-data", O_RDONLY);
  if(fd < 0 || read(fd, data, 4) != 4 || !is_spec(data))
    return -1;
  close(fd);
  if(read(result[0], &value, 1) != 1 || value != 'b')
    return -1;
  close(result[0]);
  wait();
  if(xcommit() >= 0 || xabort() < 0)
    return -1;
  fd = open("xspec-data", O_RDONLY);
  if(fd < 0 || read(fd, data, 4) != 4 ||
     data[0] != 'b' || data[1] != 'a' ||
     data[2] != 's' || data[3] != 'e')
    return -1;
  close(fd);
  unlink("xspec-data");
  return 0;
}

static int
test_create_unlink_reuse(void)
{
  int fd, first_inum, second_inum;
  struct stat st;
  char c;

  if(xbegin() <= 0)
    return -1;
  fd = open("xspec-cycle", O_CREATE|O_RDWR);
  if(fd < 0 || write(fd, "A", 1) != 1 || fstat(fd, &st) < 0)
    return -1;
  first_inum = st.ino;
  if(close(fd) < 0 || unlink("xspec-cycle") < 0)
    return -1;
  fd = open("xspec-cycle", O_CREATE|O_RDWR);
  if(fd < 0 || write(fd, "R", 1) != 1 || fstat(fd, &st) < 0)
    return -1;
  second_inum = st.ino;
  if(first_inum != second_inum || close(fd) < 0)
    return -1;
  fd = open("xspec-cycle", O_RDONLY);
  if(fd < 0 || read(fd, &c, 1) != 1 || c != 'R' || close(fd) < 0 ||
     xabort() < 0)
    return -1;
  return open("xspec-cycle", O_RDONLY) < 0 ? 0 : -1;
}

static int
test_allocation_arbitration(void)
{
  int result[2], pid, fd, mine, theirs;
  struct stat st;
  char c;

  if(pipe(result) < 0)
    return -1;
  pid = fork(); // The child remains an ordinary, non-speculative process.
  if(pid < 0)
    return -1;
  if(pid == 0){
    close(result[0]);
    sleep(10); // Allocate while the parent's speculative reservation is live.
    fd = open("xspec-ordinary", O_CREATE|O_RDWR);
    if(fd < 0 || write(fd, "O", 1) != 1 || fstat(fd, &st) < 0)
      exit();
    theirs = st.ino;
    write(result[1], &theirs, sizeof(theirs));
    close(fd);
    exit();
  }
  close(result[1]);
  if(xbegin() <= 0)
    return -1;
  fd = open("xspec-reserved", O_CREATE|O_RDWR);
  if(fd < 0 || write(fd, "S", 1) != 1 || fstat(fd, &st) < 0)
    return -1;
  mine = st.ino;
  if(read(result[0], &theirs, sizeof(theirs)) != sizeof(theirs))
    return -1;
  close(result[0]);
  wait();
  if(mine == theirs)
    return -1;
  if(close(fd) < 0)
    return -1;
  fd = open("xspec-reserved", O_RDONLY);
  if(fd < 0 || read(fd, &c, 1) != 1 || c != 'S' || close(fd) < 0 ||
     xabort() < 0)
    return -1;
  if(open("xspec-reserved", O_RDONLY) >= 0)
    return -1;
  fd = open("xspec-ordinary", O_RDONLY);
  if(fd < 0 || read(fd, &c, 1) != 1 || c != 'O' || close(fd) < 0)
    return -1;
  unlink("xspec-ordinary");
  return 0;
}

static int
test_shadow_capacity(void)
{
  int fd, n;

  memset(bulk, 'C', sizeof(bulk));
  if(xbegin() <= 0)
    return -1;
  fd = open("xspec-capacity", O_CREATE|O_RDWR);
  if(fd < 0)
    return -1;
  n = write(fd, bulk, sizeof(bulk));
  if(n >= 0 || xcommit() >= 0 || xabort() < 0 || close(fd) < 0)
    return -1;
  if(open("xspec-capacity", O_RDONLY) >= 0)
    return -1;
  return 0;
}

int
main(void)
{
  int fd;

  if(xcommit() >= 0 || xabort() >= 0 || xbegin() <= 0 || xbegin() >= 0)
    fail("lifecycle start/nesting");
  if(xcommit() < 0)
    fail("empty-context commit");

  if(test_create_visibility() < 0)
    fail("create visibility, snapshot, retry and abort");
  if(test_unlink_visibility() < 0)
    fail("unlink visibility and abort");
  if(test_write_visibility() < 0)
    fail("data write visibility and abort");
  if(test_create_unlink_reuse() < 0)
    fail("create/unlink reservation transitions");
  if(test_allocation_arbitration() < 0)
    fail("ordinary allocator reservation arbitration");
  if(test_shadow_capacity() < 0)
    fail("shadow capacity and abort-only handling");

  fd = open("xspec-private", O_RDONLY);
  if(fd >= 0){
    close(fd);
    fail("aborted create visible in base view");
  }
  if(xbegin() <= 0 || xabort() < 0 ||
     write(1, "XSPEC_PASS\n", 11) != 11)
    fail("final abort/detach");
  printf(1, "XSPEC PASS (filesystem isolation and lifecycle)\n");
  exit();
}
