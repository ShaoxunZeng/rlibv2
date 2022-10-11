#pragma once

#define CLIENT_THREAD_NUM   1
#define REQUEST_SIZE        128 * 1024
#define QUEUE_DEPTH         128
#define NIC_NUM             1
#define TEST_TIME_SEC       30

#define LOCAL_NIC(a) ((a) % NIC_NUM)
#define REMOTE_NIC(a) ((a) % NIC_NUM)
#define REMOTE_QUEUE_IDX(nic, nic_local_idx) ((nic) * CLIENT_THREAD_NUM + (nic_local_idx))