

#include <rdma/rdma_cma.h>
#include <stdio.h>
#include <netinet/in.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>


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


    rdma_alloc_buffer(id,pd);

    if(rdma_post_recv(id, 0) != 0)
    {
        return EXIT_FAILURE;
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

    rdma_wait_cm_event(id->channel,RDMA_CM_EVENT_ESTABLISHED,NULL);

    struct ibv_wc wc;
    if(0!=rdma_poll_wc(cq,&wc)||wc.opcode != IBV_WC_RECV)
    {
        perror("rdma_poll_Wc");
        return 0;
    }
    
    conn_manager_t *cm = id->context;
    printf("recv:%s\n",cm->rbuffer);


    file_type_t *type = (file_type_t*)cm->rbuffer;
    uint32_t magic = type->magic;
    size_t total = type->total;

    printf("magic = 0x%x,total = %ld\n",magic,total);

    int fd = open("b.txt",O_RDWR | O_CREAT, 0666);
    if(fd <0)
    {
        perror("open file failed");
        return -1;
    }

    ftruncate(fd,total);            //创建一个指定大小的文件

    char *p = (char*)mmap(NULL, total, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if(p==MAP_FAILED)
    {
        perror("mmap failed");
        return -1;
    }

    size_t total_chunks = total / RDMA_BUFFER_SIZE;
    if(total % RDMA_BUFFER_SIZE != 0)
    {
        total_chunks++;
    }

    size_t posted_chunks = 0;

    for(size_t slot = 0;slot < RDMA_RECV_SLOTS && posted_chunks < total_chunks;slot++)
    {
        if(rdma_post_recv(id, slot) != 0)
        {
            return EXIT_FAILURE;
        }

        posted_chunks++;
    }

    size_t idx = 0;

    while(idx < total)
    {

        if(rdma_poll_wc(cq, &wc) != 0)
        {
            return EXIT_FAILURE;
        }

        if(wc.opcode != IBV_WC_RECV)
        {
            fprintf(stderr, "Unexpected completion opcode: %d\n",
                    (int)wc.opcode);
            return EXIT_FAILURE;
        }

        if(wc.wr_id >= RDMA_RECV_SLOTS)
        {
            fprintf(stderr, "Invalid receive slot in completion\n");
            return EXIT_FAILURE;
        }

        size_t slot_index = (size_t)wc.wr_id;
        char *recv_data = cm->rbuffer + slot_index * RDMA_BUFFER_SIZE;
        printf("recv slot=%zu, file offset=%zu, received bytes=%u\n",slot_index, idx, wc.byte_len);

        if(idx + RDMA_BUFFER_SIZE < total)
        {
            memcpy(p+idx,recv_data,RDMA_BUFFER_SIZE);
            idx += RDMA_BUFFER_SIZE;
        }
        else
        {
            memcpy(p+idx,recv_data,total-idx);
            idx = total;
        }

        if(posted_chunks < total_chunks)
        {
            if(rdma_post_recv(id, slot_index) != 0)
            {
                return EXIT_FAILURE;
            }

            posted_chunks++;
        }

    }
    msync(p,total,MS_SYNC);
    munmap(p,total);
    close(fd);


    sprintf(cm->sbuffer,"sync finish");
    rdma_post_send(id);
    if(0!=rdma_poll_wc(cq,&wc)||wc.opcode != IBV_WC_SEND)
    {
        perror("rdma_poll_wc");
        return 0;
    }

    rdma_free_buffer(cm_id);
    rdma_destroy_queuepair(cm_id,pd,cq);

}