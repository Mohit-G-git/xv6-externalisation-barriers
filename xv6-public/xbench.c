// xbench.c — xv6 Externalization Barriers Performance Benchmark Suite
//
// Output format (one line per measurement):
//   BENCH,experiment,mode,procs,blocks,iterations,bytes,ticks,status
//
// status values: OK, CONFLICT, ERROR
//
// Compile into xv6 guest filesystem via Makefile (_xbench in UPROGS).
// Run inside xv6 shell: xbench

#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

#define BSIZE          512      // xv6 block size (fs.h BSIZE)
#define NSHADOW_MAX    64       // shadow.h NSHADOW
#define QUOTA_MAX      16       // shadow.h NSHADOW_QUOTA per speculation

#define N_ITER         20       // iterations for latency experiments
#define N_PIPE_SZ      64       // bytes per pipe write
#define PIPE_FULL_BYTES 1024    // bytes offered to a full-pipe workload
static int blockcounts[] = {2, 4, 8, 16, 32, 64, 128};
#define N_BLOCKSIZES   7

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Print one BENCH result line
static void
emit(char *experiment, char *mode, int procs, int blocks,
     int iterations, int ticks, char *status)
{
  int bytes;

  bytes = 0;
  if(strcmp(experiment, "file_crwrd") == 0 ||
     strcmp(experiment, "file_read") == 0 ||
     strcmp(experiment, "spec_file_read") == 0 ||
     strcmp(experiment, "spec_write_commit") == 0 ||
     strcmp(experiment, "spec_write_abort") == 0)
    bytes = blocks * BSIZE * iterations;
  else if(strcmp(experiment, "console_write") == 0 ||
          strcmp(experiment, "spec_console_commit") == 0)
    bytes = iterations;
  else if(strcmp(experiment, "pipe_wrrd") == 0 ||
          strcmp(experiment, "spec_pipe_commit") == 0)
    bytes = iterations * N_PIPE_SZ;
  else if(strcmp(experiment, "pipe_full_ordinary") == 0 ||
          strcmp(experiment, "spec_pipe_full_abort") == 0)
    bytes = PIPE_FULL_BYTES;
  else if(strcmp(experiment, "shared_ctx_commit") == 0 ||
          strcmp(experiment, "indep_ctx_commit") == 0)
    bytes = procs * blocks * BSIZE;
  else if(strcmp(experiment, "contention_same_block") == 0 ||
          strcmp(experiment, "no_contention") == 0)
    bytes = procs * blocks * BSIZE;
  else if(strcmp(experiment, "nested_commit") == 0 ||
          strcmp(experiment, "nested_inner_abort") == 0)
    bytes = iterations * blocks * BSIZE;
  else if(strcmp(experiment, "shadow_capacity") == 0)
    bytes = blocks * BSIZE;

  printf(1, "BENCH,%s,%s,%d,%d,%d,%d,%d,%s\n",
         experiment, mode, procs, blocks, iterations, bytes, ticks, status);
}

static void
emit_pipe_full(char *experiment, char *mode, int bytes, int ticks, char *status)
{
  printf(1, "BENCH,%s,%s,1,0,1,%d,%d,%s\n",
         experiment, mode, bytes, ticks, status);
}

static void
emit_sweep(char *mode, int blocks, int bytes, int ticks, char *status)
{
  printf(1, "BENCH,write_sweep,%s,1,%d,1,%d,%d,%s\n",
         mode, blocks, bytes, ticks, status);
}

// Create or truncate a file, write `nblocks` * BSIZE bytes of 'x', close.
// Returns 0 on success, -1 on error.
static int
write_blocks(char *path, int nblocks)
{
  static char buf[BSIZE];
  int fd, i;

  memset(buf, 'x', BSIZE);
  unlink(path);
  fd = open(path, O_CREATE | O_RDWR);
  if (fd < 0)
    return -1;
  for (i = 0; i < nblocks; i++) {
    if (write(fd, buf, BSIZE) != BSIZE) {
      close(fd);
      unlink(path);
      return -1;
    }
  }
  close(fd);
  return 0;
}

// Read nblocks * BSIZE bytes back from path.  Returns 0 on success.
static int
read_blocks(char *path, int nblocks)
{
  static char buf[BSIZE];
  int fd, i;

  fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  for (i = 0; i < nblocks; i++) {
    if (read(fd, buf, BSIZE) != BSIZE) {
      close(fd);
      return -1;
    }
  }
  close(fd);
  return 0;
}

static int
overwrite_blocks(char *path, int nblocks)
{
  static char buf[BSIZE];
  int fd, i;

  memset(buf, 'y', BSIZE);
  fd = open(path, O_RDWR);
  if(fd < 0)
    return -1;
  for(i = 0; i < nblocks; i++){
    if(write(fd, buf, BSIZE) != BSIZE){
      close(fd);
      return -1;
    }
  }
  if(close(fd) < 0)
    return -1;
  return 0;
}

// ---------------------------------------------------------------------------
// Section A: Baseline (ordinary xv6, no speculation)
// ---------------------------------------------------------------------------

static void
bench_baseline_file(void)
{
  int i, start, ticks;

  // Create / write / read / delete loop — single BSIZE block
  start = uptime();
  for (i = 0; i < N_ITER; i++) {
    if (write_blocks("xb_base_file", 1) < 0 ||
        read_blocks("xb_base_file", 1) < 0 ||
        unlink("xb_base_file") < 0) {
      emit("file_crwrd", "baseline", 1, 1, i, 0, "ERROR");
      return;
    }
  }
  ticks = uptime() - start;
  emit("file_crwrd", "baseline", 1, 1, N_ITER, ticks, "OK");
}

static void
bench_file_read(int nblocks, int speculative)
{
  int i, start, ticks, result;
  char *path;

  path = speculative ? "xb_spec_read" : "xb_base_read";
  if(write_blocks(path, nblocks) < 0){
    emit(speculative ? "spec_file_read" : "file_read",
         speculative ? "spec" : "baseline", 1, nblocks, 0, 0, "ERROR");
    return;
  }
  start = uptime();
  for(i = 0; i < N_ITER; i++){
    if(speculative && xbegin() <= 0){
      emit("spec_file_read", "spec", 1, nblocks, i, 0, "ERROR");
      unlink(path);
      return;
    }
    result = read_blocks(path, nblocks);
    if(result < 0 || (speculative && xcommit() < 0)){
      if(speculative)
        xabort();
      emit(speculative ? "spec_file_read" : "file_read",
           speculative ? "spec" : "baseline", 1, nblocks, i, 0,
           speculative ? "CONFLICT" : "ERROR");
      unlink(path);
      return;
    }
  }
  ticks = uptime() - start;
  unlink(path);
  emit(speculative ? "spec_file_read" : "file_read",
       speculative ? "spec" : "baseline", 1, nblocks, N_ITER, ticks, "OK");
}

static void
bench_baseline_dir(void)
{
  int i, start, ticks;

  start = uptime();
  for (i = 0; i < N_ITER; i++) {
    if (mkdir("xb_base_dir") < 0 || unlink("xb_base_dir") < 0) {
      emit("dir_mkdir_rmdir", "baseline", 1, 0, i, 0, "ERROR");
      return;
    }
  }
  ticks = uptime() - start;
  emit("dir_mkdir_rmdir", "baseline", 1, 0, N_ITER, ticks, "OK");
}

static void
bench_spec_directory(void)
{
  int i, start, ticks;

  start = uptime();
  for(i = 0; i < N_ITER; i++){
    if(xbegin() <= 0 || mkdir("xb_spec_dir") < 0 || xcommit() < 0){
      xabort();
      emit("spec_dir_mkdir_commit", "spec", 1, 0, i, 0, "ERROR");
      return;
    }
    if(unlink("xb_spec_dir") < 0){
      emit("spec_dir_mkdir_commit", "spec", 1, 0, i, 0, "ERROR");
      return;
    }
  }
  ticks = uptime() - start;
  emit("spec_dir_mkdir_commit", "spec", 1, 0, N_ITER, ticks, "OK");
}

static void
bench_baseline_console(void)
{
  int i, start, ticks;

  // Write a small string to stderr (fd 2) N_ITER times
  start = uptime();
  for (i = 0; i < N_ITER; i++) {
    write(2, ".", 1);
  }
  ticks = uptime() - start;
  printf(1, "\n");  // newline after the dots
  emit("console_write", "baseline", 1, 0, N_ITER, ticks, "OK");
}

static void
bench_baseline_pipe(void)
{
  int i, start, ticks, p[2];
  static char wbuf[N_PIPE_SZ];
  static char rbuf[N_PIPE_SZ];
  int pid, status;

  memset(wbuf, 'P', N_PIPE_SZ);

  start = uptime();
  for (i = 0; i < N_ITER; i++) {
    if (pipe(p) < 0) {
      emit("pipe_wrrd", "baseline", 1, 0, i, 0, "ERROR");
      return;
    }
    pid = fork();
    if (pid < 0) {
      emit("pipe_wrrd", "baseline", 1, 0, i, 0, "ERROR");
      close(p[0]); close(p[1]);
      return;
    }
    if (pid == 0) {
      // child: writer
      close(p[0]);
      write(p[1], wbuf, N_PIPE_SZ);
      close(p[1]);
      exit();
    }
    // parent: reader
    close(p[1]);
    read(p[0], rbuf, N_PIPE_SZ);
    close(p[0]);
    status = wait();
    (void)status;
  }
  ticks = uptime() - start;
  emit("pipe_wrrd", "baseline", 1, 0, N_ITER, ticks, "OK");
}

static void
bench_full_pipe(int speculative)
{
  int p[2], pid, start, ticks, written, total;
  char buf[PIPE_FULL_BYTES];
  char drain[64];

  memset(buf, 'F', sizeof(buf));
  if(pipe(p) < 0){
    emit_pipe_full(speculative ? "spec_pipe_full_abort" :
                   "pipe_full_ordinary",
                   speculative ? "spec" : "baseline", 0, 0, "ERROR");
    return;
  }
  if(speculative){
    if(xbegin() <= 0){
      close(p[0]);
      close(p[1]);
      emit_pipe_full("spec_pipe_full_abort", "spec", 0, 0, "ERROR");
      return;
    }
    start = uptime();
    written = write(p[1], buf, sizeof(buf));
    ticks = uptime() - start;
    if(xabort() < 0)
      written = -1;
    close(p[0]);
    close(p[1]);
    emit_pipe_full("spec_pipe_full_abort", "spec",
                   written < 0 ? 0 : written, ticks,
                   written < 0 ? "ERROR" : "OK");
    return;
  }

  pid = fork();
  if(pid < 0){
    close(p[0]);
    close(p[1]);
    emit_pipe_full("pipe_full_ordinary", "baseline", 0, 0, "ERROR");
    return;
  }
  if(pid == 0){
    close(p[1]);
    while(read(p[0], drain, sizeof(drain)) > 0)
      ;
    close(p[0]);
    exit();
  }
  close(p[0]);
  start = uptime();
  written = write(p[1], buf, sizeof(buf));
  close(p[1]);
  wait();
  ticks = uptime() - start;
  total = written == sizeof(buf) ? written : 0;
  emit_pipe_full("pipe_full_ordinary", "baseline", total, ticks,
                 total == sizeof(buf) ? "OK" : "ERROR");
}

// ---------------------------------------------------------------------------
// Section B: Speculative operations
// ---------------------------------------------------------------------------

// B1. xbegin + file write (nblocks blocks) + xcommit
static void
bench_spec_commit_write(int nblocks)
{
  int i, start, ticks, ctx;

  start = uptime();
  for (i = 0; i < N_ITER; i++) {
    ctx = xbegin();
    if (ctx <= 0) {
      emit("spec_write_commit", "spec", 1, nblocks, i, 0, "ERROR");
      return;
    }
    if (write_blocks("xb_spec_cw", nblocks) < 0) {
      xabort();
      unlink("xb_spec_cw");
      emit("spec_write_commit", "spec", 1, nblocks, i, 0, "CAPACITY");
      return;
    }
    if (xcommit() < 0) {
      xabort();
      emit("spec_write_commit", "spec", 1, nblocks, i, 0, "CONFLICT");
      unlink("xb_spec_cw");
      return;
    }
    unlink("xb_spec_cw");
  }
  ticks = uptime() - start;
  emit("spec_write_commit", "spec", 1, nblocks, N_ITER, ticks, "OK");
}

// B2. xbegin + file write (nblocks blocks) + xabort
static void
bench_spec_abort_write(int nblocks)
{
  int i, start, ticks, ctx;

  start = uptime();
  for (i = 0; i < N_ITER; i++) {
    ctx = xbegin();
    if (ctx <= 0) {
      emit("spec_write_abort", "spec", 1, nblocks, i, 0, "ERROR");
      return;
    }
    if (write_blocks("xb_spec_aw", nblocks) < 0) {
      xabort();
      emit("spec_write_abort", "spec", 1, nblocks, i, 0, "CAPACITY");
      return;
    }
    if (xabort() < 0) {
      emit("spec_write_abort", "spec", 1, nblocks, i, 0, "ERROR");
      return;
    }
    // After abort the file should not exist in the real filesystem
  }
  ticks = uptime() - start;
  emit("spec_write_abort", "spec", 1, nblocks, N_ITER, ticks, "OK");
}

static void
bench_write_size(int blocks)
{
  static char data[128 * BSIZE];
  int bytes, fd, start, write_ticks, commit_start, commit_ticks;
  int ctx, wrote, close_result, abort_start, abort_ticks;

  bytes = blocks * BSIZE;
  memset(data, 'W', bytes);
  unlink("xb_size_sweep");
  fd = open("xb_size_sweep", O_CREATE|O_RDWR);
  if(fd < 0){
    unlink("xb_size_sweep");
    emit_sweep("baseline", blocks, bytes, 0, "ERROR");
    emit_sweep("spec-write", blocks, bytes, 0, "NOT_RUN");
    emit_sweep("spec-commit", blocks, bytes, 0, "NOT_RUN");
    return;
  }
  wrote = write(fd, data, bytes);
  close_result = close(fd);
  if(wrote != bytes || close_result < 0){
    unlink("xb_size_sweep");
    emit_sweep("baseline", blocks, bytes, 0, "ERROR");
    emit_sweep("spec-write", blocks, bytes, 0, "NOT_RUN");
    emit_sweep("spec-commit", blocks, bytes, 0, "NOT_RUN");
    return;
  }

  fd = open("xb_size_sweep", O_RDWR);
  if(fd < 0){
    unlink("xb_size_sweep");
    emit_sweep("baseline", blocks, bytes, 0, "ERROR");
    return;
  }
  start = uptime();
  wrote = write(fd, data, bytes);
  write_ticks = uptime() - start;
  close(fd);
  emit_sweep("baseline", blocks, bytes, write_ticks,
             wrote == bytes ? "OK" : "ERROR");

  ctx = xbegin();
  if(ctx <= 0){
    emit_sweep("spec-write", blocks, bytes, 0, "ERROR");
    emit_sweep("spec-commit", blocks, bytes, 0, "NOT_RUN");
    unlink("xb_size_sweep");
    return;
  }
  fd = open("xb_size_sweep", O_RDWR);
  if(fd < 0){
    xabort();
    emit_sweep("spec-write", blocks, bytes, 0, "ERROR");
    emit_sweep("spec-commit", blocks, bytes, 0, "NOT_RUN");
    unlink("xb_size_sweep");
    return;
  }
  start = uptime();
  wrote = write(fd, data, bytes);
  write_ticks = uptime() - start;
  close(fd);
  emit_sweep("spec-write", blocks, wrote > 0 ? wrote : 0, write_ticks,
             wrote == bytes ? "OK" : "CAPACITY");
  if(wrote != bytes){
    xabort();
    emit_sweep("spec-commit", blocks, bytes, 0, "NOT_RUN");
    unlink("xb_size_sweep");
    return;
  }
  commit_start = uptime();
  wrote = xcommit();
  commit_ticks = uptime() - commit_start;
  emit_sweep("spec-commit", blocks, bytes, commit_ticks,
             wrote == 0 ? "OK" : "CONFLICT");
  if(wrote < 0)
    xabort();

  ctx = xbegin();
  if(ctx <= 0){
    emit_sweep("spec-abort-write", blocks, bytes, 0, "ERROR");
    emit_sweep("spec-abort-cleanup", blocks, bytes, 0, "NOT_RUN");
    unlink("xb_size_sweep");
    return;
  }
  fd = open("xb_size_sweep", O_RDWR);
  if(fd < 0){
    xabort();
    emit_sweep("spec-abort-write", blocks, bytes, 0, "ERROR");
    emit_sweep("spec-abort-cleanup", blocks, bytes, 0, "NOT_RUN");
    unlink("xb_size_sweep");
    return;
  }
  start = uptime();
  wrote = write(fd, data, bytes);
  write_ticks = uptime() - start;
  close(fd);
  emit_sweep("spec-abort-write", blocks, wrote > 0 ? wrote : 0,
             write_ticks, wrote == bytes ? "OK" : "CAPACITY");
  abort_start = uptime();
  close_result = xabort();
  abort_ticks = uptime() - abort_start;
  emit_sweep("spec-abort-cleanup", blocks, bytes, abort_ticks,
             close_result == 0 ? "OK" : "ERROR");
  unlink("xb_size_sweep");
}

// B3. xbegin + xcommit (empty transaction)
static void
bench_spec_empty_commit(void)
{
  int i, start, ticks;

  start = uptime();
  for (i = 0; i < N_ITER; i++) {
    if (xbegin() <= 0) {
      emit("spec_empty_commit", "spec", 1, 0, i, 0, "ERROR");
      return;
    }
    if (xcommit() < 0) {
      xabort();
      emit("spec_empty_commit", "spec", 1, 0, i, 0, "ERROR");
      return;
    }
  }
  ticks = uptime() - start;
  emit("spec_empty_commit", "spec", 1, 0, N_ITER, ticks, "OK");
}

// B4. xbegin + xabort (empty transaction)
static void
bench_spec_empty_abort(void)
{
  int i, start, ticks;

  start = uptime();
  for (i = 0; i < N_ITER; i++) {
    if (xbegin() <= 0) {
      emit("spec_empty_abort", "spec", 1, 0, i, 0, "ERROR");
      return;
    }
    if (xabort() < 0) {
      xabort();
      emit("spec_empty_abort", "spec", 1, 0, i, 0, "ERROR");
      return;
    }
  }
  ticks = uptime() - start;
  emit("spec_empty_abort", "spec", 1, 0, N_ITER, ticks, "OK");
}

// B5. Nested commit: xbegin + xbegin + write + xcommit + xcommit
static void
bench_nested_commit(void)
{
  int i, start, ticks;

  start = uptime();
  for (i = 0; i < N_ITER; i++) {
    if (xbegin() <= 0) {
      emit("nested_commit", "spec", 1, 1, i, 0, "ERROR");
      return;
    }
    if (xbegin() <= 0) {
      xabort();
      emit("nested_commit", "spec", 1, 1, i, 0, "ERROR");
      return;
    }
    if (write_blocks("xb_nest_c", 1) < 0) {
      xabort(); xabort();
      emit("nested_commit", "spec", 1, 1, i, 0, "ERROR");
      return;
    }
    if (xcommit() < 0) {
      xabort();
      xabort();
      emit("nested_commit", "spec", 1, 1, i, 0, "CONFLICT");
      unlink("xb_nest_c");
      return;
    }
    if (xcommit() < 0) {
      xabort();
      emit("nested_commit", "spec", 1, 1, i, 0, "CONFLICT");
      unlink("xb_nest_c");
      return;
    }
    unlink("xb_nest_c");
  }
  ticks = uptime() - start;
  emit("nested_commit", "spec", 1, 1, N_ITER, ticks, "OK");
}

// B6. Nested abort: xbegin + xbegin + write + xabort + xcommit
// Inner aborts, outer survives and commits empty
static void
bench_nested_inner_abort(void)
{
  int i, start, ticks;

  start = uptime();
  for (i = 0; i < N_ITER; i++) {
    if (xbegin() <= 0) {
      emit("nested_inner_abort", "spec", 1, 1, i, 0, "ERROR");
      return;
    }
    if (xbegin() <= 0) {
      xabort();
      emit("nested_inner_abort", "spec", 1, 1, i, 0, "ERROR");
      return;
    }
    if (write_blocks("xb_nest_a", 1) < 0) {
      xabort(); xabort();
      emit("nested_inner_abort", "spec", 1, 1, i, 0, "ERROR");
      return;
    }
    if (xabort() < 0) {   // inner abort
      xabort();
      emit("nested_inner_abort", "spec", 1, 1, i, 0, "ERROR");
      return;
    }
    if (xcommit() < 0) {   // outer commit (empty write-set)
      xabort();
      emit("nested_inner_abort", "spec", 1, 1, i, 0, "CONFLICT");
      return;
    }
    // File should NOT exist after inner abort
  }
  ticks = uptime() - start;
  emit("nested_inner_abort", "spec", 1, 1, N_ITER, ticks, "OK");
}

// B7. Console: xbegin + write to stderr + xcommit
static void
bench_spec_console(void)
{
  int i, start, ticks;

  start = uptime();
  for (i = 0; i < N_ITER; i++) {
    if (xbegin() <= 0) {
      emit("spec_console_commit", "spec", 1, 0, i, 0, "ERROR");
      return;
    }
    write(2, ".", 1);
    if (xcommit() < 0) {
      xabort();
      emit("spec_console_commit", "spec", 1, 0, i, 0, "CONFLICT");
      return;
    }
  }
  printf(1, "\n");
  ticks = uptime() - start;
  emit("spec_console_commit", "spec", 1, 0, N_ITER, ticks, "OK");
}

// B8. Pipe: xbegin + pipe_write (same-context reader) + xcommit
// Writer and reader share the same speculation context.
static void
bench_spec_pipe(void)
{
  int i, start, ticks, p[2];
  static char wbuf[N_PIPE_SZ];
  static char rbuf[N_PIPE_SZ];

  memset(wbuf, 'S', N_PIPE_SZ);

  if (pipe(p) < 0) {
    emit("spec_pipe_commit", "spec", 1, 0, 0, 0, "ERROR");
    return;
  }

  start = uptime();
  for (i = 0; i < N_ITER; i++) {
    if (xbegin() <= 0) {
      emit("spec_pipe_commit", "spec", 1, 0, i, 0, "ERROR");
      close(p[0]); close(p[1]);
      return;
    }
    if (write(p[1], wbuf, N_PIPE_SZ) != N_PIPE_SZ) {
      xabort();
      emit("spec_pipe_commit", "spec", 1, 0, i, 0, "ERROR");
      close(p[0]); close(p[1]);
      return;
    }
    // Same-context read of our own speculative bytes
    if (read(p[0], rbuf, N_PIPE_SZ) != N_PIPE_SZ) {
      xabort();
      emit("spec_pipe_commit", "spec", 1, 0, i, 0, "ERROR");
      close(p[0]); close(p[1]);
      return;
    }
    if (xcommit() < 0) {
      xabort();
      emit("spec_pipe_commit", "spec", 1, 0, i, 0, "CONFLICT");
      close(p[0]); close(p[1]);
      return;
    }
  }
  ticks = uptime() - start;
  close(p[0]);
  close(p[1]);
  emit("spec_pipe_commit", "spec", 1, 0, N_ITER, ticks, "OK");
}

// ---------------------------------------------------------------------------
// Section C: Concurrency
// ---------------------------------------------------------------------------

// C1. Multiple processes sharing ONE speculation context (via fork inside spec)
// Parent calls xbegin(), then forks `nprocs-1` children; all write independently,
// parent calls xcommit().
static void
bench_shared_context(int nprocs)
{
  int start, ticks, ctx;
  int i, pid, p[2], failed;
  char status;
  static char path[16];

  if(pipe(p) < 0){
    emit("shared_ctx_commit", "concurrency", nprocs, 1, 1, 0, "ERROR");
    return;
  }
  ctx = xbegin();
  if (ctx <= 0) {
    close(p[0]);
    close(p[1]);
    emit("shared_ctx_commit", "concurrency", nprocs, 1, 1, 0, "ERROR");
    return;
  }

  start = uptime();

  // Spawn nprocs-1 children inside the shared context
  for (i = 1; i < nprocs; i++) {
    pid = fork();
    if (pid < 0) {
      xabort();
      close(p[0]);
      close(p[1]);
      emit("shared_ctx_commit", "concurrency", nprocs, 1, 0, 0, "ERROR");
      return;
    }
    if (pid == 0) {
      // Child: write one block then exit (staying in shared spec context)
      close(p[0]);
      path[0] = 'x'; path[1] = 'b'; path[2] = 's'; path[3] = 'c';
      path[4] = '0' + i; path[5] = 0;
      status = write_blocks(path, 1) == 0 ? 'O' : 'E';
      write(p[1], &status, 1);
      close(p[1]);
      exit();
    }
  }

  // Parent also writes
  failed = 0;
  path[0] = 'x'; path[1] = 'b'; path[2] = 's'; path[3] = 'c';
  path[4] = '0'; path[5] = 0;
  if(write_blocks(path, 1) < 0)
    failed = 1;

  // Wait for all children
  for (i = 1; i < nprocs; i++)
    wait();
  close(p[1]);
  for (i = 1; i < nprocs; i++)
    if(read(p[0], &status, 1) != 1 || status != 'O')
      failed = 1;
  close(p[0]);
  if(failed){
    xabort();
    emit("shared_ctx_commit", "concurrency", nprocs, 1, 1,
         uptime() - start, "CAPACITY");
    return;
  }

  if (xcommit() < 0) {
    ticks = uptime() - start;
    xabort();
    // Clean up shadow files
    for (i = 0; i < nprocs; i++) {
      path[4] = '0' + i;
      unlink(path);
    }
    emit("shared_ctx_commit", "concurrency", nprocs, 1, 1, ticks, "CONFLICT");
    return;
  }
  ticks = uptime() - start;

  for (i = 0; i < nprocs; i++) {
    path[4] = '0' + i;
    unlink(path);
  }
  emit("shared_ctx_commit", "concurrency", nprocs, 1, 1, ticks, "OK");
}

// C2. Multiple INDEPENDENT contexts, one process each, writing different blocks.
// Each independent context starts, writes a unique block, then commits.
// We time the whole parallel run from parent's perspective.
static void
bench_independent_contexts(int nprocs)
{
  int start, ticks, p[2], failed, capacity, conflict;
  int i, j, pid;
  char status;
  static char path[16];

  if(pipe(p) < 0){
    emit("indep_ctx_commit", "concurrency", nprocs, 1, 1, 0, "ERROR");
    return;
  }
  start = uptime();

  for (i = 0; i < nprocs; i++) {
    pid = fork();
    if (pid < 0) {
      close(p[0]);
      close(p[1]);
      for (j = 0; j < i; j++)
        wait();
      emit("indep_ctx_commit", "concurrency", nprocs, 1, i, 0, "ERROR");
      return;
    }
    if (pid == 0) {
      close(p[0]);
      // Child: its own independent spec context
      path[0] = 'x'; path[1] = 'b'; path[2] = 'i'; path[3] = 'c';
      path[4] = '0' + i; path[5] = 0;
      if (xbegin() <= 0){
        status = 'Q';
      } else if (write_blocks(path, 1) < 0){
        xabort();
        status = 'Q';
      } else if (xcommit() < 0){
        xabort();
        status = 'C';
      } else {
        if(unlink(path) < 0)
          status = 'E';
        else
          status = 'O';
      }
      write(p[1], &status, 1);
      close(p[1]);
      exit();
    }
  }

  close(p[1]);
  for (i = 0; i < nprocs; i++)
    wait();
  failed = capacity = conflict = 0;
  for (i = 0; i < nprocs; i++){
    if(read(p[0], &status, 1) != 1 || status == 'E')
      failed = 1;
    else if(status == 'Q')
      capacity = 1;
    else if(status == 'C')
      conflict = 1;
  }
  close(p[0]);

  ticks = uptime() - start;
  emit("indep_ctx_commit", "concurrency", nprocs, 1, 1, ticks,
       failed ? "ERROR" : capacity ? "CAPACITY" :
       conflict ? "CONFLICT" : "OK");
}

// C3. Contention: 2 independent contexts writing the SAME block.
// One should commit; the other may get a CONFLICT.
static void
bench_contention_same_block(void)
{
  int start, ticks;
  int pid, p[2];
  char result;
  static char path[16];

  // Pre-create the file so both contexts write to same inode/block
  path[0] = 'x'; path[1] = 'b'; path[2] = 'c'; path[3] = 'n';
  path[4] = 't'; path[5] = 0;
  if(write_blocks(path, 1) < 0){
    emit("contention_same_block", "concurrency", 2, 1, 1, 0, "ERROR");
    return;
  }

  if (pipe(p) < 0) {
    unlink(path);
    emit("contention_same_block", "concurrency", 2, 1, 1, 0, "ERROR");
    return;
  }

  start = uptime();

  pid = fork();
  if (pid < 0) {
    close(p[0]); close(p[1]);
    unlink(path);
    emit("contention_same_block", "concurrency", 2, 1, 1, 0, "ERROR");
    return;
  }

  if (pid == 0) {
    // Child: spec context A writes same block
    close(p[0]);
    if (xbegin() <= 0) { write(p[1], "E", 1); close(p[1]); exit(); }
    if(overwrite_blocks(path, 1) < 0){
      xabort();
      write(p[1], "E", 1);
      close(p[1]);
      exit();
    }
    if (xcommit() < 0) {
      xabort();
      write(p[1], "C", 1);  // CONFLICT
    } else {
      write(p[1], "O", 1);  // OK
    }
    close(p[1]);
    exit();
  }

  // Parent: spec context B writes same block
  close(p[1]);
  if (xbegin() <= 0) {
    wait();
    close(p[0]);
    unlink(path);
    emit("contention_same_block", "concurrency", 2, 1, 1, 0, "ERROR");
    return;
  }
  if(overwrite_blocks(path, 1) < 0){
    xabort();
    wait();
    read(p[0], &result, 1);
    close(p[0]);
    unlink(path);
    emit("contention_same_block", "concurrency", 2, 1, 1,
         uptime() - start, "ERROR");
    return;
  }
  if (xcommit() < 0) {
    // Parent got CONFLICT; child may have succeeded
    ticks = uptime() - start;
    xabort();
    wait();
    read(p[0], &result, 1);
    close(p[0]);
    unlink(path);
    emit("contention_same_block", "concurrency", 2, 1, 1, ticks, "CONFLICT");
    return;
  }

  ticks = uptime() - start;
  wait();
  read(p[0], &result, 1);
  close(p[0]);
  unlink(path);
  // At least one of the two should have succeeded
  if (result == 'O') {
    emit("contention_same_block", "concurrency", 2, 1, 1, ticks, "OK");
  } else {
    emit("contention_same_block", "concurrency", 2, 1, 1, ticks, "CONFLICT");
  }
}

// C4. No contention: 2 independent contexts writing DIFFERENT blocks.
// Both should commit successfully.
static void
bench_no_contention(void)
{
  int start, ticks;
  int pid, p[2];
  char result;
  static char pA[16], pB[16];

  pA[0] = 'x'; pA[1] = 'b'; pA[2] = 'n'; pA[3] = 'c'; pA[4] = 'A'; pA[5] = 0;
  pB[0] = 'x'; pB[1] = 'b'; pB[2] = 'n'; pB[3] = 'c'; pB[4] = 'B'; pB[5] = 0;
  if(write_blocks(pA, 1) < 0 || write_blocks(pB, 1) < 0){
    unlink(pA);
    unlink(pB);
    emit("no_contention", "concurrency", 2, 1, 1, 0, "ERROR");
    return;
  }

  if (pipe(p) < 0) {
    emit("no_contention", "concurrency", 2, 1, 1, 0, "ERROR");
    return;
  }

  start = uptime();

  pid = fork();
  if (pid < 0) {
    close(p[0]); close(p[1]);
    emit("no_contention", "concurrency", 2, 1, 1, 0, "ERROR");
    return;
  }

  if (pid == 0) {
    close(p[0]);
    if (xbegin() <= 0) { write(p[1], "E", 1); close(p[1]); exit(); }
    if(overwrite_blocks(pB, 1) < 0){
      xabort();
      write(p[1], "E", 1);
      close(p[1]);
      exit();
    }
    if (xcommit() < 0) {
      xabort();
      write(p[1], "C", 1);
    } else {
      unlink(pB);
      write(p[1], "O", 1);
    }
    close(p[1]);
    exit();
  }

  close(p[1]);
  if (xbegin() <= 0) {
    wait();
    close(p[0]);
    unlink(pA);
    unlink(pB);
    emit("no_contention", "concurrency", 2, 1, 1, 0, "ERROR");
    return;
  }
  if(overwrite_blocks(pA, 1) < 0){
    xabort();
    wait();
    read(p[0], &result, 1);
    close(p[0]);
    unlink(pA);
    unlink(pB);
    emit("no_contention", "concurrency", 2, 1, 1,
         uptime() - start, "ERROR");
    return;
  }
  if (xcommit() < 0) {
    ticks = uptime() - start;
    xabort();
    wait();
    read(p[0], &result, 1);
    close(p[0]);
    unlink(pB);
    unlink(pA);
    emit("no_contention", "concurrency", 2, 1, 1, ticks, "CONFLICT");
    return;
  }

  ticks = uptime() - start;
  wait();
  read(p[0], &result, 1);
  close(p[0]);
  unlink(pA);
  unlink(pB);

  if (result == 'O') {
    emit("no_contention", "concurrency", 2, 1, 1, ticks, "OK");
  } else {
    emit("no_contention", "concurrency", 2, 1, 1, ticks, "CONFLICT");
  }
}

// ---------------------------------------------------------------------------
// Section D: Shadow capacity inference
// ---------------------------------------------------------------------------
// Write N distinct blocks inside one spec context to discover where the
// NSHADOW_MAX (64) or NSHADOW_QUOTA (16) limit is hit.
// We use a preallocated large file and overwrite different blocks.
static void
bench_shadow_capacity(void)
{
  static char buf[BSIZE];
  int fd, ctx, blocks_written, i;
  static char path[16];

  path[0] = 'x'; path[1] = 'b'; path[2] = 's'; path[3] = 'h';
  path[4] = 'c'; path[5] = 'a'; path[6] = 'p'; path[7] = 0;

  // Pre-create a file large enough for NSHADOW_MAX blocks
  memset(buf, 'C', BSIZE);
  unlink(path);
  fd = open(path, O_CREATE | O_RDWR);
  if (fd < 0) {
    emit("shadow_capacity", "spec", 1, 0, 1, 0, "ERROR");
    return;
  }
  for (i = 0; i < NSHADOW_MAX; i++) {
    if (write(fd, buf, BSIZE) != BSIZE) {
      close(fd);
      unlink(path);
      emit("shadow_capacity", "spec", 1, 0, 1, 0, "ERROR");
      return;
    }
  }
  close(fd);

  // Now inside a speculation, overwrite each block; count when writes fail
  ctx = xbegin();
  if (ctx <= 0) {
    unlink(path);
    emit("shadow_capacity", "spec", 1, 0, 1, 0, "ERROR");
    return;
  }

  fd = open(path, O_RDWR);
  if (fd < 0) {
    xabort();
    unlink(path);
    emit("shadow_capacity", "spec", 1, 0, 1, 0, "ERROR");
    return;
  }

  blocks_written = 0;
  for (i = 0; i < NSHADOW_MAX; i++) {
    // Seek to block i
    // xv6 has no lseek — reopen and read past earlier blocks
    // Instead, write sequentially — each write goes to next block
    if (write(fd, buf, BSIZE) == BSIZE) {
      blocks_written++;
    } else {
      break;  // quota hit
    }
  }
  close(fd);
  xabort();  // discard all speculative writes
  unlink(path);

  // Report inferred quota
  emit("shadow_capacity", "spec", 1, blocks_written, 1, 0, "OK");
  printf(1, "BENCH,shadow_capacity_note,spec,1,0,1,0,inferred_quota=%d\n",
         blocks_written);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int
main(void)
{
  int i;

  printf(1, "BENCH,experiment,mode,procs,blocks,iterations,bytes,ticks,status\n");
  printf(1, "BENCH_TIMER,uptime_ticks,1 tick per timer interrupt\n");

  // ---- Section A: Baseline ----
  printf(1, "# Section A: Baseline operations\n");
  bench_baseline_file();
  bench_baseline_dir();
  bench_spec_directory();
  bench_baseline_console();
  bench_baseline_pipe();
  bench_full_pipe(0);
  bench_full_pipe(1);
  for (i = 0; i < N_BLOCKSIZES; i++) {
    bench_file_read(blockcounts[i], 0);
    bench_file_read(blockcounts[i], 1);
  }

  // ---- Section B: Speculative operations ----
  printf(1, "# Section B: Speculative operations\n");
  for (i = 0; i < N_BLOCKSIZES; i++) {
    bench_spec_commit_write(blockcounts[i]);
    bench_spec_abort_write(blockcounts[i]);
    bench_write_size(blockcounts[i]);
  }
  bench_spec_empty_commit();
  bench_spec_empty_abort();
  bench_nested_commit();
  bench_nested_inner_abort();
  bench_spec_console();
  bench_spec_pipe();

  // ---- Section C: Concurrency ----
  printf(1, "# Section C: Concurrency\n");
  bench_shared_context(1);
  bench_shared_context(2);
  bench_shared_context(4);
  bench_shared_context(8);
  bench_shared_context(16);
  bench_shared_context(32);
  bench_independent_contexts(1);
  bench_independent_contexts(2);
  bench_independent_contexts(4);
  bench_independent_contexts(8);
  bench_independent_contexts(16);
  bench_independent_contexts(32);
  bench_contention_same_block();
  bench_no_contention();

  // ---- Section D: Shadow capacity ----
  printf(1, "# Section D: Shadow capacity inference\n");
  bench_shadow_capacity();

  xstats();
  printf(1, "XBENCH DONE\n");
  exit();
}
