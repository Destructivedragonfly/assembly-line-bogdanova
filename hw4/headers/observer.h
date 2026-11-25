#ifndef OBSERVER_H
#define OBSERVER_H

/* Наблюдатель: TCP-сервер, рассылающий события подключённым клиентам */
int observer_start(const char *ip, const char *port);
void observer_stop(void);
void observer_log(const char *line); /* передать событие клиентам */

#endif
