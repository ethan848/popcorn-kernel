/*
 * msg_ib.c - Kernel Module for Popcorn Messaging Layer
 * multi-node version over InfiniBand
 * Author: Ho-Ren(Jack) Chuang
 *
 * TODO:
 *		remove mutex in send()
 *		recv_buf_pool
 *		doesn't check RW size
 *		implemet a wait/wakup in exchaging rkey
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/semaphore.h>
#include <linux/file.h>
#include <linux/ktime.h>
#include <linux/fdtable.h>
#include <linux/time.h>
#include <asm/atomic.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/cpumask.h>
#include <linux/sched.h>
#include <linux/vmalloc.h>

/* net */
#include <linux/net.h>
#include <net/sock.h>
#include <linux/tcp.h>
#include <linux/in.h>
#include <asm/uaccess.h>
#include <linux/socket.h>

/* geting host ip */
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/inet.h>

/* pci */
#include <linux/pci.h>
#include <asm/pci.h>

/* RDMA */
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

/* page */
#include <linux/pagemap.h>
#include <popcorn/stat.h>

#include "common.h"

/* features been developed */
#define CONFIG_FARM2 1
#define CONFIG_FARM 0
#define DEMON_POST_RECV_WCS 0
#define RECV_WQ_THRESHOLD 10	/* MAX poll number in a INT */

/* IB recv */
#define MAX_RECV_WR 1500	/* important! Check it if only sender crash */

/* IB send & completetion */
#define MAX_SEND_WR 128	/* sender depth (txdepth) = MAX_SEND_WR */
#define SEND_WR_DEPTH 8	/* Completion Entries = THIS DEPTH x MAX_SEND_WR */
#define MAX_CQE MAX_SEND_WR * SEND_WR_DEPTH

/* IB qp */
#define MAX_RECV_SGE 1
#define MAX_SEND_SGE 1

/* IB buffers */
#define MAX_KMALLOC_SIZE 4 * 1024 * 1024	/* limited by MAX_ORDER */
#if CONFIG_FARM
#define MAX_RDMA_SIZE MAX_KMALLOC_SIZE - FaRM_HEAD_AND_TAIL
#endif
#define MAX_RDMA_SIZE MAX_KMALLOC_SIZE

/* FaRM: w/ 1 extra copy version of FaRM */
#if CONFIG_FARM
#define FaRM_HEAD 4 + 1	/* length + length end bit*/
#define FaRM_TAIL 1
#define FaRM_HEAD_AND_TAIL FaRM_HEAD + FaRM_TAIL

#define FaRM_IS_DATA 0x01
#define FaRM_IS_EMPTY 0xff
#endif

/* FaRM2: two WRITE version of FaRM */
#if CONFIG_FARM2
#define MAX_SGE_NUM 1
#define FaRM2_DATA_SIZE 1
#define MAX_RDMA_FARM2_SIZE 1
#endif

/* INT */
#define INT_MASK 0

/* IB connection config */
#define PORT 1000
#define LISTEN_BACKLOG 99
#define CONN_RESPONDER_RESOURCES 1
#define CONN_INITIATOR_DEPTH 1
#define CONN_RETRY_CNT 1
#define htonll(x) cpu_to_be64((x))
#define ntohll(x) cpu_to_be64((x))


/* RDMA key register */
#define RDMA_RKEY_ACT 0
#define RDMA_RKEY_PASS 1
#define RDMA_FARM2_RKEY_ACT 2
#define RDMA_FARM2_RKEY_PASS 3

/* IB runtime status */
#define IDLE 1
#define CONNECT_REQUEST 2
#define ADDR_RESOLVED 3
#define ROUTE_RESOLVED 4
#define CONNECTED 5
#define RDMA_READ_COMPLETE 6
#define RDMA_WRITE_COMPLETE 7
#define RDMA_SEND_COMPLETE 8
#define ERROR 9

/* recv_wr -> wc */
struct ib_recv_work_struct {
	struct list_head list;
	struct ib_sge *recv_sgl;
	struct ib_recv_wr *recv_wr;
	struct pcn_kmsg_message *element_addr;
};

/* workqueue arg */
typedef struct {
	struct work_struct work;
	struct pcn_kmsg_message *lmsg;
} pcn_kmsg_work_t;

/* FaRM2 */
#if CONFIG_FARM2
struct FaRM2_init_req_t {
    struct pcn_kmsg_hdr header;
    uint32_t remote_rkey;
    uint64_t remote_addr;
    //uint32_t remote_size;
};
#endif

/* InfiniBand Control Block */
struct ib_cb {
	/* Parameters */
	int server;						/* server:1/client:0/myself:-1 */
	int conn_no;
	int read_inv;
	int server_invalidate;
	u8 key;

	/* IB essentials */
	struct ib_cq *cq;				/* can split into two send/recv */
	struct ib_pd *pd;
	struct ib_qp *qp;

	/* Send buffer */
	struct ib_send_wr send_wr;		/* send work requrest info for send */
	struct ib_sge send_sgl;
	u64 send_dma_addr;
	u64 send_mapping;				/* for unmapping */

	/* RDMA common */
	struct ib_reg_wr reg_mr_wr_act;	/* reg kind of = rdma  */
	struct ib_reg_wr reg_mr_wr_pass;
	struct ib_send_wr inv_wr_act;
	struct ib_send_wr inv_wr_pass;
	struct ib_mr *reg_mr_act;
	struct ib_mr *reg_mr_pass;

	/* RDMA issuer only (passive) - wr includes sge(s)<->mr(s) */
	struct ib_rdma_wr rdma_send_wr;	/* rdma work request info for WR */
	struct ib_sge rdma_sgl[MAX_SGE_NUM];

	/* RDMA remote info */
	uint32_t remote_pass_rkey;		/* saving remote RKEY */
	uint64_t remote_pass_addr;		/* saving remote TO ADDR */
	uint32_t remote_pass_len;		/* saving remote LEN */

	/* RDMA local info */
	u64 dma_addr_act;
	u64 rdma_mapping_act;
	u64 dma_addr_pass;
	u64 rdma_mapping_pass;
#if CONFIG_FARM
	char *FaRM_pass_buf;			/* reuse passive buffer to perform WRITE */
#endif

#if CONFIG_FARM2
	unsigned long rdma_FaRM2_max_size;
	struct ib_reg_wr reg_FaRM2_mr_wr_act;
	struct ib_reg_wr reg_FaRM2_mr_wr_pass;
	struct ib_send_wr inv_FaRM2_wr_act;
	struct ib_send_wr inv_FaRM2_wr_pass;
	struct ib_mr *reg_FaRM2_mr_act;
	struct ib_mr *reg_FaRM2_mr_pass;

	struct ib_rdma_wr rdma_FaRM2_send_wr;
	struct ib_sge rdma_FaRM2_sgl;

	/* From remote */
	uint32_t remote_FaRM2_rkey;
	uint64_t remote_FaRM2_raddr;
	uint32_t remote_FaRM2_rlen;
	/* From locaol */
	uint32_t local_FaRM2_lkey;
	uint64_t local_FaRM2_laddr;
	uint32_t local_FaRM2_llen;

	/* RDMA buf for FaRM2 (local) */
	char *pass_FaRM2_buf;
	u64 pass_FaRM2_dma_addr;
	u64 pass_FaRM2_rdma_mapping;
	char *FaRM2_buf_act;
	u64 FaRM2_dma_addr_act;
	u64 FaRM2_rdma_mapping_act;
#endif

	/* Connection */
	u8 addr[16];				/* dst addr in NBO */
	const char *addr_str;		/* dst addr string */
	uint8_t addr_type;			/* ADDR_FAMILY - IPv4/V6 */

	/* CM stuff */
	struct rdma_cm_id *cm_id;		/* connection on client side */
									/* listener on server side */
	struct rdma_cm_id *child_cm_id;	/* connection on server side */

	/* Wait-event states */
	atomic_t state;				/* used for connection */
	atomic_t send_state;		/* for send completion */
	atomic_t read_state;
	atomic_t write_state;
	wait_queue_head_t sem;

	/* Synchronization */
	struct mutex send_mutex;
	struct mutex active_mutex;
	struct mutex passive_mutex;	/* passive lock*/
	struct mutex qp_mutex;		/* protect ib_post_send(qp) */
};

/* InfiniBand Control Block per connection*/
struct ib_cb *gcb[MAX_NUM_NODES];

#if DEMON_POST_RECV_WCS
struct task_struct *recv_post_demon[i];
#endif

/* For RECV ib_recv_work_struct queue */
spinlock_t rws_q_spinlock[MAX_NUM_NODES];
static struct ib_recv_work_struct rws_wait_list_head[MAX_NUM_CHANNELS];	/* Sentinel */

/* Functions */
static int __init initialize(void);
static void __ib_cq_event_handler(struct ib_cq *cq, void *ctx);

/* Popcorn utilities */
extern pcn_kmsg_cbftn callbacks[PCN_KMSG_TYPE_MAX];
extern send_cbftn send_callback;
extern send_rdma_cbftn send_callback_rdma;
extern handle_rdma_request_ftn handle_rdma_callback;
extern char *msg_layer;

/* workqueue */
struct workqueue_struct *msg_handler;


/* Reuse recv rws */
static void __enq_rws(struct ib_recv_work_struct *strc, int index)
{
	spin_lock(&rws_q_spinlock[index]);
    list_add_tail(&(strc->list), &(rws_wait_list_head[index].list));
	spin_unlock(&rws_q_spinlock[index]);
}

static struct ib_recv_work_struct *__dq_rws(int index)
{
	struct ib_recv_work_struct *tmp;

	spin_lock(&rws_q_spinlock[index]);
    if (list_empty(&rws_wait_list_head[index].list)) {
        printk(KERN_INFO "List %d is empty...\n", index);
		spin_unlock(&rws_q_spinlock[index]);
        return NULL;
    } else {
        tmp = list_first_entry(&rws_wait_list_head[index].list,
								struct ib_recv_work_struct, list);
        list_del(rws_wait_list_head[index].list.next);
		spin_unlock(&rws_q_spinlock[index]);
        return tmp;
    }
}


static int __ib_cma_event_handler(struct rdma_cm_id *cma_id,
										struct rdma_cm_event *event)
{
	int ret;
	struct ib_cb *cb = cma_id->context; /* use cm_id to retrive cb */
	static int cma_event_cnt = 0, conn_event_cnt = 0;

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		atomic_set(&cb->state, ADDR_RESOLVED);
		ret = rdma_resolve_route(cma_id, 2000);
		if (ret) {
			printk(KERN_ERR "< rdma_resolve_route error %d >\n", ret);
			wake_up_interruptible(&cb->sem);
		}
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		atomic_set(&cb->state, ROUTE_RESOLVED);
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
		atomic_set(&cb->state, CONNECT_REQUEST);
		/* distributed to other connections */
		cb->child_cm_id = cma_id;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		if (gcb[my_nid]->conn_no == cb->conn_no) {
			cma_event_cnt++;

			atomic_set(&gcb[my_nid + cma_event_cnt]->state, CONNECTED);
			wake_up_interruptible(&gcb[my_nid + cma_event_cnt]->sem);
		} else {
			atomic_set(&gcb[conn_event_cnt]->state, CONNECTED);
			wake_up_interruptible(&gcb[conn_event_cnt]->sem);
			conn_event_cnt++;
		}
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		printk(KERN_ERR "< cma event %d, error %d >\n", event->event,
														event->status);
		atomic_set(&cb->state, ERROR);
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		printk(KERN_ERR "< --- DISCONNECT EVENT conn %d --- >\n", cb->conn_no);
		//atomic_set(&cb->state, ERROR);	// TODO for rmmod
		/* Current implementation:  free all resources in __exit */
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		printk(KERN_ERR "< -----cma detected device removal!!!!----- >\n");
		break;

	default:
		printk(KERN_ERR "< -----oof bad type!----- >\n");
		wake_up_interruptible(&cb->sem);
		break;
	}
	return 0;
}

/*
 * Create a recv_sql/rq_we
 */
struct ib_recv_wr *__create_recv_wr(int conn_no)
{
	int recv_size = sizeof(struct pcn_kmsg_message);
	struct ib_cb *cb = gcb[conn_no];
	struct pcn_kmsg_message *element_addr;
	struct ib_sge *_recv_sgl;
	struct ib_recv_wr *_recv_wr;
	struct ib_recv_work_struct *rws;
	u64 element_dma_addr;

	element_addr = kmalloc(sizeof(*element_addr), GFP_KERNEL);
	if (!element_addr) {
		printk(KERN_ERR "recv_buf malloc failed\n");
		BUG();
	}

	_recv_sgl = kmalloc(sizeof(*_recv_sgl), GFP_KERNEL);
	if (!_recv_sgl) {
		printk(KERN_ERR "sgl recv_buf malloc failed\n");
		BUG();
	}

	_recv_wr =  kmalloc(sizeof(*_recv_wr), GFP_KERNEL);
	if (!_recv_wr) {
		printk(KERN_ERR "recv_wr recv_buf malloc failed\n");
		BUG();
	}

	rws = kmalloc(sizeof(*rws), GFP_KERNEL);
	if (!rws) {
		printk(KERN_ERR "rws malloc failed\n");
		BUG();
	}

	/* map buf to ib addr space */
	element_dma_addr = dma_map_single(cb->pd->device->dma_device,
						  element_addr, recv_size, DMA_BIDIRECTIONAL);

	/* set up sgl */
	_recv_sgl->length = recv_size;
	_recv_sgl->addr = element_dma_addr;
	_recv_sgl->lkey = cb->pd->local_dma_lkey;

	/* set up recv_wr */
	_recv_wr->sg_list = _recv_sgl;
	_recv_wr->num_sge = 1;
	_recv_wr->wr_id = (u64)rws;
	_recv_wr->next = NULL;

	/* save all address to release */
	rws->element_addr = element_addr;
	rws->recv_sgl = _recv_sgl;
	rws->recv_wr = _recv_wr;
	INIT_LIST_HEAD(&rws->list);

	return _recv_wr;
}


static int __ib_connect_client(struct ib_cb *cb)
{
	int ret;
	struct rdma_conn_param conn_param;

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = CONN_RESPONDER_RESOURCES;
	conn_param.initiator_depth = CONN_INITIATOR_DEPTH;
	conn_param.retry_count = CONN_RETRY_CNT;

	ret = rdma_connect(cb->cm_id, &conn_param);
	if (ret) {
		printk(KERN_ERR "rdma_connect error %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem,
							atomic_read(&cb->state) == CONNECTED);
	if (atomic_read(&cb->state) == ERROR) {
		printk(KERN_ERR "wait for CONNECTED state %d\n",
								atomic_read(&cb->state));
		return -1;
	}
	return 0;
}

static void __fill_sockaddr(struct sockaddr_storage *sin, struct ib_cb *cb)
{
	memset(sin, 0, sizeof(*sin));

	if (!cb->server) {
		/* client: load as usuall (ip=remote) */
		if (cb->addr_type == AF_INET) {
			struct sockaddr_in *sin4 = (struct sockaddr_in *)sin;
			sin4->sin_family = AF_INET;
			memcpy((void *)&sin4->sin_addr.s_addr, cb->addr, 4);
			sin4->sin_port = htons(PORT);
		}
	} else {
		/* cb->server: load from global (ip=itself) */
		if (gcb[my_nid]->addr_type == AF_INET) {
			struct sockaddr_in *sin4 = (struct sockaddr_in *)sin;
			sin4->sin_family = AF_INET;
			memcpy((void *)&sin4->sin_addr.s_addr, gcb[my_nid]->addr, 4);
			sin4->sin_port = htons(PORT);
		}
	}
}

static int __ib_bind_server(struct ib_cb *cb)
{
	int ret;
	struct sockaddr_storage sin;

	__fill_sockaddr(&sin, cb);
	ret = rdma_bind_addr(cb->cm_id, (struct sockaddr *)&sin);
	if (ret) {
		printk(KERN_ERR "rdma_bind_addr error %d\n", ret);
		return ret;
	}

	ret = rdma_listen(cb->cm_id, LISTEN_BACKLOG);
	if (ret) {
		printk(KERN_ERR "rdma_listen failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static void __setup_send_wr(struct ib_cb *cb,
							struct pcn_kmsg_message *lmsg)
{
	cb->send_dma_addr = dma_map_single(cb->pd->device->dma_device,
						lmsg, lmsg->header.size, DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(cb, send_mapping, cb->send_dma_addr);

	/* Send buffer */
	cb->send_sgl.addr = cb->send_dma_addr;
	cb->send_sgl.length = lmsg->header.size;
}

/* set up sgl */
static void __ib_setup_wr(struct ib_cb *cb)
{
	int i = 0, ret;

	/* Pre-post RECV buffers */
	for(i = 0; i < MAX_RECV_WR; i++) {
		struct ib_recv_wr *bad_wr;
		struct ib_recv_wr *_recv_wr = __create_recv_wr(cb->conn_no);

		/* 70% posted, 30% reserved */
		if (i <= MAX_RECV_WR * 70 / 100) {
			ret = ib_post_recv(cb->qp, _recv_wr, &bad_wr);
			if (ret) {
				printk(KERN_ERR "ib_post_recv failed: %d\n", ret);
				BUG();
			}
		} else {
			__enq_rws((struct ib_recv_work_struct *)_recv_wr->wr_id,
						cb->conn_no);
		}
	}

	/* send work request: unchanged parameters */
	cb->send_sgl.lkey = cb->pd->local_dma_lkey;
	cb->send_wr.opcode = IB_WR_SEND;
	cb->send_wr.send_flags = IB_SEND_SIGNALED;
	cb->send_wr.num_sge = 1;
	cb->send_wr.sg_list = &cb->send_sgl;

	/* Common for RW - RW wr */
	cb->rdma_send_wr.wr.num_sge = 1;
	cb->rdma_send_wr.wr.sg_list = &cb->rdma_sgl[0];
	cb->rdma_send_wr.wr.send_flags = IB_SEND_SIGNALED;

	/*
	 * A chain of 2 WRs, INVALDATE_MR + REG_MR.
	 * both unsignaled (no completion).  The client uses them to reregister
	 * the rdma buffers with a new key each iteration.
	 * IB_WR_REG_MR = legacy:fastreg mode
	 */
	cb->reg_mr_wr_act.wr.opcode = IB_WR_REG_MR;
	cb->reg_mr_wr_act.mr = cb->reg_mr_act;
	cb->reg_mr_wr_pass.wr.opcode = IB_WR_REG_MR;
	cb->reg_mr_wr_pass.mr = cb->reg_mr_pass;

#if CONFIG_FARM2
	cb->rdma_FaRM2_send_wr.wr.num_sge = 1;
	cb->rdma_FaRM2_send_wr.wr.sg_list = &cb->rdma_FaRM2_sgl;
	cb->rdma_FaRM2_send_wr.wr.send_flags = IB_SEND_SIGNALED;

	cb->reg_FaRM2_mr_wr_act.wr.opcode = IB_WR_REG_MR;
	cb->reg_FaRM2_mr_wr_act.mr = cb->reg_FaRM2_mr_act;
	cb->reg_FaRM2_mr_wr_pass.wr.opcode = IB_WR_REG_MR;
	cb->reg_FaRM2_mr_wr_pass.mr = cb->reg_FaRM2_mr_pass;
#endif

	/*
	 * 1. invalidate Memory Window locally
	 * 2. then register this new key to mr
	 */
	cb->inv_wr_act.opcode = IB_WR_LOCAL_INV;
	cb->inv_wr_act.next = &cb->reg_mr_wr_act.wr;
	cb->inv_wr_pass.opcode = IB_WR_LOCAL_INV;
	cb->inv_wr_pass.next = &cb->reg_mr_wr_pass.wr;
	/*  The reg mem_mode uses a reg mr on the client side for the (We are)
	 *  rw_passive_buf and rw_active_buf buffers.  Each time the client will
	 *  advertise one of these buffers, it invalidates the previous registration
	 *  and fast registers the new buffer with a new key.
	 *
	 *  If the server_invalidate	(We are not)
	 *  option is on, then the server will do the invalidation via the
	 * "go ahead" messages using the IB_WR_SEND_WITH_INV opcode. Otherwise the
	 * client invalidates the mr using the IB_WR_LOCAL_INV work request.
	 */

#if CONFIG_FARM2
	cb->inv_FaRM2_wr_act.opcode = IB_WR_LOCAL_INV;
	cb->inv_FaRM2_wr_act.next = &cb->reg_FaRM2_mr_wr_act.wr;
	cb->inv_FaRM2_wr_pass.opcode = IB_WR_LOCAL_INV;
	cb->inv_FaRM2_wr_pass.next = &cb->reg_FaRM2_mr_wr_pass.wr;
#endif

	return;
}

static int __ib_create_qp(struct ib_cb *cb)
{
	int ret;
	struct ib_qp_init_attr init_attr;

	memset(&init_attr, 0, sizeof(init_attr));

	/* send and recv queue depth */
	init_attr.cap.max_send_wr = MAX_SEND_WR;
	init_attr.cap.max_recv_wr = MAX_RECV_WR * 2;

	/* For flush_qp() */
	init_attr.cap.max_send_wr++;
	init_attr.cap.max_recv_wr++;

	init_attr.cap.max_recv_sge = MAX_RECV_SGE;
	init_attr.cap.max_send_sge = MAX_SEND_SGE;
	init_attr.qp_type = IB_QPT_RC;

	/* send and recv use a same cq */
	init_attr.send_cq = cb->cq;
	init_attr.recv_cq = cb->cq;
	init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;

	/*	The IB_SIGNAL_REQ_WR flag means that not all send requests posted to
	 *	the send queue will generate a completion -- only those marked with
	 *	the IB_SEND_SIGNALED flag.  However, the driver can't free a send
	 *	request from the send queue until it knows it has completed, and the
	 *	only way for the driver to know that is to see a completion for the
	 *	given request or a later request.  Requests on a queue always complete
	 *	in order, so if a later request completes and generates a completion,
	 *	the driver can also free any earlier unsignaled requests)
	 */

	if (cb->server) {
		ret = rdma_create_qp(cb->child_cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->child_cm_id->qp;
	} else {
		ret = rdma_create_qp(cb->cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->cm_id->qp;
	}

	return ret;
}

static int __ib_setup_qp(struct ib_cb *cb, struct rdma_cm_id *cm_id)
{
	int ret;
	struct ib_cq_init_attr attr = {0};

	cb->pd = ib_alloc_pd(cm_id->device);
	if (IS_ERR(cb->pd)) {
		printk(KERN_ERR "ib_alloc_pd failed\n");
		return PTR_ERR(cb->pd);
	}

	attr.cqe = MAX_CQE;
	attr.comp_vector = INT_MASK;
	cb->cq = ib_create_cq(cm_id->device,
							__ib_cq_event_handler, NULL, cb, &attr);
	if (IS_ERR(cb->cq)) {
		printk(KERN_ERR "ib_create_cq failed\n");
		ret = PTR_ERR(cb->cq);
		goto err1;
	}

	/* to arm CA to send eveent on next completion added to CQ */
	ret = ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
	if (ret) {
		printk(KERN_ERR "ib_create_cq failed\n");
		goto err2;
	}

	ret = __ib_create_qp(cb);
	if (ret) {
		printk(KERN_ERR "ib_create_qp failed: %d\n", ret);
		goto err2;
	}
	return 0;
err2:
	ib_destroy_cq(cb->cq);
err1:
	ib_dealloc_pd(cb->pd);
	return ret;
}

/*
 * register local buf for performing R/W (rdma_rkey)
 * Return the (possibly rebound) rkey for the rdma buffer.
 * REG mode: invalidate and rebind via reg wr.
 * Other modes: just return the mr rkey.
 *
 * mode:
 *		0:RDMA_RKEY_ACT
 *		1:RDMA_RKEY_PASS
 *		2:RDMA_FARM2_RKEY_ACT
 *		3:RDMA_FARM2_RKEY_PASS
 */
u32 __ib_rdma_rkey(struct ib_cb *cb, u64 buf,
								int post_inv, int rdma_len, int mode)
{
	int ret;
	u32 rkey;
	struct ib_send_wr *bad_wr;
	struct scatterlist sg = {0};
	struct ib_mr *reg_mr;
	struct ib_send_wr *inv_wr;
	struct ib_reg_wr *reg_mr_wr;

	if (mode == RDMA_RKEY_ACT) {
		reg_mr = cb->reg_mr_act;
		inv_wr = &cb->inv_wr_act;
		reg_mr_wr = &cb->reg_mr_wr_act;
	} else if (mode == RDMA_RKEY_PASS) {
		reg_mr = cb->reg_mr_pass;
		inv_wr = &cb->inv_wr_pass;
		reg_mr_wr = &cb->reg_mr_wr_pass;
	} else if (mode == RDMA_FARM2_RKEY_ACT) {
		reg_mr = cb->reg_FaRM2_mr_act;
		inv_wr = &cb->inv_FaRM2_wr_act;
		reg_mr_wr = &cb->reg_FaRM2_mr_wr_act;
	} else if (mode == RDMA_FARM2_RKEY_PASS) {
		reg_mr = cb->reg_FaRM2_mr_pass;
		inv_wr = &cb->inv_FaRM2_wr_pass;
		reg_mr_wr = &cb->reg_FaRM2_mr_wr_pass;
	}

	inv_wr->ex.invalidate_rkey = reg_mr->rkey;
	ib_update_fast_reg_key(reg_mr, cb->key);
	reg_mr_wr->key = reg_mr->rkey;

	reg_mr_wr->access = IB_ACCESS_REMOTE_READ	|
						IB_ACCESS_REMOTE_WRITE	|
						IB_ACCESS_LOCAL_WRITE	|
						IB_ACCESS_REMOTE_ATOMIC;

	sg_dma_address(&sg) = buf;
	sg_dma_len(&sg) = rdma_len;

	ret = ib_map_mr_sg(reg_mr, &sg, 1, PAGE_SIZE);
			// snyc: use ib_dma_sync_single_for_cpu/dev dev:accessed by IB
	BUG_ON(ret <= 0 || ret > ((((MAX_RDMA_SIZE - 1) & PAGE_MASK) + PAGE_SIZE)
	                                                            >> PAGE_SHIFT));

	mutex_lock(&cb->qp_mutex);
	if (likely(post_inv))
		/* no inv from remote, so manually does it on local side */
		ret = ib_post_send(cb->qp, inv_wr, &bad_wr);	/* INV+MR */
	else
		ret = ib_post_send(cb->qp, &reg_mr_wr->wr, &bad_wr);	/* MR */
	mutex_unlock(&cb->qp_mutex);

	if (ret) {
		printk(KERN_ERR "post send error %d\n", ret);
		atomic_set(&cb->state, ERROR);
		atomic_set(&cb->send_state, ERROR);
		atomic_set(&cb->read_state, ERROR);
		atomic_set(&cb->write_state, ERROR);
	}

	rkey = reg_mr->rkey;
	return rkey;
}

/*
 * init all buffers < 1.pd->cq->qp 2.[mr] 3.xxx >
 */
static int __ib_setup_buffers(struct ib_cb *cb)
{
	int ret, page_list_len, FaRM2_page_list_len;

	page_list_len = (((MAX_RDMA_SIZE - 1) & PAGE_MASK) + PAGE_SIZE)
															>> PAGE_SHIFT;

	/* fill out lkey and rkey */
	cb->reg_mr_act = ib_alloc_mr(cb->pd,
							IB_MR_TYPE_MEM_REG, page_list_len);
	if (IS_ERR(cb->reg_mr_act)) {
		ret = PTR_ERR(cb->reg_mr_act);
		goto bail;
	}

	cb->reg_mr_pass = ib_alloc_mr(cb->pd,
							IB_MR_TYPE_MEM_REG, page_list_len);
	if (IS_ERR(cb->reg_mr_pass)) {
		ret = PTR_ERR(cb->reg_mr_pass);
		goto bail;
	}
#if CONFIG_FARM2
	FaRM2_page_list_len =
			(((cb->rdma_FaRM2_max_size - 1) & PAGE_MASK) + PAGE_SIZE)
														>> PAGE_SHIFT;
	cb->reg_FaRM2_mr_act = ib_alloc_mr(cb->pd,
						IB_MR_TYPE_MEM_REG, FaRM2_page_list_len);
	if (IS_ERR(cb->reg_FaRM2_mr_act)) {
		ret = PTR_ERR(cb->reg_FaRM2_mr_act);
		goto bail;
	}
	cb->reg_FaRM2_mr_pass = ib_alloc_mr(cb->pd,
						IB_MR_TYPE_MEM_REG, FaRM2_page_list_len);
	if (IS_ERR(cb->reg_FaRM2_mr_pass)) {
		ret = PTR_ERR(cb->reg_FaRM2_mr_pass);
		goto bail;
	}

	cb->FaRM2_buf_act = kmalloc(FaRM2_DATA_SIZE, GFP_KERNEL);
	if (!cb->FaRM2_buf_act) {
        ret = -ENOMEM;
        goto bail;
    }
    cb->FaRM2_dma_addr_act = dma_map_single(cb->pd->device->dma_device,
				   cb->FaRM2_buf_act, FaRM2_DATA_SIZE, DMA_BIDIRECTIONAL);
    pci_unmap_addr_set(cb, FaRM2_rdma_mapping_act,
							cb->FaRM2_dma_addr_act);

	cb->pass_FaRM2_buf = kmalloc(FaRM2_DATA_SIZE, GFP_KERNEL);
	if (!cb->pass_FaRM2_buf) {
        ret = -ENOMEM;
        goto bail;
    }
    cb->pass_FaRM2_dma_addr = dma_map_single(cb->pd->device->dma_device,
                   cb->pass_FaRM2_buf, FaRM2_DATA_SIZE, DMA_BIDIRECTIONAL);
    pci_unmap_addr_set(cb, pass_FaRM2_rdma_mapping,
							cb->pass_FaRM2_dma_addr);
	*cb->FaRM2_buf_act = 0;
	*cb->pass_FaRM2_buf = 1;
#endif

	__ib_setup_wr(cb);
	return 0;
bail:
	if (cb->reg_mr_act && !IS_ERR(cb->reg_mr_act))
		ib_dereg_mr(cb->reg_mr_act);
	if (cb->reg_mr_pass && !IS_ERR(cb->reg_mr_pass))
		ib_dereg_mr(cb->reg_mr_pass);
	if (cb->reg_FaRM2_mr_act && !IS_ERR(cb->reg_FaRM2_mr_act))
		ib_dereg_mr(cb->reg_FaRM2_mr_act);
	if (cb->reg_FaRM2_mr_pass && !IS_ERR(cb->reg_FaRM2_mr_pass))
		ib_dereg_mr(cb->reg_FaRM2_mr_pass);
	return ret;
}


static int __ib_accept(struct ib_cb *cb)
{
	int ret;
	struct rdma_conn_param conn_param;
	
	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;

	ret = rdma_accept(cb->child_cm_id, &conn_param);
	if (ret) {
		printk(KERN_ERR "rdma_accept error: %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem, atomic_read(&cb->state) == CONNECTED);

	if (atomic_read(&cb->state) == ERROR) {
		printk(KERN_ERR "wait for CONNECTED state %d\n",
						atomic_read(&cb->state));
		return -1;
	}

	set_popcorn_node_online(cb->conn_no, true);
	return 0;
}

static void __ib_free_buffers(struct ib_cb *cb)
{
	if (cb->reg_mr_act)
		ib_dereg_mr(cb->reg_mr_act);
	if (cb->reg_mr_pass)
		ib_dereg_mr(cb->reg_mr_pass);
	if (cb->reg_FaRM2_mr_act)
		ib_dereg_mr(cb->reg_FaRM2_mr_act);
	if (cb->reg_FaRM2_mr_pass)
		ib_dereg_mr(cb->reg_FaRM2_mr_pass);
}

static void __ib_free_qp(struct ib_cb *cb)
{
	ib_destroy_qp(cb->qp);
	ib_destroy_cq(cb->cq);
	ib_dealloc_pd(cb->pd);
}

#if DEMON_POST_RECV_WCS
static int __recv_post(void *arg0)
{
	struct ib_cb *cb = arg0;
	struct ib_recv_wr *bad_wr;
	int ret;

	while (1) {
		struct ib_recv_work_struct *_rws = __dq_rws(cb->conn_no);
		if(unlikely(!_rws))
			break;

		ret = ib_post_recv(cb->qp, _rws->recv_wr, &bad_wr);

		if (ret) {
			printk(KERN_ERR "ib_post_recv failed: %d\n", ret);
			BUG();
		}
	}
	return 0;
}
#endif

static int __ib_server_accept(void *arg0)
{
	struct ib_cb *cb = arg0;
	int ret = -1;

	ret = __ib_setup_qp(cb, cb->child_cm_id);
	if (ret) {
		printk(KERN_ERR "setup_qp failed: %d\n", ret);
		goto err0;
	}

	ret = __ib_setup_buffers(cb);
	if (ret) {
		printk(KERN_ERR "__ib_setup_buffers failed: %d\n", ret);
		goto err1;
	}
	/* after here, you can send/recv */

	ret = __ib_accept(cb);
	if (ret) {
		printk(KERN_ERR "connect error %d\n", ret);
		goto err2;
	}
	return 0;
err2:
	__ib_free_buffers(cb);
err1:
	__ib_free_qp(cb);
err0:
	rdma_destroy_id(cb->child_cm_id);
	return ret;
}

static int __ib_run_server(void *arg0)
{
	struct ib_cb *my_cb = arg0;
	int ret, i = 0;

	ret = __ib_bind_server(my_cb);
	if (ret)
		return ret;

	/* create multiple connections */
	while (1){
		struct ib_cb *target_cb;
		i++;

		if (my_nid+i >= MAX_NUM_NODES)
			break;

		/* Wait for client's Start STAG/TO/Len */
		wait_event_interruptible(my_cb->sem,
					atomic_read(&my_cb->state) == CONNECT_REQUEST);
		if (atomic_read(&my_cb->state) != CONNECT_REQUEST) {
			printk(KERN_ERR "wait for CONNECT_REQUEST state %d\n",
										atomic_read(&my_cb->state));
			continue;
		}
		atomic_set(&my_cb->state, IDLE);

		target_cb = gcb[my_nid+i];
		target_cb->server = 1;

		/* got from INT. Will be used [setup_qp(SRWRirq)] -> setup_buf -> */
		target_cb->child_cm_id = my_cb->child_cm_id;

		if (__ib_server_accept(target_cb))
			rdma_disconnect(target_cb->child_cm_id);

		printk("conn_no %d is ready (sever)\n", target_cb->conn_no);
	}
	return 0;
}

static int __ib_bind_client(struct ib_cb *cb)
{
	struct sockaddr_storage sin;
	int ret;

	__fill_sockaddr(&sin, cb);

	ret = rdma_resolve_addr(cb->cm_id, NULL, (struct sockaddr *)&sin, 2000);
	if (ret) {
		printk(KERN_ERR "rdma_resolve_addr error %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem,
								atomic_read(&cb->state) == ROUTE_RESOLVED);
	if (atomic_read(&cb->state) != ROUTE_RESOLVED) {
		printk(KERN_ERR "addr/route resolution did not resolve: state %d\n",
													atomic_read(&cb->state));
		return -EINTR;
	}

	return 0;
}

static int __ib_run_client(struct ib_cb *cb)
{
	int ret;

	ret = __ib_bind_client(cb);
	if (ret)
		return ret;

	ret = __ib_setup_qp(cb, cb->cm_id);
	if (ret) {
		printk(KERN_ERR "setup_qp failed: %d\n", ret);
		return ret;
	}

	ret = __ib_setup_buffers(cb);
	if (ret) {
		printk(KERN_ERR "__ib_setup_buffers failed: %d\n", ret);
		goto err1;
	}

	ret = __ib_connect_client(cb);
	if (ret) {
		printk(KERN_ERR "connect error %d\n", ret);
		goto err2;
	}
	return 0;
err2:
	__ib_free_buffers(cb);
err1:
	__ib_free_qp(cb);
	return ret;
}


/*
 * IB utility functions for supporting dynamic mapping
 * specifically for RW users such as page migration
 */
u64 __ib_map_act(void *paddr, int conn_no, int rw_size)
{
struct ib_cb *cb = gcb[conn_no];
u64 dma_addr = dma_map_single(cb->pd->device->dma_device,
							  paddr, rw_size, DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(cb, rdma_mapping_act, dma_addr);
	return dma_addr;
}

u64 __ib_map_pass(void *paddr, int conn_no, int rw_size)
{
	struct ib_cb *cb = gcb[conn_no];
	u64 dma_addr = dma_map_single(cb->pd->device->dma_device,
							  paddr, rw_size, DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(cb, rdma_mapping_pass, dma_addr);
	return dma_addr;
}

/* no need to umap, just remap directly */
void __unmap_act(int conn_no, int rw_size)
{
	struct ib_cb *cb = gcb[conn_no];
	dma_unmap_single(cb->pd->device->dma_device,
					pci_unmap_addr(cb, rdma_mapping_act),
					rw_size, DMA_BIDIRECTIONAL);
}

void __unmap_pass(int conn_no, int rw_size)
{
	struct ib_cb *cb = gcb[conn_no];
	dma_unmap_single(cb->pd->device->dma_device,
					pci_unmap_addr(cb, rdma_mapping_pass),
					rw_size, DMA_BIDIRECTIONAL);
}


/*
 * User doesn't have to take care of concurrency problems.
 * This func will take care of it.
 * User has to free the allocated mem manually
 */
int __ib_kmsg_send_long(unsigned int dst,
						  struct pcn_kmsg_message *lmsg,
						  unsigned int msg_size)
{
	int ret;
	struct ib_send_wr *bad_wr;

	if ( msg_size > sizeof(struct pcn_kmsg_message)) {
		printk("%s(): ERROR - MSG %d larger than MAX_MSG_SIZE %ld\n",
					__func__, msg_size, sizeof(struct pcn_kmsg_message));
		BUG();
	}

	if (dst == my_nid) {
		printk(KERN_ERR "No support for sending msg to itself %d\n", dst);
		return 0;
	}

	lmsg->header.size = msg_size;
	lmsg->header.from_nid = my_nid;

	mutex_lock(&gcb[dst]->send_mutex);

	__setup_send_wr(gcb[dst], lmsg);

	mutex_lock(&gcb[dst]->qp_mutex);
	ret = ib_post_send(gcb[dst]->qp, &gcb[dst]->send_wr, &bad_wr);
	mutex_unlock(&gcb[dst]->qp_mutex);

	wait_event_interruptible(gcb[dst]->sem,
			atomic_read(&gcb[dst]->send_state) == RDMA_SEND_COMPLETE);
	atomic_set(&gcb[dst]->send_state, IDLE);

	dma_unmap_single(gcb[dst]->pd->device->dma_device,
					 pci_unmap_addr(gcb[dst], send_mapping),
					 msg_size, DMA_BIDIRECTIONAL);

	mutex_unlock(&gcb[dst]->send_mutex);
	return 0;
}

static void __handle_remote_thread_rdma_read_request(
						remote_thread_rdma_rw_t *inc_lmsg, void *target_paddr)
{
	remote_thread_rdma_rw_t* req =
								(remote_thread_rdma_rw_t*) inc_lmsg;
	remote_thread_rdma_rw_t *reply;
	struct ib_send_wr *bad_wr; // for ib_post_send
	struct ib_cb *cb = gcb[req->header.from_nid];
	int ret;

	mutex_lock(&cb->passive_mutex);

	cb->dma_addr_pass = __ib_map_pass(target_paddr, cb->conn_no,
												req->rdma_header.rw_size);

	/* RDMA READ echo data */
	/* remote info: */
	cb->remote_pass_rkey = ntohl(req->rdma_header.remote_rkey);
	cb->remote_pass_addr = ntohll(req->rdma_header.remote_addr);
	cb->remote_pass_len = req->rdma_header.rw_size;
	cb->rdma_send_wr.rkey = cb->remote_pass_rkey;
	cb->rdma_send_wr.remote_addr = cb->remote_pass_addr;
	//cb->rdma_send_wr.wr.sg_list->length = cb->remote_pass_len;

	/* local info: */
	// rdma_send_wr.wr.sg_list = &cb->rdma_sgl[0]
	cb->rdma_sgl[0].length = cb->remote_pass_len;
	cb->rdma_sgl[0].addr = cb->dma_addr_pass;
	cb->rdma_sgl[0].lkey = __ib_rdma_rkey(cb, cb->dma_addr_pass,
						!cb->read_inv, cb->remote_pass_len, RDMA_RKEY_PASS);

	cb->rdma_send_wr.wr.next = NULL; // one work request

	if (unlikely(cb->read_inv))
		cb->rdma_send_wr.wr.opcode = IB_WR_RDMA_READ_WITH_INV;
	else {
		/* Compose a READ sge with a invalidation */
		cb->rdma_send_wr.wr.opcode = IB_WR_RDMA_READ;
	}

	mutex_lock(&cb->qp_mutex);
	ret = ib_post_send(cb->qp, &cb->rdma_send_wr.wr, &bad_wr);
	mutex_unlock(&cb->qp_mutex);
	if (ret) {
		printk(KERN_ERR "post send error %d\n", ret);
		return;
	}
	/*	if just sent a FENCE, this should be turned on */
	//	cb->rdma_send_wr.wr.next = NULL;

	wait_event_interruptible(cb->sem,
				(int)atomic_read(&cb->read_state) == RDMA_READ_COMPLETE);
	atomic_set(&cb->read_state, IDLE);

	__unmap_pass(cb->conn_no, req->rdma_header.rw_size);

	mutex_unlock(&cb->passive_mutex);

	reply = pcn_kmsg_alloc_msg(sizeof(*reply));
	if (!reply)
		BUG();

	reply->header.type = req->rdma_header.rmda_type_res;
	reply->header.prio = PCN_KMSG_PRIO_NORMAL;
	//reply->tgroup_home_cpu = tgroup_home_cpu;
	//reply->tgroup_home_id = tgroup_home_id;

	/* RDMA R/W complete ACK */
	reply->header.is_rdma = true;
	reply->rdma_header.rdma_ack = true;
	reply->rdma_header.is_write = false;
	reply->rdma_header.remote_rkey = cb->remote_pass_rkey;
	reply->rdma_header.remote_addr = cb->remote_pass_addr;
	reply->rdma_header.rw_size = cb->remote_pass_len;

	/* for wait station */
	reply->remote_ws = inc_lmsg->remote_ws;

	__ib_kmsg_send_long(req->header.from_nid,
						(struct pcn_kmsg_message*)reply, sizeof(*reply));

	pcn_kmsg_free_msg(reply);
	return;
}

static void __handle_remote_thread_rdma_read_response(
										remote_thread_rdma_rw_t* inc_lmsg)
{
	remote_thread_rdma_rw_t* res =
								(remote_thread_rdma_rw_t*) inc_lmsg;
	struct ib_cb *cb = gcb[res->header.from_nid];

	__unmap_act(cb->conn_no, res->rdma_header.rw_size);
	mutex_unlock(&cb->active_mutex);

	return;
}


/*
 * RDMA WRITE:
 * send        ----->   irq (recv)
 *                      [lock]
 *             <=====   perform WRITE
 *                      unlock
 * irq (recv)  <-----   send
 *
 *
 * FaRM RDMA WRITE:
 * send        ----->   irq (recv)
 * poll                 [lock]
 *             <=====   perform WRITE
 *                      unlock
 * done					done
 */
static void __handle_remote_thread_rdma_write_request(
					remote_thread_rdma_rw_t *inc_lmsg, void *target_paddr)
{
	remote_thread_rdma_rw_t *req = (remote_thread_rdma_rw_t*) inc_lmsg;
#if !CONFIG_FARM && !CONFIG_FARM2
	remote_thread_rdma_rw_t *reply;
#endif

	struct ib_cb *cb = gcb[req->header.from_nid];
#if CONFIG_FARM
#if !CONFIG_FARM2
	char *_FaRM_pass_buf = cb->FaRM_pass_buf;
#endif
#endif
	struct ib_send_wr *bad_wr;
	int ret;

	mutex_lock(&cb->passive_mutex);

#if CONFIG_FARM
#if !CONFIG_FARM2
	/* FaRM w/ buffer copying - make a new buffer aligned with the formate */
	if (target_paddr && req->rdma_header.rw_size!=0) {
		/* FaRM head: length + 1B */
		int rw_size = req->rdma_header.rw_size;
		*(_FaRM_pass_buf + 3) = (char)((rw_size) >> 0);
		*(_FaRM_pass_buf + 2) = (char)((rw_size) >> 8);
		*(_FaRM_pass_buf + 1) = (char)((rw_size) >> 16);
		*(_FaRM_pass_buf + 0) = (char)((rw_size) >> 24);
		*(_FaRM_pass_buf + FaRM_HEAD - 1) = FaRM_IS_DATA;
		/* payload */
		memcpy(_FaRM_pass_buf + FaRM_HEAD, target_paddr,
				req->rdma_header.rw_size);
		/* FaRM tail: 1B */
		memset(_FaRM_pass_buf + req->rdma_header.rw_size +
				FaRM_HEAD_AND_TAIL - 1, 1, 1);
	}
	else
		memset(_FaRM_pass_buf, FaRM_IS_EMPTY, FaRM_HEAD);

	cb->dma_addr_pass = __ib_map_pass(_FaRM_pass_buf, cb->conn_no,
						req->rdma_header.rw_size + FaRM_HEAD_AND_TAIL);
#else
	cb->dma_addr_pass = __ib_map_pass(target_paddr,
						cb->conn_no, req->rdma_header.rw_size);
#endif
#endif

	/* remote info: */
	 cb->remote_pass_rkey = ntohl(req->rdma_header.remote_rkey);
	 cb->remote_pass_addr = ntohll(req->rdma_header.remote_addr);
#if CONFIG_FARM
	 cb->remote_pass_len = req->rdma_header.rw_size + FaRM_HEAD_AND_TAIL;
#else
	 cb->remote_pass_len = req->rdma_header.rw_size;
#endif

	cb->rdma_send_wr.wr.opcode = IB_WR_RDMA_WRITE;
	cb->rdma_send_wr.rkey = cb->remote_pass_rkey;
	cb->rdma_send_wr.remote_addr = cb->remote_pass_addr;
	//cb->rdma_send_wr.wr.sg_list->length = cb->remote_pass_len;

	cb->rdma_send_wr.wr.next = NULL;

	/*
	struct ib_send_wr inv;			//- for ib_post_send -//
	cb->rdma_send_wr.wr.next = &inv;	//- followed by a inv -//
	memset(&inv, 0, sizeof inv);
	inv.opcode = IB_WR_LOCAL_INV;
	inv.ex.invalidate_rkey = cb->reg_mr_pass->rkey;
	inv.send_flags = IB_SEND_FENCE;
	*/

	/* RDMA local info: */
	cb->rdma_sgl[0].length = cb->remote_pass_len;
	cb->rdma_sgl[0].addr = cb->dma_addr_pass;
	cb->rdma_sgl[0].lkey = __ib_rdma_rkey(
							cb, cb->dma_addr_pass, 1,
							cb->remote_pass_len, RDMA_RKEY_PASS);


	mutex_lock(&cb->qp_mutex);
	ret = ib_post_send(cb->qp, &cb->rdma_send_wr.wr, &bad_wr);
	mutex_unlock(&cb->qp_mutex);
	if (ret) {
		printk(KERN_ERR "post send error %d\n", ret);
		return;
	}
	/* If just sent a FENCE, this should be turned on */
	//cb->rdma_send_wr.wr.next = NULL;

	/* Wait for completion */
	ret = wait_event_interruptible(cb->sem,
			atomic_read(&cb->write_state) == RDMA_WRITE_COMPLETE);
	atomic_set(&cb->write_state, IDLE);

#if CONFIG_FARM
	__unmap_pass(cb->conn_no,
					req->rdma_header.rw_size + FaRM_HEAD_AND_TAIL);
#elif CONFIG_FARM2
	mutex_lock(&cb->qp_mutex);
	ret = ib_post_send(cb->qp, &cb->rdma_FaRM2_send_wr.wr, &bad_wr);
	mutex_unlock(&cb->qp_mutex);
	if (ret) {
		printk(KERN_ERR "post send error %d\n", ret);
		return;
	}
	/* If just sent a FENCE, this should be turned on */
	//cb->rdma_FaRM2_send_wr.wr.next = NULL;

	ret = wait_event_interruptible(cb->sem,
					atomic_read(&cb->write_state) == RDMA_WRITE_COMPLETE);
	atomic_set(&cb->write_state, IDLE);

	__unmap_pass(cb->conn_no, FaRM2_DATA_SIZE);

	mutex_unlock(&cb->passive_mutex);
#elif !CONFIG_FARM
	reply = pcn_kmsg_alloc_msg(sizeof(*reply));
	if (!reply)
		BUG();

	reply->header.type = req->rdma_header.rmda_type_res;
	reply->header.prio = PCN_KMSG_PRIO_NORMAL;
	//reply->tgroup_home_cpu = tgroup_home_cpu;
	//reply->tgroup_home_id = tgroup_home_id;

	/* RDMA W/R complete ACK */
	reply->header.is_rdma = true;
	reply->rdma_header.rdma_ack = true;
	reply->rdma_header.is_write = true;
	reply->rdma_header.remote_rkey = cb->remote_pass_rkey;
	reply->rdma_header.remote_addr = cb->remote_pass_addr;
	reply->rdma_header.rw_size = cb->remote_pass_len;

	/* for wait station */
	reply->remote_ws = inc_lmsg->remote_ws;

	__ib_kmsg_send_long(req->header.from_nid,
				(struct pcn_kmsg_message*) reply, sizeof(*reply));

	pcn_kmsg_free_msg(reply);
#endif

	return;
}

static void __handle_remote_thread_rdma_write_response(
										remote_thread_rdma_rw_t* inc_lmsg)
{
	remote_thread_rdma_rw_t* res =
								(remote_thread_rdma_rw_t*) inc_lmsg;

#if !CONFIG_FARM
	struct ib_cb *cb = gcb[res->header.from_nid];
#endif

#if CONFIG_FARM2|| !CONFIG_FARM
	__unmap_act(cb->conn_no, res->rdma_header.rw_size);
	mutex_unlock(&cb->active_mutex);
#endif

	return;
}

/*
 *paddr: ptr of pages you wanna perform on passive side
 */
void handle_rdma_request(remote_thread_rdma_rw_t *inc_lmsg, void *paddr)
{
	remote_thread_rdma_rw_t *lmsg = inc_lmsg;
	if (likely(lmsg->header.is_rdma)) {
		if(unlikely(inc_lmsg->rdma_header.rw_size > MAX_RDMA_SIZE))
			BUG();
		if (!lmsg->rdma_header.rdma_ack) {
			if (lmsg->rdma_header.is_write)
				__handle_remote_thread_rdma_write_request(lmsg, paddr);
			else
				__handle_remote_thread_rdma_read_request(lmsg, paddr);
		} else {
			if (lmsg->rdma_header.is_write)
				__handle_remote_thread_rdma_write_response(lmsg);
			else
				__handle_remote_thread_rdma_read_response(lmsg);
		}
	} else {
		printk(KERN_ERR "This is not a rdma request you shouldn't call"
						"\"pcn_kmsg_handle_remote_rdma_request\"\n"
						"from=%u, type=%d, msg_size=%u\n\n",
						lmsg->header.from_nid,
						lmsg->header.type,
						lmsg->header.size);
	}
}

/*
 * Pass msg to upper layer and do the corresponding callback function
 */
static void __pcn_kmsg_handler_BottomHalf(struct work_struct *work)
{
	pcn_kmsg_work_t *w = (pcn_kmsg_work_t *) work;
	struct pcn_kmsg_message *lmsg;
	pcn_kmsg_cbftn ftn;

	lmsg = w->lmsg;
	if ( lmsg->header.type < 0 || lmsg->header.type >= PCN_KMSG_TYPE_MAX) {
		printk(KERN_ERR "Received invalid message type %d > MAX %d\n",
						lmsg->header.type, PCN_KMSG_TYPE_MAX);
	} else {
		ftn = callbacks[lmsg->header.type];
		if (ftn != NULL) {
#ifdef CONFIG_POPCORN_STAT
			account_pcn_message_recv(lmsg);
#endif
			ftn((void*)lmsg);
		} else {
			printk(KERN_ERR "Recieved message type %d size %d "
							"has no registered callback!\n",
							lmsg->header.type, lmsg->header.size);
			pcn_kmsg_free_msg(lmsg);
			BUG();
		}
	}

	kfree((void*)w);
	return;
}

/*
 * Parse recved msg in the buf to msg_layer in INT
 */
static int __ib_kmsg_recv_long(struct ib_cb *cb,
								 struct ib_recv_work_struct *rws)
{
	struct pcn_kmsg_message *lmsg = rws->element_addr;
	pcn_kmsg_work_t *kmsg_work;

	if (unlikely( lmsg->header.size > sizeof(struct pcn_kmsg_message))) {
		printk(KERN_ERR "Received invalide message size > MAX %lu\n",
									sizeof(struct pcn_kmsg_message));
		BUG();
	}

	/* - alloc & cpy msg to upper layer */
	kmsg_work = kmalloc(sizeof(*kmsg_work), GFP_ATOMIC);
	if (unlikely(!kmsg_work)) {
		printk(KERN_ERR "Failed to kmalloc work structure!\n");
		BUG();
	}
	kmsg_work->lmsg = kmalloc(lmsg->header.size, GFP_ATOMIC);
	if (unlikely(!kmsg_work->lmsg)) {
		printk(KERN_ERR "Failed to kmalloc msg in work structure!\n");
		BUG();
	}

	if (unlikely(!memcpy(kmsg_work->lmsg, lmsg, lmsg->header.size)))
		BUG();

	INIT_WORK((struct work_struct *)kmsg_work, __pcn_kmsg_handler_BottomHalf);
	if (unlikely(!queue_work(msg_handler, (struct work_struct *)kmsg_work)))
		BUG();

	return 0;
}

static void __ib_cq_event_handler(struct ib_cq *cq, void *ctx)
{
	struct ib_cb *cb = ctx;
	struct ib_wc wc;	/* work complition->wr_id (work request ID) */
	struct ib_recv_wr *bad_wr;
	int ret;
	int i, recv_cnt = 0;
	struct ib_wc *_wc;

	BUG_ON(cb->cq != cq);
	if (atomic_read(&cb->state) == ERROR) {
		printk(KERN_ERR "< cq completion in ERROR state >\n");
		return;
	}

	/* get a completion */
	while ((ret = ib_poll_cq(cb->cq, 1, &wc)) > 0) {
		_wc = &wc;

		if (_wc->status) { // !=IBV_WC_SUCCESS(0)
			if (_wc->status == IB_WC_WR_FLUSH_ERR) {
				printk("< cq flushed >\n");
			} else {
				printk(KERN_ERR "< cq completion failed with "
						"wr_id %Lx status %d opcode %d vender_err %x >\n",
						_wc->wr_id, _wc->status, _wc->opcode, _wc->vendor_err);
				BUG_ON(_wc->status);
				goto error;
			}
		}

		switch (_wc->opcode) {
		case IB_WC_SEND:
			atomic_set(&cb->send_state, RDMA_SEND_COMPLETE);
			wake_up_interruptible(&cb->sem);
			break;

		case IB_WC_RDMA_WRITE:
			atomic_set(&cb->write_state, RDMA_WRITE_COMPLETE);
			wake_up_interruptible(&cb->sem);
			break;

		case IB_WC_RDMA_READ:
			atomic_set(&cb->read_state, RDMA_READ_COMPLETE);
			wake_up_interruptible(&cb->sem);
			break;

		case IB_WC_RECV:
			recv_cnt++;
			ret = __ib_kmsg_recv_long(cb,
				(struct ib_recv_work_struct*)_wc->wr_id);
			if (ret) {
				printk(KERN_ERR "< recv wc error: %d >\n", ret);
				goto error;
			}
			__enq_rws((struct ib_recv_work_struct*)_wc->wr_id, cb->conn_no);
			break;

		default:
			printk(KERN_ERR "< %s:%d Unexpected opcode %d, Shutting down >\n",
											__func__, __LINE__, _wc->opcode);
			goto error;	/* TODO for rmmod */
			//wake_up_interruptible(&cb->sem);
			//ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
			return;
		}

		if (recv_cnt >= RECV_WQ_THRESHOLD)
			break;
	}

	for(i = 0; i < recv_cnt; i++) {
		struct ib_recv_work_struct *_rws = __dq_rws(cb->conn_no);
		if(unlikely(!_rws))
			break;

		ret = ib_post_recv(cb->qp, _rws->recv_wr, &bad_wr);
		if (ret) {
			printk(KERN_ERR "ib_post_recv failed: %d\n", ret);
			BUG();
		}
	}

	ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
	return;

error:
	atomic_set(&cb->state, ERROR);
	wake_up_interruptible(&cb->sem);
}


/*
 * Your request must be done by kmalloc()
 * You have to free the buf by yourself
 *
 * rw_size: size you wanna passive remote side to READ/WRITE
 *
 * READ/WRITE:
 * if R/W
 * [active lock]
 * send		 ----->   irq (recv)
 *					   |-passive lock R/W
 *					   |-perform R/W
 *					   |-passive unlock R/W
 * irq (recv)   <-----   |-send
 *  |-active unlock
 *
 * FaRM WRITE:
 * [active lock]
 * send		 ----->   irq (recv)
 * polling				|-passive lock R/W
 *						|-perform WRITE
 *						|-passive unlock R/W
 * active unlock
 */
char *ib_kmsg_send_rdma(unsigned int dst, remote_thread_rdma_rw_t *lmsg,
					  unsigned int msg_size, unsigned int rw_size)
{
	uint32_t rkey;
#if !CONFIG_FARM2 && CONFIG_FARM
	char *poll_tail_at, *FaRM_act_buf;
#endif
	struct ib_cb *cb = gcb[dst];

	if (rw_size <= 0)
		BUG();

	if (!lmsg->rdma_header.is_write)
		if (!lmsg->rdma_header.your_buf_ptr)
			BUG();

	if (dst == my_nid) {
		printk(KERN_ERR "No support for sending msg to itself %d\n", dst);
		return 0;
	}

	lmsg->header.is_rdma = true;
	lmsg->header.from_nid = my_nid;
	lmsg->rdma_header.rdma_ack = false;
	lmsg->rdma_header.rw_size = rw_size;

#if !CONFIG_FARM2 && CONFIG_FARM
	if (lmsg->rdma_header.is_write)
		FaRM_act_buf = kzalloc(rw_size + FaRM_HEAD_AND_TAIL, GFP_KERNEL);
#endif

	mutex_lock(&cb->active_mutex);

#if !CONFIG_FARM2 && CONFIG_FARM
	if (lmsg->rdma_header.is_write) {
		cb->dma_addr_act = __ib_map_act(FaRM_act_buf,
							cb->conn_no, rw_size + FaRM_HEAD_AND_TAIL);
	}
#else
	cb->dma_addr_act = __ib_map_act(lmsg->rdma_header.your_buf_ptr,
									cb->conn_no, rw_size);
#endif

	/* form rdma meta data */
	if (lmsg->rdma_header.is_write)
#if CONFIG_FARM
		rkey = __ib_rdma_rkey(cb, cb->dma_addr_act,
				!cb->server_invalidate,
				rw_size + FaRM_HEAD_AND_TAIL, RDMA_RKEY_ACT);
#else
		rkey = __ib_rdma_rkey(cb, cb->dma_addr_act,
				!cb->server_invalidate, rw_size, RDMA_RKEY_ACT);
#endif
	else
		rkey = __ib_rdma_rkey(cb, cb->dma_addr_act,
				!cb->server_invalidate, rw_size, RDMA_RKEY_ACT);

	lmsg->rdma_header.remote_addr = htonll(cb->dma_addr_act);
	lmsg->rdma_header.remote_rkey = htonl(rkey);

	__ib_kmsg_send_long(dst, (struct pcn_kmsg_message*) lmsg, msg_size);

	if (lmsg->rdma_header.is_write) {
#if CONFIG_FARM2
		while (*cb->FaRM2_buf_act == 0)
			schedule();
		*cb->FaRM2_buf_act = 0;
		mutex_unlock(&cb->active_mutex);
#else
		/* polling - not done:0  */
		while (*(FaRM_act_buf + FaRM_HEAD - 1) == 0)
			schedule();

		/* check size - if empty (0xff) */
		if(*(FaRM_act_buf + FaRM_HEAD - 1) == FaRM_IS_EMPTY) {
			kfree(FaRM_act_buf);
			return NULL;
		}

		/* poll at tail */
		poll_tail_at = FaRM_act_buf + rw_size + FaRM_HEAD_AND_TAIL - 1;
		while (*poll_tail_at == 0)
			schedule();
		__unmap_act(cb->conn_no, rw_size + FaRM_HEAD_AND_TAIL);
		mutex_unlock(&cb->active_mutex);

		return FaRM_act_buf;
#endif
	}
	return 0;
}

int ib_kmsg_send_long(unsigned int dst,
					  struct pcn_kmsg_message *lmsg,
					  unsigned int msg_size)
{
	lmsg->header.is_rdma = false;
	return __ib_kmsg_send_long(dst, lmsg, msg_size);
}

static void rdma_FaRM2_request(int dst)
{
	struct FaRM2_init_req_t *req = kmalloc(sizeof(*req), GFP_KERNEL);
	struct ib_cb *cb = gcb[dst];
	u32 rkey;

	req->header.type = PCN_KMSG_TYPE_RDMA_FARM2_REQUEST;
	req->header.prio = PCN_KMSG_PRIO_NORMAL;

	rkey = __ib_rdma_rkey(cb, cb->FaRM2_dma_addr_act,
								!cb->server_invalidate,
								FaRM2_DATA_SIZE, RDMA_FARM2_RKEY_ACT);
	req->remote_addr = htonll(cb->FaRM2_dma_addr_act);
	req->remote_rkey = htonl(rkey);
	ib_kmsg_send_long(dst, (void*)req, sizeof(*req));
}

static void handle_rdma_FaRM2_request(struct pcn_kmsg_message *inc_msg)
{
	struct FaRM2_init_req_t *req = (struct FaRM2_init_req_t*) inc_msg;
	struct ib_cb *cb = gcb[req->header.from_nid];

	/* remote info: */
	cb->remote_FaRM2_rkey = ntohl(req->remote_rkey);
	cb->remote_FaRM2_raddr = ntohll(req->remote_addr);
	cb->remote_FaRM2_rlen = FaRM2_DATA_SIZE;

	/* local info: */
	cb->local_FaRM2_llen = FaRM2_DATA_SIZE;
	cb->local_FaRM2_laddr = cb->pass_FaRM2_dma_addr;
	cb->local_FaRM2_lkey = __ib_rdma_rkey(cb,
						cb->pass_FaRM2_dma_addr, !cb->read_inv,
						FaRM2_DATA_SIZE, RDMA_FARM2_RKEY_PASS);

	cb->rdma_FaRM2_sgl.addr = cb->local_FaRM2_laddr;
	cb->rdma_FaRM2_sgl.lkey = cb->local_FaRM2_lkey;
	cb->rdma_FaRM2_sgl.length = cb->local_FaRM2_llen;

	cb->rdma_FaRM2_send_wr.wr.opcode = IB_WR_RDMA_WRITE;
	cb->rdma_FaRM2_send_wr.rkey = cb->remote_FaRM2_rkey;
	cb->rdma_FaRM2_send_wr.remote_addr = cb->remote_FaRM2_raddr;
	//cb->rdma_FaRM2_send_wr.wr.sg_list->length = FaRM2_DATA_SIZE;
	cb->rdma_FaRM2_send_wr.wr.next = NULL;

	kfree(req);
}


/* Initialize callback table to null, set up control and data channels */
int __init initialize()
{
	int i, err, conn_no;
	msg_layer = "IB";

	pcn_kmsg_register_callback(PCN_KMSG_TYPE_RDMA_FARM2_REQUEST,
								(pcn_kmsg_cbftn)handle_rdma_FaRM2_request);

	/* Establish node numbers with ip */
	if (!init_ip_table()) return -EINVAL;

	/* Create a workqueue for bottom-half */
	msg_handler = alloc_workqueue("MSGHandBotm", WQ_MEM_RECLAIM, 0);

	/* Initilaize the IB: Each node has a connection table like tihs
	 * -------------------------------------------------------------------
	 * | connect | (many)... | my_nid(one) | accept | accept | (many)... |
	 * -------------------------------------------------------------------
	 * my_nid:  no need to talk to itself
	 * connect: connecting to existing nodes
	 * accept:  waiting for the connection requests from later nodes
	 */
	for (i=0; i<MAX_NUM_NODES; i++) {
		/* Create global Control Block context for each connection */
		gcb[i] = kzalloc(sizeof(struct ib_cb), GFP_KERNEL);

		/* Settup node number */
		conn_no = i;
		gcb[i]->conn_no = i;
		set_popcorn_node_online(i, false);

		/* Setup locks */
		mutex_init(&gcb[i]->send_mutex);
		mutex_init(&gcb[i]->active_mutex);
		mutex_init(&gcb[i]->passive_mutex);
		mutex_init(&gcb[i]->qp_mutex);

		/* Init common parameters */
		gcb[i]->state.counter = IDLE;
		gcb[i]->send_state.counter = IDLE;
		gcb[i]->read_state.counter = IDLE;
		gcb[i]->write_state.counter = IDLE;

		/* For RECV ib_recv_work_struct queue */
        INIT_LIST_HEAD(&rws_wait_list_head[i].list);
		spin_lock_init(&(rws_q_spinlock[i]));

		/* set up IPv4 address */
		gcb[i]->addr_str = ip_addresses[conn_no];
		in4_pton(ip_addresses[conn_no], -1, gcb[i]->addr, -1, NULL);
		gcb[i]->addr_type = AF_INET;		/* [IPv4]/v6 */

		/* register event handler */
		gcb[i]->cm_id = rdma_create_id(&init_net,
				__ib_cma_event_handler, gcb[i], RDMA_PS_TCP, IB_QPT_RC);
		if (IS_ERR(gcb[i]->cm_id)) {
			err = PTR_ERR(gcb[i]->cm_id);
			printk(KERN_ERR "rdma_create_id error %d\n", err);
			goto out;
		}

		gcb[i]->key = i;
		gcb[i]->server = -1;
		gcb[i]->read_inv = 0;
		gcb[i]->server_invalidate = 0;
		init_waitqueue_head(&gcb[i]->sem);
		gcb[i]->rdma_FaRM2_max_size = MAX_RDMA_FARM2_SIZE;

#if CONFIG_FARM
		/* passive RW buffer */
		gcb[i]->FaRM_pass_buf = kzalloc(MAX_RDMA_SIZE, GFP_KERNEL);
		if(!gcb[i]->FaRM_pass_buf)
			BUG();
#endif
	}

	/* Establish connections
	 * Each node has a connection table like tihs:
	 * -------------------------------------------------------------------
	 * | connect | (many)... | my_nid(one) | accept | accept | (many)... |
	 * -------------------------------------------------------------------
	 * my_nid:  no need to talk to itself
	 * connect: connecting to existing nodes
	 * accept:  waiting for the connection requests from later nodes
	 */
	set_popcorn_node_online(my_nid, true);

	/* case 1: [<my_nid: connect] | =my_nid | >=my_nid: accept */
	for (i = 0; i < MAX_NUM_NODES; i++) {
		if (i == my_nid)
			continue;

		conn_no = i;
		if (conn_no < my_nid) {
			/* [connect] | my_nid | accept */
			gcb[conn_no]->server = 0;

			/* server/client dependant init */
			if (__ib_run_client(gcb[conn_no])) {
				printk("WRONG!!\n");
				rdma_disconnect(gcb[conn_no]->cm_id);
				return err;
			}

			set_popcorn_node_online(conn_no, true);
			smp_mb();
			printk("conn_no %d is ready (client)\n", conn_no);
		}
	}

	/* case 2: <my_nid: connect | =my_nid | [>=my_nid: accept] */
	__ib_run_server(gcb[my_nid]);

	for (i = 0;i < MAX_NUM_NODES; i++) {
		while ( !get_popcorn_node_online(i) ) {
			printk("waiting for get_popcorn_node_online(%d)\n", i);
			msleep(3000);
			//TODO: do semephor up down
		}
	}

#if DEMON_POST_RECV_WCS
	for (i = 0; i < MAX_NUM_NODES; i++) {
		if (i == my_nid)
			continue;
		recv_post_demon[i] = kthread_run(__recv_post, gcb[i],
							"demon for recyclingly posting recv_wc");
		BUG_ON(IS_ERR(recv_post_demon[i]));
	}
#endif

	for (i = 0; i < MAX_NUM_NODES; i++) {
		atomic_set(&gcb[i]->state, IDLE);
		atomic_set(&gcb[i]->send_state, IDLE);
		atomic_set(&gcb[i]->read_state, IDLE);
		atomic_set(&gcb[i]->write_state, IDLE);
	}

	send_callback = (send_cbftn)ib_kmsg_send_long;
	send_callback_rdma = (send_rdma_cbftn)ib_kmsg_send_rdma;
	handle_rdma_callback = (handle_rdma_request_ftn)handle_rdma_request;
	smp_mb();

	/* Popcorn exchanging arch info */
	for (i = 0; i < MAX_NUM_NODES; i++) {
		if (i == my_nid)
			continue;
		notify_my_node_info(i);
	}
	printk("------------------------------------------\n");
	printk("- Popcorn Messaging Layer IB Initialized -\n");
	printk("------------------------------------------\n"
										"\n\n\n\n\n\n\n");
	for (i = 0; i < MAX_NUM_NODES; i++) {
		if (i == my_nid)
			continue;
		rdma_FaRM2_request(i);
		//TODO: waiting by wait_station
	}

	return 0;

out:
	for (i = 0; i < MAX_NUM_NODES; i++){
		if (atomic_read(&gcb[i]->state)) {
			kfree(gcb[i]);
			/* TODO: cut connections */
		}
	}
	return err;
}


/*
 *  Not yet done.
 */
static void __exit unload(void)
{
	int i;
	printk("TODO: Stop kernel threads\n");

	printk("Release general\n");
	for (i = 0; i < MAX_NUM_NODES; i++) {
		/* mutex */
		mutex_destroy(&gcb[i]->send_mutex);

		/* IB FaRM WRITE passive buffer */
#if CONFIG_FARM
		if (gcb[i]->FaRM_pass_buf)
			kfree(gcb[i]->FaRM_pass_buf);
#endif
	}

	printk("Release threadss\n");
	for (i = 0; i < MAX_NUM_NODES; i++) {
#if DEMON_POST_RECV_WCS
		kthread_stop(recv_post_demon[i]);
#endif
	}

	printk("Release IB recv pre-post buffers and flush it\n");
	for (i = 0; i < MAX_NUM_NODES; i++) {
	}

	/* TODO: test rdma_disconnect() */
	printk("rdma_disconnect() only on one side\n");
	for (i = 0; i < MAX_NUM_NODES; i++) {
		if (i == my_nid)
			continue;
		if (i < my_nid) {
			/* client */
			if (gcb[i]->cm_id)
				//if (rdma_disconnect(gcb[i]->cm_id))
				//	BUG();
				;
		} else {
			/* server */
			if (gcb[i]->child_cm_id)
				if (rdma_disconnect(gcb[i]->child_cm_id))
					BUG();
		}
		//if (gcb[i]->cm_id)
		//	rdma_disconnect(gcb[i]->cm_id);
	}

	printk("Release IB server/client productions \n");
	for (i = 0; i < MAX_NUM_NODES; i++) {
		struct ib_cb *cb = gcb[i];

		if (!get_popcorn_node_online(i))
			continue;

		set_popcorn_node_online(i, false);

		if (i == my_nid)
			continue;

		if (i < my_nid) {
			/* client */
			__ib_free_buffers(cb);
			__ib_free_qp(cb);
		} else {
			/* server */
			__ib_free_buffers(cb);
			__ib_free_qp(cb);
			rdma_destroy_id(cb->child_cm_id);
		}
	}

	printk("Release RDMA relavant\n");
	for (i = 0; i < MAX_NUM_NODES; i++) {
		kfree(gcb[i]->FaRM2_buf_act);
		kfree(gcb[i]->pass_FaRM2_buf);
		dma_unmap_single(gcb[i]->pd->device->dma_device,
						pci_unmap_addr(gcb[i], pass_FaRM2_rdma_mapping),
						FaRM2_DATA_SIZE, DMA_BIDIRECTIONAL);
		dma_unmap_single(gcb[i]->pd->device->dma_device,
						pci_unmap_addr(gcb[i], FaRM2_rdma_mapping_act),
						FaRM2_DATA_SIZE, DMA_BIDIRECTIONAL);
	}

	printk("Release cb context\n");
	for (i = 0; i < MAX_NUM_NODES; i++) {
		kfree(gcb[i]);
	}

	printk("Successfully unloaded module!\n");
}

module_init(initialize);
module_exit(unload);
MODULE_LICENSE("GPL");
