
### rte
    sudo modprobe siw
    sudo rdma link add siw1 type siw netdev ens37
    ibv_devices

server:
        rdma_create_event_channel();    //创建事件通道 相当于epoll
        rdma_create_id();               //相当于fd
        rdma_bind_addr();
        rdma_listen();                  //可以参考epoll
        rdma_wait_cm_event();
        rdma_create_queuepair();
        rdma_alloc_buffer();
        


先理解几个基本概念:
        客户端和服务端在接收数据的时候与tcp不同 没有内核缓冲区 在接受数据前需要提早准备好接收缓冲区 也就是调用rdma_post_recv
        

RDMA队列:
SQ (Send Queue，发送队列)：发货货架。你想发数据，就把“发货任务单”放在这里。网卡会一直盯着这个货架，有单子就干活（去内存里拿数据发出去）。
RQ (Receive Queue，接收队列)：收货货架。你要接数据，必须提前把“空箱子”和“收货任务单”放在这里。网卡收到远端发来的数据，就会从这里找个空箱子把数据塞进去。
CQ (Completion Queue，完成队列)：回执货架。网卡干完活（不管是发完了，还是收完了，或者是出错了），它不会直接打断你的 CPU，而是把一张“回执单”（WC, Work Completion）扔在这个货架上。
注意：SQ 和 RQ 合在一起，就叫 QP (Queue Pair)
WR 任务单
