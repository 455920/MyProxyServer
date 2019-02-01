.PHONY:all

all:tranfer socks5

tranfer:tranfer.cc epoll.cc
	g++ -o $@ $^

socks5:socks5.cc epoll.cc
	g++ -o $@ $^
