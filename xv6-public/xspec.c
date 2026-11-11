#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

static int
check_child_inheritance(void)
{
  int pid, fd;

  pid = fork();
  if(pid < 0)
    return -1;
  if(pid == 0){
    fd = open("xspec-child-created", O_CREATE|O_RDWR);
    if(fd >= 0){
      close(fd);
    }
    fd = open("xspec-file", O_RDWR);
    if(fd >= 0){
      write(fd, "evil", 4);
      close(fd);
    }
    exit();
  }
  wait();
  fd = open("xspec-child-created", O_RDONLY);
  if(fd >= 0){
    close(fd);
    return -1;
  }
  if(open("xspec-file", O_RDONLY) < 0)
    return -1;
  return 0;
}

int
main(void)
{
  int fd, i, id, lastid;
  char buf[8];

  unlink("xspec-file");
  fd = open("xspec-file", O_CREATE|O_RDWR);
  if(fd < 0 || write(fd, "seed", 4) != 4){
    printf(2, "FAIL xspec setup\n");
    exit();
  }
  close(fd);

  if(xcommit() >= 0 || xabort() >= 0){
    printf(2, "FAIL invalid completion accepted\n");
    exit();
  }
  if(xbegin() <= 0 || xbegin() >= 0){
    printf(2, "FAIL begin or nested-begin semantics\n");
    exit();
  }
  if(check_child_inheritance() < 0){
    printf(2, "FAIL child context inheritance\n");
    exit();
  }

  fd = open("xspec-file", O_RDWR);
  if(fd < 0 || write(fd, "bad", 3) >= 0){
    printf(2, "FAIL speculative write was not blocked\n");
    exit();
  }
  close(fd);
  if(open("xspec-created", O_CREATE|O_RDWR) >= 0 ||
     unlink("xspec-file") >= 0){
    printf(2, "FAIL speculative filesystem mutation was not blocked\n");
    exit();
  }
  if(xcommit() < 0){
    printf(2, "FAIL empty-context commit\n");
    exit();
  }

  fd = open("xspec-file", O_RDONLY);
  if(fd < 0 || read(fd, buf, 4) != 4 || buf[0] != 's' || buf[1] != 'e' ||
     buf[2] != 'e' || buf[3] != 'd'){
    printf(2, "FAIL committed context changed base file\n");
    exit();
  }
  close(fd);

  if(xbegin() <= 0 || xabort() < 0 || xabort() >= 0){
    printf(2, "FAIL abort lifecycle\n");
    exit();
  }
  if(write(1, "xspec output after abort\n", 25) != 25){
    printf(2, "FAIL process did not detach after abort\n");
    exit();
  }
  if(unlink("xspec-file") < 0){
    printf(2, "FAIL cleanup\n");
    exit();
  }
  lastid = 0;
  for(i = 0; i < 20; i++){
    id = xbegin();
    if(id <= 0 || id == lastid || xabort() < 0){
      printf(2, "FAIL repeated allocation/reclamation\n");
      exit();
    }
    lastid = id;
  }
  printf(1, "XSPEC PASS (lifecycle and fail-closed guard)\n");
  exit();
}
