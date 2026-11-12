#include "types.h"
#include "user.h"

static void
fail(char *message)
{
  xabort();
  printf(2, "XEXEC FAIL: %s\n", message);
  exit();
}

int
main(int argc, char **argv)
{
  int observer, participant;
  char *child_argv[3];

  if(argc == 2 && strcmp(argv[1], "child") == 0){
    if(write(1, "EXEC_CHILD_OUTPUT\n", 18) != 18)
      exit();
    exit();
  }

  observer = fork();
  if(observer < 0)
    fail("observer fork");
  if(observer == 0){
    sleep(10);
    write(1, "EXEC_PRECOMMIT\n", 15);
    exit();
  }

  if(xbegin() <= 0)
    fail("begin");
  participant = fork();
  if(participant < 0)
    fail("participant fork");
  if(participant == 0){
    child_argv[0] = "xexec";
    child_argv[1] = "child";
    child_argv[2] = 0;
    exec("xexec", child_argv);
    exit();
  }

  wait();
  wait();
  if(xcommit() < 0)
    fail("commit after exec participant exit");
  if(write(1, "EXEC_COMMIT_DONE\n", 17) != 17)
    fail("completion marker");
  exit();
}
