/*
 * msg_ib.c - Kernel Module for Popcorn Messaging Layer
 * multi-node version over InfiniBand
 * Author: Ho-Ren(Jack) Chuang
 *
 * TODO:
 *		define 0~1 to enum if needed
 *		(perf!)sping when send
 *	RDMA:
 *		request_ib_rdma(..., is_write)
 *		remove req_rdma->rdma_header.is_write = true;
 */

#include <linux/module.h>
#include <linux/kernel.h>
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
#define CONFIG_FARM 0			/* Original FaRM - user follows convention */
#define CONFIG_RDMA_POLL 1		/* with one extra buf copy */
#define CONFIG_RDMA_NOTIFY 0	/* Two WRITE */

/* self-testing */
#define CHECK_WQ_WR 1
#define CHECK_RECV_WR 0

/* IB recv */
#define MAX_RECV_WR 128	/* important! Check it if only sender crash */
/* IB send & completetion */
#define MAX_SEND_WR 128
#define MAX_CQE MAX_SEND_WR + MAX_RECV_WR

/* RDMA MR POOL */
#define MR_POOL_SIZE 64

/* RDMA POLL conventionals: w/ 1 extra copy version of RDMA */
#if CONFIG_RDMA_POLL
#define POLL_HEAD 4 + 1	/* length + length end bit*/
#define POLL_TAIL 1
#define POLL_HEAD_AND_TAIL POLL_HEAD + POLL_TAIL

#define POLL_IS_DATA 0x01
#endif

#define POLL_IS_IDLE 0

/* IB buffers */
#if CONFIG_RDMA_POLL
#define MAX_RDMA_SIZE PCN_KMSG_MAX_SIZE - POLL_HEAD_AND_TAIL
#else
#define MAX_RDMA_SIZE PCN_KMSG_MAX_SIZE
#endif
#define MAX_RDMA_PAGES ((MAX_RDMA_SIZE + PAGE_SIZE - 1) >> PAGE_SHIFT)

/* RDMA_POLL: two WRITE version of RDMA */
#if CONFIG_RDMA_NOTIFY
#define RDMA_NOTIFY_ACT_DATA_SIZE MAX_SEND_WR
#define RMDA_NOTIFY_PASS_DATA_SIZE 1
#define MAX_RDMA_NOTIFY_SIZE 1
#endif

/* IB connection config */
#define PORT 10453
#define LISTEN_BACKLOG 99
#define CONN_RESPONDER_RESOURCES 1
#define CONN_INITIATOR_DEPTH 1
#define CONN_RETRY_CNT 1

/* RDMA key register */
enum IB_MR_TYPES {
	RDMA_MR = 0,
	RDMA_FARM_NOTIFY_RKEY_ACT,
	RDMA_FARM_NOTIFY_RKEY_PASS,
	RDMA_MR_TYPES,
};

/* IB runtime status */
enum IB_CM_STATUS {
	IDLE = 0,
	CONNECT_REQUEST,
	ADDR_RESOLVED,
	ROUTE_RESOLVED,
	CONNECTED,
	ERROR,
};

/* workqueue arg */
struct recv_work_t {
	struct ib_recv_wr recv_wr;
	struct ib_sge sgl;
	struct pcn_kmsg_message msg;
};

/*for testing */
#if CHECK_RECV_WR
void *rws_ptr[MAX_NUM_NODES][MAX_RECV_WR];
void *msg_ptr[MAX_NUM_NODES][MAX_RECV_WR];
#endif

/* rdma_notify */
#if CONFIG_RDMA_NOTIFY
struct rdma_notify_init_req_t {
    struct pcn_kmsg_hdr header;
    uint32_t remote_key;
    uint64_t remote_addr;
    //uint32_t remote_size;
	struct completion *comp;
};

struct rdma_notify_init_res_t {
    struct pcn_kmsg_hdr header;
	struct completion *comp;
};
#endif

/* InfiniBand Control Block */
struct ib_cb {
	/* Parameters */
	int server;						/* server:1/client:0/myself:-1 */
	int conn_no;
	u8 key;

	/* IB essentials */
	struct ib_cq *cq;				/* can split into two send/recv */
	struct ib_pd *pd;
	struct ib_qp *qp;

	/* how many WR in Work Queue */
#if CHECK_WQ_WR
	atomic_t WQ_WR_cnt;
#endif

	/* RDMA common */
	struct ib_mr *mr_pool[MR_POOL_SIZE];
	struct ib_reg_wr reg_wr_pool[MR_POOL_SIZE];	/* reg kind of = rdma  */
	struct ib_send_wr inv_wr_pool[MR_POOL_SIZE];

#if CONFIG_RDMA_POLL
	char *rdma_poll_buffer[MR_POOL_SIZE];
#endif

#if CONFIG_RDMA_NOTIFY
	struct ib_mr *reg_rdma_notify_mr_act;
	struct ib_mr *reg_rdma_notify_mr_pass[MR_POOL_SIZE];

	struct ib_reg_wr reg_rdma_notify_mr_wr_act;
	struct ib_reg_wr reg_rdma_notify_mr_wr_pass[MR_POOL_SIZE];
	struct ib_send_wr inv_rdma_notify_wr_act;
	struct ib_send_wr inv_rdma_notify_wr_pass[MR_POOL_SIZE];

	/* From remote */
	uint32_t remote_key;
	uint64_t remote_addr;
	//uint32_t remote_rdma_notify_rlen;
	/* From locaol */
	uint32_t local_key[MR_POOL_SIZE];
	uint64_t local_addr[MR_POOL_SIZE];
	//uint32_t local_rdma_notify_llen;

	/* RDMA buf for rdma_notify (local) */
	char *rdma_notify_buf_act;
	char *rdma_notify_buf_pass[MR_POOL_SIZE];
	u64 rdma_notify_dma_addr_act;
	u64 rdma_notify_dma_addr_pass[MR_POOL_SIZE];
#endif

	/* Connection */
	u8 addr[16];				/* dst addr in NBO */
	atomic_t state;
	wait_queue_head_t sem;

	/* CM stuffs */
	struct rdma_cm_id *cm_id;		/* connection on client side */
									/* listener on server side */
	struct rdma_cm_id *peer_cm_id;	/* connection on server side */
};

/* InfiniBand Control Block per connection*/
struct ib_cb *gcb[MAX_NUM_NODES];

/* Functions */
static void __cq_event_handler(struct ib_cq *cq, void *ctx);

/* MR bit map */
static spinlock_t mr_pool_lock[MAX_NUM_NODES][RDMA_MR_TYPES];
static unsigned long mr_pool[MAX_NUM_NODES][RDMA_MR_TYPES]
										[BITS_TO_LONGS(MR_POOL_SIZE)];
/* Wrapped by a sem for reducing cpu usage? */
static u32 __get_mr(int dst, int mode)
{
    int ofs;
retry:
    spin_lock(&mr_pool_lock[dst][mode]);
	ofs = find_first_zero_bit(mr_pool[dst][mode], MR_POOL_SIZE);
	if (ofs >= MR_POOL_SIZE) {
		spin_unlock(&mr_pool_lock[dst][mode]);
		printk(KERN_WARNING "mr full !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
		schedule();
		goto retry;
	}
    set_bit(ofs, mr_pool[dst][mode]);
    spin_unlock(&mr_pool_lock[dst][mode]);
    return ofs;
}

static void __put_mr(int dst, u32 ofs, int mode)
{
    spin_lock(&mr_pool_lock[dst][mode]);
	BUG_ON(!test_bit(ofs, mr_pool[dst][mode]));
    clear_bit(ofs, mr_pool[dst][mode]);
    spin_unlock(&mr_pool_lock[dst][mode]);
}

#if CHECK_WQ_WR
static inline void selftest_wr_wq_inc(struct ib_cb *cb)
{
	atomic_inc(&cb->WQ_WR_cnt);
	BUG_ON(atomic_read(&cb->WQ_WR_cnt) >= MAX_SEND_WR);
}

static inline void selftest_wr_wq_dec(struct ib_cb *cb)
{
	atomic_dec(&cb->WQ_WR_cnt);
}
#else
static inline void selftest_wr_wq_inc(struct ib_cb *cb) {}
static inline void selftest_wr_wq_dec(struct ib_cb *cb) {}
#endif

inline void selftest_recv_pool(struct recv_work_t *rws, struct pcn_kmsg_message *msg)
{
#if CHECK_RECV_WR
	int i;
	bool good_rws = false, good_msg = false;
	for (i = 0; i < MAX_RECV_WR; i++) {
		if (msg == msg_ptr[msg->header.from_nid][i])
			good_msg = true;
		if ( rws == rws_ptr[msg->header.from_nid][i])
			good_rws = true;
	}
	if(good_msg == false) {
		printk("%p\n", msg);
		BUG();
	}
	if(good_rws == false) {
		printk("%p\n", rws);
		BUG();
	}
#endif
}


static int __cm_event_handler(struct rdma_cm_id *cm_id, struct rdma_cm_event *event)
{
	int ret;
	struct ib_cb *cb = cm_id->context; /* use cm_id to retrive cb */
	static int cm_event_cnt = 0, conn_event_cnt = 0;

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		atomic_set(&cb->state, ADDR_RESOLVED);
		ret = rdma_resolve_route(cm_id, 2000);
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
		cb->peer_cm_id = cm_id;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		if (gcb[my_nid]->conn_no == cb->conn_no) {
			cm_event_cnt++;

			atomic_set(&gcb[my_nid + cm_event_cnt]->state, CONNECTED);
			wake_up_interruptible(&gcb[my_nid + cm_event_cnt]->sem);
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
		printk(KERN_ERR "< cm event %d, error %d >\n",
				event->event, event->status);
		atomic_set(&cb->state, ERROR);
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		printk(KERN_ERR "< --- %d DISCONNECTED --- >\n", cb->conn_no);
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		printk(KERN_ERR "< ----- Device removed ----- >\n");
		break;

	default:
		printk(KERN_ERR "< ----- Unknown event type %d----- >\n", event->event);
		wake_up_interruptible(&cb->sem);
		break;
	}
	return 0;
}

/*
 * Create a recv scatter-gather list(entries) & work request
 */
static struct recv_work_t *__alloc_recv_wr(int conn_no)
{
	int ret;
	struct ib_sge *sgl;
	struct ib_recv_wr *recv_wr;
	struct ib_cb *cb = gcb[conn_no];
	struct recv_work_t *work;

	work = kmalloc(sizeof(*work), GFP_KERNEL);
	BUG_ON(!work);

	/* set up sgl */
	sgl = &work->sgl;
	sgl->length = PCN_KMSG_MAX_SIZE;
	sgl->lkey = cb->pd->local_dma_lkey;
	sgl->addr = dma_map_single(cb->pd->device->dma_device,
					  &work->msg, PCN_KMSG_MAX_SIZE, DMA_BIDIRECTIONAL);
	ret = dma_mapping_error(cb->pd->device->dma_device, sgl->addr);
	BUG_ON(ret);

	/* set up recv_wr */
	recv_wr = &work->recv_wr;
	recv_wr->sg_list = sgl;
	recv_wr->num_sge = 1;
	recv_wr->next = NULL;
	recv_wr->wr_id = (u64)work;

	return work;
}


static void fill_sockaddr(struct sockaddr_storage *sin, struct ib_cb *cb)
{
	struct sockaddr_in *sin4 = (struct sockaddr_in *)sin;
	u8 *addr;

	memset(sin4, 0, sizeof(*sin4));
	if (cb->server) {
		/* server: load from global (ip=itself) */
		addr = gcb[my_nid]->addr;
	} else {
		/* client: load as usuall (ip=remote) */
		addr = cb->addr;
	}
	sin4->sin_family = AF_INET;
	sin4->sin_port = htons(PORT);
	memcpy((void *)&sin4->sin_addr.s_addr, addr, 4);
}

static int ib_bind_server(struct ib_cb *cb)
{
	int ret;
	struct sockaddr_storage sin;

	fill_sockaddr(&sin, cb);
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

/* set up sgl */
static void __setup_recv_wr(struct ib_cb *cb)
{
	int i = 0, ret;

	/* Pre-post RECV buffers */
	for(i = 0; i < MAX_RECV_WR; i++) {
		struct ib_recv_wr *bad_wr;
		struct recv_work_t *work = __alloc_recv_wr(cb->conn_no);

		ret = ib_post_recv(cb->qp, &work->recv_wr, &bad_wr);
		BUG_ON(ret && "ib_post_recv failed");

#if CHECK_RECV_WR
		rws_ptr[cb->conn_no][i] = work;
		msg_ptr[cb->conn_no][i] = &work->msg;
#endif
	}
	return;
}

static int _ib_create_qp(struct ib_cb *cb)
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

	init_attr.cap.max_recv_sge = 1;
	init_attr.cap.max_send_sge = 1;
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
		ret = rdma_create_qp(cb->peer_cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->peer_cm_id->qp;
	} else {
		ret = rdma_create_qp(cb->cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->cm_id->qp;
	}

	return ret;
}

static int ib_setup_qp(struct ib_cb *cb, struct rdma_cm_id *cm_id)
{
	int ret;
	struct ib_cq_init_attr attr = {0};

	cb->pd = ib_alloc_pd(cm_id->device);
	if (IS_ERR(cb->pd)) {
		printk(KERN_ERR "ib_alloc_pd failed\n");
		return PTR_ERR(cb->pd);
	}

	attr.cqe = MAX_CQE;
	attr.comp_vector = 0;
	cb->cq = ib_create_cq(cm_id->device, __cq_event_handler, NULL, cb, &attr);
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

	ret = _ib_create_qp(cb);
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
 *		0:RDMA_MR
 *		1:RDMA_FARM_NOTIFY_RKEY_ACT
 *		2:RDMA_FARM_NOTIFY_RKEY_PASS
 */
static u32 __map_rdma_mr(struct ib_cb *cb, u64 dma_addr, int dma_len, u32 mr_id, int mode)
{
	int ret;
	struct ib_send_wr *bad_wr;
	struct scatterlist sg = {0};
	struct ib_mr *reg_mr;

	struct ib_send_wr *inv_wr;
	struct ib_reg_wr *reg_wr;

	if (mode == RDMA_MR) {
		reg_mr = cb->mr_pool[mr_id];
		inv_wr = &cb->inv_wr_pool[mr_id];
		reg_wr = &cb->reg_wr_pool[mr_id];
#if CONFIG_RDMA_NOTIFY
	} else if (mode == RDMA_FARM_NOTIFY_RKEY_ACT) {
		reg_mr = cb->reg_rdma_notify_mr_act;
		inv_wr = &cb->inv_rdma_notify_wr_act;
		reg_wr = &cb->reg_rdma_notify_mr_wr_act;
	} else if (mode == RDMA_FARM_NOTIFY_RKEY_PASS) {
		reg_mr = cb->reg_rdma_notify_mr_pass[mr_id];
		inv_wr = &cb->inv_rdma_notify_wr_pass[mr_id];
		reg_wr = &cb->reg_rdma_notify_mr_wr_pass[mr_id];
#endif
	}
	inv_wr->ex.invalidate_rkey = reg_mr->rkey;

	sg_dma_address(&sg) = dma_addr;
	sg_dma_len(&sg) = dma_len;
	ib_update_fast_reg_key(reg_mr, cb->key);
	ret = ib_map_mr_sg(reg_mr, &sg, 1, PAGE_SIZE);
	// snyc: use ib_dma_sync_single_for_cpu/dev dev:accessed by IB
	BUG_ON(ret <= 0 || ret > MAX_RDMA_PAGES);

	reg_wr->key = reg_mr->rkey;
	reg_wr->access = IB_ACCESS_REMOTE_READ	|
					IB_ACCESS_REMOTE_WRITE	|
					IB_ACCESS_LOCAL_WRITE	|
					IB_ACCESS_REMOTE_ATOMIC;

	ret = ib_post_send(cb->qp, inv_wr, &bad_wr);	// INV+MR //
	BUG_ON(ret);

	return reg_mr->rkey;
}


#if CONFIG_RDMA_NOTIFY
static int ib_setup_buffers_rdma_notify(struct ib_cb *cb)
{
	int i, ret;
	const int nr_pages_notify =
			(MAX_RDMA_NOTIFY_SIZE + PAGE_SIZE - 1) >> PAGE_SHIFT;
	for (i = 0; i < MR_POOL_SIZE; i++) {
		cb->reg_rdma_notify_mr_pass[i] =
				ib_alloc_mr(cb->pd, IB_MR_TYPE_MEM_REG, nr_pages_notify);
		if (IS_ERR(cb->reg_rdma_notify_mr_pass[i])) {
			ret = PTR_ERR(cb->reg_rdma_notify_mr_pass[i]);
			goto bail;
		}
		cb->reg_rdma_notify_mr_wr_pass[i].wr.opcode = IB_WR_REG_MR;
		cb->reg_rdma_notify_mr_wr_pass[i].mr = cb->reg_rdma_notify_mr_pass[i];

		cb->inv_rdma_notify_wr_pass[i].opcode = IB_WR_LOCAL_INV;
		cb->inv_rdma_notify_wr_pass[i].next = &cb->reg_rdma_notify_mr_wr_pass[i].wr;

		cb->rdma_notify_buf_pass[i] =
				kmalloc(RMDA_NOTIFY_PASS_DATA_SIZE, GFP_KERNEL);
		if (!cb->rdma_notify_buf_pass[i]) {
			ret = -ENOMEM;
			goto bail;
		}
		cb->rdma_notify_dma_addr_pass[i] = dma_map_single(
				cb->pd->device->dma_device, cb->rdma_notify_buf_pass[i],
				RMDA_NOTIFY_PASS_DATA_SIZE, DMA_BIDIRECTIONAL);
		ret = dma_mapping_error(
				cb->pd->device->dma_device, cb->rdma_notify_dma_addr_pass[i]);
		BUG_ON(ret);
		*cb->rdma_notify_buf_pass[i] = 1;
	}

	cb->reg_rdma_notify_mr_act = ib_alloc_mr(cb->pd,
						IB_MR_TYPE_MEM_REG, nr_pages_notify);
	if (IS_ERR(cb->reg_rdma_notify_mr_act)) {
		ret = PTR_ERR(cb->reg_rdma_notify_mr_act);
		goto bail;
	}

	cb->rdma_notify_buf_act = kmalloc(RDMA_NOTIFY_ACT_DATA_SIZE, GFP_KERNEL);
	if (!cb->rdma_notify_buf_act) {
        ret = -ENOMEM;
        goto bail;
    }
    cb->rdma_notify_dma_addr_act = dma_map_single(
			cb->pd->device->dma_device, cb->rdma_notify_buf_act,
			RDMA_NOTIFY_ACT_DATA_SIZE, DMA_BIDIRECTIONAL);
	ret = dma_mapping_error(
			cb->pd->device->dma_device, cb->rdma_notify_dma_addr_act);
	BUG_ON(ret);
	for (i = 0; i < RDMA_NOTIFY_ACT_DATA_SIZE; i++)
		*(cb->rdma_notify_buf_act + i) = 0;

	cb->reg_rdma_notify_mr_wr_act.wr.opcode = IB_WR_REG_MR;
	cb->reg_rdma_notify_mr_wr_act.mr = cb->reg_rdma_notify_mr_act;

	cb->inv_rdma_notify_wr_act.opcode = IB_WR_LOCAL_INV;
	cb->inv_rdma_notify_wr_act.next = &cb->reg_rdma_notify_mr_wr_act.wr;

	return 0;
bail:
	for (i = 0; i < MR_POOL_SIZE; i++) {
		if (cb->reg_rdma_notify_mr_pass[i] && !IS_ERR(cb->reg_rdma_notify_mr_pass[i]))
			ib_dereg_mr(cb->reg_rdma_notify_mr_pass[i]);
	}
	if (cb->reg_rdma_notify_mr_act && !IS_ERR(cb->reg_rdma_notify_mr_act))
		ib_dereg_mr(cb->reg_rdma_notify_mr_act);
	return ret;
}
#endif

/*
 * init all buffers < 1.pd->cq->qp 2.[mr] 3.xxx >
 */
static int ib_setup_buffers(struct ib_cb *cb)
{
	int i, ret;
	for (i = 0; i < MR_POOL_SIZE; i++) {
		/* fill out lkey and rkey */
		cb->mr_pool[i] = ib_alloc_mr(cb->pd, IB_MR_TYPE_MEM_REG, MAX_RDMA_PAGES);
		if (IS_ERR(cb->mr_pool[i])) {
			ret = PTR_ERR(cb->mr_pool[i]);
			goto bail;
		}

		/*
		 * A chain of 2 WRs, INVALDATE_MR + REG_MR.
		 * both unsignaled (no completion).  The client uses them to reregister
		 * the rdma buffers with a new key each iteration.
		 * IB_WR_REG_MR = legacy:fastreg mode
		 */
		cb->reg_wr_pool[i].wr.opcode = IB_WR_REG_MR;
		cb->reg_wr_pool[i].mr = cb->mr_pool[i];
		cb->reg_wr_pool[i].wr.opcode = IB_WR_REG_MR;
		cb->reg_wr_pool[i].mr = cb->mr_pool[i];

		/*
		 * 1. invalidate Memory Window locally
		 * 2. then register this new key to mr
		 */
		cb->inv_wr_pool[i].opcode = IB_WR_LOCAL_INV;
		cb->inv_wr_pool[i].next = &cb->reg_wr_pool[i].wr;
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

	}

#if CONFIG_RDMA_NOTIFY
	if ((ret = ib_setup_buffers_rdma_notify(cb))) {
		goto bail;
	}
#endif
	__setup_recv_wr(cb);
	return 0;
bail:
	for (i = 0; i < MR_POOL_SIZE; i++) {
		if (cb->mr_pool[i] && !IS_ERR(cb->mr_pool[i]))
			ib_dereg_mr(cb->mr_pool[i]);
	}
	return ret;
}


static void ib_free_buffers(struct ib_cb *cb, u32 mr_id)
{
	if (cb->mr_pool[mr_id])
		ib_dereg_mr(cb->mr_pool[mr_id]);
#if CONFIG_RDMA_NOTIFY
	if (cb->reg_rdma_notify_mr_act)
		ib_dereg_mr(cb->reg_rdma_notify_mr_act);
	if (cb->reg_rdma_notify_mr_pass[mr_id])
		ib_dereg_mr(cb->reg_rdma_notify_mr_pass[mr_id]);
#endif
}

static void ib_free_qp(struct ib_cb *cb)
{
	ib_destroy_qp(cb->qp);
	ib_destroy_cq(cb->cq);
	ib_dealloc_pd(cb->pd);
}

static int ib_accept(struct ib_cb *cb)
{
	int ret;
	struct rdma_conn_param conn_param = {
		.responder_resources = 1,
		.initiator_depth = 1,
	};

	ret = rdma_accept(cb->peer_cm_id, &conn_param);
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
	return 0;
}

static int ib_server_accept(struct ib_cb *cb)
{
	int i, ret = -1;

	ret = ib_setup_qp(cb, cb->peer_cm_id);
	if (ret) {
		printk(KERN_ERR "setup_qp failed: %d\n", ret);
		goto err0;
	}

	ret = ib_setup_buffers(cb);
	if (ret) {
		printk(KERN_ERR "ib_setup_buffers failed: %d\n", ret);
		goto err1;
	}
	/* after here, you can send/recv */

	ret = ib_accept(cb);
	if (ret) {
		printk(KERN_ERR "connect error %d\n", ret);
		goto err2;
	}
	return 0;
err2:
	for (i = 0; i < MR_POOL_SIZE; i++)
		ib_free_buffers(cb, i);
err1:
	ib_free_qp(cb);
err0:
	rdma_destroy_id(cb->peer_cm_id);
	return ret;
}

static int ib_run_server(struct ib_cb *my_cb)
{
	int ret, i = 0;

	ret = ib_bind_server(my_cb);
	if (ret)
		return ret;

	/* create multiple connections */
	for (i = my_nid + 1; i < MAX_NUM_NODES; i++) {
		struct ib_cb *peer_cb;

		/* Wait for client's Start STAG/TO/Len */
		wait_event_interruptible(my_cb->sem,
					atomic_read(&my_cb->state) == CONNECT_REQUEST);
		if (atomic_read(&my_cb->state) != CONNECT_REQUEST) {
			printk(KERN_ERR "wait for CONNECT_REQUEST state %d\n",
										atomic_read(&my_cb->state));
			continue;
		}
		atomic_set(&my_cb->state, IDLE);

		peer_cb = gcb[i];
		peer_cb->server = 1;

		/* got from INT. Will be used [setup_qp(SRWRirq)] -> setup_buf -> */
		peer_cb->peer_cm_id = my_cb->peer_cm_id;

		if (ib_server_accept(peer_cb))
			rdma_disconnect(peer_cb->peer_cm_id);

		printk("conn_no %d is ready (sever)\n", peer_cb->conn_no);
		set_popcorn_node_online(peer_cb->conn_no, true);
	}
	return 0;
}


static int ib_bind_client(struct ib_cb *cb)
{
	struct sockaddr_storage sin;
	int ret;

	fill_sockaddr(&sin, cb);

	ret = rdma_resolve_addr(cb->cm_id, NULL, (struct sockaddr *)&sin, 2000);
	if (ret) {
		printk(KERN_ERR "rdma_resolve_addr error %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem, atomic_read(&cb->state) == ROUTE_RESOLVED);
	if (atomic_read(&cb->state) != ROUTE_RESOLVED) {
		printk(KERN_ERR "addr/route resolution did not resolve: state %d\n",
													atomic_read(&cb->state));
		return -EINTR;
	}

	return 0;
}


static int ib_connect_client(struct ib_cb *cb)
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

static int ib_run_client(struct ib_cb *cb)
{
	int i, ret;

	ret = ib_bind_client(cb);
	if (ret)
		return ret;

	ret = ib_setup_qp(cb, cb->cm_id);
	if (ret) {
		printk(KERN_ERR "setup_qp failed: %d\n", ret);
		return ret;
	}

	ret = ib_setup_buffers(cb);
	if (ret) {
		printk(KERN_ERR "ib_setup_buffers failed: %d\n", ret);
		goto err1;
	}

	ret = ib_connect_client(cb);
	if (ret) {
		printk(KERN_ERR "connect error %d\n", ret);
		goto err2;
	}
	return 0;
err2:
	for (i = 0; i < MR_POOL_SIZE; i++)
		ib_free_buffers(cb, i);
err1:
	ib_free_qp(cb);
	return ret;
}


/*
 * User doesn't have to take care of concurrency problems.
 * This func will take care of it.
 * User has to free the allocated mem manually
 */
static int __ib_kmsg_send(unsigned int dst,
				  struct pcn_kmsg_message *msg,
				  unsigned int msg_size)
{
	int ret;
	DECLARE_COMPLETION_ONSTACK(comp);
	struct ib_send_wr *bad_wr;
	struct ib_cb *cb = gcb[dst];
	struct ib_sge send_sgl = {
		.length = msg_size,
		.lkey = cb->pd->local_dma_lkey,
	};
	struct ib_send_wr send_wr = {
		.opcode = IB_WR_SEND,
		.send_flags = IB_SEND_SIGNALED,
		.num_sge = 1,
		.sg_list = &send_sgl,
		.next = NULL,
		.wr_id = (unsigned long)&comp,
	};
	u64 dma_addr;

	msg->header.size = msg_size;
	msg->header.from_nid = my_nid;

	dma_addr = dma_map_single(cb->pd->device->dma_device,
							msg, msg_size, DMA_BIDIRECTIONAL);
	ret = dma_mapping_error(cb->pd->device->dma_device, dma_addr);
	BUG_ON(ret);

	send_sgl.addr = dma_addr;

	selftest_wr_wq_inc(cb);
	ret = ib_post_send(cb->qp, &send_wr, &bad_wr);
	BUG_ON(ret);

	if (!try_wait_for_completion(&comp))
		wait_for_completion(&comp);

	dma_unmap_single(cb->pd->device->dma_device,
					 dma_addr, msg->header.size, DMA_BIDIRECTIONAL);
	return 0;
}


/*
 * RDMA READ:
 * send        ----->   irq (recv)
 *                      lock
 *             <=====   perform READ
 *                      unlock
 * irq (recv)  <-----   send
 *
 */
static void __respond_rdma_read(pcn_kmsg_perf_rdma_t *req, void *res, u32 res_size)
{
	pcn_kmsg_perf_rdma_t reply;
	struct ib_send_wr *bad_wr;
	int ret, from = req->header.from_nid;
	struct ib_cb *cb = gcb[from];
	DECLARE_COMPLETION_ONSTACK(comp);
	u32 mr_id;
	u64 dma_addr_pass = dma_map_single(cb->pd->device->dma_device,
				res, res_size, DMA_BIDIRECTIONAL);
	struct ib_sge sgl = {
		.length = res_size,
		.addr = dma_addr_pass,
	};
	struct ib_rdma_wr rdma_wr = {
		.wr = {
			.opcode = IB_WR_RDMA_READ,
			.send_flags = IB_SEND_SIGNALED,
			.sg_list = &sgl,
			.num_sge = 1,
			.wr_id = (u64)&comp,
			.next = NULL,
		},
		.rkey = req->rdma_header.remote_rkey,
		.remote_addr = req->rdma_header.remote_addr,
	};
	ret = dma_mapping_error(cb->pd->device->dma_device, dma_addr_pass);
	BUG_ON(ret);

	/* Compose a READ sge with a invalidation */
	mr_id = __get_mr(cb->conn_no, RDMA_MR);
	sgl.lkey = __map_rdma_mr(cb, dma_addr_pass, res_size, mr_id, RDMA_MR),

	ret = ib_post_send(cb->qp, &rdma_wr.wr, &bad_wr);
	selftest_wr_wq_inc(cb);
	BUG_ON(ret);

	if (!try_wait_for_completion(&comp))
		wait_for_completion(&comp);

	__put_mr(cb->conn_no, mr_id, RDMA_MR);
	dma_unmap_single(cb->pd->device->dma_device,
					dma_addr_pass, res_size, DMA_BIDIRECTIONAL);

	/* ACK */
	reply.header.type = req->rdma_header.rmda_type_res;
	reply.header.prio = PCN_KMSG_PRIO_NORMAL;

	/* RDMA R/W complete ACK */
	reply.header.is_rdma = true;
	reply.rdma_header.rdma_ack = true;
	reply.rdma_header.is_write = false;
	reply.rdma_header.remote_rkey = req->rdma_header.remote_rkey;
	reply.rdma_header.remote_addr = req->rdma_header.remote_addr;
	reply.rdma_header.rw_size = res_size;

	reply.mr_id = req->mr_id;
	reply.remote_ws = req->remote_ws;
	reply.dma_addr_act = req->dma_addr_act;

	__ib_kmsg_send(req->header.from_nid,
						(struct pcn_kmsg_message*)&reply, sizeof(reply));
	return;
}


/*
 * RDMA WRITE:
 * send        ----->   irq (recv)
 *                      lock
 *             <=====   perform WRITE
 *                      unlock
 * irq (recv)  <-----   send
 *
 *
 * FaRM WRITE:
 * send        ----->   irq (recv)
 * poll                 lock
 *             <=====   perform WRITE
 *                      unlock
 * done					done
 */
static void __respond_rdma_write(
			pcn_kmsg_perf_rdma_t *req, void *res, u32 res_size)
{
#if !CONFIG_RDMA_POLL && !CONFIG_RDMA_NOTIFY && !CONFIG_FARM
	pcn_kmsg_perf_rdma_t reply;
#endif
#if CONFIG_RDMA_POLL || CONFIG_RDMA_NOTIFY
	char flush;
#endif
	struct ib_cb *cb = gcb[req->header.from_nid];
	int ret;
	u64 dma_addr;
	DECLARE_COMPLETION_ONSTACK(comp);
	uint32_t dma_len;
	struct ib_send_wr *bad_wr;
	struct ib_sge sgl;
	struct ib_rdma_wr rdma_wr = {
		.wr = {
			.opcode = IB_WR_RDMA_WRITE,
			.send_flags = IB_SEND_SIGNALED,
			.sg_list = &sgl,
			.num_sge = 1,
			.wr_id = (u64)&comp,
			.next = NULL,
		},
		.rkey = req->rdma_header.remote_rkey,
		.remote_addr = req->rdma_header.remote_addr,
	};
	u32 mr_id = __get_mr(req->header.from_nid, RDMA_MR);
	char *payload;

#if CONFIG_RDMA_NOTIFY
	DECLARE_COMPLETION_ONSTACK(comp2);
	struct ib_sge rdma_notify_sgl = {
		.addr = cb->local_addr[mr_id],
		.lkey = cb->local_key[mr_id],
		.length = RMDA_NOTIFY_PASS_DATA_SIZE,
	};
	struct ib_rdma_wr rdma_notify_send_wr = {
		.wr = {
			.opcode = IB_WR_RDMA_WRITE,
			.send_flags = IB_SEND_SIGNALED,
			.sg_list = &rdma_notify_sgl,
			.num_sge = 1,
			.wr_id = (u64)&comp2,
			.next = NULL,
		},
		.rkey = cb->remote_key,
		.remote_addr = cb->remote_addr + req->mr_id,
	};
#endif

#if CONFIG_RDMA_POLL
	payload = cb->rdma_poll_buffer[mr_id];
	dma_len = res_size + POLL_HEAD_AND_TAIL;

	*(u32 *)payload = res_size;					/* payload size (sizeof(u32)) */
	*(payload + sizeof(u32)) = POLL_IS_DATA;	/* poll head (1 byte) */
	memcpy(payload + POLL_HEAD, res, res_size);	/* payload (res_size) */
	payload[dma_len - 1] = POLL_IS_DATA;		/* poll tail (1 byte) */
#else
	payload = res;
	dma_len = res_size;
#endif
	dma_addr = dma_map_single(cb->pd->device->dma_device,
					  payload, dma_len, DMA_BIDIRECTIONAL);
	ret = dma_mapping_error(cb->pd->device->dma_device, dma_addr);
	BUG_ON(ret);

	sgl.addr = dma_addr;
	sgl.length = dma_len;
	sgl.lkey = __map_rdma_mr(cb, dma_addr, dma_len, mr_id, RDMA_MR);

	ret = ib_post_send(cb->qp, &rdma_wr.wr, &bad_wr);
#if CONFIG_RDMA_POLL
	flush = *(payload + dma_len - 1);	/* touch for flushing */
#endif
	selftest_wr_wq_inc(cb);
	BUG_ON(ret);

	/* Awaken by cq_event_handler */
	if (!try_wait_for_completion(&comp))
		wait_for_completion(&comp);

	dma_unmap_single(cb->pd->device->dma_device,
			dma_addr, dma_len, DMA_BIDIRECTIONAL);

#if CONFIG_RDMA_NOTIFY
	ret = ib_post_send(cb->qp, &rdma_notify_send_wr.wr, &bad_wr);
	flush = *cb->rdma_notify_buf_pass[mr_id];	/* touch for flushing */
	selftest_wr_wq_inc(cb);
	BUG_ON(ret);

	if (!try_wait_for_completion(&comp2))
		wait_for_completion(&comp2);
	/* No need to umap rdma_notify_WRITE polling bits */
#elif !CONFIG_RDMA_POLL && !CONFIG_RDMA_NOTIFY && !CONFIG_FARM
	reply.header.type = req->rdma_header.rmda_type_res;
	//reply.header.prio = PCN_KMSG_PRIO_NORMAL;

	/* RDMA W/R complete ACK */
	reply.header.is_rdma = true;
	reply.rdma_header.rdma_ack = true;
	reply.rdma_header.is_write = true;
	reply.rdma_header.remote_rkey = req->rdma_header.remote_rkey;
	reply.rdma_header.remote_addr = req->rdma_header.remote_addr;
	reply.rdma_header.rw_size = dma_len;

	reply.mr_id = req->mr_id;
	reply.remote_ws = req->remote_ws;
	reply.dma_addr_act = req->dma_addr_act;

	__ib_kmsg_send(req->header.from_nid, &reply, sizeof(reply));
#endif

	__put_mr(cb->conn_no, mr_id, RDMA_MR);
	return;
}

/* FARM implementations will never call this func */
static void __respond_rdma(pcn_kmsg_perf_rdma_t *res)
{
	struct ib_cb *cb = gcb[res->header.from_nid];

	__put_mr(res->header.from_nid, res->mr_id, RDMA_MR);
	dma_unmap_single(cb->pd->device->dma_device,
					res->dma_addr_act,
					res->rdma_header.rw_size, DMA_BIDIRECTIONAL);

	/* completed outside is fine by wait station */
	return;
}


/*
 * Caller has to free the msg by him/herself
 * paddr: ptr of pages you wanna perform on RDMA R/W passive side
 */
void respond_ib_rdma(pcn_kmsg_perf_rdma_t *req, void *res, u32 res_size)
{
	BUG_ON(!req->header.is_rdma);
	BUG_ON(res_size > MAX_RDMA_SIZE);

	if (!req->rdma_header.rdma_ack) {
		if (req->rdma_header.is_write)
			__respond_rdma_write(req, res, res_size);
		else
			__respond_rdma_read(req, res, res_size);
	} else {
		__respond_rdma(req);
	}
}


static void __process_recv_work(struct recv_work_t *w)
{
	pcn_kmsg_cbftn ftn;
	struct pcn_kmsg_message *msg = &w->msg;

	BUG_ON(msg->header.type < 0 || msg->header.type >= PCN_KMSG_TYPE_MAX);
	BUG_ON(msg->header.size < 0 || msg->header.size > PCN_KMSG_MAX_SIZE);

	ftn = pcn_kmsg_cbftns[msg->header.type];
	BUG_ON(!ftn);

#ifdef CONFIG_POPCORN_STAT
	account_pcn_message_recv(msg);
#endif
	ftn(&w->msg);
	return;
}

static void __cq_event_handler(struct ib_cq *cq, void *ctx)
{
	struct ib_cb *cb = ctx;
	int ret, err;
	struct ib_wc wc;	/* work complition->wr_id (work request ID) */

	BUG_ON(cb->cq != cq);
	if (atomic_read(&cb->state) == ERROR) {
		printk(KERN_ERR "< cq completion in ERROR state >\n");
		return;
	}

retry:
	while ((ret = ib_poll_cq(cb->cq, 1, &wc)) > 0) {
		if (wc.status) {
			if (wc.status == IB_WC_WR_FLUSH_ERR) {
				printk("< cq flushed >\n");
			} else {
				printk(KERN_ERR "< cq completion failed with "
					"wr_id %Lx status %d opcode %d vender_err %x >\n",
					wc.wr_id, wc.status, wc.opcode, wc.vendor_err);
				BUG_ON(wc.status);
				goto error;
			}
		}

		switch (wc.opcode) {
		case IB_WC_SEND:
			selftest_wr_wq_dec(gcb[cb->conn_no]);
			complete((struct completion *)wc.wr_id);
			break;

		case IB_WC_RECV:
			__process_recv_work((struct recv_work_t *)wc.wr_id);
			break;

		case IB_WC_RDMA_WRITE:
			selftest_wr_wq_dec(gcb[cb->conn_no]);
			complete((struct completion *)wc.wr_id);
			break;

		case IB_WC_RDMA_READ:
			selftest_wr_wq_dec(gcb[cb->conn_no]);
			complete((struct completion *)wc.wr_id);
			break;

		case IB_WC_LOCAL_INV:
			printk("IB_WC_LOCAL_INV:\n");
			break;

		case IB_WC_REG_MR:
			printk("IB_WC_REG_MR:\n");
			//complete((struct completion *)wc.wr_id);
			break;

		default:
			printk(KERN_ERR "< %s:%d Unexpected opcode %d, Shutting down >\n",
							__func__, __LINE__, wc.opcode);
			goto error;	/* TODO for rmmod */
			//wake_up_interruptible(&cb->sem);
			//ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
		}
	}
	err = ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP | IB_CQ_REPORT_MISSED_EVENTS);
	BUG_ON(err < 0);
	if (err > 0)
		goto retry;

	return;

error:
	atomic_set(&cb->state, ERROR);
	wake_up_interruptible(&cb->sem);
}


/*
 * Your request must be done by kmalloc()
 * You have to free the buf by yourself
 *
 * rw_size: size you wanna RDMA RW to perform
 *
 * READ/WRITE:
 * if R/W
 * [active lock]
 * send		 ----->   irq (recv)
 *					   |-passive lock R/W
 *					   |-perform R/W
 *					   |-passive unlock R/W
 * irq (recv)   <----- |-send
 *  |-active unlock
 *
 * FaRM WRITE: user gives last bit for polling
 * [active lock]
 * send		 ----->   irq (recv)
 * 						|-passive lock R/W
 * polling				|-perform WRITE
 *						|-passive unlock R/W
 * active unlock
 *
 * rdma_notify_WRITE:
 * [active lock]
 * send		 ----->   irq (recv)
 * 						|-passive lock R/W
 *						|-perform WRITE
 *						|-passive unlock R/W
 * polling				|- WRITE (signal)
 * active unlock
 */
void *request_ib_rdma(unsigned int dst, pcn_kmsg_perf_rdma_t *msg,
						  unsigned int msg_size, unsigned int rw_size)
{
	int ret;
	u32 mr_id;
	uint32_t rkey;
	u64 dma_addr;
	unsigned int dma_size;
	struct ib_cb *cb = gcb[dst];

	char *payload;
#if CONFIG_RDMA_NOTIFY || CONFIG_RDMA_POLL || CONFIG_FARM
	char *poll_tail_at;
#endif
#if CONFIG_RDMA_POLL
	char *dma_buffer;
	unsigned int remote_rw_size = 0;
	pcn_kmsg_perf_rdma_t *rp;
#endif

	BUG_ON(rw_size <= 0);

	msg->header.is_rdma = true;
	msg->header.from_nid = my_nid;
	msg->rdma_header.rdma_ack = false;
	msg->rdma_header.rw_size = rw_size;

#if CONFIG_RDMA_POLL
	BUG_ON((!msg->rdma_header.is_write) && (!msg->rdma_header.your_buf_ptr));
	if (msg->rdma_header.is_write) {
		dma_size = rw_size + POLL_HEAD_AND_TAIL;

		dma_buffer = kzalloc(dma_size, GFP_KERNEL);
		BUG_ON(!dma_buffer);
		payload = dma_buffer;
	} else {
		dma_size = rw_size;
		payload = msg->rdma_header.your_buf_ptr;
	}
#else
	BUG_ON(!msg->rdma_header.your_buf_ptr);
	payload = msg->rdma_header.your_buf_ptr;
	dma_size = rw_size;
#endif

	dma_addr = dma_map_single(cb->pd->device->dma_device,
					payload, dma_size, DMA_BIDIRECTIONAL);
	ret = dma_mapping_error(cb->pd->device->dma_device, dma_addr);
	BUG_ON(ret);

	mr_id = __get_mr(dst, RDMA_MR);
	rkey = __map_rdma_mr(cb, dma_addr, dma_size, mr_id, RDMA_MR);

	msg->rdma_header.remote_addr = dma_addr;
	msg->rdma_header.remote_rkey = rkey;

	if (msg->rdma_header.is_write) {
#if !CONFIG_FARM && !CONFIG_RDMA_POLL && !CONFIG_RDMA_NOTIFY
		/* free when it's done */
		msg->dma_addr_act = dma_addr;
		msg->mr_id = mr_id;
#elif CONFIG_RDMA_NOTIFY
		msg->mr_id = mr_id;
#endif
	} else {
		/* free when it's done */
		msg->dma_addr_act = dma_addr;
	}

#if CONFIG_RDMA_NOTIFY
	poll_tail_at = cb->rdma_notify_buf_act + mr_id;
	*poll_tail_at = POLL_IS_IDLE;
#elif CONFIG_FARM
	poll_tail_at = msg->rdma_header.your_buf_ptr + rw_size - 1;
	*poll_tail_at = POLL_IS_IDLE;
#elif CONFIG_RDMA_POLL
	*(dma_buffer + POLL_HEAD - 1) = POLL_IS_IDLE;
#endif

	__ib_kmsg_send(dst, (struct pcn_kmsg_message *)msg, msg_size);

	if (!msg->rdma_header.is_write) return NULL;

#if CONFIG_RDMA_NOTIFY
	while (*poll_tail_at == POLL_IS_IDLE)
		io_schedule();

	__put_mr(dst, mr_id, RDMA_MR);
	dma_unmap_single(cb->pd->device->dma_device,
					dma_addr, dma_size, DMA_BIDIRECTIONAL);
#elif CONFIG_RDMA_POLL
	/* polling - not done:0  */
	while (*(dma_buffer + sizeof(u32)) == POLL_IS_IDLE)
		io_schedule();

	/* remote write size */
	remote_rw_size  = *(u32 *)dma_buffer;

	/* poll at tail */
	poll_tail_at = dma_buffer + remote_rw_size + POLL_HEAD_AND_TAIL - 1;

	while (*poll_tail_at == POLL_IS_IDLE)
		io_schedule();

	__put_mr(dst, mr_id, RDMA_MR);
	dma_unmap_single(cb->pd->device->dma_device,
			dma_addr, dma_size, DMA_BIDIRECTIONAL);

	/* pointer for usr to free */
	rp = (pcn_kmsg_perf_rdma_t *)(dma_buffer + POLL_HEAD);
	rp->private = dma_buffer;

	/* for dsm */
	rp->header.is_rdma = true;
	rp->rdma_header.rdma_ack = true;
	rp->rdma_header.is_write = true;
	//rp->rdma_header.rw_size = remote_rw_size;

#ifdef CONFIG_POPCORN_STAT
	account_pcn_message_recv((struct pcn_kmsg_message *)rp);
#endif
	return dma_buffer + POLL_HEAD;

#elif CONFIG_FARM
	while (*poll_tail_at == POLL_IS_IDLE)
		io_schedule();

	__put_mr(dst, mr_id, RDMA_MR);
	dma_unmap_single(cb->pd->device->dma_device, dma_addr,
					rw_size, DMA_BIDIRECTIONAL);
#else
	/* handle rdma response handler will complete and free dma_addr */
#endif
	return NULL;
}

int ib_kmsg_send(unsigned int dst,
				  struct pcn_kmsg_message *msg,
				  unsigned int msg_size)
{
	msg->header.is_rdma = false;
	return __ib_kmsg_send(dst, msg, msg_size);
}


static void __putback_recv_wr(struct pcn_kmsg_message *msg)
{
	if (msg->header.from_nid == my_nid) {
		kfree(msg);
	} else {
		struct ib_recv_wr *bad_wr;
		struct recv_work_t *rws = container_of(msg, struct recv_work_t, msg);
		selftest_recv_pool(rws, msg);
		ib_post_recv(gcb[msg->header.from_nid]->qp, &rws->recv_wr, &bad_wr);
	}
}

static void ib_kmsg_free_ftn(struct pcn_kmsg_message *msg)
{
#ifdef CONFIG_POPCORN_KMSG_IB_RDMA
	if (msg->header.is_rdma) {
		pcn_kmsg_rdma_t *msg_rdma = (pcn_kmsg_rdma_t *)msg;
		if (msg_rdma->rdma_header.rdma_ack && msg_rdma->rdma_header.is_write) {
#if CONFIG_RDMA_POLL
			kfree(msg_rdma->private);
#elif !CONFIG_RDMA_POLL && !CONFIG_RDMA_NOTIFY && !CONFIG_FARM
			__putback_recv_wr(msg);// this is a ack msg
#else
			kfree(msg);
#endif
		} else if (!msg_rdma->rdma_header.rdma_ack) {
			__putback_recv_wr(msg); //recv?  (is this a req msg? yes I guess)
		} else {
			kfree(msg);
		}
	}
	else
#endif
	{
		__putback_recv_wr(msg);
	}
}

#if CONFIG_RDMA_NOTIFY
static void __exchange_rdma_keys(int dst)
{
	u32 rkey;
	struct ib_cb *cb = gcb[dst];
	DECLARE_COMPLETION_ONSTACK(comp);
	struct rdma_notify_init_req_t req = {
		.header = {
			.type = PCN_KMSG_TYPE_RDMA_KEY_EXCHANGE_REQUEST,
		},
		.comp = &comp,
	};

	rkey = __map_rdma_mr(cb, cb->rdma_notify_dma_addr_act,
			RDMA_NOTIFY_ACT_DATA_SIZE, 0, RDMA_FARM_NOTIFY_RKEY_ACT);
	req.remote_addr = cb->rdma_notify_dma_addr_act;
	req.remote_key = rkey;

	ib_kmsg_send(dst, (struct pcn_kmsg_message *)&req, sizeof(req));
	wait_for_completion(&comp);
}

static void handle_rdma_key_exchange_request(struct pcn_kmsg_message *msg)
{
	int i;
	struct rdma_notify_init_req_t *req = (struct rdma_notify_init_req_t*)msg;
	struct rdma_notify_init_res_t res = {
		.header = {
			.type = PCN_KMSG_TYPE_RDMA_KEY_EXCHANGE_RESPONSE,
		},
		.comp = req->comp,
	};
	struct ib_cb *cb = gcb[req->header.from_nid];

	/* remote info: */
	cb->remote_key = req->remote_key;
	cb->remote_addr = req->remote_addr;

	/* local info: */
	//cb->local_rdma_notify_llen = RMDA_NOTIFY_PASS_DATA_SIZE;
	for (i = 0; i < MR_POOL_SIZE; i++) {
		cb->local_addr[i] = cb->rdma_notify_dma_addr_pass[i];
		cb->local_key[i] = __map_rdma_mr(cb, cb->rdma_notify_dma_addr_pass[i],
				RMDA_NOTIFY_PASS_DATA_SIZE, i, RDMA_FARM_NOTIFY_RKEY_PASS);
	}

	ib_kmsg_send(req->header.from_nid,
			(struct pcn_kmsg_message *)&res, sizeof(res));
	pcn_kmsg_free_msg(req);
}


static void handle_rdma_key_exchange_response(struct pcn_kmsg_message *msg)
{
	struct rdma_notify_init_res_t *res = (struct rdma_notify_init_res_t*)msg;
	complete(res->comp);
	pcn_kmsg_free_msg(res);
}
#endif


#if CONFIG_RDMA_POLL
static void __init_rdma_poll(void)
{
	int index;
	for (index = 0; index < MAX_NUM_NODES; index++) {
		/* passive RW buffer */
		struct ib_cb *cb = gcb[index];
		int i, j;
		for (i = 0; i < MR_POOL_SIZE; i++) {
			cb->rdma_poll_buffer[i] = kzalloc(MAX_RDMA_SIZE, GFP_KERNEL);
			BUG_ON(!cb->rdma_poll_buffer[i]);
		}

		for (i = 0; i < RDMA_MR_TYPES; i++) {
			for (j = 0; j < MR_POOL_SIZE; j++)
				clear_bit(j, mr_pool[index][i]);

			spin_lock_init(&mr_pool_lock[index][i]);
		}
	}
}
#endif

int __init initialize(void)
{
	int i, err;
	pcn_kmsg_layer_type = PCN_KMSG_LAYER_TYPE_IB;

	printk("- Popcorn Messaging Layer IB Initialization Start -\n");
	/* Establish node numbers according to its IP */
	if (!identify_myself()) {
		return -EINVAL;
	}

#if CONFIG_RDMA_NOTIFY
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_RDMA_KEY_EXCHANGE_REQUEST,
				(pcn_kmsg_cbftn)handle_rdma_key_exchange_request);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_RDMA_KEY_EXCHANGE_RESPONSE,
				(pcn_kmsg_cbftn)handle_rdma_key_exchange_response);
#endif

	pcn_kmsg_send_ftn = (send_ftn)ib_kmsg_send;
	pcn_kmsg_request_rdma_ftn = (request_rdma_ftn)request_ib_rdma;
	pcn_kmsg_respond_rdma_ftn = (respond_rdma_ftn)respond_ib_rdma;
	pcn_kmsg_free_ftn = (free_ftn)ib_kmsg_free_ftn;

	/* Initilaize the IB: Each node has a connection table like tihs
	 * -------------------------------------------------------------------
	 * | connect | (many)... | my_nid(one) | accept | accept | (many)... |
	 * -------------------------------------------------------------------
	 * my_nid:  no need to talk to itself
	 * connect: connecting to existing nodes
	 * accept:  waiting for the connection requests from later nodes
	 */
	for (i = 0; i < MAX_NUM_NODES; i++) {
		/* Create global Control Block context for each connection */
		gcb[i] = kzalloc(sizeof(struct ib_cb), GFP_KERNEL);
		BUG_ON(!gcb[i]);

		/* Settup node number */
		gcb[i]->conn_no = i;
		gcb[i]->key = i;
		gcb[i]->server = -1;
		init_waitqueue_head(&gcb[i]->sem);

		/* Init common parameters */
		atomic_set(&gcb[i]->state, IDLE);
#if CHECK_WQ_WR
		gcb[i]->WQ_WR_cnt.counter = 0;
#endif
		/* set up IPv4 address */
		in4_pton(ip_addresses[i], -1, gcb[i]->addr, -1, NULL);

		/* register event handler */
		gcb[i]->cm_id = rdma_create_id(&init_net,
				__cm_event_handler, gcb[i], RDMA_PS_TCP, IB_QPT_RC);
		if (IS_ERR(gcb[i]->cm_id)) {
			err = PTR_ERR(gcb[i]->cm_id);
			printk(KERN_ERR "rdma_create_id error %d\n", err);
			goto out;
		}
	}
#if CONFIG_RDMA_POLL
	__init_rdma_poll();
#endif

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
	for (i = 0; i < my_nid; i++) {
		/* [connect] | my_nid | accept */
		gcb[i]->server = 0;

		/* server/client dependant init */
		if (ib_run_client(gcb[i])) {
			printk("WRONG!!\n");
			rdma_disconnect(gcb[i]->cm_id);
			return err;
		}

		set_popcorn_node_online(i, true);
		printk("Node %d is ready (client)\n", i);
	}

	/* case 2: <my_nid: connect | =my_nid | [>=my_nid: accept] */
	ib_run_server(gcb[my_nid]);

	for (i = 0; i < MAX_NUM_NODES; i++) {
		if (i == my_nid) continue;
		while (!get_popcorn_node_online(i)) {
			msleep(10);
		}
		atomic_set(&gcb[i]->state, IDLE);
		notify_my_node_info(i);
#if CONFIG_RDMA_NOTIFY
		__exchange_rdma_keys(i);
#endif
	}

	printk("------------------------------------------\n");
	printk("- Popcorn Messaging Layer IB Initialized -\n");
	printk("------------------------------------------\n");

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
void __exit unload(void)
{
	int i, j;
	printk("TODO: Stop kernel threads\n");

	printk("Release general\n");
	for (i = 0; i < MAX_NUM_NODES; i++) {

#if CONFIG_RDMA_POLL
		for (j = 0; j < MR_POOL_SIZE; j++) {
			if (gcb[i]->rdma_poll_buffer[j])
				kfree(gcb[i]->rdma_poll_buffer[j]);
		}
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
			if (gcb[i]->peer_cm_id)
				if (rdma_disconnect(gcb[i]->peer_cm_id))
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
			for (j = 0; j < MR_POOL_SIZE; j++)
				ib_free_buffers(cb, j);
			ib_free_qp(cb);
		} else {
			/* server */
			for (j = 0; j < MR_POOL_SIZE; j++)
				ib_free_buffers(cb, j);
			ib_free_qp(cb);
			rdma_destroy_id(cb->peer_cm_id);
		}
	}

#if CONFIG_RDMA_NOTIFY
	printk("Release RDMA relavant\n");
	for (i = 0; i < MAX_NUM_NODES; i++) {
		kfree(gcb[i]->rdma_notify_buf_act);
		kfree(gcb[i]->rdma_notify_buf_pass);
		for (j = 0; j < MR_POOL_SIZE; j++) {
			dma_unmap_single(gcb[i]->pd->device->dma_device,
							gcb[i]->rdma_notify_dma_addr_pass[j],
							RMDA_NOTIFY_PASS_DATA_SIZE, DMA_BIDIRECTIONAL);
		}
		dma_unmap_single(gcb[i]->pd->device->dma_device,
						gcb[i]->rdma_notify_dma_addr_act,
						RDMA_NOTIFY_ACT_DATA_SIZE, DMA_BIDIRECTIONAL);
	}
#endif

	printk("Release cb context\n");
	for (i = 0; i < MAX_NUM_NODES; i++) {
		kfree(gcb[i]);
	}

	printk("Successfully unloaded module!\n");
}

module_init(initialize);
module_exit(unload);
MODULE_LICENSE("GPL");
