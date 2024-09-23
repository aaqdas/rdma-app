
IP ?= 100.100.100.1


setup:
	sudo ifconfig ens16np0 100.100.100.1 up

start_server:
	./rdma_app -g 1 

start_client:
	./rdma_app ${IP}


build:
	gcc ./rdma_app.cpp -o rdma_app -libverbs -g -O0 -Wall

debug: 
	gdb ./rdma_app


