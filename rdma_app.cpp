/*
* BUILD COMMAND:
* gcc -Wall -I/usr/local/ofed/include -O2 -o RDMA_RC_example -L/usr/local/ofed/lib64 -L/usr/local/ofed/lib -lib-
verbs RDMA_RC_example.c *
* Copyright (c) 2009 Mellanox Technologies. All rights reserved. *
* This software is available to you under a choice of one of two
* licenses. You may choose to be licensed under the terms of the GNU
* General Public License (GPL) Version 2, available from the file
* COPYING in the main directory of this source tree, or the
* OpenIB.org BSD license below: *
* Redistribution and use in source and binary forms, with or
* without modification, are permitted provided that the following
* conditions are met:
*
* - Redistributions of source code must retain the above
* copyright notice, this list of conditions and the following
* disclaimer. *
* - Redistributions in binary form must reproduce the above
* copyright notice, this list of conditions and the following
* disclaimer in the documentation and/or other materials
* provided with the distribution. *
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
* NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
* BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
* ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
* CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE. *
*/
/****************************************************************************** *
*
*
*
*
*
*
*
*
* *****************************************************************************/
#include <stdio.h> 
#include <stdlib.h>
#include <iostream>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <endian.h>
#include <byteswap.h>
#include <getopt.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <sys/types.h>
#include <sys/socket.h> 
#include <netdb.h>
/* poll CQ timeout in millisec (2 seconds) */ 
#define MAX_POLL_CQ_TIMEOUT 2000 
#define MSG "SEND operation "
#define RDMAMSGR "RDMA read operation " 
#define RDMAMSGW "RDMA write operation" 
#define MSG_SIZE (strlen(RDMAMSGW) + 1)
#define Q_KEY 0x80000000

#if __BYTE_ORDER == __LITTLE_ENDIAN
static inline uint64_t htonll(uint64_t x) { return bswap_64(x); }
static inline uint64_t ntohll(uint64_t x) { return bswap_64(x); }
#elif __BYTE_ORDER == __BIG_ENDIAN
static inline uint64_t htonll(uint64_t x) { return x; }
static inline uint64_t ntohll(uint64_t x) { return x; }
#else
#error __BYTE_ORDER is neither __LITTLE_ENDIAN nor __BIG_ENDIAN 
#endif
/* structure of test parameters */ 
enum QP {
    QP_RC,
    QP_RD,
    QP_UD
};


struct config_t
{
const char *dev_name;
char *server_name;
uint32_t tcp_port;
int ib_port;
int gid_idx;
char *qp_type;
};

struct cm_con_data_t {
    uint64_t addr;
    uint32_t rkey;
    uint32_t qp_num;
    uint16_t lid;
    uint8_t gid[16];
    uint32_t qkey;
} __attribute__((packed));

struct resources {
    struct ibv_device_attr device_attr;
    struct ibv_port_attr port_attr;
    struct cm_con_data_t remote_props;
    struct ibv_context *ib_ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    struct ibv_ah *ah;
    char          *buf;
    int sock;

};

struct config_t config = 
{
    NULL,
    NULL,
    19875,
    1,
    -1,
    "rc"
};
static int sock_connect(const char *servername, int port) {
    struct addrinfo *resolved_addr = NULL;
    struct addrinfo *iterator;
    char service[6];
    int sockfd = -1;
    int listenfd = 0;
    int tmp;
    struct addrinfo hints = {
        .ai_flags = AI_PASSIVE,
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM
    };

    if(sprintf(service, "%d", port) < 0) {
        goto socket_connect_exit;
    }

    sockfd = getaddrinfo(servername, service, &hints, &resolved_addr);

    if (sockfd < 0) {
        fprintf(stderr, "%s for %s: %d\n",gai_strerror(sockfd),servername,port);
        goto socket_connect_exit;
    }

    for (iterator = resolved_addr; iterator != NULL; iterator = iterator -> ai_next) {
        sockfd = socket(iterator->ai_family,iterator->ai_socktype,iterator->ai_protocol);
        if (sockfd >= 0) {
            if(servername) {
                if (tmp = connect(sockfd,iterator->ai_addr,iterator->ai_addrlen)) {
                    perror("Failed to Connect");
                    close(sockfd);
                    sockfd = -1;
                }
            }
                else {
                    listenfd = sockfd;
                    sockfd = -1;
                    if (bind(listenfd, iterator->ai_addr, iterator->ai_addrlen)) {
                        goto socket_connect_exit;
                    }
                    listen(listenfd, 1); 
                    sockfd = accept(listenfd, NULL, 0);
                }
            }
        }
    
    socket_connect_exit:
    if(listenfd) {
        close(listenfd);
    }
    if(resolved_addr) {
        freeaddrinfo(resolved_addr);
    }
    if (sockfd < 0) {
        if(servername) {
            perror("Couldn't Connect ot Server");
        }
        else {
            perror("Server Accept Failed");
        }
    }

    return sockfd;

}


int sock_sync_data (int sock, int xfer_size, char *local_data, char *remote_data) {
    int rc;
    int read_bytes = 0;
    int total_read_bytes = 0;

    rc = write(sock,local_data,xfer_size);
    if (rc < xfer_size) {
        perror("Failed Writing Data During sock_sync_data\n");
    }
    else {
        rc = 0;
    }

    while(!rc && total_read_bytes < xfer_size) {
        read_bytes = read(sock,remote_data,xfer_size);
        if (read_bytes > 0) {
            total_read_bytes += read_bytes;
        }
        else {
            rc = read_bytes;
        }
    }
    return rc;
}


int poll_completion_queue(struct resources *res) {
    struct ibv_wc wc;
    unsigned long start_time_msec;
    unsigned long curr_time_msec;
    struct timeval cur_time;
    int poll_result;
    int rc = 0;

    gettimeofday(&cur_time,NULL);
    start_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);

    do {
        poll_result = ibv_poll_cq(res->cq,1,&wc);
        gettimeofday(&cur_time, NULL);
        curr_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
    } while ((poll_result == 0) && ((curr_time_msec - start_time_msec) < MAX_POLL_CQ_TIMEOUT));

    if (poll_result < 0) {
        fprintf(stderr, "Poll CQ Failed\n");
        rc = 1;
    }
    else if (poll_result == 0) {
        fprintf(stdout, "completion was found in CQ after timeout");
    }
    else {
        fprintf(stdout, "completion was found in CQ with status 0x%x\n", wc.status);
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr,"Got Bad Completion with Status; 0x%x, vendor syndrome: 0x%x\n",wc.status,wc.vendor_err);
            rc = 1;
        }
    }

    return rc;
}

static int post_send(struct resources *res, ibv_wr_opcode opcode) {
    struct ibv_send_wr sr;
    struct ibv_sge sge;
    struct ibv_send_wr *bad_wr = NULL;
    int rc;

    memset(&sge,0,sizeof(sge));
    sge.addr = (uintptr_t)res->buf; sge.length = MSG_SIZE; sge.lkey = res->mr->lkey;

    /* prepare the send work request */ 
    memset(&sr, 0, sizeof(sr));
    sr.next = NULL;
    sr.wr_id = 0;
    sr.sg_list = &sge;
    sr.num_sge = 1;
    sr.opcode = opcode;
    sr.send_flags = IBV_SEND_SIGNALED;
    if (strcmp(config.qp_type, "ud") == 0) {
        sr.wr.ud.ah            = res->ah;          //TODO: Look into this if the application messes up.
        sr.wr.ud.remote_qpn    = res->remote_props.qp_num;
        sr.wr.ud.remote_qkey   = res->remote_props.qkey;
    }

    if(opcode != IBV_WR_SEND) {
        sr.wr.rdma.remote_addr = res->remote_props.addr;
        sr.wr.rdma.rkey = res->remote_props.rkey;
        // sr.wr.ah = res->ib_ctx
    }

    rc = ibv_post_send(res->qp, &sr, &bad_wr);
    if (rc)
        fprintf(stderr, "failed to post SR\n"); 
    else
    {
        switch(opcode) {
        case IBV_WR_SEND:
        fprintf(stdout, "Send Request was posted\n"); break;
        case IBV_WR_RDMA_READ:
        fprintf(stdout, "RDMA Read Request was posted\n"); break;
        case IBV_WR_RDMA_WRITE:
        fprintf(stdout, "RDMA Write Request was posted\n"); break;
        default:
        fprintf(stdout, "Unknown Request was posted\n");
        break;
        }
    }
    return rc;
}

static int post_receive(struct resources *res) {
    struct ibv_recv_wr rr;
    struct ibv_sge sge;
    struct ibv_recv_wr *bad_wr;
    int rc;

    memset(&sge, 0, sizeof(sge)); 
    sge.addr = (uintptr_t)res->buf;
    sge.length = MSG_SIZE;
    sge.lkey = res->mr->lkey;
    /* prepare the receive work request */
    memset(&rr, 0, sizeof(rr));
    rr.next = NULL;
    rr.wr_id = 0;
    rr.sg_list = &sge;
    rr.num_sge = 1;
    /* post the Receive Request to the RQ */
    rc = ibv_post_recv(res->qp, &rr, &bad_wr);
    if (rc)
        fprintf(stderr, "failed to post RR\n"); 
    else
        fprintf(stdout, "Receive Request was posted\n");
    return rc;

}

void resources_init(struct resources *res) {
    memset(res, 0, sizeof(res));
    res->sock = -1;
}


int resources_create(struct resources *res) {
    struct ibv_device **dev_list = NULL;
    struct ibv_qp_init_attr qp_init_attr;
    struct ibv_device *ib_dev = NULL;
    size_t size;
    int i;
    int mr_flags = 0;
    int cq_size = 0;
    int num_devices = 0;
    int rc = 0;

    /* if client side */
    if (config.server_name) {
        res->sock = sock_connect(config.server_name, config.tcp_port);
        if (res->sock < 0) {
            fprintf(stderr, "failed to establish TCP connection to server %s, port %d\n", config.server_name, config.tcp_port);
            rc = -1;
            goto resources_create_exit; 
        }
    }
    else {
        fprintf(stdout, "waiting on port %d for TCP connection\n", config.tcp_port);
        res->sock = sock_connect(NULL, config.tcp_port); 
        if (res->sock < 0) {
            fprintf(stderr, "failed to establish TCP connection with client on port %d\n", config.tcp_port);
            rc = -1;
            goto resources_create_exit; 
        }
    }
    fprintf(stdout, "TCP connection was established\n");
    fprintf(stdout, "searching for IB devices in host\n");

    dev_list = ibv_get_device_list(&num_devices); 
    if (!dev_list) {
        fprintf(stderr, "failed to get IB devices list\n"); rc = 1;
        goto resources_create_exit;
    }
/* if there isn't any IB device in host */ 
    if (!num_devices) {
        fprintf(stderr, "found %d device(s)\n", num_devices); rc = 1;
        goto resources_create_exit;
    }
    fprintf(stdout, "found %d device(s)\n", num_devices);
    for (i = 0; i < num_devices; i ++) {
        if(!config.dev_name) {
            config.dev_name = strdup(ibv_get_device_name(dev_list[i]));
            fprintf(stdout, "device not specified, using first one found: %s\n", config.dev_name); 
        }
        if (!strcmp(ibv_get_device_name(dev_list[i]), config.dev_name)) {
            ib_dev = dev_list[i]; 
            break;
        }
    }

    if (!ib_dev) {
        fprintf(stderr, "IB device %s wasn't found\n", config.dev_name); 
        rc = 1;
        goto resources_create_exit;
    }

    res->ib_ctx = ibv_open_device(ib_dev); if (!res->ib_ctx) {
        fprintf(stderr, "failed to open device %s\n", config.dev_name); rc = 1;
        goto resources_create_exit;
    }

    ibv_free_device_list(dev_list); 
    dev_list = NULL;
    ib_dev = NULL;

    if (ibv_query_port(res->ib_ctx, config.ib_port, &res->port_attr)) {
        fprintf(stderr, "ibv_query_port on port %u failed\n", config.ib_port); rc = 1;
        goto resources_create_exit;
    }

    res->pd = ibv_alloc_pd(res->ib_ctx); if (!res->pd) {
        fprintf(stderr, "ibv_alloc_pd failed\n"); rc = 1;
        goto resources_create_exit;
    }

    cq_size = 1;
    res->cq = ibv_create_cq(res->ib_ctx, cq_size, NULL, NULL, 0);
    if (!res->cq) {
        fprintf(stderr, "failed to create CQ with %u entries\n", cq_size); 
        rc = 1;
        goto resources_create_exit;
    }

    /* allocate the memory buffer that will hold the data */
    size = MSG_SIZE;
    res->buf = (char *) malloc(size);
    if (!res->buf ) {
        fprintf(stderr, "failed to malloc %Zu bytes to memory buffer\n", size); 
        rc = 1;
        goto resources_create_exit;
    }

    memset(res->buf, 0 , size);

    /* only in the server side put the message in the memory buffer */ 
    if (!config.server_name) {
        strcpy(res->buf, MSG);
        fprintf(stdout, "going to send the message: '%s'\n", res->buf); 
        }
    else 
        memset(res->buf, 0, size);
    /* register the memory buffer */
    mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE ;
    res->mr = ibv_reg_mr(res->pd, res->buf, size, mr_flags); 
    if (!res->mr) {
        fprintf(stderr, "ibv_reg_mr failed with mr_flags=0x%x\n", mr_flags); rc = 1;
        goto resources_create_exit;
    }

    fprintf(stdout, "MR was registered with addr=%p, lkey=0x%x, rkey=0x%x, flags=0x%x\n", res->buf, res->mr->lkey, res->mr->rkey, mr_flags);

    /* create the Queue Pair */ 
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.qp_type = !(strcmp(config.qp_type,"rc")) ? IBV_QPT_RC : !(strcmp(config.qp_type,"uc")) ? IBV_QPT_UC : IBV_QPT_UD;
    qp_init_attr.sq_sig_all = 1;
    qp_init_attr.send_cq = res->cq;
    qp_init_attr.recv_cq = res->cq;  
    qp_init_attr.cap.max_send_wr = 1; 
    qp_init_attr.cap.max_recv_wr = 1; 
    qp_init_attr.cap.max_send_sge = 1; 
    qp_init_attr.cap.max_recv_sge = 1;
    res->qp = ibv_create_qp(res->pd, &qp_init_attr);
    if (!res->qp) {
        fprintf(stderr, "failed to create QP\n"); 
        rc = 1;
        goto resources_create_exit;
    }
    
    fprintf(stdout, "QP was created, QP number=0x%x\n", res->qp->qp_num);
    resources_create_exit:


    if(rc) {
/* Error encountered, cleanup */
        if(res->qp) {
            ibv_destroy_qp(res->qp);
            res->qp = NULL; 
            }
        if(res->mr) {
            ibv_dereg_mr(res->mr);
            res->mr = NULL; 
            }
        if(res->buf) {
            free(res->buf);
            res->buf = NULL; 
            }
        if(res->cq) {
            ibv_destroy_cq(res->cq);
            res->cq = NULL; 
            }
        if(res->pd) {
            ibv_dealloc_pd(res->pd);
            res->pd = NULL; 
        }
        if(res->ib_ctx) {
            ibv_close_device(res->ib_ctx);
            res->ib_ctx = NULL; 
        }
        if(dev_list) {
            ibv_free_device_list(dev_list);
            dev_list = NULL; 
        }
        if (res->sock >= 0) {
            if (close(res->sock))
                fprintf(stderr, "failed to close socket\n");
            res->sock = -1; 
        }
    }
    return rc; 
}

static int modify_qp_to_init(struct ibv_qp *qp) {
    struct ibv_qp_attr attr;
    int flags;
    int rc;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.port_num = config.ib_port;
    attr.pkey_index = 0;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    /* For Unconnected Communication  */
    if (strcmp(config.qp_type, "ud") == 0) {
        attr.qkey = Q_KEY; //Q_Keys from 0x80000000 - 8000FFFF are available for general use by applications
        flags = flags | IBV_QP_QKEY;
    }
    rc = ibv_modify_qp(qp, &attr, flags);
    if (rc)
        fprintf(stderr, "failed to modify QP state to INIT\n");
    return rc;
}

static int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn, uint16_t dlid, uint8_t *dgid) {
    struct ibv_qp_attr attr;
    int flags;
    int rc;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR; 
    attr.path_mtu = IBV_MTU_256; 
    flags = IBV_QP_STATE | IBV_QP_PATH_MTU;
    if ((strcmp(config.qp_type,"rc") == 0) || (strcmp(config.qp_type,"uc") == 0)) {
        attr.dest_qp_num = remote_qpn; 
        attr.ah_attr.is_global = 0; 
        attr.ah_attr.dlid = dlid;
        attr.ah_attr.sl = 0; 
        attr.ah_attr.src_path_bits = 0; 
        attr.ah_attr.port_num = config.ib_port;
        attr.rq_psn = 0; 
        attr.max_dest_rd_atomic = 1; 
        attr.min_rnr_timer = 0x12;                  //Receiver Not Ready Timer 
        if (config.gid_idx >= 0) {
            attr.ah_attr.is_global = 1;
            attr.ah_attr.port_num = 1; 
            memcpy(&attr.ah_attr.grh.dgid, dgid, 16); 
            attr.ah_attr.grh.flow_label = 0; 
            attr.ah_attr.grh.hop_limit = 1; 
            attr.ah_attr.grh.sgid_index = config.gid_idx; 
            attr.ah_attr.grh.traffic_class = 0;
        }
        flags = flags | IBV_QP_AV | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    }
    if (strcmp(config.qp_type,"ud") == 0) {
        attr.qkey = Q_KEY;
        flags = flags | IBV_QP_QKEY;
    }
    
    // flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    rc = ibv_modify_qp(qp, &attr, flags); 
    if (rc) {
        fprintf(stderr, "failed to modify QP state to RTR\n");
    }
    return rc;
}

static int modify_qp_to_rts (struct ibv_qp *qp) {
    struct ibv_qp_attr attr;
    int flags;
    int rc;

    memset(&attr,0,sizeof(attr));

    attr.qp_state = IBV_QPS_RTS;
    flags = IBV_QP_STATE;
    if ((strcmp(config.qp_type,"rc") == 0) || (strcmp(config.qp_type,"uc") == 0)) {
        attr.timeout = 0x12;
        attr.retry_cnt = 6;
        attr.rnr_retry = 0;
        attr.sq_psn = 0;
        attr.max_rd_atomic = 1;
        flags = flags | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    }
    // May Update Q-Key If The Program Fails Here.

    // flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;

    rc = ibv_modify_qp(qp, &attr, flags); 
    if (rc)
        fprintf(stderr, "failed to modify QP state to RTS\n");
    return rc; 
}

static int connect_qp(struct resources *res) {
    struct cm_con_data_t local_con_data;
    struct cm_con_data_t remote_con_data;
    struct cm_con_data_t tmp_con_data;
    int rc = 0;
    char temp_char;
    union ibv_gid my_gid;

    struct ibv_ah_attr ah_attr = {
          .dlid       = 0,
          .sl         = 0,
          .src_path_bits = 0,
          .is_global  = 0,
          .port_num = config.ib_port
      };


    if (config.gid_idx >= 0) {
        rc = ibv_query_gid(res->ib_ctx, config.ib_port, config.gid_idx, &my_gid); 
        if (rc) {
            fprintf(stderr, "could not get gid for port %d, index %d\n", config.ib_port, config.gid_idx);
            return rc;
        }
    } 
    else
        memset(&my_gid, 0, sizeof my_gid);

    local_con_data.addr = htonll((uintptr_t)res->buf);
    local_con_data.rkey = htonl(res->mr->rkey); 
    local_con_data.qp_num = htonl(res->qp->qp_num); 
    local_con_data.lid = htons(res->port_attr.lid); 
    
    if (strcmp(config.qp_type,"ud") == 0) local_con_data.qkey  = Q_KEY;

    memcpy(local_con_data.gid, &my_gid, 16);
    fprintf(stdout, "\nLocal LID = 0x%x\n", res->port_attr.lid);
    if (sock_sync_data(res->sock, sizeof(struct cm_con_data_t), (char *) &local_con_data, (char *) &tmp_con_data) < 0) {
        fprintf(stderr, "failed to exchange connection data between sides\n"); 
        rc = 1;
        goto connect_qp_exit;
    }

    remote_con_data.addr = ntohll(tmp_con_data.addr);
    remote_con_data.rkey = ntohl(tmp_con_data.rkey); 
    remote_con_data.qp_num = ntohl(tmp_con_data.qp_num);
    remote_con_data.lid = ntohs(tmp_con_data.lid); 

    if (strcmp(config.qp_type,"ud") == 0) remote_con_data.qkey = ntohl(tmp_con_data.qkey);

    memcpy(remote_con_data.gid, tmp_con_data.gid, 16);

    /* save the remote side attributes, we will need it for the post SR */ 
    res->remote_props = remote_con_data;
    fprintf(stdout, "Remote address = 0x%"PRIx64"\n", remote_con_data.addr); 
    fprintf(stdout, "Remote rkey = 0x%x\n", remote_con_data.rkey);
    fprintf(stdout, "Remote QP number = 0x%x\n", remote_con_data.qp_num); 
    fprintf(stdout, "Remote LID = 0x%x\n", remote_con_data.lid);
    
    if (strcmp(config.qp_type,"ud") == 0) fprintf(stdout, "Remote Q-Key = 0x%x\n", remote_con_data.qkey);


    if (config.gid_idx >= 0) {
        uint8_t *p = remote_con_data.gid; fprintf(stdout, "Remote GID = %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]); 
        }

    if (strcmp(config.qp_type,"ud") == 0) {

        // if (config.ib_p) TODO: Configure for GID.GLOBAL_INTERFACE_ID for DESTINATION IF IT DOESNOT WORK (See Line 110-121, ud_ping_pong.c)
        ah_attr.dlid = remote_con_data.lid;
        res->ah = ibv_create_ah(res->pd,&ah_attr);
        if(!res->ah) {
            fprintf(stderr,"Failed to Create Address Handle (AH)\n");
            rc = 1;
        }
        // if (rc) {
        //     return rc;
        // }
    }


    /* modify the QP to init */
    rc = modify_qp_to_init(res->qp); 
    if (rc) { 
        fprintf(stderr, "change QP state to INIT failed\n");
        goto connect_qp_exit; 
    }

    /* let the client post RR to be prepared for incoming messages */ 
    if (config.server_name) {
    rc = post_receive(res); 
    if (rc) { 
        fprintf(stderr, "failed to post RR\n");
        goto connect_qp_exit; }
    }

    rc = modify_qp_to_rtr(res->qp, remote_con_data.qp_num, remote_con_data.lid, remote_con_data.gid); 
    if (rc) {
        fprintf(stderr, "failed to modify QP state to RTR\n");
        goto connect_qp_exit; 
    }

    rc = modify_qp_to_rts(res->qp); 
    if (rc) {
        fprintf(stderr, "failed to modify QP state to RTR\n");
        goto connect_qp_exit; 
    }
    fprintf(stdout, "QP state was change to RTS\n");
    if (sock_sync_data(res->sock, 1, "Q", &temp_char)) /* just send a dummy char back and forth */
    {
        fprintf(stderr,"Sync Error After QPs are were moved to RTS\n");
        rc = 1;
    }

    connect_qp_exit:
    return rc;
}

static int resources_destroy (struct resources *res) {
    int rc = 0;
    if (res->qp) 
    if (ibv_destroy_qp(res->qp)) {
        fprintf(stderr, "failed to destroy QP\n"); rc = 1;
    }
    if (res->mr)
        if (ibv_dereg_mr(res->mr)) {
        fprintf(stderr, "failed to deregister MR\n"); rc = 1;
        }
    if (res->buf) free(res->buf);
    if (res->cq)
    if (ibv_destroy_cq(res->cq)) {
        fprintf(stderr, "failed to destroy CQ\n"); rc = 1;
    }
    if (res->pd)
    if (ibv_dealloc_pd(res->pd)) {
        fprintf(stderr, "failed to deallocate PD\n"); rc = 1;
    }
    if (res->ib_ctx)
        if (ibv_close_device(res->ib_ctx)) {
            fprintf(stderr, "failed to close device context\n"); rc = 1;
    }
    if (res->sock >= 0)
    if (close(res->sock)) {
        fprintf(stderr, "failed to close socket\n"); rc = 1;
    }
    return rc; 
}

static void print_config(void) {
    fprintf(stdout, "----------------------------\n");
    fprintf(stdout, "Device Name    : \"%s\"\n", config.dev_name);
    fprintf(stdout, "IB Port        : %u\n",config.ib_port);
    fprintf(stdout, "QP Type        : %s\n",config.qp_type);
    if (config.server_name) {
        fprintf(stdout, "IP         : %s\n", config.server_name);
    }
        fprintf(stdout, "TCP Port   :%u\n",config.tcp_port);
    
    if(config.gid_idx >= 0) {
        fprintf(stdout, "GID IDX    :%u\n",config.gid_idx);
    }
        fprintf(stdout,"-------------------------------\n\n");
    
}


static void usage(const char *argv0) {
fprintf(stdout, "Usage:\n");
fprintf(stdout, " %s start a server and wait for connection\n", argv0);
fprintf(stdout, " %s <host> connect to server at <host>\n", argv0);
fprintf(stdout, "\n");
fprintf(stdout, "Options:\n");
fprintf(stdout, " -p, --port <port> listen on/connect to port <port> (default 18515)\n"); 
fprintf(stdout, " -d, --ib-dev <dev> use IB device <dev> (default first device found)\n"); 
fprintf(stdout, " -i, --ib-port <port> use port <port> of IB device (default 1)\n"); 
fprintf(stdout, " -g, --gid-idx <git index> gid index to be used in GRH (default not used) (Set it to 1 for RoCE-V2)\n");
fprintf(stdout, " -q, --qp-type Queue Pair Type (rc | uc | ud)\n");
}


int main(int argc, char *argv[]) {
    struct resources res;
    int rc = 1;
    char temp_char;

    while(1) {
        int c;
        static struct option long_options[] = 
        {
            {.name = "port", .has_arg = 1, .val = 'p'},
            {.name = "ib-dev", .has_arg = 1, .val = 'd'},
            {.name = "ib-port", .has_arg = 1, .val = 'i'},
            {.name = "gid-idx", .has_arg = 1, .val = 'g'},
            {.name = "qp-type", .has_arg = 1, .val = 'q'},
            {.name = "NULL", .has_arg = 1, .val = '\0'},
        };

        c = getopt_long(argc,argv,"p:d:i:g:q:",long_options, NULL);
        if (c == -1) {
            // std::cout << "C = " << c << std::endl;
            break;
        }

        switch (c) {
            case 'p':
                config.tcp_port =strtoul(optarg,NULL,0);
                break;
            case 'd':
                config.dev_name = strdup(optarg);
                break;
            case 'i':
                config.ib_port = strtoul(optarg,NULL,0);
                if (config.ib_port < 0) {
                    usage(argv[0]);
                    return 1;
                }
                break;
            case 'g':
                config.gid_idx = strtoul(optarg,NULL,0);
                if (config.gid_idx < 0) {
                    usage(argv[0]);
                    return 1;
                }
                break;
            case 'q':
                config.qp_type = strdup(optarg);
                std::cout << config.qp_type;
                if(!((strcmp(config.qp_type,"rc") == 0) || (strcmp(config.qp_type,"uc")==0) || (strcmp(config.qp_type,"ud")==0))) {
                    usage(argv[0]);
                    return 1;
                }
                break;
            default:
                usage(argv[0]);
                return 1;
        }
    }
    if (optind == argc - 1) {
        // std::cout << "Optind :" << optind << " argv[optind] = " << argv[optind] << std::endl;
        config.server_name = argv[optind];
    }
    else if (optind < argc) {
        usage(argv[0]);
        return 1;
    }

    print_config();

    /*Allocate Memory to Resources (QPs, PD, Memory Buffer Etc.)*/
    resources_init(&res);

    /*Create Resources (QPs, Protection Domains, Memory Regions Initialization)*/
    if (resources_create(&res)) {
        fprintf(stderr,"failed to create resources\n");
        goto main_exit;
    }

    /*Connect Queue Pairs to Physical Ports*/
    if (connect_qp(&res)) {
        fprintf(stderr,"failed to connect QPs\n");
        goto main_exit;
    }
    /*Post a Send Request*/
    if (!config.server_name) {
        if (post_send(&res,IBV_WR_SEND)) {
            fprintf(stderr, "failed to post sr\n");
            goto main_exit;
        }
    }

    if (poll_completion_queue(&res)) {
        fprintf(stderr, "Poll Completion Failed\n");
        goto main_exit;
    }

    if (config.server_name) {
        fprintf(stdout, "Message is: %s\n",res.buf);
    }
    else {
        strcpy(res.buf,RDMAMSGR);
    }

    if (sock_sync_data(res.sock,1,"R",&temp_char))
    {
        fprintf(stderr,"Sync Error before RDMA OPs\n");
        rc = 1;
        goto main_exit;
    }

    if (config.server_name) {
        if (post_send(&res,IBV_WR_RDMA_READ)) {
            fprintf(stderr,"Failed to Post SR 2\n");
            rc = 1;
            goto main_exit;
        }
        if (poll_completion_queue(&res)) {
        fprintf(stderr, "Poll Completion Failed 2\n");
        rc = 1;
        goto main_exit;
        }
        fprintf(stdout, "Contents of server's buffer: '%s'\n", res.buf);
        strcpy(res.buf, RDMAMSGW);
        fprintf(stdout, "Now replacing it with: '%s'\n", res.buf);

        if (post_send(&res, IBV_WR_RDMA_WRITE)) {
            fprintf(stderr, "failed to post SR 3\n"); rc = 1;
            goto main_exit;
        }

        if (poll_completion_queue(&res)) {
            fprintf(stderr, "poll completion failed 3\n"); rc = 1;
            goto main_exit;
        } 
    }

    if (sock_sync_data(res.sock, 1, "W", &temp_char)) /* just send a dummy char back and forth */ {
        fprintf(stderr, "sync error after RDMA ops\n"); rc = 1;
        goto main_exit;
    }

    if(!config.server_name) {
        fprintf(stdout, "Contents of server buffer: '%s'\n", res.buf);
    }
        rc = 0;  
    main_exit:
        if(resources_destroy(&res)) {
            fprintf(stderr,"Failed to Destroy Resources\n");
            rc = 1;
        }
        if(config.dev_name) free((char *) config.dev_name);
        
        fprintf(stdout, "\ntest result is %d\n", rc);
        return rc;

}