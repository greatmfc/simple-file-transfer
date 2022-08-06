#if __cplusplus > 201703L
module;
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cassert>
#include <sys/socket.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <exception>
#include <iostream>
#include <string_view>
#include <thread>
#include <sys/epoll.h>
#include <fstream>
#include <unordered_map>
#include <functional>
#include <future>
#include <memory>

#define DEFAULT_PORT 9007
#define VERSION 1.730
#define BUFFER_SIZE 128
#define ALARM_TIME 300

#define LOG_MSG(_addr,_msg) log::get_instance()->submit_missions(MESSAGE_TYPE,_addr,_msg)
#define LOG_FILE(_addr,_msg) log::get_instance()->submit_missions(FILE_TYPE,_addr,_msg)
#define LOG_ACCEPT(_addr) log::get_instance()->submit_missions(ACCEPT,_addr,"")
#define LOG_CLOSE(_addr) log::get_instance()->submit_missions(CLOSE,_addr,"")
#define LOG_ERROR(_addr) log::get_instance()->submit_missions(ERROR_TYPE,_addr,strerror(errno))
#define LOG_VOID(_msg) log::get_instance()->submit_missions(_msg)

export module sft;
import structs;
import thpool;
import epoll_util;
import log;

using std::cout;
using std::endl;
using std::invalid_argument;
using std::to_string;
using std::move;
using std::forward;
using std::exit;
using std::thread;
using std::string;
using std::ios;
using std::ofstream;
using std::string_view;
using std::runtime_error;
using std::cerr;
using std::unordered_map;
using std::bind;
using std::future;
using std::make_shared;
using std::function;
using std::packaged_task;

export{
	class setup
	{
	public:
		setup();
		setup(char* ip_addr, int port);
		~setup() = default;
		int socket_fd;
		struct sockaddr_in addr;

	private:
		unsigned long target_addr;
		int write_to_sock;
	};

	class basic_action
	{
	protected:
		basic_action() = default;
		~basic_action() = default;
		int socket_fd;
		int status_code;
		setup* pt;
	};

	class receive_loop : public basic_action
	{
	public:
		receive_loop() = default;
		receive_loop(setup& s);
		~receive_loop() = default;
		static void stop_loop();
		void loop();

	private:
		struct sockaddr_in addr;
		socklen_t len;
		epoll_utility epoll_instance;
		unordered_map<int, data_info> connection_storage;
		unordered_map<unsigned int, ofstream*> addr_to_stream;
		static inline bool running;
		static inline int pipe_fd[2];

		int decide_action(int fd);
		void deal_with_file(int fd);
		void deal_with_mesg(int fd);
		void deal_with_gps(int fd);
		void deal_with_get_file(int fd);
		void close_connection(int fd);
		int get_prefix(int fd);
		static void alarm_handler(int sig);
	};

	class send_file : public basic_action
	{
	public:
		send_file() = default;
		send_file(setup& s, char*& path);
		~send_file() = default;
		void write_to();

	private:
		char* file_path;
	};

	class send_msg : public basic_action
	{
	public:
		send_msg() = default;
		send_msg(setup& s, char*& msg);
		void write_to();
		~send_msg() = default;
	private:
		char* msg;
	};

	class get_file : public basic_action
	{
	public:
		get_file() = delete;
		get_file(setup& s, string_view&& msg);
		~get_file() = default;
		void get_it();

	private:
		string_view file_name;

	};
}

#else
#include "common_headers.h"
#endif // __cplusplus > 201703L

template<>
inline future<void> thread_pool::submit_to_pool<void (receive_loop::*)(int fd), receive_loop*, int&>
	(void (receive_loop::*&& f)(int fd), receive_loop*&& arg, int& args) {
	function<void()> func_bind = bind(forward<void (receive_loop::*)(int fd)>(f), forward<receive_loop*>(arg),forward<int&>(args));
	auto task_ptr = make_shared<packaged_task<void()>>(func_bind);
	function<void()> wrapper_func = [task_ptr]() {
		(*task_ptr)();
	};
	m_queue.push(wrapper_func);
	m_cv.notify_one();
	return task_ptr->get_future();
}

receive_loop::receive_loop(setup& s)
{
	pt = &s;
	len = sizeof(addr);
	socket_fd = pt->socket_fd;
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pipe_fd) < 0) {
		LOG_ERROR(pt->addr);
		exit(1);
	}
}

void receive_loop::stop_loop()
{
	running = false;
	char msg = '1';
	send(pipe_fd[1], &msg, 1, 0);
}

void receive_loop::loop()
{
	running = true;
	int ret=listen(socket_fd, 5);
	assert(ret >= 0);
	epoll_instance.add_fd_or_event_to_epoll(socket_fd,false,true,0);
	epoll_instance.add_fd_or_event_to_epoll(pipe_fd[0], false, true, 0);
	epoll_instance.set_fd_no_block(socket_fd);
	signal(SIGALRM, alarm_handler);
	alarm(ALARM_TIME);
	LOG_VOID("Server starts.");
	thread_pool tp;
	tp.init_pool();
	while (running) {
		int count = epoll_instance.wait_for_epoll();
		if (count < 0 && errno != EINTR) {
			LOG_ERROR(pt->addr);
			perror("Error epoll_wait");
			exit(1);
		}
		for (int i = 0; i < count; ++i) {
			int react_fd = epoll_instance.events[i].data.fd;

			if (react_fd == socket_fd) {
				while (true)
				{
					int accepted_fd = accept(socket_fd, (struct sockaddr*)&addr, &len);
					if (accepted_fd < 0) {
						if (errno != EAGAIN) {
							LOG_ERROR(addr);
							perror("Accept failed");
							exit(1);
						}
						else {
							break;
						}
					}
				#ifdef DEBUG
					cout << "Accept from client:" << inet_ntoa(addr.sin_addr) << endl;
				#endif // DEBUG
					LOG_ACCEPT(addr);
					epoll_instance.add_fd_or_event_to_epoll(accepted_fd, false, true, 0);
					connection_storage[accepted_fd].address = addr;
					addr_to_stream[addr.sin_addr.s_addr] = nullptr;
				}
			}
			else if (epoll_instance.events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
			#ifdef DEBUG
				cout << "Disconnect from client:" << inet_ntoa(connection_storage[react_fd].address.sin_addr) << endl;
			#endif // DEBUG
				LOG_CLOSE(connection_storage[react_fd].address);
				epoll_instance.remove_fd_from_epoll(react_fd);
				close_connection(react_fd);
			}
			else if (react_fd == pipe_fd[0]) {
				char signal='2';
				recv(pipe_fd[0], &signal, sizeof signal, 0);
				if (signal == '1') continue;
				LOG_VOID("Alarm.");
				alarm(ALARM_TIME);
			}
			else if (epoll_instance.events[i].events & EPOLLIN) {
				status_code = decide_action(react_fd);
				switch (status_code)
				{
				case FILE_TYPE:
					deal_with_file(react_fd);
					break;
				case MESSAGE_TYPE:
					tp.submit_to_pool(&receive_loop::deal_with_mesg,this,react_fd);
					break;
				case GPS_TYPE:
					tp.submit_to_pool(&receive_loop::deal_with_gps,this,react_fd);
					break;
				case GET_TYPE:
					tp.submit_to_pool(&receive_loop::deal_with_get_file, this, react_fd);
					break;
				}
			}
		}
	}
	tp.shutdown_pool();
	for (auto& [addr, stream] : addr_to_stream) {
		if (stream != nullptr) {
			delete stream;
		}
	}
	LOG_VOID("Server quits.");
	exit(0);
}

int receive_loop::decide_action(int fd)
{
	ssize_t ret = 0;
	ret = read(fd, connection_storage[fd].buffer_for_pre_messsage, BUFFER_SIZE);
	if (ret < 0 && errno != EAGAIN) {
		LOG_ERROR(connection_storage[fd].address);
#ifdef DEBUG
		perror("Something happened while read from client");
#endif // DEBUG
		goto end;
	}
#ifdef DEBUG
	cout << "Read msg from client: " << connection_storage[fd].buffer_for_pre_messsage << endl;
#endif // DEBUG
	switch (connection_storage[fd].buffer_for_pre_messsage[0])
	{
	case 'f':return FILE_TYPE;
	case 'n':return GPS_TYPE;
	case 'g':return GET_TYPE;
	default:return MESSAGE_TYPE;
	}
	end:return -1;
}

void receive_loop::deal_with_file(int fd)
{
	char msg1 = '1';
	write(fd, &msg1, sizeof(msg1));
	char full_path[64]="./";
	char* msg = connection_storage[fd].buffer_for_pre_messsage;
	msg += 2;
	ssize_t size = atoi(strchr(msg, '/')+1);
	strncat(full_path, msg, strcspn(msg, "/"));
	int write_fd = open(full_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	connection_storage[fd].bytes_to_deal_with = size;
	connection_storage[fd].buf = new char[size];
	char* buf = connection_storage[fd].buf;
	ssize_t ret = 0;
	while (size)
	{
		ret=read(fd, buf, size);
		if (ret < 0)
		{
			LOG_ERROR(connection_storage[fd].address);
			perror("Receieve failed");
			exit(1);
		}
		size -= ret;
		buf += ret;
	}
	//di->fd = write_fd;
	buf = connection_storage[fd].buf;
	for (;;) {
		ret = write(write_fd, connection_storage[fd].buf, connection_storage[fd].bytes_to_deal_with);
		if (ret < 0) {
			LOG_ERROR(connection_storage[fd].address);
#ifdef DEBUG
			perror("Server write to local failed");
#endif // DEBUG
			exit(1);
		}
		connection_storage[fd].bytes_to_deal_with -= ret;
		connection_storage[fd].buf += ret;
		if (connection_storage[fd].bytes_to_deal_with <= 0) {
			break;
		}
	}
#ifdef DEBUG
	cout << "Success on receiving file: " << msg << endl;
#endif // DEBUG
	LOG_FILE(connection_storage[fd].address, move(msg));
	delete buf;
	close(write_fd);
	memset(connection_storage[fd].buffer_for_pre_messsage, 0, BUFFER_SIZE);
}

void receive_loop::deal_with_mesg(int fd)
{
	char* pt = connection_storage[fd].buffer_for_pre_messsage;
	char buffer[128]{ 0 };
	strcpy(buffer, pt+2);
	strcat(buffer, "\n");
	char code = '1';
	write(fd, &code, sizeof code);
	LOG_MSG(connection_storage[fd].address, move(buffer));
#ifdef DEBUG
	cout << "Success on receiving message: " << buffer;
#endif // DEBUG
	memset(connection_storage[fd].buffer_for_pre_messsage, 0, BUFFER_SIZE);
}

void receive_loop::deal_with_gps(int fd)
{
	char file_name[32] = "gps_";
	in_addr tmp_addr = connection_storage[fd].address.sin_addr;
	strcat(file_name, inet_ntoa(tmp_addr));
	if (addr_to_stream[tmp_addr.s_addr] == nullptr) {
		addr_to_stream[tmp_addr.s_addr] = new ofstream(file_name, ios::app | ios::out);
		if (addr_to_stream[tmp_addr.s_addr]->fail()) {
			LOG_ERROR(connection_storage[fd].address);
			cout << "Open gps file failed\n";
			exit(1);
		}
	}
	char* pt = connection_storage[fd].buffer_for_pre_messsage;
	while (*pt == ' ') ++pt;
	char buffer[128]{ 0 };
	strcpy(buffer, pt+2);
	strcat(buffer, "\n");
	*addr_to_stream[tmp_addr.s_addr] << buffer;
	//addr_to_stream[tmp_addr.s_addr].flush();
	LOG_MSG(connection_storage[fd].address, "GPS content");
#ifdef DEBUG
	cout << "Success on receiving GPS: " << buffer;
#endif // DEBUG
	memset(connection_storage[fd].buffer_for_pre_messsage, 0, BUFFER_SIZE);
}

void receive_loop::deal_with_get_file(int fd)
{
	char* tmp_pt = &connection_storage[fd].buffer_for_pre_messsage[2];
	char full_path[64] = "./";
	strncat(full_path, tmp_pt, strlen(tmp_pt));
	struct stat st;
	stat(full_path, &st);
	if (access(full_path, R_OK) != 0 || !S_ISREG(st.st_mode)) {
		LOG_FILE(connection_storage[fd].address,
			"No access to request file or it's not regular file.");
		memset(connection_storage[fd].buffer_for_pre_messsage, 0, BUFFER_SIZE);
		char code = '0';
		write(fd, &code, sizeof code);
		return;
	}
	char* ptr = full_path;

	int file_fd = open(full_path, O_RDONLY);
	if (file_fd < 0) {
		perror("Open file failed");
		close(fd);
		memset(connection_storage[fd].buffer_for_pre_messsage, 0, BUFFER_SIZE);
		return;
	}
	fstat(file_fd, &st);
	ptr += 2;
	ssize_t ret = 0;
	strcat(ptr, "/");
	strcat(ptr, to_string(st.st_size).data());
	write(fd, ptr, strlen(ptr));
	char flag = '0';
	ret = recv(fd, &flag, sizeof(flag), 0);
	if (flag != '1' || ret <= 0) {
#ifdef DEBUG
		cout << "Receive flag from server failed.\n";
		cout << __LINE__ << endl;
#endif // DEBUG
		LOG_ERROR(connection_storage[fd].address);
		LOG_CLOSE(connection_storage[fd].address);
		epoll_instance.remove_fd_from_epoll(fd);
		close(fd);
		memset(connection_storage[fd].buffer_for_pre_messsage, 0, BUFFER_SIZE);
		return;
	}
	off_t off = 0;
	long send_size = st.st_size;
	while (send_size > 0) {
		ssize_t ret = sendfile(fd, file_fd, &off, send_size);
		if(ret < 0)
		{
			LOG_ERROR(connection_storage[fd].address);
		#ifdef DEBUG
			perror("Sendfile failed");
		#endif // DEBUG
			break;
		}
	#ifdef DEBUG
		cout << "have send :" << ret << endl;
	#endif // DEBUG
		send_size -= ret;
	}
	LOG_FILE(connection_storage[fd].address, "Send file.");
	memset(connection_storage[fd].buffer_for_pre_messsage, 0, BUFFER_SIZE);
	close(file_fd);
}

void receive_loop::close_connection(int fd)
{
	in_addr tmp_addr = connection_storage[fd].address.sin_addr;
	if (addr_to_stream[tmp_addr.s_addr] != nullptr){
		if (addr_to_stream[tmp_addr.s_addr]->is_open()) {
			addr_to_stream[tmp_addr.s_addr]->close();
		}
		delete addr_to_stream[tmp_addr.s_addr];
		addr_to_stream[tmp_addr.s_addr] = nullptr;
	}
}

int receive_loop::get_prefix(int fd)
{
	for (auto& charc : connection_storage[fd].buffer_for_pre_messsage) {
		switch (charc)
		{
		case 'f':return FILE_TYPE;
		case 'g':return GPS_TYPE;
		default:charc = ' ';
		}
	}
	return MESSAGE_TYPE;
}

void receive_loop::alarm_handler(int)
{
	int save_errno = errno;
	char msg = '0';
	send(pipe_fd[1], &msg, 1, 0);
	errno = save_errno;
}

send_file::send_file(setup& s, char*& path)
{
	pt = &s;
	file_path = path;
	socket_fd = pt->socket_fd;
}

void send_file::write_to()
{
	int file_fd = open(file_path, O_RDONLY);
	if (file_fd < 0) {
		perror("Open file failed");
		close(socket_fd);
		return;
	}
	struct stat st;
	fstat(file_fd, &st);
	ssize_t ret = 0;
	char *msg = strrchr(file_path, '/') + 1;
	strcat(msg, "/");
	strcat(msg, to_string(st.st_size).data());
	char complete_msg[] = "f/";
	strcat(complete_msg, msg);
	write(socket_fd, complete_msg, strlen(complete_msg));
	char flag = '0';
	ret = recv(socket_fd, &flag, sizeof(flag), 0);
	assert(ret >= 0);
	if (flag != '1' || ret < 0) {
#ifdef DEBUG
		cout << "Receive flag from server failed.\n";
		cout << __LINE__ << endl;
#endif // DEBUG
		exit(1);
	}
	off_t off = 0;
	long send_size = st.st_size;
	while (send_size > 0) {
		ssize_t ret = sendfile(socket_fd, file_fd, &off, send_size);
		if(ret < 0)
		{
			perror("Sendfile failed");
			break;
		}
	#ifdef DEBUG
		cout << "have send :" << ret << endl;
	#endif // DEBUG
		send_size -= ret;
	}
	close(socket_fd);
	close(file_fd);
}

setup::setup()
{
	bzero(&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY); 
	addr.sin_port = htons(DEFAULT_PORT);
	socket_fd = socket(PF_INET, SOCK_STREAM, 0);
	int flag = 1;
	setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
	int ret = bind(socket_fd, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		LOG_ERROR(addr);
		perror("Binding failed");
		exit(-1);
	}
}

setup::setup(char* ip_addr,int port)
{
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	try
	{
		target_addr = inet_addr(ip_addr);
		if (target_addr == INADDR_NONE) {
			throw invalid_argument("Invalid address:");
		}
	}
	catch (const std::exception& ex)
	{
		cout << ex.what() << ip_addr << endl;
		exit(1);
	}
	memcpy(&addr.sin_addr, &target_addr, sizeof(target_addr));
	unsigned short pt = static_cast<unsigned short>(port);
	addr.sin_port = htons(pt);
	socket_fd = socket(AF_INET, SOCK_STREAM, 0);
	int ret = connect(socket_fd, (struct sockaddr*)&addr, sizeof(addr));
	if (ret < 0) {
		perror("Connect failed");
		exit(1);
	}
}

send_msg::send_msg(setup& s, char*& msg) : msg(msg)
{
	pt = &s;
	socket_fd = pt->socket_fd;
}

void send_msg::write_to()
{
	char pre_msg[128]{ 0 };
	//strcpy(pre_msg, "m/");
	strcpy(pre_msg, msg);
	write(socket_fd, pre_msg, strlen(pre_msg));
	char code = '0';
	read(socket_fd, &code, sizeof code);
	if (code != '1') {
		perror("Something wrong with the server");
	}
	close(socket_fd);
}

get_file::get_file(setup& s, string_view&& msge) {
	pt = &s;
	socket_fd = pt->socket_fd;
	file_name = msge;
}

void get_file::get_it()
{
	char pre_msg[128]{ 0 };
	strcpy(pre_msg, "g/");
	strcat(pre_msg, file_name.data());
	write(socket_fd, pre_msg, strlen(pre_msg));
	//send a request first
	data_info dis{};
	memset(&dis, 0, sizeof dis);
	ssize_t ret = read(socket_fd, dis.buffer_for_pre_messsage, sizeof dis.buffer_for_pre_messsage);
	if (ret <= 1) {
		cerr<<"File might not be found in the server.";
		exit(1);
	}
	char code = '1';
	write(socket_fd, &code, sizeof(code));
	//inform the server to send formal data

	char full_path[64]="./";
	char* msg = dis.buffer_for_pre_messsage;
	ssize_t size = atoi(strchr(msg, '/')+1);
	strncat(full_path, msg, strcspn(msg, "/"));
	int write_fd = open(full_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	dis.bytes_to_deal_with = size;
	dis.buf = new char[size];
	char* buf = dis.buf;
	while (size)
	{
		ret=read(socket_fd, buf, size);
		if (ret < 0)
		{
			LOG_ERROR(dis.address);
			perror("Receieve failed");
			exit(1);
		}
		size -= ret;
		buf += ret;
	}
	//di->fd = write_fd;
	buf = dis.buf;
	for (;;) {
		ret = write(write_fd, dis.buf, dis.bytes_to_deal_with);
		if (ret < 0) {
			LOG_ERROR(dis.address);
#ifdef DEBUG
			perror("Server write to local failed");
#endif // DEBUG
			exit(1);
		}
		dis.bytes_to_deal_with -= ret;
		dis.buf += ret;
		if (dis.bytes_to_deal_with <= 0) {
			break;
		}
	}
#ifdef DEBUG
	cout << "Success on getting file: " << msg << endl;
#endif // DEBUG
	LOG_FILE(dis.address, move(msg));
	delete buf;
	close(write_fd);
	memset(dis.buffer_for_pre_messsage, 0, BUFFER_SIZE);
}

