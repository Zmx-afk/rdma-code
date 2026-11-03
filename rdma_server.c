

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
    if (rdma_poll_wc(cq, &wc) != 0)
    {
        return EXIT_FAILURE;
    }

    if (wc.opcode != IBV_WC_RECV || wc.wr_id != 0 ||
        wc.byte_len < sizeof(file_type_t) ||
        wc.byte_len > RDMA_BUFFER_SIZE)
    {
        fprintf(stderr, "Invalid file header completion\n");
        return EXIT_FAILURE;
    }

    conn_manager_t *cm = id->context;
    file_type_t type = {0};
    memcpy(&type, cm->rbuffer, sizeof(type));

    if (type.magic != 0x12345678)
    {
        fprintf(stderr, "Invalid file header magic\n");
        return EXIT_FAILURE;
    }

    size_t total = type.total;
    off_t file_size = (off_t)total;

    if (file_size < 0 || (uintmax_t)file_size != (uintmax_t)total)
    {
        fprintf(stderr, "File size is not representable as off_t\n");
        return EXIT_FAILURE;
    }


    int fd = open("b.txt",O_RDWR | O_CREAT, 0666);
    if(fd <0)
    {
        perror("open file failed");
        return -1;
    }

    if (ftruncate(fd, file_size) != 0)          //创建一个指定大小的文件
    {
        perror("ftruncate");
        return EXIT_FAILURE;
    }            

    char *p = NULL;

    if (total > 0)
    {
        p = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED)
        {
            perror("mmap failed");
            return EXIT_FAILURE;
        }
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

    int control_send_pending = 0;

    if (posted_chunks > 0)
    {
        if (rdma_post_control(id, RDMA_CTRL_CREDIT,
                              posted_chunks) != 0)
        {
            return EXIT_FAILURE;
        }

        control_send_pending = 1;

        printf("Initial send limit: %zu chunks\n", posted_chunks);
    }

    size_t idx = 0;
    size_t received_chunks = 0;

    while (idx < total || control_send_pending)
    {
        if (received_chunks == posted_chunks && posted_chunks < total_chunks && !control_send_pending)
        {
            for (size_t slot = 0;slot < RDMA_RECV_SLOTS && posted_chunks < total_chunks; slot++)
            {
                if (rdma_post_recv(id, slot) != 0)
                {
                    return EXIT_FAILURE;
                }

                posted_chunks++;
            }

            if (rdma_post_control(id, RDMA_CTRL_CREDIT,
                                  posted_chunks) != 0)
            {
                return EXIT_FAILURE;
            }

            control_send_pending = 1;

            printf("Updated send limit: %zu chunks\n", posted_chunks);
        }

        if(rdma_poll_wc(cq, &wc) != 0)
        {
            return EXIT_FAILURE;
        }

        if (wc.opcode == IBV_WC_SEND)
        {
            if (!control_send_pending)
            {
                fprintf(stderr, "Unexpected control send completion\n");
                return EXIT_FAILURE;
            }

            control_send_pending = 0;
            continue;
        }

        if (wc.opcode != IBV_WC_RECV)
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

        if (wc.byte_len != RDMA_BUFFER_SIZE)
        {
            fprintf(stderr, "Invalid data message length: %u\n", wc.byte_len);
            return EXIT_FAILURE;
        }

        if (received_chunks >= posted_chunks || idx >= total)
        {
            fprintf(stderr, "Received data beyond the expected amount\n");
            return EXIT_FAILURE;
        }

        size_t slot_index = (size_t)wc.wr_id;
        char *recv_data = cm->rbuffer + slot_index * RDMA_BUFFER_SIZE;
        size_t chunk_size = total - idx;
        if (chunk_size > RDMA_BUFFER_SIZE)
        {
            chunk_size = RDMA_BUFFER_SIZE;
        }

        // printf("recv slot=%zu, file offset=%zu, received bytes=%u, file bytes=%zu\n",
        //        slot_index, idx, wc.byte_len, chunk_size);

        memcpy(p + idx, recv_data, chunk_size);
        idx += chunk_size;
        received_chunks++;

    }
    
    if (received_chunks != total_chunks)
    {
        fprintf(stderr, "Received chunk count does not match file size\n");
        return EXIT_FAILURE;
    }

    if (total > 0 && msync(p, total, MS_SYNC) != 0)
    {
        perror("msync");
        return EXIT_FAILURE;
    }

    if (fsync(fd) != 0)
    {
        perror("fsync");
        return EXIT_FAILURE;
    }

    if (total > 0 && munmap(p, total) != 0)
    {
        perror("munmap");
        return EXIT_FAILURE;
    }

    if (close(fd) != 0)
    {
        perror("close output file");
        return EXIT_FAILURE;
    }

    if (rdma_post_control(id, RDMA_CTRL_DONE, 0) != 0)
    {
        return EXIT_FAILURE;
    }

    if (rdma_poll_wc(cq, &wc) != 0)
    {
        return EXIT_FAILURE;
    }

    if (wc.opcode != IBV_WC_SEND)
    {
        fprintf(stderr, "Unexpected completion while waiting for DONE send: %d\n",
                (int)wc.opcode);
        return EXIT_FAILURE;
    }

    printf("Received and synchronized %zu file bytes\n", total);

        if (rdma_wait_cm_event(channel, RDMA_CM_EVENT_DISCONNECTED, NULL) != 0)
    {
        return EXIT_FAILURE;
    }

    if (rdma_disconnect(id) != 0)
    {
        perror("rdma_disconnect");
        return EXIT_FAILURE;
    }

    rdma_destroy_qp(id);

    if (rdma_free_buffer(id) != 0)
    {
        return EXIT_FAILURE;
    }

    rdma_destroy_queuepair(id, pd, cq);

    if (rdma_destroy_id(id) != 0)
    {
        perror("rdma_destroy_id connection");
        return EXIT_FAILURE;
    }

    if (rdma_destroy_id(cm_id) != 0)
    {
        perror("rdma_destroy_id listener");
        return EXIT_FAILURE;
    }

    rdma_destroy_event_channel(channel);
    return EXIT_SUCCESS;

}