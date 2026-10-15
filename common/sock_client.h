// common/sock_client.h UDS client common library: connect, encode, send/receive. Each business client embeds sock_client in its own struct to reuse it.
#ifndef SOCK_CLIENT_H
#define SOCK_CLIENT_H
#include <stdint.h>

#define SOCK_HDR_LEN   16
#define SOCK_VERSION   1
#define SOCK_OP_ERROR  0xFFFFFFFFu

typedef struct {
    int      fd;
    uint16_t version;       /* protocol version used when sending, defaults to SOCK_VERSION, can be changed to test malformed packets */
} sock_client;

typedef struct {
    uint32_t op;
    uint32_t len;
    uint8_t *body;          /* '\0'-terminated, the caller frees it with sock_reply_free */
} sock_reply;

/* Return value: 0 on success; -1 on communication/system error */
int  sock_connect(sock_client *c, const char *path);

/* Send a request and wait for the reply. Returns 0 on success; 1 if the server returned an error packet (out->body is the error text); -1 on communication error */
int  sock_call(sock_client *c, uint32_t op, const void *body, uint32_t len, sock_reply *out);

void sock_reply_free(sock_reply *r);
void sock_close(sock_client *c);
#endif /* SOCK_CLIENT_H */
