CC := gcc
CFLAGS := -Wall -O2 -g
LIBS := -lrdmacm -libverbs
TARGET := rdma_server


$(TARGET): rdma_server.c
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

.PHONY: clean
clean:
	rm -f $(TARGET)