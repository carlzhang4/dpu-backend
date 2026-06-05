#include "dma_copy.h"
#include "bench.h"
#include "entities.h"
#include <sys/socket.h>
#include "libr.hpp"
#include <time.h>
using namespace std;
std::mutex IO_LOCK;

// #define KEY_SIZE 8
// #define VALUE_SIZE 8
#define RDMA_OUTSTANDING_LIMIT 4

#define MAX_THREADS 48

constexpr uint32_t QP_DEPTH = 64;


int ITERATIONS;
int NUM_PACK;
int NUM_THREADS;
string DEVICE_NAME;
int GID_INDEX;
bool IS_READ;
int PAYLOAD;
int BATCH_SIZE;
int NUMA_NODE;
int BIND_OFFSET;
string EXPORTER_IP;
int REQUEST_PER_DPU = 1;
int KEY_SIZE = 8;
int VALUE_SIZE = 8;
int FIELD_LENGTH;




#define MAX_DPU_NUM 64
int total_dpu_num;
int slice_id_array[MAX_DPU_NUM];
int dpu_array[MAX_DPU_NUM];

volatile double total_bw = 0;
volatile bool start_flag = false;

DEFINE_int32(iterations, 100, "iterations");
DEFINE_int32(numPack, 1, "numPack");
DEFINE_int32(threads, 1, "num_threads");
DEFINE_string(deviceName, "mlx5_0", "deviceName");
DEFINE_int32(gidIndex, 3, "gidIndex");
DEFINE_bool(isRead, false, "isRead");
DEFINE_int32(payload, 64*8*2, "payload");
DEFINE_int32(batchSize, 1, "batchSize");
DEFINE_int32(numaNode, 0, "numaNode");
DEFINE_int32(bindOffset, 0, "bindOffset");
DEFINE_int32(port, 6666, "bind_port");
DEFINE_int32(life_time, 15, "time(s) to live");
DEFINE_string(serverIp, "", "serverIp");
DEFINE_int32(key_size,8,"key_size");
DEFINE_int32(value_size,8,"value_size");
DEFINE_int32(field_length, 64, "field_length");

// sudo ./dma_copy_bench -numaNode 0 -deviceName mlx5_2 -gidIndex 1  -payload 1024 -batchSize 64 -threads 1 -serverIp 10.0.0.100 -life_time 15

long long timespec_to_us(struct timespec *ts) {
    return (long long)ts->tv_sec * 1000000LL + (ts->tv_nsec / 1000LL);
}


char* bytes2string(void* addr, int bytes){
	char *res = (char *)malloc(sizeof(char) * (bytes*4 + 1));
	unsigned char *byte_addr = (unsigned char *)addr;
	int offset=0;
	for(int i=0;i<bytes;i++){
		if((i+1)%8==0){
			sprintf(res + offset, "%02X", byte_addr[i]);
			offset+=2;
			sprintf(res + offset, " ");
			offset+=1;
		}else{
			sprintf(res + offset, "%02X_", byte_addr[i]);
			offset+=3;
		}

		if((i+1)%64==0){
			sprintf(res + offset, "\n");
			offset+=1;
		}
	}
	return res;
}

ibv_qp *create_dma_qp(struct ibv_context *ibv_ctx,
    struct ibv_pd *pd, struct ibv_cq *rq_cq, struct ibv_cq *sq_cq) {
    struct ibv_qp_cap qp_cap = {
    .max_send_wr = QP_DEPTH,
    .max_recv_wr = QP_DEPTH,
    .max_send_sge = 1,
    .max_recv_sge = 1,
    .max_inline_data = 64
    };

    struct ibv_qp_init_attr_ex init_attr = {
        .qp_context = NULL,
        .send_cq = sq_cq,
        .recv_cq = rq_cq,
        .cap = qp_cap,
        .qp_type = IBV_QPT_RC,
        .sq_sig_all = 1,

        .comp_mask = IBV_QP_INIT_ATTR_PD | IBV_QP_INIT_ATTR_SEND_OPS_FLAGS,
        .pd = pd,
        .send_ops_flags = IBV_QP_EX_WITH_RDMA_WRITE | IBV_QP_EX_WITH_RDMA_WRITE_WITH_IMM | \
                          IBV_QP_EX_WITH_SEND | IBV_QP_EX_WITH_SEND_WITH_IMM | IBV_QP_EX_WITH_RDMA_READ,
    };

    struct mlx5dv_qp_init_attr attr_dv = {
        .comp_mask = MLX5DV_QP_INIT_ATTR_MASK_SEND_OPS_FLAGS,
        .send_ops_flags = MLX5DV_QP_EX_WITH_MEMCPY,
    };

    struct ibv_qp *qp = mlx5dv_create_qp(ibv_ctx, &init_attr, &attr_dv);
    if (NULL == qp) {
        LOG_E("failed to create qp\n");
        exit(__LINE__);
    }

    struct ibv_qp_attr qpa = {};
    struct ibv_qp_init_attr qpia = {};
    if (ibv_query_qp(qp, &qpa, IBV_QP_CAP, &qpia)) {
        LOG_E("failed to query qp cap\n");
        exit(__LINE__);
    }

    LOG_I("create qp with qpn = 0x%x, max_send_wr = 0x%x, max_recv_wr = 0x%x, "
        "max_send_sge = 0x%x, max_recv_sge = 0x%x, max_inline_data = 0x%x\n",
        qp->qp_num, qpa.cap.max_send_wr, qpa.cap.max_recv_wr, qpa.cap.max_send_sge,
        qpa.cap.max_recv_sge, qpa.cap.max_inline_data);

    return qp;
}

ibv_cq *create_dma_cq(ibv_context *ibv_ctx) {
    struct ibv_cq_init_attr_ex cq_attr = {
    .cqe = QP_DEPTH,
    .cq_context = NULL,
    .channel = NULL,
    .comp_vector = 0
    };

    struct ibv_cq_ex *cq_ex = mlx5dv_create_cq(ibv_ctx, &cq_attr, NULL);
    if (NULL == cq_ex) {
        LOG_E("failed to create cq\n");
        exit(__LINE__);
    }

    return ibv_cq_ex_to_cq(cq_ex);
}

void init_dma_qp(ibv_qp *qp) {
    int mask = IBV_QP_STATE | IBV_QP_PORT | IBV_QP_PKEY_INDEX | IBV_QP_ACCESS_FLAGS;
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ,
        .pkey_index = 0,
        .port_num = 1,
    };

    if (ibv_modify_qp(qp, &attr, mask)) {
        LOG_E("failed to modify qp:0x%x to init\n", qp->qp_num);
        exit(__LINE__);
    }
}

void dma_qp_self_connected(ibv_qp *qp) {
    ibv_gid gid = {};

    if (ibv_query_gid(qp->context, 1, GID_INDEX, &gid)) {
        printf("failed to query port gid\n");
        exit(__LINE__);
    }

    int mask = IBV_QP_STATE | IBV_QP_AV | \
        IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | \
        IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    struct ibv_qp_attr qpa = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = IBV_MTU_1024,
        .rq_psn = 0,
        .dest_qp_num = qp->qp_num,
        .ah_attr = {
            .grh = {
                .dgid = gid,
                .sgid_index = static_cast<uint8_t>(GID_INDEX) ,
                .hop_limit = 64,
            },
            .is_global = 1,
            .port_num = 1,
        },
        .max_dest_rd_atomic = 1,
        .min_rnr_timer = 0x12,
    };

    if (ibv_modify_qp(qp, &qpa, mask)) {
        LOG_E("failed to modify qp:0x%x to rtr, errno 0x%x\n", qp->qp_num, errno);
        exit(__LINE__);
    }

    qpa.qp_state = IBV_QPS_RTS;
    qpa.timeout = 12;
    qpa.retry_cnt = 6;
    qpa.rnr_retry = 0;
    qpa.sq_psn = 0;
    qpa.max_rd_atomic = 1;
    mask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | \
        IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    if (ibv_modify_qp(qp, &qpa, mask)) {
        printf("failed to modify qp:0x%x to rts, errno 0x%x\n", qp->qp_num, errno);
        exit(__LINE__);
    }
}

uint32_t get_mmo_dma_max_length(struct ibv_context *ibv_ctx) {
    struct mlx5dv_context attrs_out = {};

    attrs_out.comp_mask = MLX5DV_CONTEXT_MASK_WR_MEMCPY_LENGTH;
    if (mlx5dv_query_device(ibv_ctx, &attrs_out)) {
        printf("failed to query mmo dma max length\n");
        exit(__LINE__);
    }

    if (attrs_out.comp_mask & MLX5DV_CONTEXT_MASK_WR_MEMCPY_LENGTH) {
        return attrs_out.max_wr_memcpy_length;
    }

    return 0;
}

void pim_test(vhca_resource *resource){
    
    QpHandler **qp_handlers = new QpHandler * [2]();
    size_t ops = size_t(1) * ITERATIONS * NUM_PACK;
	size_t * field_length_array = new size_t[ops/REQUEST_PER_DPU]();
	string field_length_file_path = "../log/dump_files/kvstore_ycsb_dump_" + std::to_string(FIELD_LENGTH)+ ".txt";
	std::ifstream field_length_file(field_length_file_path);
	if (!field_length_file.is_open()) {
		std::cerr << "Failed to open " << field_length_file_path << std::endl;
		return;
	}
	for (size_t i = 0; i < ops/REQUEST_PER_DPU && field_length_file >> field_length_array[i]; ++i);
	field_length_file.close();

    std::cout << "file read done"<<std::endl;
	for(size_t i = 0; i < ops/REQUEST_PER_DPU; ++i){
		std::cout << field_length_array[i] << " ";
	}

	vhca_resource *dma_resource = resource+1;
    vhca_resource *server_resource = resource;
    vhca_resource *client_resource = resource+2;
    std::cout <<"start test"<<std::endl;
    size_t BUF_SIZE = 760UL*1024*1024;
	void *local_buffer = malloc_2m_numa(BUF_SIZE, NUMA_NODE);
	memset(local_buffer,0,BUF_SIZE);
	// for(int i=0;i<BUF_SIZE;i++){
	// 	((char*)local_buffer)[i] = 98;
	// }
    rt_assert(local_buffer);
    std::cout << "malloc local buffer success"<<std::endl;
	ibv_mr *local_mr = ibv_reg_mr(server_resource->pd, local_buffer,BUF_SIZE, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ
        | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_HUGETLB | IBV_ACCESS_RELAXED_ORDERING);
    if (!local_mr) {
        LOG_E("can't create local_mr\n");
        exit(__LINE__);
    }


    
    std::cout << "register local mr success"<<std::endl;
	// sleep(1);

    //* establish DMA connection with Host
    ibv_cq *sq_cq = create_dma_cq(server_resource->pd->context);
    ibv_cq *rq_cq = create_dma_cq(server_resource->pd->context);

    ibv_qp *dma_qp = create_dma_qp(server_resource->pd->context, server_resource->pd, rq_cq, sq_cq);

    
    init_dma_qp(dma_qp);

    dma_qp_self_connected(dma_qp);

    ibv_qp_ex *dma_qpx = ibv_qp_to_qp_ex(dma_qp);
    mlx5dv_qp_ex *dma_mqpx = mlx5dv_qp_ex_from_ibv_qp_ex(dma_qpx);
    // dma_mqpx->wr_memcpy_direct_init(dma_mqpx);

    uint32_t local_mr_mkey = local_mr->lkey;
    uint32_t remote_mr_mkey = devx_mr_query_mkey(server_resource->mr);
    
    uint32_t client_mr_mkey = devx_mr_query_mkey(client_resource->mr);

    std::cout << "finish establish DMA connection with Host"<<std::endl;

    


    //* test dma with host
 

    std::cout<<" remote addr : "  <<std::hex<<(uint64_t)server_resource->addr<<std::dec<<std::endl;
    std::cout<<" client addr : "  <<std::hex<<(uint64_t)client_resource->addr<<std::dec<<std::endl;

   
    std::cout << "finish write 1st key-value to server"<<std::endl;
 



    //* start KVStore Benchmark
    

	uint64_t dma_start_index = 0;
    // uint64_t KEY_OFFSET = get_addr(0,0,0);
	// uint64_t VALUE_OFFSET = get_addr(0,0,8*1024);
    // std::cout<<"KEY_SIZE : "<<KEY_SIZE<<std::endl;
    // std::cout<<"VALUE_SIZE : "<<VALUE_SIZE<<std::endl;
    KEY_SIZE = 64;
    uint64_t MAGIC_OFFSET = 64*512;
    uint64_t VALUE_MAGIC_OFFSET ;//= VALUE_SIZE*REQUEST_PER_DPU/64*64+64;
    std::cout<<"MAGIC_OFFSET : "<<MAGIC_OFFSET<<std::endl;
	// uint64_t TRANSFER_LENGTH = MAGIC_OFFSET*16+64;
    
    struct ibv_wc *wc_send = NULL;
    ibv_wc *wc = new ibv_wc[16];
	ALLOCATE(wc_send, struct ibv_wc, CTX_POLL_BATCH);
	uint8_t magic_number = 1;
	uint64_t t1,t2,t3,t4,t5,t6;
	uint64_t t[4];
	uint64_t d1=0,d2=0,d3=0,d4=0,d5=0;
    int warm_up =1;
        
    bool all_complete = false;
    // getchar();
    size_t polling_size = sizeof(uint8_t);
    
    
    for (uint64_t i = 0; i < ops/512; i+= 1) {
        
        std::cout<<" =================== iteration : "<<i<<" =================="<<std::endl;
        VALUE_SIZE = 0;
        for(int j=i*512;j< (i+1)*512;j++){
            VALUE_SIZE += field_length_array[j];
        }
        VALUE_MAGIC_OFFSET = VALUE_SIZE*REQUEST_PER_DPU/64*64+64;
        // std::cout<<"VALUE_MAGIC_OFFSET : "<<VALUE_MAGIC_OFFSET<<std::endl;
        // std::cout<<"VALUE_SIZE : "<<VALUE_SIZE<<std::endl;
        size_t remote_offset;
        //* polling for completion
        while(! all_complete){          
            ibv_wr_start(dma_qpx);
            dma_qpx->wr_id = dma_start_index;
            dma_qpx->wr_flags = IBV_SEND_SIGNALED;
            
            // mlx5dv_wr_memcpy(dma_mqpx, local_mr_mkey, (uint64_t)local_buffer , remote_mr_mkey, (uint64_t)resource->addr + remote_offset, 64);
            int polling_wqe_cnt = 0;
            int valid_polled_cnt = 0;
            while(valid_polled_cnt <total_dpu_num){
                remote_offset = get_addr(slice_id_array[valid_polled_cnt],dpu_array[valid_polled_cnt],MAGIC_OFFSET);
                // std::cout << "polling dpu "<<dpu_array[valid_polled_cnt]<<" slice "<<slice_id_array[valid_polled_cnt]<<" offset "<<std::hex<<remote_offset<<std::dec<<std::endl;
                mlx5dv_wr_memcpy(dma_mqpx, local_mr_mkey, (uint64_t)local_buffer+valid_polled_cnt*8 , remote_mr_mkey, (uint64_t)server_resource->addr + remote_offset, 8*16);
                if (ibv_wr_complete(dma_qpx)) {
                    LOG_E("failed to exe memcpy\n");
                    exit(__LINE__);
                }
                valid_polled_cnt += 16;
                polling_wqe_cnt++;
            }
            
            
            size_t ops_comp=0;
            while(ops_comp < polling_wqe_cnt){
                uint32_t num_wc = ibv_poll_cq(sq_cq, 16, wc);
                ops_comp += num_wc;
                // num_wc = ibv_poll_cq(rq_cq, 16, wc);
                // ops_comp += num_wc;
            }

            // for(int j=0;j<total_dpu_num*8;j+=128) {
            //     for(int k=0;k<16;k++){
            //         std::cout<<"local_buffer "<<j+k*8<<" : "<<static_cast<int>(((uint8_t*)local_buffer)[j+k*8])<<std::endl;
            //     }
            // }

            all_complete = true;
            for (int j=0;j<total_dpu_num;j++) {
                // if(((uint8_t*)local_buffer)[get_addr(slice_id_array[i],dpu_array[i],MAGIC_OFFSET)-get_addr(slice_id_array[0],dpu_array[0],MAGIC_OFFSET)]!= magic_number){
                //     all_complete = false;
                // }
                if(((uint8_t*)local_buffer)[64*(j/8) + j%8]!= magic_number){
                    all_complete = false;
                }
                // std::cout << "offset : "<<64*(j/8) + j%8<<std::endl;
                //std::cout<<"dpu "<<dpu_array[j]<<" get value : "<<static_cast<int>(((uint8_t*)local_buffer)[64*(j/8) + j%8])<<std::endl;
            }
            //  getchar();
            
        }
        // std::cout<<"all dpu complete !"<<std::endl;
        all_complete = false;
        // ibv_wr_start(dma_qpx);
        // dma_start_index++;
        // dma_qpx->wr_id = dma_start_index;
        
        // dma_qpx->wr_flags = IBV_SEND_SIGNALED;
        
        
        //mlx5dv_wr_memcpy(dma_mqpx, remote_mr_mkey, (uint64_t)server_resource->addr+get_addr(0,0,0) ,local_mr_mkey, (uint64_t)local_buffer , 64); 
        //std::cout<<"write to remote offset : "<<std::hex<<(uint64_t)client_resource->addr<<std::dec<<std::endl;
        // t1 = get_tsc();

        struct timespec start, end;

        // 获取开始时间
        if (clock_gettime(CLOCK_MONOTONIC, &start) == -1) {
            perror("clock_gettime");
            return ;
        }

        int wqe_cnt = 0;
        int cmpt_dpu_cnt = 0;
        int outstanding_dma_wqe = 0;
        int cmpt_wqe_cnt = 0;
        
        // std::cout<<"start send wqe to dma qp !"<<std::endl;
        while(cmpt_dpu_cnt < total_dpu_num){
            if(total_dpu_num - cmpt_dpu_cnt >= 16){
                //* send continuous address of 16 dpus
                int start_dpu = cmpt_dpu_cnt;
                uint64_t write_size =0;
				while(write_size <  (VALUE_MAGIC_OFFSET+8)){
                    uint64_t remote_offset = get_addr(slice_id_array[start_dpu],dpu_array[start_dpu],write_size);
                    ibv_wr_start(dma_qpx);
                    dma_start_index++;
                    dma_qpx->wr_id = dma_start_index;
                    dma_qpx->wr_flags = IBV_SEND_SIGNALED;
                    if((VALUE_MAGIC_OFFSET+8)- write_size >= 8192){
                        mlx5dv_wr_memcpy(dma_mqpx,client_mr_mkey, (uint64_t)client_resource->addr + remote_offset , remote_mr_mkey, (uint64_t)server_resource->addr + remote_offset , 8192*16);
                        write_size += 8192;
                    }else{
                        mlx5dv_wr_memcpy(dma_mqpx,client_mr_mkey, (uint64_t)client_resource->addr + remote_offset , remote_mr_mkey, (uint64_t)server_resource->addr + remote_offset , 16*((VALUE_MAGIC_OFFSET+8) - write_size));
                        write_size += (VALUE_MAGIC_OFFSET+8) - write_size;
                    }
                    if (ibv_wr_complete(dma_qpx)) {
                        LOG_E("failed to exe memcpy\n");
                        exit(__LINE__);
                    }
                    wqe_cnt++;
                
                    outstanding_dma_wqe++;

                    if(outstanding_dma_wqe >= RDMA_OUTSTANDING_LIMIT){
                        int ops_comp=0;
                        while(ops_comp < RDMA_OUTSTANDING_LIMIT/2){
                            uint32_t num_wc = ibv_poll_cq(sq_cq, 16, wc);
                            ops_comp += num_wc;
                            cmpt_wqe_cnt += num_wc;
                        }
                        outstanding_dma_wqe -= ops_comp;
                    }
                }
                // uint64_t remote_offset = get_addr(slice_id_array[start_dpu],dpu_array[start_dpu],0);
                // ibv_wr_start(dma_qpx);
                // dma_start_index++;
                // dma_qpx->wr_id = dma_start_index;
                // dma_qpx->wr_flags = IBV_SEND_SIGNALED;
                // std::cout << "DMA SIZE : "<<(VALUE_MAGIC_OFFSET+8)*16<<std::endl;
                // mlx5dv_wr_memcpy(dma_mqpx,client_mr_mkey, (uint64_t)client_resource->addr + remote_offset, remote_mr_mkey, (uint64_t)server_resource->addr + remote_offset, (VALUE_MAGIC_OFFSET+8)*16);
                // if (ibv_wr_complete(dma_qpx)) {
                //     LOG_E("failed to exe memcpy\n");
                //     exit(__LINE__);
                // }
                
                // wqe_cnt++;
                
                // outstanding_dma_wqe++;

                // if(outstanding_dma_wqe >= RDMA_OUTSTANDING_LIMIT){
                //     int ops_comp=0;
                //     while(ops_comp < RDMA_OUTSTANDING_LIMIT/2){
                //         uint32_t num_wc = ibv_poll_cq(sq_cq, 16, wc);
                //         ops_comp += num_wc;
                //         cmpt_wqe_cnt += num_wc;
                //     }
                //     outstanding_dma_wqe -= ops_comp;
                // }
                
                cmpt_dpu_cnt += 16;
                
            }else if(total_dpu_num - cmpt_dpu_cnt >= 8){
                int start_dpu = cmpt_dpu_cnt;
                // int remain_dpu = total_dpu_num - cmpt_dpu_cnt;
                int send_size = 0;
                // std::cout<<"remain dpu : "<<remain_dpu<<std::endl;
                while(send_size < VALUE_MAGIC_OFFSET+8){
                    //* dma 8 bytes for each dpu
                    uint64_t remote_offset = get_addr(slice_id_array[start_dpu],dpu_array[start_dpu],send_size);
                    ibv_wr_start(dma_qpx);
                    dma_start_index++;
                    dma_qpx->wr_id = dma_start_index;
                    dma_qpx->wr_flags = IBV_SEND_SIGNALED;
                    // std::cout << "dma offset : "<<std::hex<<remote_offset<<std::dec<<std::endl;
                    mlx5dv_wr_memcpy(dma_mqpx,client_mr_mkey, (uint64_t)client_resource->addr + remote_offset, remote_mr_mkey, (uint64_t)server_resource->addr + remote_offset, 64);
                    if( ibv_wr_complete(dma_qpx)) {
                        std::cout << "dpu id : "<<dpu_array[start_dpu]<< "slice id " << slice_id_array[start_dpu] <<std::endl;
                        std::cout << "send_size : "<<send_size<<std::endl;  
                        std::cout << "write to remote offset : "<<std::hex<<remote_offset<<std::dec<<std::endl;
                        std::cout << "server addr : "<<std::hex<<(uint64_t)server_resource->addr + remote_offset<<std::dec<<std::endl;
                        std::cout << "write size : "<<64<<std::endl;
                        std::cout << "errno : "<<errno<<" reason : "<<strerror(errno)<<std::endl;
                        LOG_E("failed to exe memcpy\n");
                        exit(__LINE__);
                    }
                    outstanding_dma_wqe++;
                    
                    // while(ibv_poll_cq(sq_cq, 1, wc));
                    // outstanding_dma_wqe--;
                    // cmpt_wqe_cnt++;
                    if(outstanding_dma_wqe >= RDMA_OUTSTANDING_LIMIT){
                        int ops_comp=0;
                        while(ops_comp < RDMA_OUTSTANDING_LIMIT/2){
                            uint32_t num_wc = ibv_poll_cq(sq_cq, 16, wc);
                            ops_comp += num_wc;
                            cmpt_wqe_cnt += num_wc;
                        }
                        outstanding_dma_wqe -= ops_comp;
                    }
                    //std::cout<<"write 64 bytes to dpu "<<dpu_array[start_dpu]<<" slice "<<slice_id_array[start_dpu]<<" offset "<<std::hex<<remote_offset<<std::dec<<std::endl;
                    send_size += 8;
                    wqe_cnt++;
                }
                cmpt_dpu_cnt +=8;
            }else{
                int start_dpu = cmpt_dpu_cnt;
				int remain_dpu = total_dpu_num - cmpt_dpu_cnt;
				int send_size = 0;
				//* for less than 8 dpu, send remaining dpu num bytes each time
                while(send_size < VALUE_MAGIC_OFFSET+8){
                    uint64_t remote_offset = get_addr(slice_id_array[start_dpu],dpu_array[start_dpu],send_size);
                    ibv_wr_start(dma_qpx);
                    dma_start_index++;
                    dma_qpx->wr_id = dma_start_index;
                    dma_qpx->wr_flags = IBV_SEND_SIGNALED;
                    // std::cout << "dma offset : "<<std::hex<<remote_offset<<std::dec<<std::endl;
                    mlx5dv_wr_memcpy(dma_mqpx,client_mr_mkey, (uint64_t)client_resource->addr + remote_offset, remote_mr_mkey, (uint64_t)server_resource->addr + remote_offset, remain_dpu);
                    if( ibv_wr_complete(dma_qpx)) {
                        std::cout << "dpu id : "<<dpu_array[start_dpu]<< "slice id " << slice_id_array[start_dpu] <<std::endl;
                        std::cout << "send_size : "<<send_size<<std::endl;  
                        std::cout << "write to remote offset : "<<std::hex<<remote_offset<<std::dec<<std::endl;
                        std::cout << "server addr : "<<std::hex<<(uint64_t)server_resource->addr + remote_offset<<std::dec<<std::endl;
                        std::cout << "write size : "<<64<<std::endl;
                        std::cout << "errno : "<<errno<<" reason : "<<strerror(errno)<<std::endl;
                        LOG_E("failed to exe memcpy\n");
                        exit(__LINE__);
                    }
                    outstanding_dma_wqe++;
                    
                    // while(ibv_poll_cq(sq_cq, 1, wc));
                    // outstanding_dma_wqe--;
                    // cmpt_wqe_cnt++;
                    if(outstanding_dma_wqe >= RDMA_OUTSTANDING_LIMIT){
                        int ops_comp=0;
                        while(ops_comp < RDMA_OUTSTANDING_LIMIT/2){
                            uint32_t num_wc = ibv_poll_cq(sq_cq, 16, wc);
                            ops_comp += num_wc;
                            cmpt_wqe_cnt += num_wc;
                        }
                        outstanding_dma_wqe -= ops_comp;
                    }
                    send_size += 1;
                    wqe_cnt++;
                    //std::cout<<"write 64 bytes to dpu "<<dpu_array[start_dpu]<<" slice "<<slice_id_array[start_dpu]<<" offset "<<std::hex<<remote_offset<<std
                }
                cmpt_dpu_cnt += remain_dpu;
            }
        }
        // std::cout<<"send all wqe to dma qp !"<<std::endl;
        
        // std::cout<<"wqe cnt : "<<wqe_cnt<<std::endl;
        while(cmpt_wqe_cnt < wqe_cnt){
            int ne = ibv_poll_cq(sq_cq, 16, wc);
            if(ne > 0){
                cmpt_wqe_cnt += ne;
            }
        }
        // std::cout<<"complete all wqe !"<<std::endl;

        if (clock_gettime(CLOCK_MONOTONIC, &end) == -1) {
            perror("clock_gettime");
            return ;
        }
        long long start_us = timespec_to_us(&start);
        long long end_us = timespec_to_us(&end);
        long long elapsed = end_us - start_us;
        if(i>= warm_up){
        // d1 += t6 - t1;
            d2 += elapsed;
        }
        magic_number++;
        
	}



	LOG_I("\nDone\n");
    std::cout << "RDMA duration: " << (double)(d2)/(ops - warm_up) << "us" << std::endl;

	delete[]wc;

    ibv_destroy_qp(dma_qp);
    ibv_destroy_cq(sq_cq);
    ibv_destroy_cq(rq_cq);
    ibv_dereg_mr(local_mr);
}


int main(int argc, char *argv[]) {

    
    
    signal(SIGINT, ctrl_c_handler);
    signal(SIGTERM, ctrl_c_handler);

    gflags::ParseCommandLineFlags(&argc, &argv, true);

    ITERATIONS = FLAGS_iterations;
    NUM_THREADS = FLAGS_threads;
    DEVICE_NAME = FLAGS_deviceName;
    GID_INDEX = FLAGS_gidIndex;
    IS_READ = FLAGS_isRead;
    PAYLOAD = FLAGS_payload;
    BATCH_SIZE = FLAGS_batchSize;
    NUMA_NODE = FLAGS_numaNode;
    BIND_OFFSET = FLAGS_bindOffset;
    EXPORTER_IP = FLAGS_serverIp;
    NUM_PACK = FLAGS_numPack;
    FIELD_LENGTH = FLAGS_field_length;
  

    // NetParam net_param;
    // net_param.numNodes = 2;
    // net_param.nodeId = 1;
    // net_param.serverIp = EXPORTER_IP;
    // net_param.device_name = DEVICE_NAME;
    // net_param.gid_index = GID_INDEX;
    // net_param.numa_node = NUMA_NODE;
    // net_param.batch_size = BATCH_SIZE;
    // net_param.sock_port = FLAGS_port;
    // // use devx context for devx vhca
    // net_param.use_devx_context = true;
    // net_param.ib_port = 1;//minimum 1
    // net_param.page_size = sysconf(_SC_PAGESIZE);
    // net_param.cacheline_size = get_cache_line_size();

    NetParam net_param_ptr[MAX_THREADS];
    vhca_resource resources[MAX_THREADS][3];
    for(int i = 0; i < NUM_THREADS; i++) {
        
		net_param_ptr[i].numNodes = 2;
		net_param_ptr[i].nodeId = 1;
		net_param_ptr[i].serverIp = EXPORTER_IP;
		net_param_ptr[i].device_name = DEVICE_NAME;
		net_param_ptr[i].gid_index = GID_INDEX;
		net_param_ptr[i].numa_node = NUMA_NODE;
		net_param_ptr[i].batch_size = BATCH_SIZE;
		net_param_ptr[i].sock_port = FLAGS_port + i;
		net_param_ptr[i].use_devx_context = true;
        net_param_ptr[i].ib_port = 1;//minimum 1
        net_param_ptr[i].page_size = sysconf(_SC_PAGESIZE);
        net_param_ptr[i].cacheline_size = get_cache_line_size();
		init_net_param(net_param_ptr[i]);
        net_param_ptr[i].nodeId = 2;
        socket_init(net_param_ptr[i]);
        net_param_ptr[i].nodeId = 1;
        roce_init(net_param_ptr[i], 2);
        recv(net_param_ptr[i].sockfd[0], &ITERATIONS, sizeof(int), 0);
        sleep(1);
	}


    

    

    uint32_t mmo_dma_max_length = get_mmo_dma_max_length(net_param_ptr[0].contexts[0]);
    LOG_I("mmo_dma_max_length %u\n", mmo_dma_max_length);
    rt_assert(mmo_dma_max_length >= static_cast<uint32_t>(PAYLOAD));

    uint8_t access_key[32] = { 0 };
    for (int i = 0; i < 32;i++) {
        access_key[i] = 1;
    }

    
	

    std::cout<<"start exchange vhca data !"<<std::endl;

    // //* receive Server PIM VHCA
    // read(net_param.sockfd[0], &(resources[0]),   sizeof(vhca_resource));
    
    // resources[0].pd = ibv_alloc_pd(net_param.contexts[0]);
    // if (resources[0].pd == nullptr) {
    //     LOG_E("ibv_alloc_pd failed\n");
    //     return -1;
    // }
    // resources[0].mr = devx_create_crossing_mr(resources[0].pd, resources[0].addr, resources[0].size, resources[0].vhca_id, resources[0].mkey, access_key, sizeof(access_key));
    // if (resources[0].mr == nullptr) {
    //     LOG_E("devx_create_crossing_mr failed\n");
    //     return -1;
    // }
    
    // //* receive client VHCA from server
    // read(net_param.sockfd[0], &(resources[2]),   sizeof(vhca_resource));
    // std::cout<<"finish exchange resource2 data !"<<std::endl;

    // //* Import Client resource
    // resources[2].pd = resources[0].pd;//ibv_alloc_pd(net_param.contexts[0]);
    // if (resources[2].pd == nullptr) {
    //     LOG_E("ibv_alloc_pd failed\n");
    //     return -1;
    // }
    // resources[2].mr = devx_create_crossing_mr(resources[2].pd, resources[2].addr, resources[2].size, resources[2].vhca_id, resources[2].mkey, access_key, sizeof(access_key));
    // if (resources[2].mr == nullptr) {
    //     LOG_E("devx_create_crossing_mr failed\n");
    //     return -1;
    // }


    // //* estblish DMA connection with pim 

    // read(net_param.sockfd[0], &(resources[1]),   sizeof(vhca_resource));
    // std::cout<<"finish exchange resource1 data !"<<std::endl;
  

    // rt_assert(QP_DEPTH >= BATCH_SIZE);

    // resources[1].pd = ibv_alloc_pd(net_param.contexts[1]);
    // if (resources[1].pd == nullptr) {
    //     LOG_E("ibv_alloc_pd failed\n");
    //     return -1;
    // }
    // resources[1].mr = devx_create_crossing_mr(resources[1].pd, resources[1].addr, resources[1].size, resources[1].vhca_id, resources[1].mkey, access_key, sizeof(access_key));
    // if (resources[1].mr == nullptr) {
    //     LOG_E("devx_create_crossing_mr failed\n");
    //     return -1;
    // }
    
    // recv(net_param.sockfd[0], &total_dpu_num, sizeof(int), 0); 
    // for(int i=0;i<total_dpu_num;i++){
    //     recv(net_param.sockfd[0], &(slice_id_array[i]),sizeof(int), 0);
    //     recv(net_param.sockfd[0], &(dpu_array[i]),sizeof(int), 0);
    // }

    for(int i = 0; i < NUM_THREADS; i++) {
        //* receive Server PIM VHCA
        read(net_param_ptr[i].sockfd[0], &(resources[i][0]),   sizeof(vhca_resource));
        
        resources[i][0].pd = ibv_alloc_pd(net_param_ptr[i].contexts[0]);
        if (resources[i][0].pd == nullptr) {
            LOG_E("ibv_alloc_pd failed\n");
            return -1;
        }
        resources[i][0].mr = devx_create_crossing_mr(resources[i][0].pd, resources[i][0].addr, resources[i][0].size, resources[i][0].vhca_id, resources[i][0].mkey, access_key, sizeof(access_key));
        if (resources[i][0].mr == nullptr) {
            LOG_E("devx_create_crossing_mr failed\n");
            return -1;
        }
    }

    for(int i = 0; i < NUM_THREADS; i++) {
        // //* receive client VHCA from server
        read(net_param_ptr[i].sockfd[0], &(resources[i][2]),   sizeof(vhca_resource));
        std::cout<<"finish exchange resource2 data !"<<std::endl;

        // //* Import Client resource
        resources[i][2].pd = resources[i][0].pd;//ibv_alloc_pd(net_param.contexts[0]);
        if (resources[i][2].pd == nullptr) {
            LOG_E("ibv_alloc_pd failed\n");
            return -1;
        }
        resources[i][2].mr = devx_create_crossing_mr(resources[i][2].pd, resources[i][2].addr, resources[i][2].size, resources[i][2].vhca_id, resources[i][2].mkey, access_key, sizeof(access_key));
        if (resources[i][2].mr == nullptr) {
            LOG_E("devx_create_crossing_mr failed\n");
            return -1;
        }
    }

    for(int i = 0; i < NUM_THREADS; i++) {
        // //* estblish DMA connection with pim 
        read(net_param_ptr[i].sockfd[0], &(resources[i][1]),   sizeof(vhca_resource));
        std::cout<<"finish exchange resource1 data !"<<std::endl;
        
        
        resources[i][1].pd = ibv_alloc_pd(net_param_ptr[i].contexts[1]);
        if (resources[i][1].pd == nullptr) {
            LOG_E("ibv_alloc_pd failed\n");
            return -1;
        }
        resources[i][1].mr = devx_create_crossing_mr(resources[i][1].pd, resources[i][1].addr, resources[i][1].size, resources[i][1].vhca_id, resources[i][1].mkey, access_key, sizeof(access_key));
        if (resources[i][1].mr == nullptr) {
            LOG_E("devx_create_crossing_mr failed\n");
            return -1;
        }


    }


    for(int i = 0; i < NUM_THREADS; i++) {
        recv(net_param_ptr[i].sockfd[0], &total_dpu_num, sizeof(int), 0); 
        for(int j=0;j<total_dpu_num;j++){
            recv(net_param_ptr[i].sockfd[0], &(slice_id_array[j]),sizeof(int), 0);
            recv(net_param_ptr[i].sockfd[0], &(dpu_array[j]),sizeof(int), 0);
        }
    }

    // TODO bench_runner
	// pim_test(resources);
    std::vector<thread> threads(NUM_THREADS);
    for(int i = 0; i < NUM_THREADS; i++) {
        threads[i] = std::thread(pim_test, resources[i]);
    }
    for(int i = 0; i < NUM_THREADS; i++) {
        threads[i].join();
    }

    usleep(1000);
    // close(net_param.sockfd[0]);
    // for (int i = 0;i < NUM_THREADS;i++) {
    //     if (devx_dereg_mr(resources[i].mr) != 0) {
    //         LOG_E("can't devx_dereg_mr\n");
    //         return -1;
    //     }
    //     ibv_dealloc_pd(resources[i].pd);
    //     ibv_close_device(net_param.contexts[i]);
    // }

    for (int i = 0;i < NUM_THREADS;i++) {
        if (devx_dereg_mr(resources[i][0].mr) != 0) {
            LOG_E("can't devx_dereg_mr\n");
            return -1;
        }
        ibv_dealloc_pd(resources[i][0].pd);
        ibv_close_device(net_param_ptr[i].contexts[0]);
        if (devx_dereg_mr(resources[i][1].mr) != 0) {
            LOG_E("can't devx_dereg_mr\n");
            return -1;
        }
        ibv_dealloc_pd(resources[i][1].pd);
        ibv_close_device(net_param_ptr[i].contexts[1]);
        if (devx_dereg_mr(resources[i][2].mr) != 0) {
            LOG_E("can't devx_dereg_mr\n");
            return -1;
        }
        ibv_dealloc_pd(resources[i][2].pd);
        ibv_close_device(net_param_ptr[i].contexts[0]);
    }
}