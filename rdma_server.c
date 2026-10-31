

#include <rdma/rdma_cma.h>
#include <stdio.h>
#include <netinet/in.h>

#define RDMA_SERVER_PORT 9000
#define RDMA_CQ_DEPTH            32


static int rdma_wait_cm_event(struct rdma_event_channel *channel, enum rdma_cm_event_type expect,struct rdma_cm_event **out)
{
    int ret = 0;
    struct rdma_cm_event *event;
    if(0!=rdma_get_cm_event(channel, &event))
    {
        perror("rdma_get_cm_event\n");
        return -1;
    }

    if(event->event != expect)
    {
        fprintf(stderr,"unexpect RDMA CM event %s,expected = %s\n", rdma_event_str(event->event), rdma_event_str(expect));
        ret = -1;
        goto finished;
    }

    if(out)
    {
        *out = event;
        return 0;
    }

finished:
    rdma_ack_cm_event(event);
    return ret;

}

static int rdma_create_queuepair(struct rdma_cm_id *id,struct ibv_pd **pd,struct ibv_cq **cq)
{
    *pd = ibv_alloc_pd(id->verbs);
    if(!*pd)
    {
        perror("ibv_alloc_pd\n");
        return -1;
    }

    *cq = ibv_create_cq(id->verbs, RDMA_CQ_DEPTH, NULL, NULL, 0);
    if(!*cq)
    {
        perror("ibv_create_cq\n");
        return -1;
    }

    struct ibv_qp_init_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.send_cq = *cq;
    attr.recv_cq = *cq;
    attr.qp_type = IBV_QPT_RC;
    attr.cap.max_send_wr = RDMA_CQ_DEPTH;
    attr.cap.max_recv_wr = RDMA_CQ_DEPTH;
    attr.cap.max_send_sge = 1;
    attr.cap.max_recv_sge = 1;

    

    //recvbuffer sendbuffer

    return rdma_create_qp(id, *pd, &attr);;

}


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