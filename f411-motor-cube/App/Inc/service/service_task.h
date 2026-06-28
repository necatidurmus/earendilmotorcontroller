#ifndef SERVICE_TASK_H
#define SERVICE_TASK_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SVC_NONE = 0,
    SVC_SCAN,
    SVC_TEST,
    SVC_IDENTIFY
} ServiceTaskType;

void ServiceTask_Init(void);

/* Request a service task.  Motor must be stopped. */
void ServiceTask_Request(ServiceTaskType task);

/* Cancel any active service task. */
void ServiceTask_Cancel(void);

/* Returns true if a service task is active. */
bool ServiceTask_IsActive(void);

/* Returns true if the active service task is driving motor outputs
 * (identify, test). Scan is passive — returns false. */
bool ServiceTask_IsDriving(void);

/* Called from App_Loop every iteration.  Non-blocking. */
void ServiceTask_Update(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_TASK_H */
