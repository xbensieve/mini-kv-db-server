#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "server.h"

void protocol_execute_command(server_t *server, char *line, size_t len,
                               char *resp_buf, size_t resp_buf_size,
                               size_t *out_len, int *out_close);

#endif /* PROTOCOL_H */
