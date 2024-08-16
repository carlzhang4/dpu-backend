#include "util.hpp"
#include "libr.hpp"
#include "dma_export.h"

static bool stop_flag;

static void ctrl_c_handler(int) { stop_flag = true; }


using namespace std;
std::mutex IO_LOCK;

int NUM_THREADS = 1;
int NUMA_NODE = 0;
int PORT = 6666;
string DEVICE_NAME = "mlx5_0";

int export_buffer(void *buffer, int size){
	signal(SIGINT, ctrl_c_handler);
    signal(SIGTERM, ctrl_c_handler);

	NetParam net_param;
    net_param.numNodes = 2;
    net_param.nodeId = 0;
    net_param.device_name = DEVICE_NAME;
    net_param.numa_node = NUMA_NODE;
    net_param.sock_port = PORT;
    net_param.sockfd = new int[128];
    net_param.ib_port = 1;//minimum 1
    net_param.page_size = sysconf(_SC_PAGESIZE);
    net_param.cacheline_size = get_cache_line_size();

	roce_init(net_param, NUM_THREADS);
    struct devx_hca_capabilities caps;
    if (devx_query_hca_caps(net_param.contexts[0], &caps) != 0) {
        printf("can't query_hca_caps\n");
        return -1;
    }

	printf("vhca_id %u\n", caps.vhca_id);
    printf("vhca_resource_manager %u\n", caps.vhca_resource_manager);
    printf("hotplug_manager %u\n", caps.hotplug_manager);
    printf("eswitch_manager %u\n", caps.eswitch_manager);
    // direct access host physical address
    printf("introspection_mkey_access_allowed %d\n", caps.introspection_mkey_access_allowed);
    printf("introspection_mkey %u\n", caps.introspection_mkey);
    // import representor introspection mkey
    printf("crossing_vhca_mkey_supported %d\n", caps.crossing_vhca_mkey_supported);
    // local mkey to remote mkey
    printf("cross_gvmi_mkey_enabled %d\n", caps.cross_gvmi_mkey_enabled);
    printf("---------------------------\n");

	vhca_resource *resources = new vhca_resource[NUM_THREADS];

    for (int i = 0;i < NUM_THREADS;i++) {
        resources[i].pd = ibv_alloc_pd(net_param.contexts[i]);
    }

    uint8_t access_key[32] = { 0 };

    for (size_t i = 0; i < 32;i++) {
        access_key[i] = 1;
    }

	for (int i = 0;i < 1;i++) {
        resources[i].vhca_id = caps.vhca_id;
        resources[i].addr = buffer;
        if (!resources[i].addr) {
            LOG_E("can't malloc_2m_numa\n");
            return -1;
        }
        resources[i].size = size;
        resources[i].mr = devx_reg_mr(resources[i].pd, resources[i].addr, resources[i].size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ
            | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_RELAXED_ORDERING);
        if (!resources[i].mr) {
            LOG_I("can't devx_reg_mr\n");
            return -1;
        }
        resources[i].mkey = devx_mr_query_mkey(resources[i].mr);
        if (devx_mr_allow_other_vhca_access(resources[i].mr, access_key, sizeof(access_key)) != 0) {
            LOG_E("can't allow_other_vhca_access\n");
            return -1;
        }
        LOG_I("mr (umem): thread %d vhca_id %u addr %p mkey %u\n", i, caps.vhca_id, resources[i].addr, resources[i].mkey);
    }
	socket_init(net_param);
    exchange_vhca_data(net_param, resources, NUM_THREADS);

	LOG_I("Exchange Done\n");
	// sleep(3);

	// for (int i = 0;i < 1;i++) {
    //     if (devx_dereg_mr(resources[i].mr) != 0) {
    //         LOG_E("can't devx_dereg_mr\n");
    //         return -1;
    //     }
    //     ibv_dealloc_pd(resources[i].pd);
    //     ibv_close_device(net_param.contexts[i]);

	// 	return 0;
    // }
	return 0;
}