#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

static void
fail(char *message)
{
  printf(2, "XCRASH FAIL: %s\n", message);
  exit();
}

int
main(int argc, char **argv)
{
  char value;
  int fd;
  struct stat original, alias;

  if(argc != 2)
    fail("mode required");

  if(strcmp(argv[1], "crash") == 0){
    if(xbegin() <= 0)
      fail("begin");
    fd = open("xcrash-data", O_CREATE|O_RDWR);
    if(fd < 0 || write(fd, "C", 1) != 1 || close(fd) < 0)
      fail("speculative create/write");
    if(mkdir("xcrash-dir") < 0)
      fail("speculative directory create");
    fd = open("xcrash-dir/member", O_CREATE|O_RDWR);
    if(fd < 0 || write(fd, "D", 1) != 1 || close(fd) < 0 ||
       link("xcrash-dir/member", "xcrash-link") < 0)
      fail("speculative directory entry and hard link");
    if(write(1, "XCRASH_PENDING_CONSOLE\n", 23) != 23)
      fail("buffer speculative console output");
    if(xcommit() < 0)
      fail("commit returned before injected crash point");
    printf(1, "XCRASH COMMIT RETURNED\n");
    exit();
  }

  if(strcmp(argv[1], "verify-present") == 0){
    fd = open("xcrash-data", O_RDONLY);
    if(fd < 0 || read(fd, &value, 1) != 1 || value != 'C' ||
       close(fd) < 0 || unlink("xcrash-data") < 0)
      fail("committed data missing after recovery");
    fd = open("xcrash-dir/member", O_RDONLY);
    if(fd < 0 || read(fd, &value, 1) != 1 || value != 'D' ||
       close(fd) < 0 || stat("xcrash-dir/member", &original) < 0 ||
       stat("xcrash-link", &alias) < 0 || original.ino != alias.ino ||
       original.nlink != 2 || alias.nlink != 2 ||
       unlink("xcrash-link") < 0 || unlink("xcrash-dir/member") < 0 ||
       unlink("xcrash-dir") < 0)
      fail("recovered directory or inode references are inconsistent");
    fd = open("xcrash-post-recovery", O_CREATE|O_RDWR);
    if(fd < 0 || write(fd, "R", 1) != 1 || close(fd) < 0 ||
       unlink("xcrash-post-recovery") < 0)
      fail("filesystem unusable after recovery");
    printf(1, "XCRASH RECOVERY PRESENT PASS\n");
    exit();
  }

  if(strcmp(argv[1], "verify-absent") == 0){
    fd = open("xcrash-data", O_RDONLY);
    if(fd >= 0){
      close(fd);
      fail("uncommitted data became visible after recovery");
    }
    if(open("xcrash-dir/member", O_RDONLY) >= 0 ||
       open("xcrash-link", O_RDONLY) >= 0 || open("xcrash-dir", O_RDONLY) >= 0)
      fail("uncommitted directory entries became visible after recovery");
    fd = open("xcrash-post-recovery", O_CREATE|O_RDWR);
    if(fd < 0 || write(fd, "R", 1) != 1 || close(fd) < 0 ||
       unlink("xcrash-post-recovery") < 0)
      fail("filesystem unusable after recovery");
    printf(1, "XCRASH RECOVERY ABSENT PASS\n");
    exit();
  }

  fail("unknown mode");
  return 0;
}
