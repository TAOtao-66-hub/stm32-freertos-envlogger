#ifndef PROJECT_COMMANDS_H
#define PROJECT_COMMANDS_H
#include "storage_service.h"
#include "stm32f1xx_hal.h"

/* All entry points belong to serialTask. Queues isolate the storage owner. */
void ProjectCommands_Bind(QueueHandle_t request, QueueHandle_t replies);
HAL_StatusTypeDef ProjectCommands_Execute(const char *command, uint8_t *handled);
void ProjectCommands_Service(void);
/* serialTask gates telemetry and parser error text during a framed stream. */
uint8_t ProjectCommands_TransferActive(void);
void ProjectCommands_DeferNotice(void);
#endif
