sudo modprobe siw
sudo rdma link add siw1 type siw netdev ens37

客户端: rdma_connect (发请求)
        ↓
服务端: 收到 CONNECT_REQUEST 事件 (被唤醒)
服务端: rdma_accept (发确认包) ---- 发完立刻返回，继续执行
        ↓
客户端: 收到确认包，底层发最终ACK
        ↓
服务端: 底层收到最终ACK，产生 ESTABLISHED 事件 (你再 wait 一次)