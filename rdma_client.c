


#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <string.h>

#include "rdma.h"


int main()
{
    struct rdma_event_channel *channel = rdma_create_event_channel();
    if(!channel)
    {
        perror("rdma_create_event_channel\n");
        return 0;
    }

    struct rdma_cm_id *cm_id;
    if(0 != rdma_create_id(channel, &cm_id, NULL, RDMA_PS_TCP))
    {
        perror("rdma_create_id\n");
        return 0;
    }

    struct sockaddr_in dst_addr;
    memset(&dst_addr, 0, sizeof(dst_addr));
    dst_addr.sin_family = AF_INET;
    dst_addr.sin_port = htons(RDMA_SERVER_PORT);
    dst_addr.sin_addr.s_addr = inet_addr(RDMA_SERVER_IP);

    struct sockaddr_in src_addr;
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = htons(RDMA_CLIENT_PORT);
    src_addr.sin_addr.s_addr = inet_addr(RDMA_CLIENT_IP);

    //connect to the server
    if(0!=rdma_resolve_addr(cm_id, (struct sockaddr*)&src_addr, (struct sockaddr *)&dst_addr, 2000))
    {
        perror("rdma_resolve_addr\n");
        return 0;
    }

    rdma_wait_cm_event(channel, RDMA_CM_EVENT_ADDR_RESOLVED,NULL);

    if(0!=rdma_resolve_route(cm_id, 2000))
    {
        perror("rdma_resolve_route\n");
        return 0;
    }
    rdma_wait_cm_event(channel, RDMA_CM_EVENT_ROUTE_RESOLVED,NULL);

    struct ibv_pd *pd;
    struct ibv_cq *cq;
    rdma_create_queuepair(cm_id, &pd, &cq);

    struct rdma_conn_param param;
    memset(&param, 0, sizeof(param));
    param.responder_resources = 1;
    param.initiator_depth = 1;
    param.retry_count = 3;

    if(0!=rdma_connect(cm_id, &param))
    {
        perror("rdma_connect\n");
        return -1;
    }

    rdma_wait_cm_event(channel, RDMA_CM_EVENT_ESTABLISHED,NULL);
    printf("RDMA connect successful\n");

    return 0;
}