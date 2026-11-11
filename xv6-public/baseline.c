#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

#define MAXWRITE (64*1024)
#define RACESIZE 2048

static char *sizestr[] = { "1024", "2048", "4096", "8192", "16384", "32768", "65536" };
static int sizes[] = { 1024, 2048, 4096, 8192, 16384, 32768, 65536 };
static char racebuf[RACESIZE];

static void
racewriter(int gatefd, char value)
{
  char token;
  int fd;

  if(read(gatefd, &token, 1) != 1)
    exit();
  close(gatefd);
  memset(racebuf, value, sizeof(racebuf));
  fd = open("baserace", O_WRONLY);
  if(fd < 0 || write(fd, racebuf, sizeof(racebuf)) != sizeof(racebuf))
    exit();
  close(fd);
  exit();
}

static int
basic_tests(void)
{
  int fd, p[2], pid, n, i;
  char buf[32], check[RACESIZE];

  unlink("basefile");
  fd = open("basefile", O_CREATE|O_RDWR);
  if(fd < 0 || write(fd, "file-ok", 7) != 7){
    printf(2, "FAIL file create/write\n");
    return -1;
  }
  close(fd);
  fd = open("basefile", O_RDONLY);
  if(fd < 0 || (n = read(fd, buf, sizeof(buf))) != 7){
    printf(2, "FAIL file open/read\n");
    return -1;
  }
  buf[n] = 0;
  if(strcmp(buf, "file-ok") != 0){
    printf(2, "FAIL file contents: %s\n", buf);
    return -1;
  }
  close(fd);
  if(unlink("basefile") < 0){
    printf(2, "FAIL file unlink\n");
    return -1;
  }

  if(pipe(p) < 0){
    printf(2, "FAIL pipe create\n");
    return -1;
  }
  pid = fork();
  if(pid < 0){
    printf(2, "FAIL fork\n");
    return -1;
  }
  if(pid == 0){
    close(p[0]);
    if(write(p[1], "pipe-ok", 7) != 7)
      exit();
    close(p[1]);
    exit();
  }
  close(p[1]);
  n = read(p[0], buf, sizeof(buf));
  close(p[0]);
  wait();
  if(n != 7){
    printf(2, "FAIL pipe transfer (%d)\n", n);
    return -1;
  }
  buf[n] = 0;
  if(strcmp(buf, "pipe-ok") != 0){
    printf(2, "FAIL pipe contents: %s\n", buf);
    return -1;
  }

  unlink("baserace");
  fd = open("baserace", O_CREATE|O_RDWR);
  if(fd < 0){
    printf(2, "FAIL concurrent test file create\n");
    return -1;
  }
  close(fd);
  if(pipe(p) < 0){
    printf(2, "FAIL concurrent test pipe\n");
    return -1;
  }
  pid = fork();
  if(pid < 0){
    printf(2, "FAIL first concurrent fork\n");
    return -1;
  }
  if(pid == 0){
    close(p[1]);
    racewriter(p[0], 'a');
  }
  pid = fork();
  if(pid < 0){
    printf(2, "FAIL second concurrent fork\n");
    return -1;
  }
  if(pid == 0){
    close(p[1]);
    racewriter(p[0], 'b');
  }
  close(p[0]);
  if(write(p[1], "go", 2) != 2){
    printf(2, "FAIL concurrent test start\n");
    return -1;
  }
  close(p[1]);
  wait();
  wait();
  fd = open("baserace", O_RDONLY);
  if(fd < 0 || read(fd, check, sizeof(check)) != sizeof(check)){
    printf(2, "FAIL concurrent file readback\n");
    return -1;
  }
  close(fd);
  for(i = 0; i < sizeof(check); i++)
    if(check[i] != 'a' && check[i] != 'b'){
      printf(2, "FAIL concurrent file contents at %d\n", i);
      return -1;
    }
  if(unlink("baserace") < 0){
    printf(2, "FAIL concurrent test cleanup\n");
    return -1;
  }

  printf(1, "baseline: file, unlink, pipe, fork and concurrent writes passed\n");
  printf(1, "baseline: console output check\n");
  return 0;
}

static int
bench(void)
{
  char *buf;
  int i, fd, start, elapsed, n;
  char path[16];

  if((buf = malloc(MAXWRITE)) == 0){
    printf(2, "FAIL benchmark allocation\n");
    return -1;
  }
  memset(buf, 'x', MAXWRITE);
  printf(1, "benchmark,size_bytes,elapsed_ticks,bytes_written\n");
  for(i = 0; i < 7; i++){
    path[0] = 'b'; path[1] = '0' + i; path[2] = 0;
    unlink(path);
    fd = open(path, O_CREATE|O_RDWR);
    if(fd < 0){
      printf(2, "FAIL benchmark file create %s\n", path);
      free(buf);
      return -1;
    }
    start = uptime();
    n = write(fd, buf, sizes[i]);
    elapsed = uptime() - start;
    close(fd);
    if(n != sizes[i]){
      printf(2, "FAIL benchmark write size=%s returned=%d\n", sizestr[i], n);
      unlink(path);
      free(buf);
      return -1;
    }
    printf(1, "write,%s,%d,%d\n", sizestr[i], elapsed, n);
    if(unlink(path) < 0){
      printf(2, "FAIL benchmark unlink %s\n", path);
      free(buf);
      return -1;
    }
  }
  free(buf);
  return 0;
}

int
main(void)
{
  if(basic_tests() < 0 || bench() < 0){
    printf(2, "BASELINE FAIL\n");
    exit();
  }
  printf(1, "BASELINE PASS\n");
  exit();
}
