#ifndef SERVER_H
#define SERVER_H

int ecewo(unsigned short PORT);
void shutdown_hook(void (*hook)(void));
int init_router(void);
void reset_router(void);

#endif
