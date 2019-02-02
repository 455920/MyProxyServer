#include "epoll.h"

/*
 * 1.实现了套接字创建,绑定,监听.
 * 2.实现epoll的监听事件的能力  EventLoop
 * 3.事件有读事件，写事件
 * 4.定义了转发，删除用户表其中一个节点，发送循环，写事件具体
 * 
 * 
 * 
 */

void EpollServer::Start()
{
	_listenfd = socket(PF_INET, SOCK_STREAM, 0);
	if (_listenfd == -1)
	{
		ErrorLog("create socket");
		return;
	}

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(_port);
	// 监听本机的any网卡
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
  setsockopt(_listenfd,SOL_SOCKET,SO_REUSEADDR,&(addr),sizeof(addr));
	if(bind(_listenfd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
	{
		ErrorLog("bind socket");
		return;
	}

	if(listen(_listenfd, 100000) < 0)
	{
		ErrorLog("listen");
		return;
	}

	TraceLog("epoll server listen on %d", _port);

	_eventfd = epoll_create(100000);
	if (_eventfd == -1)
	{
		ErrorLog("epoll_create");
		return;
	}

	// 添加listenfd到epoll，监听连接事件
	SetNonblocking(_listenfd);
	OPEvent(_listenfd, EPOLLIN, EPOLL_CTL_ADD);

	// 进入事件循环
	EventLoop();
}

void EpollServer::EventLoop()
{
	struct epoll_event events[100000];
	while (1)
	{
		int n = epoll_wait(_eventfd, events, 100000, 0);
		for (int i = 0; i < n; ++i)
		{
			if (events[i].data.fd == _listenfd)
			{
				struct sockaddr clientaddr;
				socklen_t len;
				int connectfd = accept(_listenfd, &clientaddr, &len);
				if (connectfd < 0)
					ErrorLog("accept");

				ConnectEventHandle(connectfd);
			}
			else if (events[i].events & EPOLLIN)
			{
				ReadEventHandle(events[i].data.fd);
			}
			else if (events[i].events & EPOLLOUT)
			{
				WriteEventHandle(events[i].data.fd);
			}
			else
			{
				ErrorLog("event: %d", events[i].data.fd);
			}
		}
	}
}

/*
 *删除监听的描述符 
 *1.删除epoll监听的描述符
 *2.一个Connect结构中有两个fd，所以采用引用计数的方式，如果两个fd都关闭了才删除这个链接
 *3.两个fd都和他们的connect结构共享一个结构体
 * 
 */
void EpollServer::RemoveConnect(int fd)
{
	OPEvent(fd, 0, EPOLL_CTL_DEL);
	map<int, Connect*>::iterator it = _fdConnectMap.find(fd);
	if (it != _fdConnectMap.end())
	{
		Connect* con = it->second;
		if (--con->_ref == 0)
		{
			delete con;
			_fdConnectMap.erase(it);
  	}
	}
	else
	{
		assert(false);
	}
}

/*转发消息，连接客户端和目标主机的两个套接字进行消息转发
 * 1.转发消息必定有一个缓存其中一端消息的缓冲区，这个缓冲区就在Connect结构中
 * 2.转发的过程是先从一端读取，然后用SendLoop发送给另一个端
 * 
 */
void EpollServer::Forwarding(Channel* clientChannel, Channel* serverChannel,
							 bool sendencry, bool recvdecrypt)
{
	char buf[4096];
	int rlen = recv(clientChannel->_fd, buf, 4096, 0);
	if (rlen < 0)
	{
		ErrorLog("recv : %d", clientChannel->_fd);
	}
	else if (rlen == 0)
	{
		// client channel 发起关闭
		shutdown(serverChannel->_fd, SHUT_WR);
		RemoveConnect(clientChannel->_fd);
	}
	else
	{
      //加密服务器接受浏览器的socks数据包后，需要先进行解密，然后再把消息转发给socks5
		if (recvdecrypt)
		{
      Encry(buf,rlen);  
		}
      //加密服务器接收socks服务器返回的目标服务器请求的时候，需要先解密，再转发给浏览器
		if (sendencry)
		{
      Decrypt(buf,rlen);
		}

		buf[rlen] = '\0';
		SendInLoop(serverChannel->_fd, buf, rlen);
	}
}

/*
 *SendLoop存在的意义是因为，很有可能想写入到发送缓冲区的数据太多了
 * 此时发送缓冲区不够了，导致没有全部写入，为了避免这种情况，就要设置
 * 一个机制，确保我们想发送出去的和实际发送出去的数据一样
 * 
 * 
 * 
 */
void EpollServer::SendInLoop(int fd, const char* buf, int len)
{
	int slen = send(fd, buf, len, 0);
	if (slen < 0)
	{
		ErrorLog("send to %d", fd);
	}
	else if (slen < len)
	{
    //打印发送了多少数据
		TraceLog("recv %d bytes, send %d bytes, left %d send in loop", len, slen, len-slen);
		map<int, Connect*>::iterator it = _fdConnectMap.find(fd);
    //确保fd是个
		if (it != _fdConnectMap.end())
		{
			Connect* con = it->second;
			Channel* channel = &con->_clientChannel;
			if (fd == con->_serverChannel._fd)
				channel = &con->_serverChannel;
      //对于写事件，触发一次，这个标记就会自动消除
			int events = EPOLLOUT | EPOLLIN | EPOLLONESHOT;
			OPEvent(fd, events, EPOLL_CTL_MOD);//修改

			channel->_buff.append(buf+slen);
		}
		else
		{
			assert(false);
		}
	}
}

void EpollServer::WriteEventHandle(int fd)
{
	map<int, Connect*>::iterator it = _fdConnectMap.find(fd);
	if (it != _fdConnectMap.end())
	{
		Connect* con = it->second;
		Channel* channel = &con->_clientChannel;
		if (fd == con->_serverChannel._fd)
		{
			channel = &con->_serverChannel;
		}

		string buff;
		buff.swap(channel->_buff);
		SendInLoop(fd, buff.c_str(), buff.size());
	}
	else
	{
		assert(fd);
	}
}
