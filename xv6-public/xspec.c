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

  if(xcommit() < 0)
    return -1;
  fd = open("xspec-private", O_RDONLY);
  if(fd < 0 || read(fd, data, 4) != 4 || !is_priv(data))
    return -1;
  close(fd);
  if(unlink("xspec-private") < 0)
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
  if(xcommit() < 0)
    return -1;
  return open("xspec-unlink", O_RDONLY) < 0 ? 0 : -1;
}

static int
test_namespace_visibility(void)
{
  int fd, result;
  struct stat original, alias;
  char data[4];

  if(probe_start("xspec-dir/file", &result) < 0 || xbegin() <= 0)
    return -1;
  if(mkdir("xspec-dir") < 0)
    return -1;
  fd = open("xspec-dir/file", O_CREATE|O_RDWR);
  if(fd < 0 || write(fd, "priv", 4) != 4 ||
     fstat(fd, &original) < 0 || close(fd) < 0)
    return -1;
  if(link("xspec-dir/file", "xspec-link") < 0 ||
     probe_finish(result, 0) < 0)
    return -1;
  fd = open("xspec-link", O_RDONLY);
  if(fd < 0 || fstat(fd, &alias) < 0 ||
     read(fd, data, sizeof(data)) != sizeof(data) ||
     !is_priv(data) || alias.ino != original.ino || close(fd) < 0)
    return -1;
  if(xcommit() < 0)
    return -1;
  fd = open("xspec-dir/file", O_RDONLY);
  if(fd < 0 || read(fd, data, sizeof(data)) != sizeof(data) ||
     !is_priv(data) || close(fd) < 0)
    return -1;
  if(unlink("xspec-link") < 0 || unlink("xspec-dir/file") < 0 ||
     unlink("xspec-dir") < 0)
    return -1;
  return 0;
}

static int
test_chdir_lifecycle(void)
{
  int fd;
  char value;

  if(xbegin() <= 0 || mkdir("xspec-cwd-abort") < 0 ||
     chdir("xspec-cwd-abort") < 0)
    return -1;
  fd = open("inside", O_CREATE|O_RDWR);
  if(fd < 0 || write(fd, "A", 1) != 1 || close(fd) < 0 ||
     xabort() < 0)
    return -1;
  if(open("xspec-cwd-abort/inside", O_RDONLY) >= 0)
    return -1;
  fd = open("xspec-root-abort", O_CREATE|O_RDWR);
  if(fd < 0 || close(fd) < 0 || unlink("xspec-root-abort") < 0)
    return -1;

  if(xbegin() <= 0 || mkdir("xspec-cwd-commit") < 0 ||
     chdir("xspec-cwd-commit") < 0)
    return -1;
  fd = open("inside", O_CREATE|O_RDWR);
  if(fd < 0 || write(fd, "C", 1) != 1 || close(fd) < 0 ||
     xcommit() < 0)
    return -1;
  fd = open("xspec-cwd-commit/inside", O_RDONLY);
  if(fd < 0 || read(fd, &value, 1) != 1 || value != 'C' || close(fd) < 0)
    return -1;
  fd = open("xspec-root-commit", O_CREATE|O_RDWR);
  if(fd < 0 || close(fd) < 0 || unlink("xspec-root-commit") < 0 ||
     unlink("xspec-cwd-commit/inside") < 0 ||
     unlink("xspec-cwd-commit") < 0)
    return -1;
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
  if(xcommit() < 0)
    return -1;
  fd = open("xspec-data", O_RDONLY);
  if(fd < 0 || read(fd, data, 4) != 4 ||
     data[0] != 's' || data[1] != 'p' ||
     data[2] != 'e' || data[3] != 'c')
    return -1;
  close(fd);
  unlink("xspec-data");
  return 0;
}

static void
conflict_child(int which, int ready[2], int go1[2], int go2[2],
               int result[2])
{
  int fd, control, status;
  char token, value;

  close(ready[0]);
  close(result[0]);
  if(which == 1){
    close(go1[1]);
    close(go2[0]);
    close(go2[1]);
    control = go1[0];
    value = '1';
  } else {
    close(go2[1]);
    close(go1[0]);
    close(go1[1]);
    control = go2[0];
    value = '2';
  }
  token = 'R';
  if(write(ready[1], &token, 1) != 1 || xbegin() <= 0)
    exit();
  fd = open("xspec-conflict", O_RDWR);
  if(fd < 0 || write(fd, &value, 1) != 1 || close(fd) < 0 ||
     read(control, &token, 1) != 1)
    exit();
  close(control);
  if(which == 1)
    status = xcommit() == 0 ? 'C' : 'E';
  else if(xcommit() < 0 && xabort() == 0)
    status = 'X';
  else
    status = 'E';
  write(result[1], (char*)&status, 1);
  exit();
}

static int
test_write_conflict(void)
{
  int fd, ready[2], go1[2], go2[2], result[2], pid1, pid2;
  char token, value;

  fd = open("xspec-conflict", O_CREATE|O_RDWR);
  if(fd < 0 || write(fd, "0", 1) != 1 || close(fd) < 0 ||
     pipe(ready) < 0 || pipe(go1) < 0 || pipe(go2) < 0 ||
     pipe(result) < 0)
    return -1;
  pid1 = fork();
  if(pid1 < 0)
    return -1;
  if(pid1 == 0)
    conflict_child(1, ready, go1, go2, result);
  pid2 = fork();
  if(pid2 < 0)
    return -1;
  if(pid2 == 0)
    conflict_child(2, ready, go1, go2, result);

  close(ready[1]);
  close(go1[0]);
  close(go2[0]);
  close(result[1]);
  if(read(ready[0], &token, 1) != 1 ||
     read(ready[0], &token, 1) != 1)
    return -1;
  close(ready[0]);
  sleep(10); // Let both children write privately before either commits.
  token = 'C';
  if(write(go1[1], &token, 1) != 1)
    return -1;
  if(read(result[0], &token, 1) != 1 || token != 'C')
    return -1;
  wait();
  if(write(go2[1], &token, 1) != 1)
    return -1;
  if(read(result[0], &token, 1) != 1 || token != 'X')
    return -1;
  wait();
  close(go1[1]);
  close(go2[1]);
  close(result[0]);

  fd = open("xspec-conflict", O_RDONLY);
  if(fd < 0 || read(fd, &value, 1) != 1 || value != '1' || close(fd) < 0)
    return -1;
  if(unlink("xspec-conflict") < 0)
    return -1;
  return 0;
}

// A transaction which only reads a committed block must not commit after an
// outside writer changes that block.  This exercises the MVCC read set rather
// than the existing write-write conflict path.
static int
test_read_conflict(void)
{
  int fd, done[2], pid;
  char token, value;

  fd = open("xspec-read-conflict", O_CREATE|O_RDWR);
  if(fd < 0 || write(fd, "R", 1) != 1 || close(fd) < 0 ||
     pipe(done) < 0)
    return -1;
  pid = fork();
  if(pid < 0)
    return -1;
  if(pid == 0){
    close(done[0]);
    token = 'R';
    if(write(done[1], &token, 1) != 1)
      exit();
    sleep(20);
    fd = open("xspec-read-conflict", O_RDWR);
    if(fd < 0 || write(fd, "W", 1) != 1 || close(fd) < 0)
      exit();
    token = 'D';
    write(done[1], &token, 1);
    close(done[1]);
    exit();
  }
  close(done[1]);
  if(read(done[0], &token, 1) != 1 || token != 'R')
    return -1;
  if(xbegin() <= 0)
    return -1;
  fd = open("xspec-read-conflict", O_RDONLY);
  if(fd < 0 || read(fd, &value, 1) != 1 || value != 'R' || close(fd) < 0)
    return -1;
  if(read(done[0], &token, 1) != 1 || token != 'D')
    return -1;
  close(done[0]);
  wait();
  if(xcommit() >= 0 || xabort() < 0)
    return -1;
  fd = open("xspec-read-conflict", O_RDONLY);
  if(fd < 0 || read(fd, &value, 1) != 1 || value != 'W' || close(fd) < 0 ||
     unlink("xspec-read-conflict") < 0)
    return -1;
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
  if(n >= 0 || xcommit() >= 0 || xabort() < 0 || xabort() >= 0 ||
     close(fd) < 0)
    return -1;
  if(open("xspec-capacity", O_RDONLY) >= 0)
    return -1;
  return 0;
}

static int
test_journal_capacity(void)
{
  int fd, n, committed;
  struct stat st;
  char value;

  memset(bulk, 'J', sizeof(bulk));
  if(xbegin() <= 0)
    return -1;
  fd = open("xspec-journal-capacity", O_CREATE|O_RDWR);
  if(fd < 0)
    return -1;
  n = write(fd, bulk, 6144);
  if(close(fd) < 0)
    return -1;
  if(n != 6144){
    if(xabort() < 0)
      return -1;
    fd = open("xspec-journal-capacity", O_RDONLY);
    if(fd >= 0){
      close(fd);
      return -1;
    }
  } else {
    committed = xcommit();
    if(committed < 0){
      if(xabort() < 0)
        return -1;
      fd = open("xspec-journal-capacity", O_RDONLY);
      if(fd >= 0){
        close(fd);
        return -1;
      }
    } else {
      fd = open("xspec-journal-capacity", O_RDONLY);
      if(fd < 0 || fstat(fd, &st) < 0 || st.size != 6144 ||
         read(fd, &value, 1) != 1 || value != 'J' || close(fd) < 0 ||
         unlink("xspec-journal-capacity") < 0)
        return -1;
    }
  }

  if(xbegin() <= 0)
    return -1;
  fd = open("xspec-journal-capacity", O_CREATE|O_RDWR);
  if(fd < 0 || write(fd, bulk, 1024) != 1024 ||
     close(fd) < 0 || xcommit() < 0)
    return -1;
  fd = open("xspec-journal-capacity", O_RDONLY);
  if(fd < 0 || fstat(fd, &st) < 0 || st.size != 1024 ||
     read(fd, &value, 1) != 1 || value != 'J' || close(fd) < 0 ||
     unlink("xspec-journal-capacity") < 0)
    return -1;
  return 0;
}

static int
test_context_reuse(void)
{
  int i;

  for(i = 0; i < 32; i++)
    if(xbegin() <= 0 || xcommit() < 0)
      return -1;
  return 0;
}

static int
test_context_capacity(void)
{
  int depth;

  for(depth = 0; depth < 16; depth++)
    if(xbegin() <= 0)
      return -1;
  if(xbegin() > 0)
    return -1;
  for(depth = 0; depth < 16; depth++)
    if(xabort() < 0)
      return -1;
  return 0;
}

int
main(void)
{
  int fd;

  printf(1, "XSPEC_STAGE lifecycle\n");
  if(xcommit() >= 0 || xabort() >= 0 || xbegin() <= 0)
    fail("lifecycle start");
  if(xcommit() < 0)
    fail("empty-context commit");

  printf(1, "XSPEC_STAGE create-visibility\n");
  if(test_create_visibility() < 0)
    fail("create visibility, snapshot, retry and abort");
  printf(1, "XSPEC_STAGE namespace-visibility\n");
  if(test_namespace_visibility() < 0)
    fail("directory and hard-link isolation");
  printf(1, "XSPEC_STAGE chdir-lifecycle\n");
  if(test_chdir_lifecycle() < 0)
    fail("current-directory restore on commit and abort");
  printf(1, "XSPEC_STAGE unlink-visibility\n");
  if(test_unlink_visibility() < 0)
    fail("unlink visibility and abort");
  printf(1, "XSPEC_STAGE write-visibility\n");
  if(test_write_visibility() < 0)
    fail("data write visibility and abort");
  printf(1, "XSPEC_STAGE write-conflict\n");
  if(test_write_conflict() < 0)
    fail("stale write-set conflict validation");
  printf(1, "XSPEC_STAGE read-conflict\n");
  if(test_read_conflict() < 0)
    fail("stale read-set conflict validation");
  printf(1, "XSPEC_STAGE create-unlink-reuse\n");
  if(test_create_unlink_reuse() < 0)
    fail("create/unlink reservation transitions");
  printf(1, "XSPEC_STAGE allocation-arbitration\n");
  if(test_allocation_arbitration() < 0)
    fail("ordinary allocator reservation arbitration");
  printf(1, "XSPEC_STAGE shadow-capacity\n");
  if(test_shadow_capacity() < 0)
    fail("shadow capacity and abort-only handling");
  printf(1, "XSPEC_STAGE journal-capacity\n");
  if(test_journal_capacity() < 0)
    fail("publication up to actual journal capacity");
  printf(1, "XSPEC_STAGE context-reuse\n");
  if(test_context_reuse() < 0)
    fail("terminal context reclamation");
  printf(1, "XSPEC_STAGE context-capacity\n");
  if(test_context_capacity() < 0)
    fail("context-table exhaustion and nested cleanup");

  printf(1, "XSPEC_STAGE final-cleanup\n");
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
