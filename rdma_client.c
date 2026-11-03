


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

typedef struct client_state_s
{
    uint64_t send_limit;        //服务端允许客户端累计发送到第几块正文
    int send_pending;           //当前是否还有一次发送没有取到完成事件，1 表示有，0 表示没有
    int done;                   //是否收到服务端的 RDMA_CTRL_DONE
} client_state_t;

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

static int client_poll_completion(struct rdma_cm_id *cm_id,struct ibv_cq *cq,client_state_t *state)
{
    struct ibv_wc wc;

    if (rdma_poll_wc(cq, &wc) != 0)
    {
        return -1;
    }

    if (wc.opcode == IBV_WC_SEND)
    {
        state->send_pending = 0;
        return 0;
    }

    if (wc.opcode != IBV_WC_RECV)
    {
        fprintf(stderr, "Unexpected completion opcode: %d\n",
                (int)wc.opcode);
        return -1;
    }

    if (wc.wr_id >= RDMA_RECV_SLOTS ||
        wc.byte_len < sizeof(control_msg_t) ||
        wc.byte_len > RDMA_BUFFER_SIZE)
    {
        fprintf(stderr, "Invalid control message completion\n");
        return -1;
    }

    conn_manager_t *cm = cm_id->context;
    size_t slot_index = (size_t)wc.wr_id;

    control_msg_t message;
    memcpy(&message,
           cm->rbuffer + slot_index * RDMA_BUFFER_SIZE,
           sizeof(message));

    if (message.type == RDMA_CTRL_DONE)
    {
        state->done = 1;
        return 0;
    }

    if (message.type != RDMA_CTRL_CREDIT ||
        state->done ||
        message.send_limit < state->send_limit)
    {
        fprintf(stderr, "Invalid credit message\n");
        return -1;
    }

    if (rdma_post_recv(cm_id, slot_index) != 0)
    {
        return -1;
    }

    state->send_limit = message.send_limit;
    return 0;
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

    if (rdma_create_queuepair(cm_id, &pd, &cq) != 0)
    {
        fprintf(stderr, "Failed to create queue pair\n");
        return EXIT_FAILURE;
    }

    if (rdma_alloc_buffer(cm_id, pd) != 0)
    {
        fprintf(stderr, "Failed to allocate RDMA buffers\n");
        return EXIT_FAILURE;
    }

    //此循环用于接收服务端控制信息
    for (size_t slot = 0; slot < RDMA_RECV_SLOTS; slot++)
    {
        if (rdma_post_recv(cm_id, slot) != 0)
        {
            return EXIT_FAILURE;
        }
    }
    

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
    int fd = open(RDMA_FILENAME,O_RDONLY);
    if(fd <0)
    {
        perror("open file failed");
        return EXIT_FAILURE;
    }

    struct stat st;
    if(0!=fstat(fd,&st))
    {
        perror("fstat");
        close(fd);
        return EXIT_FAILURE;
    }

    if (!S_ISREG(st.st_mode))
    {
        fprintf(stderr, "Input must be a regular file\n");
        close(fd);
        return EXIT_FAILURE;
    }

    if (st.st_size < 0 || (uintmax_t)st.st_size > (uintmax_t)SIZE_MAX)
    {
        fprintf(stderr, "Input file size is not representable as size_t\n");
        close(fd);
        return EXIT_FAILURE;
    }

    size_t total = st.st_size;
    printf("file size = %ld\n",total);

   

// read file
    char *p = NULL;

    if (total > 0)
    {
        p = mmap(NULL, total, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED)
        {
            perror("mmap failed");
            return EXIT_FAILURE;
        }
    }

    size_t offset = 0;
    uint64_t sent_chunks = 0;
    client_state_t state = {0};

    file_type_t type = {0};
    type.magic = 0x12345678;
    type.total = total;

    memset(cm->sbuffer, 0, RDMA_BUFFER_SIZE);
    memcpy(cm->sbuffer, &type, sizeof(type));

    int send_result = rdma_post_send(cm_id);
    if (send_result != 0)
    {
        fprintf(stderr, "Failed to send file header: %s\n",strerror(send_result));
        return EXIT_FAILURE;
    }

    state.send_pending = 1;

    while (offset < total)
    {
        if (state.done)
        {
            fprintf(stderr, "Received DONE before all data was sent\n");
            return EXIT_FAILURE;
        }

        if (state.send_pending || sent_chunks >= state.send_limit)
        {
            if (client_poll_completion(cm_id, cq, &state) != 0)
            {
                return EXIT_FAILURE;
            }

            continue;
        }

        size_t chunk_size = total - offset;
        if (chunk_size > RDMA_BUFFER_SIZE)
        {
            chunk_size = RDMA_BUFFER_SIZE;
        }

        memset(cm->sbuffer, 0, RDMA_BUFFER_SIZE);
        memcpy(cm->sbuffer, p + offset, chunk_size);

        send_result = rdma_post_send(cm_id);
        if (send_result != 0)
        {
            fprintf(stderr, "Failed to send file data: %s\n",
                    strerror(send_result));
            return EXIT_FAILURE;
        }

        state.send_pending = 1;
        sent_chunks++;
        offset += chunk_size;

        printf("Submitted %zu/%zu file bytes\n", offset, total);
    }

    while (state.send_pending || !state.done)
    {
        if (client_poll_completion(cm_id, cq, &state) != 0)
        {
            return EXIT_FAILURE;
        }
    }

    printf("Server confirmed file transfer completion\n");

    if (total > 0 && munmap(p, total) != 0)
    {
        perror("munmap");
        return EXIT_FAILURE;
    }
    close(fd);

    if (rdma_disconnect(cm_id) != 0)
    {
        perror("rdma_disconnect");
        return EXIT_FAILURE;
    }

    if (rdma_wait_cm_event(channel, RDMA_CM_EVENT_DISCONNECTED, NULL) != 0)
    {
        return EXIT_FAILURE;
    }

    rdma_destroy_qp(cm_id);

    if (rdma_free_buffer(cm_id) != 0)
    {
        return EXIT_FAILURE;
    }

    rdma_destroy_queuepair(cm_id, pd, cq);

    if (rdma_destroy_id(cm_id) != 0)
    {
        perror("rdma_destroy_id");
        return EXIT_FAILURE;
    }

    rdma_destroy_event_channel(channel);

    return 0;
}