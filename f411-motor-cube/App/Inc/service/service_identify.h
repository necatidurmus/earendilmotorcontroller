/* ============================================================
 * App/Inc/service/service_identify.h
 * Identify algorithm: toggle sectors, read Hall, build map.
 * ============================================================ */
#ifndef SERVICE_IDENTIFY_H
#define SERVICE_IDENTIFY_H

#include <stdbool.h>

void ServiceIdentify_Start(void);
/* Returns true while running, false when done. */
bool ServiceIdentify_Update(void);
void ServiceIdentify_Cancel(void);

#endif /* SERVICE_IDENTIFY_H */
