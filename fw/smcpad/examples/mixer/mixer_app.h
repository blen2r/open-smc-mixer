/* SMC-Mixer application (examples/mixer). */
#ifndef MIXER_APP_H
#define MIXER_APP_H

#include <stdint.h>

void mixer_app_run(void);                   /* never returns */

/* mixer_blemidi.c */
void mixer_blemidi_begin(void);
void mixer_blemidi_send(const uint8_t *msg, uint32_t len);
void mixer_blemidi_poll(void);
int mixer_blemidi_read(uint8_t msg[3]);
int mixer_blemidi_connected(void);

#endif
