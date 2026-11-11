#ifndef XV6_SPEC_H
#define XV6_SPEC_H

struct proc;
struct spec_ctx;

enum spec_state {
  SPEC_FREE = 0,
  SPEC_ACTIVE,
  SPEC_COMMITTING,
  SPEC_COMMITTED,
  SPEC_ABORTED
};

void specinit(void);
int spec_begin(struct proc *p);
int spec_commit(struct proc *p);
int spec_abort(struct proc *p);
void spec_fork_attach(struct proc *parent, struct proc *child);
void spec_proc_exit(struct proc *p);
int spec_mutation_allowed(struct proc *p);
int spec_op_enter(struct proc *p, struct spec_ctx **ctx);
void spec_op_leave(struct spec_ctx *ctx);

#endif
