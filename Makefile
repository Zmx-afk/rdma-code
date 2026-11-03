CC := gcc
CFLAGS := -Wall -O2 -g
LDLIBS := -lrdmacm -libverbs
TARGETS := rdma_server rdma_client


$(TARGETS): %: %.c rdma.h
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGETS)