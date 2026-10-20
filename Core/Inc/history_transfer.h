#ifndef HISTORY_TRANSFER_H
#define HISTORY_TRANSFER_H

#include "storage_service.h"

/* Sole owner: storageTask. One pending reply is retained on queue backpressure.
 * Poll never waits for UART and never allocates a task, queue or history array. */
void HistoryTransfer_Init(QueueHandle_t replies);
uint8_t HistoryTransfer_Begin(const Storage_Request *request);
void HistoryTransfer_Cancel(uint32_t request_id);
uint8_t HistoryTransfer_IsActive(void);
void HistoryTransfer_Poll(void);

#endif
