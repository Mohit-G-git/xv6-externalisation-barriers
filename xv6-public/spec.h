#ifndef XV6_SPEC_H
#define XV6_SPEC_H

struct proc;
struct spec_ctx;

// A reference to one active speculation operation. Subsystems use spec_id
// while this token is held; ctx remains opaque outside spec.c.
struct spec_op {
  struct spec_ctx *ctx;
  int spec_id;
  int active;
};

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
int spec_op_enter(struct proc *p, struct spec_op *op);
int spec_op_enter_exit(struct proc *p, struct spec_op *op);
void spec_op_abort_only(struct spec_op *op);
void spec_op_leave(struct spec_op *op);
int spec_file_attach(int spec_id);
void spec_file_detach(int spec_id);
enum spec_state spec_context_state(int spec_id);

#endif
