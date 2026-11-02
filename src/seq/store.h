#ifndef SEQ_STORE_H
#define SEQ_STORE_H

// ____________________________________________________________________________
//
// Pattern persistence in the HAL's 1KB flash user area.
// Save with the Setup button; patterns are restored on power-up.
// ____________________________________________________________________________

void seq_store_save(void);

// returns 1 if a valid save was found and loaded, 0 otherwise
u8 seq_store_load(void);

#endif
