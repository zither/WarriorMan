#include "wm_socket.h"
#include "socket.h"
#include "loop.h"

static bool bufferIsFull(wmSocket *socket);
static void checkBufferWillFull(wmSocket *socket);
static bool event_wait(wmSocket *socket, int event);
static int total_num = 0;
static socklen_t addr_len = sizeof(struct sockaddr);

/**
 * 设置socket的各种错误
 */
static inline void set_err(wmSocket *socket, int e) {
	socket->errCode = errno = e;
	socket->errMsg = e ? wmCode_str(e) : "";
}

/**
 * 获取socket执行某个类型的协程
 */
static inline wmCoroutine* get_bound_co(wmSocket *socket, const enum wmEvent_type event) {
	if (event & WM_EVENT_READ) {
		if (socket->read_co) {
			return socket->read_co;
		}
	}
	if (event & WM_EVENT_WRITE) {
		if (socket->write_co) {
			return socket->write_co;
		}
	}
	return NULL;
}

/**
 * 如果读写某个协程正在被使用，不允许再被使用s
 */
static inline void check_bound_co(wmSocket *socket, const enum wmEvent_type event) {
	wmCoroutine *co = get_bound_co(socket, event);
	if (co) {
		wmCoroutine *curr_co = wmCoroutine_get_current();
		wmError("Socket#%d has already been bound to another coroutine#%ld,%s of the same socket in coroutine#%ld at the same time is not allowed", socket->fd,
			co->cid,
			(event == WM_EVENT_READ ?
				"reading" :
				(event == WM_EVENT_WRITE ? "writing" : (socket->read_co && socket->write_co ? "reading or writing" : (socket->read_co ? "reading" : "writing")))),
			curr_co->cid);
	}
}

/**
 * 是否运行执行
 */
static inline bool is_available(wmSocket *socket, const enum wmEvent_type event) {
	if (event != WM_EVENT_NULL) {
		check_bound_co(socket, event);
	}
	if (socket->closed) {
		set_err(socket, ECONNRESET);
		return false;
	}
	return true;
}

/**
 * 检查errno
 */
static inline int check_error(int err) {
	switch (err) {
	case EFAULT:
		abort();
		return WM_SOCKET_ERROR;
	case EBADF:
	case ECONNRESET:
#ifdef __CYGWIN__
		case ECONNABORTED:
#endif
	case EPIPE:
	case ENOTCONN:
	case ETIMEDOUT:
	case ECONNREFUSED:
	case ENETDOWN:
	case ENETUNREACH:
	case EHOSTDOWN:
	case EHOSTUNREACH:
		return WM_SOCKET_CLOSE;
	case EAGAIN:
#ifdef HAVE_KQUEUE
		case ENOBUFS:
#endif
	case 0:
		return WM_SOCKET_SUCCESS;
	default:
		return WM_SOCKET_ERROR;
	}
}

/**
 * 读超时处理
 */
void timer_read_callback(void *_socket) {
	wmSocket *socket = (wmSocket*) _socket;
	set_err(socket, ETIMEDOUT);
	socket->read_timer = NULL;
	loop_callback_func_t fn = wmWorkerLoop_get_handler(EPOLLIN, socket->loop_type);
	fn(socket, EPOLLIN);
}

/**
 * 写超时处理
 */
void timer_write_callback(void *_socket) {
	wmSocket *socket = (wmSocket*) _socket;
	socket->write_timer = NULL;
	loop_callback_func_t fn = wmWorkerLoop_get_handler(EPOLLOUT, socket->loop_type);
	fn(socket, EPOLLOUT);
}

/**
 * 添加一个写定时器
 */
void timer_add(wmSocket *socket, int event, uint32_t ticks) {
	if (event == WM_EVENT_READ) {
		if (socket->read_timer) {
			return;
		}
		//添加定时器,1秒之后回调timer_callback接口
		socket->read_timer = wmTimerWheel_add_quick(&WorkerG.timer, timer_read_callback, (void*) socket, ticks);
	} else if (event == WM_EVENT_WRITE) {
		if (socket->write_timer) {
			return;
		}
		//添加定时器,1秒之后回调timer_callback接口
		socket->write_timer = wmTimerWheel_add_quick(&WorkerG.timer, timer_write_callback, (void*) socket, ticks);
	} else {
		abort();
	}
}

/**
 * 判断是否调用了定时器，并且相应处理
 * 如果调用了定时器，返回true
 */
void timer_del(wmSocket *socket, int event) {
	if (event == WM_EVENT_READ) {
		if (socket->read_timer) { //如果没使用相应定时器，那么删除
			wmTimerWheel_del(socket->read_timer); //没触发超时的话，删除定时器节点
			socket->read_timer = NULL;
		}
	} else if (event & WM_EVENT_WRITE) {
		if (socket->write_timer) { //如果没使用相应定时器，那么删除
			wmTimerWheel_del(socket->write_timer); //没触发超时的话，删除定时器节点
			socket->write_timer = NULL;
		}
	} else {
		abort();
	}
}

/**
 * 检查timer是否使用过
 */
bool timer_used(wmSocket *socket, int event) {
	if (event == WM_EVENT_READ) {
		if (!socket->read_timer) { //如果使用了
			errno = ETIMEDOUT;
			return true;
		}
		return false;
	} else if (event & WM_EVENT_WRITE) {
		if (!socket->write_timer) { //如果使用了
			errno = ETIMEDOUT;
			return true;
		}
		return false;
	} else {
		abort();
	}
	return false;
}

/**
 * 创建一个socket对象
 */
wmSocket* wmSocket_create(int transport, int loop_type) {
	int fd = -1;
	switch (transport) {
	case WM_SOCK_TCP:
		fd = wm_socket_create(AF_INET, SOCK_STREAM, 0);
		break;
	case WM_SOCK_UDP:
		fd = wm_socket_create(AF_INET, SOCK_DGRAM, 0);
		break;
	default:
		return NULL;
	}
	if (fd < 0) {
		return NULL;
	}
	return wmSocket_pack(fd, transport, loop_type);
}

/**
 * 包装一个socket对象
 */
wmSocket* wmSocket_pack(int fd, int transport, int loop_type) {
	wmSocket *socket = (wmSocket*) wm_malloc(sizeof(wmSocket));
	socket->fd = fd;

	socket->write_buffer = NULL;
	socket->closed = false;
	socket->maxSendBufferSize = 0; //应用层发送缓冲区
	socket->loop_type = loop_type;
	socket->transport = transport;
	socket->removed = false;

	socket->read_co = NULL;
	socket->write_co = NULL;
	socket->read_timer = NULL;
	socket->write_timer = NULL;
	socket->read_timeout = -1; //这个socket读操作的超时时间

	socket->connect_host = NULL;
	socket->connect_port = 0;

	socket->onBufferWillFull = NULL;
	socket->events = WM_EVENT_NULL;
	socket->errCode = 0; //默认没有错误
	socket->errMsg = NULL;
	socket->shutdown_read = false;
	socket->shutdown_write = false;
	socket->udp_addr = NULL;
	socket->remoteIp = NULL;
	if (transport == WM_SOCK_TCP) {
		wm_socket_set_nonblock(socket->fd);
	}
	total_num++;
	return socket;
}

/**
 * 等待连接
 */
wmSocket* wmSocket_accept(wmSocket *socket, int new_socket_loop_type, uint32_t timeout) {
	if (!is_available(socket, WM_EVENT_READ)) {
		return NULL;
	}
	int connfd;
	struct sockaddr_in addr;
	while (!socket->closed) {
		do {
			connfd = wm_socket_accept(socket->fd, &addr, &addr_len);
		} while (connfd < 0 && errno == EINTR);

		if (connfd < 0) {
			//添加一个读定时器,如果读成功了，就不加了
			timer_add(socket, WM_EVENT_READ, timeout);
			if (errno == EAGAIN && event_wait(socket, WM_EVENT_READ) && !timer_used(socket, WM_EVENT_READ)) {
				continue;
			}
			set_err(socket, errno);
			wmWarn("wmSocket_accept fail. %s", socket->errMsg);
			timer_del(socket, WM_EVENT_READ);
			return NULL;
		}
		set_err(socket, 0);
		timer_del(socket, WM_EVENT_READ);
		wmSocket *socket2 = wmSocket_pack(connfd, socket->transport, new_socket_loop_type); //将得到的fd，包装成socket结构体
		//设置客户端IP和端口
		char *client_ip = inet_ntoa(addr.sin_addr);
		socket2->remoteIp = wmString_dup(client_ip, strlen(client_ip));
		socket2->remotePort = ntohs(addr.sin_port);
		return socket2;
	}
	set_err(socket, WM_ERROR_SESSION_CLOSED);
	timer_del(socket, WM_EVENT_READ);
	return NULL;
}

/**
 * 主动连接
 */
bool wmSocket_connect(wmSocket *socket, char *_host, int _port, uint32_t timeout) {
	if (!is_available(socket, WM_EVENT_WRITE) || !is_available(socket, WM_EVENT_READ)) {
		return false;
	}

	int retval;
	if (!socket->closed) {
		do {
			retval = wm_socket_connect(socket->fd, _host, _port);
		} while (retval < 0 && errno == EINTR);

		if (retval < 0) {
			if (errno != EINPROGRESS) {
				set_err(socket, errno);
				wmWarn("wmSocket_connect fail. host=%s port=%d errno=%d", _host, _port, errno);
				return false;
			}
			//添加一个写定时器
			timer_add(socket, WM_EVENT_WRITE, timeout);
			if (!event_wait(socket, WM_EVENT_WRITE) || timer_used(socket, WM_EVENT_WRITE)) {
				set_err(socket, errno);
				return false;
			}

			//这里需要使用epoll监听可写事件，然后还需要有一个超时设置，
			socklen_t len = sizeof(socket->errCode);
			if (getsockopt(socket->fd, SOL_SOCKET, SO_ERROR, &socket->errCode, &len) < 0 || socket->errCode != 0) {
				set_err(socket, socket->errCode);
				timer_del(socket, WM_EVENT_WRITE);
				return false;
			}
		}
		socket->connect_host = _host;
		socket->connect_port = _port;
		set_err(socket, 0);
		timer_del(socket, WM_EVENT_WRITE);
		return retval;
	}
	set_err(socket, WM_ERROR_SESSION_CLOSED);
	return false;
}

/**
 * 偷看一下之前读取的数据
 * 暂时只有runtime用
 */
ssize_t wmSocket_peek(wmSocket *socket, void *__buf, size_t __n) {
	ssize_t retval;
	int __flags = MSG_PEEK;
	do {
		retval = recv(socket->fd, __buf, __n, __flags);
	} while (retval < 0 && errno == EINTR);

	wmTrace("peek %ld/%ld bytes, errno=%d", retval, __n, errno);
	set_err(socket, retval < 0 ? errno : 0);
	return retval;
}

/**
 * udp读数据
 */
ssize_t wmSocket_recv(wmSocket *server, wmSocket *socket, void *__buf, size_t __n, uint32_t timeout) {
	socket->udp_addr = wm_malloc(sizeof(struct sockaddr_in));
	ssize_t st = wmSocket_recvfrom(server, __buf, __n, (struct sockaddr*) socket->udp_addr, &addr_len, timeout);
	//设置客户端IP和端口
	char *client_ip = inet_ntoa(socket->udp_addr->sin_addr);
	socket->remoteIp = wmString_dup(client_ip, strlen(client_ip));
	socket->remotePort = ntohs(socket->udp_addr->sin_port);
	return st;
}

/**
 * udp读数据
 * 暂时只有runtime用
 */
ssize_t wmSocket_recvfrom(wmSocket *socket, void *__buf, size_t __n, struct sockaddr *_addr, socklen_t *_socklen, uint32_t timeout) {
	if (!is_available(socket, WM_EVENT_READ)) {
		return WM_SOCKET_CLOSE;
	}
	ssize_t retval;
	while (!socket->closed) {
		do {
			retval = recvfrom(socket->fd, __buf, __n, 0, _addr, _socklen);
		} while (retval < 0 && errno == EINTR);
		//正常返回
		if (retval >= 0) {

			//设置客户端IP和端口
			if (socket->remoteIp) {
				wmString_free(socket->remoteIp);
			}
			struct sockaddr_in *addr2 = (struct sockaddr_in*) _addr;
			char *client_ip = inet_ntoa(addr2->sin_addr);
			socket->remoteIp = wmString_dup(client_ip, strlen(client_ip));
			socket->remotePort = ntohs(addr2->sin_port);

			set_err(socket, 0);
			timer_del(socket, WM_EVENT_READ);
			return retval;
		}
		//添加一个读定时器,如果读成功了，就不加了
		timer_add(socket, WM_EVENT_READ, timeout);
		if (errno == EAGAIN && event_wait(socket, WM_EVENT_READ) && !timer_used(socket, WM_EVENT_READ)) {
			continue;
		}
		set_err(socket, errno);
		timer_del(socket, WM_EVENT_READ);
		return WM_SOCKET_ERROR;
	}
	set_err(socket, WM_ERROR_SESSION_CLOSED);
	timer_del(socket, WM_EVENT_READ);
	return WM_SOCKET_CLOSE;
}

/**
 * 读数据
 */
int wmSocket_read(wmSocket *socket, char *buf, int len, uint32_t timeout) {
	if (!is_available(socket, WM_EVENT_READ)) {
		return WM_SOCKET_CLOSE;
	}
	int ret;
	while (!socket->closed) {
		do {
			ret = wm_socket_recv(socket->fd, buf, len, 0);
		} while (ret < 0 && errno == EINTR);

		//正常返回
		if (ret > 0) {
			timer_del(socket, WM_EVENT_READ);
			return ret;
		}
		//连接关闭
		if (ret == 0) {
			socket->closed = true;
			set_err(socket, WM_ERROR_SESSION_CLOSED_BY_CLIENT);
			timer_del(socket, WM_EVENT_READ);
			return WM_SOCKET_CLOSE;
		}
		//添加一个读定时器,如果读成功了，就不加了
		timer_add(socket, WM_EVENT_READ, timeout);
		if (errno == EAGAIN && event_wait(socket, WM_EVENT_READ) && !timer_used(socket, WM_EVENT_READ)) {
			continue;
		}
		set_err(socket, errno);
		timer_del(socket, WM_EVENT_READ);
		return WM_SOCKET_ERROR;
	}
	timer_del(socket, WM_EVENT_READ);
	set_err(socket, WM_ERROR_SESSION_CLOSED);
	return WM_SOCKET_CLOSE;
}

/**
 * 所有写入都是协程同步的
 */
int wmSocket_send(wmSocket *socket, const void *buf, size_t len) {
	if (!is_available(socket, WM_EVENT_WRITE)) {
		return WM_SOCKET_CLOSE;
	}
	if (!socket->write_buffer) {
		socket->write_buffer = wmString_new(WM_BUFFER_SIZE_DEFAULT); //默认是1024
	}
	if (socket->write_buffer->offset == socket->write_buffer->length) {
		socket->write_buffer->length = 0;
		socket->write_buffer->offset = 0;
	}
	if (bufferIsFull(socket)) {
		set_err(socket, WM_ERROR_SEND_BUFFER_FULL); //发送区满了
		return WM_SOCKET_ERROR;
	}
	wmString_append_ptr(socket->write_buffer, buf, len); //要发送的字符，添加进去
	checkBufferWillFull(socket); //检查这一次加完，会不会缓冲区满
	int ret;
	int ret_num = 0; //一共发送多少字节
	while (!socket->closed) {
		//能发多少发多少，不用客气
		do {
			ret = wm_socket_send(socket->fd, socket->write_buffer->str + socket->write_buffer->offset,
				socket->write_buffer->length - socket->write_buffer->offset, 0);
		} while (ret < 0 && errno == EINTR);

		//如果是未知错误，我们检查socket是否关闭
		if (ret < 0) {
			if (check_error(errno) == WM_SOCKET_CLOSE) {
				set_err(socket, errno);
				socket->closed = true;
				return WM_SOCKET_CLOSE;
			}
			ret = 0;
		}
		socket->write_buffer->offset += ret;
		ret_num += ret;
		if (socket->write_buffer->offset == socket->write_buffer->length) {
			wmWorkerLoop_remove(socket, WM_EVENT_WRITE);
			socket->write_buffer->offset = 0;
			socket->write_buffer->length = 0;
			return ret_num;
		}
		event_wait(socket, WM_EVENT_WRITE);
	}
	wmWorkerLoop_remove(socket, WM_EVENT_WRITE);
	set_err(socket, WM_ERROR_SESSION_CLOSED);
	return WM_SOCKET_CLOSE;
}

/**
 * 全发送,不管缓冲区
 */
int wmSocket_write(wmSocket *socket, const void *buf, size_t len) {
	if (!is_available(socket, WM_EVENT_WRITE)) {
		return WM_SOCKET_CLOSE;
	}
	int ret;
	int ret_num = 0;
	while (!socket->closed) {
		//能发多少发多少，不用客气
		do {
			ret = wm_socket_send(socket->fd, buf, len - ret_num, 0);
		} while (ret < 0 && errno == EINTR);
		//如果发生错误
		if (ret < 0) {
			if (check_error(errno) == WM_SOCKET_CLOSE) {
				set_err(socket, errno);
				socket->closed = true;
				return WM_SOCKET_CLOSE;
			}
			ret = 0;
		}
		ret_num += ret;
		if (ret_num >= len) {
			wmWorkerLoop_remove(socket, WM_EVENT_WRITE);
			return ret_num;
		}
		event_wait(socket, WM_EVENT_WRITE);
	}
	set_err(socket, WM_ERROR_SESSION_CLOSED);
	return WM_SOCKET_CLOSE;
}

/**
 * 不同的loop_type操作是不同的
 */
bool event_wait(wmSocket *socket, int event) {
	//如果没有事件监听,就加上
	if (!(socket->events & event)) {
		if (!wmWorkerLoop_add(socket, event)) {
			return false;
		}
	}
	if (event & WM_EVENT_READ) {
		socket->read_co = wmCoroutine_get_current();
	}
	if (event & WM_EVENT_WRITE) {
		socket->write_co = wmCoroutine_get_current();
	}
	wmCoroutine_yield();

	//下面删除对应的co
	if (event & WM_EVENT_READ) {
		socket->read_co = NULL;
	}
	if (event & WM_EVENT_WRITE) {
		socket->write_co = NULL;
	}

	return true;
}

//检查写缓冲区是不是已经满了
bool bufferIsFull(wmSocket *socket) {
	if (socket->maxSendBufferSize <= (socket->write_buffer->length - socket->write_buffer->offset)) {
		return true;
	}
	return false;
}

//检查应用层发送缓冲区是否这次添加之后，已经满了
void checkBufferWillFull(wmSocket *socket) {
	if (socket->maxSendBufferSize <= (socket->write_buffer->length - socket->write_buffer->offset)) {
		if (socket->onBufferWillFull) {
			if (socket->onBufferWillFull) {
				socket->onBufferWillFull(socket->owner);
			}
		}
	}
}

/**
 * runtime用
 */
bool wmSocket_shutdown(wmSocket *socket, int __how) {
	set_err(socket, 0);
	if ((socket->closed && socket->shutdown_read && socket->shutdown_write) || (__how == SHUT_RD && socket->shutdown_read)
		|| (__how == SHUT_WR && socket->shutdown_write)) {
		errno = ENOTCONN;
	} else {
		if (errno == ENOTCONN) {
			// connection reset by server side
			__how = SHUT_RDWR;
		}
		switch (__how) {
		case SHUT_RD:
			socket->shutdown_read = true;
			break;
		case SHUT_WR:
			socket->shutdown_write = true;
			break;
		default:
			socket->shutdown_read = socket->shutdown_write = true;
			break;
		}
		return true;
	}
	set_err(socket, errno);
	return false;
}

/**
 * 关闭这个连接
 */
int wmSocket_close(wmSocket *socket) {
	socket->closed = true;
	if (socket->events != WM_EVENT_NULL) {
		//socket->events = WM_EVENT_NULL;
		wmWorkerLoop_del(socket); //释放事件
		if (socket->read_co) {
			wmCoroutine_resume(socket->read_co);
		}
		if (socket->write_co) {
			wmCoroutine_resume(socket->write_co);
		}
	}
	int ret = 0;
	if (!socket->removed && socket->transport == WM_SOCK_TCP) {
		socket->closed = true;
		ret = wm_socket_close(socket->fd);
		socket->removed = true;
	}

	return ret;
}

/**
 * 释放申请的内存
 */
void wmSocket_free(wmSocket *socket) {
	if (!socket) {
		return;
	}
	wmSocket_close(socket);
	if (socket->write_buffer) {
		wmString_free(socket->write_buffer);
	}
	if (socket->remoteIp) {
		wmString_free(socket->remoteIp);
	}
	if (socket->read_timer) {
		wmTimerWheel_del(socket->read_timer);
	}
	if (socket->write_timer) {
		wmTimerWheel_del(socket->write_timer);
	}
	if (socket->udp_addr) {
		wm_free(socket->udp_addr);
	}
	wm_free(socket);	//释放socket
	socket = NULL;
	total_num--;
}
