#ifndef XV6_SHADOW_H
#define XV6_SHADOW_H


// A shadow block is private to one speculation and is keyed by the tuple
// (speculation ID, device, block number).
#define NSHADOW 64
#define NSHADOW_QUOTA 16

#define SPEC_RES_INODE 1
#define SPEC_RES_BLOCK 2
#define SPEC_RES_ALLOCATED 1
#define SPEC_RES_DEFERRED_FREE 2
#define NSPEC_RESERVATIONS 1024

struct shadow_block {
  uint dev;
  uint blockno;
  char data[512];
};

struct shadow_snapshot {
  int spec_id;
  uint cursor;
  uint count;
  int active;
};

// A token represents one operation's change to a resource reservation.
// cancel restores exactly the prior state; finish keeps a speculative state
// or drops an ordinary allocator's short-lived arbitration claim.
struct spec_reservation_token {
  int spec_id;
  int kind;
  uint dev;
  uint number;
  uint previous_version;
  uint installed_version;
  int previous_state;
  int installed_state;
  int active;
};

void shadowinit(void);
int shadow_read(int spec_id, uint dev, uint blockno, char *dst);
int shadow_write(int spec_id, uint dev, uint blockno, const char *src);
int shadow_read_image(int spec_id, uint dev, uint blockno, char *dst);
int shadow_write_image(int spec_id, uint dev, uint blockno, const char *src);
int shadow_write_versioned(int spec_id, uint dev, uint blockno,
                           uint base_version, const char *base,
                           const char *src);
uint shadow_base_version(uint dev, uint blockno);
int shadow_note_read(int spec_id, uint dev, uint blockno);
void shadow_note_base_write(uint dev, uint blockno);
int shadow_validate_versions(int spec_id);
int shadow_freeze(int spec_id);
int shadow_thaw(int spec_id);
int shadow_snapshot_begin(int spec_id, struct shadow_snapshot *snap);
int shadow_snapshot_next(struct shadow_snapshot *snap,
                         struct shadow_block *block);
void shadow_snapshot_end(struct shadow_snapshot *snap);
int shadow_validate(int spec_id);
void shadow_discard(int spec_id);
int shadow_count(int spec_id);
int shadow_capacity(void);
int shadow_peak(void);

int spec_reservation_claim(int spec_id, int kind, uint dev, uint number,
                           int state, struct spec_reservation_token *token);
int spec_reservation_check(int spec_id, int kind, uint dev, uint number);
int spec_reservation_state(int spec_id, int kind, uint dev, uint number);
void spec_reservation_cancel(struct spec_reservation_token *token);
void spec_reservation_finish(struct spec_reservation_token *token);
void spec_reservation_discard(int spec_id);

#endif
