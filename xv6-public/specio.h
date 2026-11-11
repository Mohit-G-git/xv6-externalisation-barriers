#ifndef XV6_SPECIO_H
#define XV6_SPECIO_H

int spec_console_write(int, const char *, int);
void spec_output_publish(int);
void spec_output_discard(int);
void console_output_publish(const char *, int);

#endif
