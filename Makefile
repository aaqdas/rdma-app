
IP ?= 100.100.100.1
QP ?= rc

setup:
	sudo ifconfig ens16np0 100.100.100.1 up

start_server:
	./rdma_app -g 1 -q $(QP)

start_client:
	./rdma_app -g 1 -q $(QP) ${IP} 


build:
	g++ ./rdma_app.cpp -o rdma_app -libverbs -g -O0 -Wall

debug: 
	gdb ./rdma_app


