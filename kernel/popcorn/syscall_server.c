#include <popcorn/pcn_kmsg.h>
#include <popcorn/types.h>
#include "syscall_server.h"
#include "types.h"
#include "wait_station.h"
#include <linux/socket.h>
#include <linux/unistd.h>
#include <linux/eventpoll.h>
#include <linux/file.h>

/* Syscall Definitions are put here*/

/* Define redirection functions*/

/* Socket related */
DEFINE_SYSCALL_REDIRECT(socket, PCN_SYSCALL_SOCKET_CREATE, int, family, int,
			type, int, protocol);
DEFINE_SYSCALL_REDIRECT(setsockopt, PCN_SYSCALL_SETSOCKOPT, int, fd,
			int, level, int, optname, char __user*, optval, int,
			optlen);
DEFINE_SYSCALL_REDIRECT(bind, PCN_SYSCALL_BIND,int, fd, struct sockaddr __user*,
			umyaddr, int, addrlen);
DEFINE_SYSCALL_REDIRECT(listen, PCN_SYSCALL_LISTEN, int, fd, int,
			backlog);
DEFINE_SYSCALL_REDIRECT(accept4, PCN_SYSCALL_ACCEPT4, int, fd, struct
			sockaddr __user*, upper_sockaddr, int __user*,
			upper_addrlen, int, flag);
DEFINE_SYSCALL_REDIRECT(shutdown, PCN_SYSCALL_SHUTDOWN, int, fd, int, how);
DEFINE_SYSCALL_REDIRECT(recvfrom, PCN_SYSCALL_RECVFROM, int, fd, void __user *,
			ubuf, size_t, size, unsigned int, flags,
			struct sockaddr __user *, addr, int __user *, addr_len);

/* Epoll related */
DEFINE_SYSCALL_REDIRECT(epoll_create1, PCN_SYSCALL_EPOLL_CREATE1, int, flags);
DEFINE_SYSCALL_REDIRECT(epoll_wait, PCN_SYSCALL_EPOLL_WAIT, int, epfd,
			struct epoll_event __user *,
			events, int, maxevents, int, timeout);
DEFINE_SYSCALL_REDIRECT(epoll_ctl, PCN_SYSCALL_EPOLL_CTL, int, epfd,
			int, op, int, fd, struct epoll_event __user *,
			event);


/* General fs/driver read/write/open/close calls */
DEFINE_SYSCALL_REDIRECT(read, PCN_SYSCALL_READ, unsigned int, fd, char __user*,
			buf, size_t, count);
DEFINE_SYSCALL_REDIRECT(write, PCN_SYSCALL_WRITE, unsigned int, fd, const char
			__user *, buf, size_t, count);
DEFINE_SYSCALL_REDIRECT(open, PCN_SYSCALL_OPEN, const char __user *, filename,
			int, flags, umode_t, mode);
DEFINE_SYSCALL_REDIRECT(close, PCN_SYSCALL_CLOSE, unsigned int, fd);
DEFINE_SYSCALL_REDIRECT(ioctl, PCN_SYSCALL_IOCTL, unsigned int, fd,
			unsigned int, cmd, unsigned long, arg);
DEFINE_SYSCALL_REDIRECT(writev, PCN_SYSCALL_WRITEV, unsigned long,
			fd, const struct iovec __user *, vec,
			unsigned long, vlen);
DEFINE_SYSCALL_REDIRECT(fstat, PCN_SYSCALL_FSTAT, unsigned int, fd,
			struct __old_kernel_stat __user *, statbuf);
/**
 * Syscalls needed in the kernel
 * */
extern int sys_socket(int family, int type, int protocol);
extern int sys_bind(int fd, struct sockaddr __user *umyaddr, int addrlen);
extern int sys_listen(int fd, int backlog);
extern int sys_accept4(int fd, struct sockaddr __user *upeer_sockaddr,
		     int __user *upeer_addrlen, int flag);
extern int sys_setsockopt(int fd, int level, int optname, char __user *optval,
			  int optlen);
extern long sys_recvfrom(int, void __user *, size_t, unsigned,
				struct sockaddr __user *, int __user *);
extern long sys_shutdown(int, int);
extern long sys_epoll_create1(int flags);
extern long sys_epoll_ctl(int epfd, int op, int fd,
				struct epoll_event __user *event);
extern long sys_epoll_wait(int epfd, struct epoll_event __user *events,
				int maxevents, int timeout);
extern long sys_read(unsigned int fd, char __user *buf, size_t count);
extern long sys_write(unsigned int fd, const char __user *buf, size_t count);
extern long sys_open(const char __user *filename, int flags, umode_t mode);
extern long sys_close(unsigned int fd);
extern long sys_ioctl(unsigned int fd, unsigned int cmd, unsigned long arg);
extern long sys_writev(unsigned long fd,
			   const struct iovec __user *vec,
			   unsigned long vlen);
extern long sys_fstat(unsigned int fd,
			struct __old_kernel_stat __user *statbuf);

int process_remote_syscall(struct pcn_kmsg_message *msg)
{
	int retval = 0;
	syscall_fwd_t *req = (syscall_fwd_t *)msg;
	syscall_rep_t *rep = kmalloc(sizeof(*rep), GFP_KERNEL);

	/*Call the original system call and pass in delivered params. Due to the
	 * way the macro is set up on the remote side, params are filled
	 * backwards. 3 params, the request will go param2 = 1st argument;
	 * param1 = 2nd argument, param0 = 3rd argument. For 2 params, param1 =
	 * 1st argument, param0 = 2nd argument*/
	switch(req->call_type) {
	case PCN_SYSCALL_SOCKET_CREATE:
	/*int family; int type; int protocol*/
		retval = sys_socket((int)req->param2, (int)req->param1,
				    (int)req->param0);
		break;
	case PCN_SYSCALL_SETSOCKOPT:
		retval = sys_setsockopt((int)req->param4, (int)req->param3,
					(int)req->param2,
					(char __user*)req->param1,
					(int)req->param0);
		break;
	case PCN_SYSCALL_BIND:
        retval = sys_bind((int)req->param2, (struct sockaddr __user*)
				  req->param1, (int)req->param0);
		break;
	case PCN_SYSCALL_LISTEN:
		retval = sys_listen((int)req->param1, (int)req->param0);
		break;
	case PCN_SYSCALL_ACCEPT4:
		retval = sys_accept4((int)req->param3,
				     (struct sockaddr __user*)req->param2,
				     (int __user*)req->param1,
				     (int)req->param0);
		break;
	case PCN_SYSCALL_SHUTDOWN:
		retval = sys_shutdown((int)req->param1, (int)req->param0);
		break;
	/* Event poll related syscalls */
	case PCN_SYSCALL_EPOLL_CREATE1:
		retval = sys_epoll_create1((int)req->param0);
		break;
	case PCN_SYSCALL_EPOLL_WAIT:
		printk(KERN_INFO "epoll_wait called on host\n");
		retval = sys_epoll_wait((int)req->param3,
				(struct epoll_event __user *)req->param2,
				(int)req->param1, (int)req->param0);
		printk(KERN_INFO "epoll_wait returned: %d\n", retval);
		break;
	case PCN_SYSCALL_EPOLL_CTL:
		retval = sys_epoll_ctl((int)req->param3, (int)req->param2,
				       (int)req->param1, (struct epoll_event
				       __user *)req->param0);
		break;

	case PCN_SYSCALL_READ:
		retval = sys_read((unsigned int)req->param2,
				  (char __user *)req->param1,
				  (size_t) req->param0);
		break;
	case PCN_SYSCALL_WRITE:
		retval = sys_write((unsigned int)req->param2,
				  (const char __user *)req->param1,
				  (size_t) req->param0);
		break;
	case PCN_SYSCALL_OPEN:
		retval = sys_open((const char __user *)req->param2,
				  (int)req->param1,
				  (umode_t)req->param0);
		break;
	case PCN_SYSCALL_CLOSE:
		retval = sys_close((unsigned int)req->param0);
		break;
	case PCN_SYSCALL_IOCTL:
		retval = sys_ioctl((unsigned int)req->param2,
				   (unsigned int)req->param1,
				   (unsigned long)req->param0);
		break;
	case PCN_SYSCALL_WRITEV:
		retval = sys_writev((unsigned long)req->param2,
				   (const struct iovec __user *)req->param1,
				   (unsigned long)req->param0);
		break;
	case PCN_SYSCALL_RECVFROM:
		retval = sys_recvfrom((int)req->param5,
				   (void __user *)req->param4,
				   (size_t)req->param3,
				   (unsigned)req->param2,
				   (struct sockaddr __user *)req->param1,
				   (int __user *)req->param0);
		break;
	case PCN_SYSCALL_FSTAT:
		retval = sys_fstat((unsigned int)req->param1,
				   (struct __old_kernel_stat __user *)req->param0);
		break;
	default:
		retval = -EINVAL;
	}
	rep->origin_pid = current->origin_pid;
	rep->remote_ws = req->remote_ws;
	rep->ret = retval;
	pcn_kmsg_send(PCN_KMSG_TYPE_SYSCALL_REP, current->remote_nid, rep,
		      sizeof(*rep));
	kfree(rep);
	return retval;
}

static int handle_syscall_reply(struct pcn_kmsg_message *msg)
{
	syscall_rep_t *rep = (syscall_rep_t *)msg;
	struct wait_station *ws = wait_station(rep->remote_ws);

	ws->private = rep;
	complete(&ws->pendings);
	return 0;
}

DEFINE_KMSG_RW_HANDLER(syscall_fwd, syscall_fwd_t, origin_pid);

int __init syscall_server_init(void)
{
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_SYSCALL_FWD,
			      syscall_fwd);
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_SYSCALL_REP,
			      syscall_reply);
	return 0;
}
