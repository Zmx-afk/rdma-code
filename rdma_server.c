

#include <rdma/rdma_cma.h>
#include <stdio.h>
#include <netinet/in.h>

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
    if(0!=rdma_create_id(channel,&cm_id,NULL,RDMA_PS_TCP))
    {
        perror("rdma_create_id\n");
        return 0;
    }

    struct sockaddr_in src_addr;
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = htons(RDMA_SERVER_PORT);
    src_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if(0!=rdma_bind_addr(cm_id,(struct sockaddr *)&src_addr))
    {
        perror("rdma_bind_addr\n");
        return 0;
    }

    if(0!=rdma_listen(cm_id,10))
    {
        perror("rdma_listen\n");
        return 0;
    }

    struct rdma_cm_event *event = NULL;
    rdma_wait_cm_event(channel,RDMA_CM_EVENT_CONNECT_REQUEST,&event);
    struct rdma_cm_id *id = event->id;
    rdma_ack_cm_event(event);
    
//

    struct ibv_pd *pd;
    struct ibv_cq *cq;
    if(0!=rdma_create_queuepair(id,&pd,&cq))
    {
        perror("rdma_create_queuepair");
        return 0;
    }

    struct rdma_conn_param param;
    memset(&param, 0, sizeof(param));
    param.responder_resources = 1;
    param.initiator_depth = 1;
    param.retry_count = 3;

    if(0!=rdma_accept(id, &param))
    {
        perror("rdma_accept");
        return 0;
    }

    rdma_wait_cm_event(id->channel,RDMA_CM_EVENT_ESTABLISHED,&event);


}