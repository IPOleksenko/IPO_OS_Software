#ifndef OPENSSL_ERR_H
#define OPENSSL_ERR_H
unsigned long ERR_get_error(void);
unsigned long ERR_peek_error(void);
void ERR_clear_error(void);
const char *ERR_error_string(unsigned long e, char *buf);
#endif
