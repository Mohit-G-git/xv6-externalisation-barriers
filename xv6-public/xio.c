#include "types.h"
#include "user.h"

#define PIPESIZE 512

// Keep the large console-capacity buffer out of main's one-page user stack.
static char console_test_buf[2048];

static void
fail(char *s)
{
  printf(2, "XIO FAIL: %s\n", s);
  exit();
}

static int
observer(void)
{
  int p[2];
  int pid, writer, reader;
  char c;

  if(pipe(p) < 0)
    return -1;
  pid = fork();
  if(pid < 0)
    return -1;
  if(pid == 0){
    close(p[1]);
    reader = dup(p[0]);
    close(p[0]);
    if(reader < 0 || read(reader, &c, 1) != 1)
      exit();
    printf(1, "OUTSIDE_PIPE_%c\n", c);
    exit();
  }
  close(p[0]);
  writer = dup(p[1]);
  close(p[1]);
  if(writer < 0 || xbegin() <= 0 || write(writer, "C", 1) != 1)
    return -1;
  sleep(10);
  printf(1, "PIPE_COMMIT_MARK\n");
  if(xcommit() < 0)
    return -1;
  close(writer);
  wait();
  return 0;
}

static int
abort_observer(void)
{
  int p[2];
  int pid, reader;
  char c;

  if(pipe(p) < 0)
    return -1;
  pid = fork();
  if(pid < 0)
    return -1;
  if(pid == 0){
    close(p[1]);
    reader = dup(p[0]);
    close(p[0]);
    if(reader < 0)
      exit();
    if(read(reader, &c, 1) == 0)
      printf(1, "OUTSIDE_PIPE_ABORT_EOF\n");
    else
      printf(1, "OUTSIDE_PIPE_ABORT_LEAK\n");
    exit();
  }
  close(p[0]);
  if(xbegin() <= 0 || write(p[1], "A", 1) != 1)
    return -1;
  sleep(5);
  if(xabort() < 0)
    return -1;
  close(p[1]);
  wait();
  return 0;
}

static int
same_context_pipe(void)
{
  int data[2], signal[2], pid;
  char c;

  if(pipe(data) < 0 || pipe(signal) < 0 || xbegin() <= 0)
    return -1;
  pid = fork();
  if(pid < 0)
    return -1;
  if(pid == 0){
    close(data[1]);
    close(signal[0]);
    if(read(data[0], &c, 1) != 1 || c != 'S' ||
       write(signal[1], "R", 1) != 1)
      exit();
    exit();
  }
  close(data[0]);
  close(signal[1]);
  if(write(data[1], "S", 1) != 1 || read(signal[0], &c, 1) != 1 ||
     c != 'R')
    return -1;
  printf(1, "SAME_CONTEXT_PIPE_OK\n");
  if(xcommit() < 0)
    return -1;
  wait();
  close(data[1]);
  close(signal[0]);
  return 0;
}

static int
wrap_and_capacity(void)
{
  int p[2];
  char in[500], out[500], tail[400], check[400];
  int i;

  if(pipe(p) < 0 || xbegin() <= 0)
    return -1;
  memset(in, 'a', sizeof(in));
  memset(tail, 'b', sizeof(tail));
  if(write(p[1], in, sizeof(in)) != sizeof(in) ||
     read(p[0], out, sizeof(out)) != sizeof(out) ||
     write(p[1], tail, sizeof(tail)) != sizeof(tail) ||
     read(p[0], check, sizeof(check)) != sizeof(check))
    return -1;
  for(i = 0; i < sizeof(out); i++)
    if(out[i] != 'a')
      return -1;
  for(i = 0; i < sizeof(check); i++)
    if(check[i] != 'b')
      return -1;
  if(xabort() < 0)
    return -1;
  close(p[0]);
  close(p[1]);
  return 0;
}

static int
full_pipe_commit(void)
{
  int p[2], pid;
  char data[PIPESIZE], got[PIPESIZE];
  int n;

  memset(data, 'F', sizeof(data));
  if(pipe(p) < 0)
    return -1;
  if(write(p[1], "O", 1) != 1)
    return -1;
  pid = fork();
  if(pid < 0)
    return -1;
  if(pid == 0){
    close(p[1]);
    sleep(20);
    n = read(p[0], got, sizeof(got));
    if(n == sizeof(got) && got[0] == 'O' && got[1] == 'F' && got[n-1] == 'F')
      printf(1, "FULL_PIPE_READER_OK\n");
    else
      printf(1, "FULL_PIPE_READER_BAD\n");
    exit();
  }
  close(p[0]);
  if(xbegin() <= 0 || write(p[1], data, sizeof(data)) != PIPESIZE-1 ||
     write(p[1], "X", 1) != 0 || xcommit() < 0)
    return -1;
  close(p[1]);
  wait();
  return 0;
}

static int
full_pipe_abort(void)
{
  int p[2], pid;
  char data[PIPESIZE], c;
  int n;

  memset(data, 'D', sizeof(data));
  if(pipe(p) < 0)
    return -1;
  pid = fork();
  if(pid < 0)
    return -1;
  if(pid == 0){
    close(p[1]);
    n = read(p[0], &c, 1);
    if(n == 0)
      printf(1, "FULL_PIPE_ABORT_EOF\n");
    else
      printf(1, "FULL_PIPE_ABORT_LEAK\n");
    exit();
  }
  close(p[0]);
  if(xbegin() <= 0 || write(p[1], data, sizeof(data)) != sizeof(data) ||
     write(p[1], "X", 1) != 0 || xabort() < 0)
    return -1;
  close(p[1]);
  wait();
  return 0;
}

static int
aborted_child_guard(void)
{
  int signal[2], pid;
  char c;

  if(pipe(signal) < 0 || xbegin() <= 0)
    return -1;
  pid = fork();
  if(pid < 0)
    return -1;
  if(pid == 0){
    close(signal[1]);
    if(read(signal[0], &c, 1) != 1)
      exit();
    if(write(1, "CHILD_ABORT_LEAK", 16) >= 0)
      exit();
    if(xbegin() <= 0 || write(1, "CHILD_ABORT_GUARD_OK\n", 21) != 21 ||
       xcommit() < 0)
      exit();
    exit();
  }
  close(signal[0]);
  if(xabort() < 0 || write(signal[1], "R", 1) != 1)
    return -1;
  close(signal[1]);
  wait();
  return 0;
}

static int
mixed_readers(void)
{
  int data[2], signal[2], outside, inside;
  char c;

  if(pipe(data) < 0 || pipe(signal) < 0)
    return -1;
  outside = fork();
  if(outside < 0)
    return -1;
  if(outside == 0){
    close(data[1]);
    close(signal[0]);
    close(signal[1]);
    if(read(data[0], &c, 1) == 1)
      printf(1, "MIXED_OUTSIDE_GOT_%c\n", c);
    else
      printf(1, "MIXED_OUTSIDE_EOF\n");
    exit();
  }
  if(xbegin() <= 0)
    return -1;
  inside = fork();
  if(inside < 0)
    return -1;
  if(inside == 0){
    close(data[1]);
    close(signal[0]);
    if(read(data[0], &c, 1) != 1 || c != 'A' ||
       write(signal[1], "R", 1) != 1)
      exit();
    exit();
  }
  close(signal[1]);
  if(write(data[1], "AB", 2) != 2 || read(signal[0], &c, 1) != 1 ||
     c != 'R')
    return -1;
  close(data[0]);
  if(xcommit() < 0)
    return -1;
  close(data[1]);
  close(signal[0]);
  wait();
  wait();
  return 0;
}

static int
multiple_writers(void)
{
  int p[2], reader, writer1, writer2, pid;
  char data[128], got[256];
  int n, total, acount, bcount, i;

  if(pipe(p) < 0)
    return -1;
  reader = fork();
  if(reader < 0)
    return -1;
  if(reader == 0){
    close(p[1]);
    total = 0;
    while(total < sizeof(got)){
      n = read(p[0], got + total, sizeof(got) - total);
      if(n <= 0)
        break;
      total += n;
    }
    if(total != sizeof(got) || read(p[0], got, 1) != 0)
      exit();
    acount = bcount = 0;
    for(i = 0; i < total; i++){
      if(got[i] == 'A')
        acount++;
      else if(got[i] == 'B')
        bcount++;
      else
        exit();
    }
    // Each write is smaller than PIPESIZE and must stay contiguous.
    if(acount != 128 || bcount != 128 ||
       !((got[0] == 'A' && got[127] == 'A' && got[128] == 'B' && got[255] == 'B') ||
         (got[0] == 'B' && got[127] == 'B' && got[128] == 'A' && got[255] == 'A')))
      exit();
    printf(1, "MULTI_WRITER_PIPE_OK\n");
    exit();
  }
  close(p[0]);
  if(xbegin() <= 0)
    return -1;
  memset(data, 'A', sizeof(data));
  writer1 = fork();
  if(writer1 < 0)
    return -1;
  if(writer1 == 0){
    if(write(p[1], data, sizeof(data)) != sizeof(data))
      exit();
    exit();
  }
  memset(data, 'B', sizeof(data));
  writer2 = fork();
  if(writer2 < 0)
    return -1;
  if(writer2 == 0){
    if(write(p[1], data, sizeof(data)) != sizeof(data))
      exit();
    exit();
  }
  if(wait() < 0 || wait() < 0 || xcommit() < 0)
    return -1;
  close(p[1]);
  pid = wait();
  return pid == reader ? 0 : -1;
}

static int
reader_exit(void)
{
  int p[2], pid, result;

  if(pipe(p) < 0)
    return -1;
  pid = fork();
  if(pid < 0)
    return -1;
  if(pid == 0){
    close(p[0]);
    close(p[1]);
    exit();
  }
  close(p[0]);
  if(wait() != pid || xbegin() <= 0)
    return -1;
  result = write(p[1], "X", 1);
  if(result != 0 || xabort() < 0){
    close(p[1]);
    return -1;
  }
  close(p[1]);
  return 0;
}

static int
io_metrics(void)
{
  int p[2], pid, start, ordinary_console_ticks;
  int speculative_console_ticks, ordinary_pipe_ticks, speculative_pipe_ticks;
  char data[PIPESIZE], c;

  memset(data, '.', sizeof(data));
  start = uptime();
  if(write(1, data, sizeof(data)) != sizeof(data))
    return -1;
  ordinary_console_ticks = uptime() - start;

  if(xbegin() <= 0)
    return -1;
  start = uptime();
  if(write(1, data, sizeof(data)) != sizeof(data))
    return -1;
  speculative_console_ticks = uptime() - start;
  if(xabort() < 0)
    return -1;

  if(pipe(p) < 0)
    return -1;
  pid = fork();
  if(pid < 0)
    return -1;
  if(pid == 0){
    close(p[1]);
    sleep(5);
    if(read(p[0], &c, 1) != 1)
      exit();
    exit();
  }
  close(p[0]);
  if(write(p[1], data, sizeof(data)) != sizeof(data))
    return -1;
  start = uptime();
  if(write(p[1], "W", 1) != 1)
    return -1;
  ordinary_pipe_ticks = uptime() - start;
  close(p[1]);
  if(wait() != pid)
    return -1;

  if(pipe(p) < 0 || xbegin() <= 0)
    return -1;
  if(write(p[1], data, sizeof(data)) != sizeof(data))
    return -1;
  start = uptime();
  if(write(p[1], "W", 1) != 0)
    return -1;
  speculative_pipe_ticks = uptime() - start;
  if(xabort() < 0)
    return -1;
  close(p[0]);
  close(p[1]);

  printf(1, "IO_METRICS bytes=%d ordinary_console_ticks=%d speculative_console_ticks=%d ordinary_pipe_wait_ticks=%d speculative_full_pipe_ticks=%d\n",
         PIPESIZE, ordinary_console_ticks, speculative_console_ticks,
         ordinary_pipe_ticks, speculative_pipe_ticks);
  return 0;
}

static int
ordinary_pipe(void)
{
  int p[2];
  char in[128], out[128];
  int i;

  if(pipe(p) < 0)
    return -1;
  for(i = 0; i < sizeof(in); i++)
    in[i] = i;
  if(write(p[1], in, sizeof(in)) != sizeof(in) ||
     read(p[0], out, sizeof(out)) != sizeof(out))
    return -1;
  for(i = 0; i < sizeof(in); i++)
    if(in[i] != out[i])
      return -1;
  close(p[0]);
  close(p[1]);
  return 0;
}

static int
blocked_reader_transition(int commit)
{
  int p[2], pid;
  char c;

  if(pipe(p) < 0 || xbegin() <= 0)
    return -1;
  pid = fork();
  if(pid < 0)
    return -1;
  if(pid == 0){
    close(p[1]);
    // No writer data will arrive. Terminal transition must wake this read.
    if(read(p[0], &c, 1) >= 0)
      exit();
    exit();
  }
  close(p[0]);
  sleep(5); // let the child enter piperead's wait queue.
  if(commit){
    if(xcommit() < 0)
      return -1;
  } else if(xabort() < 0)
    return -1;
  close(p[1]);
  wait();
  return 0;
}

int
main(int argc, char **argv)
{
  int pid;

  if(argc < 2)
    fail("mode required");
  if(strcmp(argv[1], "console-abort") == 0){
    pid = fork();
    if(pid < 0)
      fail("fork");
    if(pid == 0){
      sleep(8);
      printf(1, "CONSOLE_ABORT_OBSERVER\n");
      exit();
    }
    if(xbegin() <= 0 || write(1, "CONSOLE_ABORT_LEAK\n", 19) < 0)
      fail("console abort setup");
    sleep(15);
    if(xabort() < 0)
      fail("console abort");
    wait();
    printf(1, "CONSOLE_ABORT_DONE\n");
  } else if(strcmp(argv[1], "console-commit") == 0){
    pid = fork();
    if(pid < 0)
      fail("fork");
    if(pid == 0){
      sleep(8);
      printf(1, "CONSOLE_COMMIT_OBSERVER\n");
      exit();
    }
    if(xbegin() <= 0 || write(1, "CONSOLE_COMMIT_PAYLOAD\n", 23) != 23)
      fail("console commit setup");
    sleep(15);
    if(xcommit() < 0)
      fail("console commit");
    wait();
    printf(1, "CONSOLE_COMMIT_DONE\n");
  } else if(strcmp(argv[1], "console-cap") == 0){
    memset(console_test_buf, 'Q', sizeof(console_test_buf));
    if(xbegin() <= 0 || write(1, console_test_buf, sizeof(console_test_buf)) !=
       sizeof(console_test_buf) ||
       write(1, "Z", 1) >= 0 || xabort() < 0)
      fail("console capacity handling");
    printf(1, "CONSOLE_CAPACITY_OK\n");
  } else if(strcmp(argv[1], "pipe-same") == 0){
    if(same_context_pipe() < 0)
      fail("same-context pipe");
    printf(1, "PIPE_SAME_DONE\n");
  } else if(strcmp(argv[1], "pipe-commit") == 0){
    if(observer() < 0)
      fail("outside pipe commit");
    printf(1, "PIPE_COMMIT_DONE\n");
  } else if(strcmp(argv[1], "pipe-abort") == 0){
    if(abort_observer() < 0)
      fail("outside pipe abort");
    printf(1, "PIPE_ABORT_DONE\n");
  } else if(strcmp(argv[1], "pipe-wrap") == 0){
    if(wrap_and_capacity() < 0)
      fail("ring wrap");
    printf(1, "PIPE_WRAP_OK\n");
  } else if(strcmp(argv[1], "pipe-full") == 0){
    if(full_pipe_commit() < 0)
      fail("full pipe commit");
    printf(1, "PIPE_FULL_DONE\n");
  } else if(strcmp(argv[1], "pipe-full-abort") == 0){
    if(full_pipe_abort() < 0)
      fail("full pipe abort");
    printf(1, "PIPE_FULL_ABORT_DONE\n");
  } else if(strcmp(argv[1], "pipe-mixed") == 0){
    if(mixed_readers() < 0)
      fail("mixed pipe readers");
    printf(1, "PIPE_MIXED_DONE\n");
  } else if(strcmp(argv[1], "pipe-multi-writer") == 0){
    if(multiple_writers() < 0)
      fail("multiple pipe writers");
    printf(1, "PIPE_MULTI_WRITER_DONE\n");
  } else if(strcmp(argv[1], "pipe-reader-exit") == 0){
    if(reader_exit() < 0)
      fail("pipe reader exit");
    printf(1, "PIPE_READER_EXIT_DONE\n");
  } else if(strcmp(argv[1], "io-metrics") == 0){
    if(io_metrics() < 0)
      fail("I/O metrics");
  } else if(strcmp(argv[1], "pipe-ordinary") == 0){
    if(ordinary_pipe() < 0)
      fail("ordinary pipe");
    printf(1, "PIPE_ORDINARY_OK\n");
  } else if(strcmp(argv[1], "pipe-blocked-commit") == 0){
    if(blocked_reader_transition(1) < 0)
      fail("blocked reader commit drain");
    printf(1, "PIPE_BLOCKED_COMMIT_DRAINED\n");
  } else if(strcmp(argv[1], "pipe-blocked-abort") == 0){
    if(blocked_reader_transition(0) < 0)
      fail("blocked reader abort drain");
    printf(1, "PIPE_BLOCKED_ABORT_DRAINED\n");
  } else if(strcmp(argv[1], "owner-exit") == 0){
    pid = fork();
    if(pid < 0)
      fail("fork");
    if(pid == 0){
      sleep(15);
      printf(1, "OWNER_EXIT_OBSERVER\n");
      exit();
    }
    if(xbegin() <= 0 || write(1, "OWNER_EXIT_LEAK\n", 16) < 0)
      fail("owner exit setup");
    exit();
  } else if(strcmp(argv[1], "child-abort") == 0){
    if(aborted_child_guard() < 0)
      fail("aborted child guard");
    printf(1, "CHILD_ABORT_DONE\n");
  } else {
    fail("unknown mode");
  }
  exit();
}
