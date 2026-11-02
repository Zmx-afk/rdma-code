


#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>


#include "rdma.h"

static struct rdma_cm_id *rdma_create_cm_id(struct rdma_event_channel *channel,char *ip, unsigned short port)
{
    struct rdma_cm_id *cm_id;
    if(0 != rdma_create_id(channel, &cm_id, NULL, RDMA_PS_TCP))
    {
        perror("rdma_create_id\n");
        return 0;
    }

    struct sockaddr_in dst_addr;
    memset(&dst_addr, 0, sizeof(dst_addr));
    dst_addr.sin_family = AF_INET;
    dst_addr.sin_port = htons(port);
    dst_addr.sin_addr.s_addr = inet_addr(ip);

    //connect to the server
    if(0!=rdma_resolve_addr(cm_id, NULL, (struct sockaddr *)&dst_addr, 2000))
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

    return cm_id;
}


int main()
{
    struct rdma_event_channel *channel = rdma_create_event_channel();
    if(!channel)
    {
        perror("rdma_create_event_channel\n");
        return 0;
    }

    struct rdma_cm_id *cm_id = rdma_create_cm_id(channel,RDMA_SERVER_IP, RDMA_SERVER_PORT);
    if(!cm_id)
    {
        perror("rdma_create_cm_id\n");
        return -1;
    }

    struct ibv_pd *pd = NULL;
    struct ibv_cq *cq = NULL;
    rdma_create_queuepair(cm_id, &pd, &cq);
    rdma_alloc_buffer(cm_id, pd);
    

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

    rdma_wait_cm_event(cm_id->channel, RDMA_CM_EVENT_ESTABLISHED,NULL);
    printf("RDMA connect successful\n");

    conn_manager_t *cm = cm_id->context;
    //sprintf(cm->sbuffer,"abcdefghijkmnopqrstuvwxyz");


//open file
    int fd = open(RDMA_FILENAME,O_RDWR);
    if(fd <0)
    {
        perror("open file failed");
        return -1;
    }

    struct stat st;
    if(0!=fstat(fd,&st))
    {
        perror("fstat failed");
        return -1;
    }

    size_t total = st.st_size;
    printf("file size = %ld\n",total);

   

// read file
    char *p = (char*)mmap(NULL, total, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if(p==MAP_FAILED)
    {
        perror("mmap failed");
        return -1;
    }

    int idx = 0;
    struct ibv_wc wc;

    file_type_t type = {0};
    type.magic = 0x12345678;
    type.total = total;
    memcpy(cm->sbuffer,&type,sizeof(file_type_t));
    rdma_post_send(cm_id);
    if(0!=rdma_poll_wc(cq,&wc)||wc.opcode != IBV_WC_SEND)
    {
        perror("rdma_poll_wc");
        return 0;
    }

    while(idx < total)
    {
        if(idx + RDMA_BUFFER_SIZE < total)
        {
            memcpy(cm->sbuffer, p+idx, RDMA_BUFFER_SIZE);
            idx += RDMA_BUFFER_SIZE;
        }
        else
        {
            memcpy(cm->sbuffer, p+idx, total - idx);
            idx += (total - idx);
        }

        printf("send %d bytes\n",idx);
        rdma_post_send(cm_id);
        if(0!=rdma_poll_wc(cq,&wc)||wc.opcode != IBV_WC_SEND)
        {
            perror("rdma_poll_wc");
            return 0;
        }

       

    }

//
    munmap(p, total);
    close(fd);

    if(rdma_post_recv(cm_id, 0) != 0)
    {
        return EXIT_FAILURE;
    }
    if(0!=rdma_poll_wc(cq,&wc)||wc.opcode != IBV_WC_RECV)
    {
        perror("rdma_poll_Wc");
        return 0;
    }

    printf("RECV: %s\n",cm->rbuffer);
    getchar();

    rdma_free_buffer(cm_id);
    rdma_destroy_queuepair(cm_id,pd,cq);

    return 0;
}