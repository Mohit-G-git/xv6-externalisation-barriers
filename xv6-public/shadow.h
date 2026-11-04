#ifndef XV6_SHADOW_H
#define XV6_SHADOW_H

// A shadow block is private to one speculation and is keyed by the tuple
// (speculation ID, device, block number).
#define NSHADOW 64
#define NSHADOW_QUOTA 16

void shadowinit(void);
int shadow_read(int spec_id, uint dev, uint blockno, char *dst);
int shadow_write(int spec_id, uint dev, uint blockno, const char *src);
int shadow_validate(int spec_id);
void shadow_discard(int spec_id);
int shadow_count(int spec_id);
int shadow_capacity(void);
int shadow_peak(void);

#endif
