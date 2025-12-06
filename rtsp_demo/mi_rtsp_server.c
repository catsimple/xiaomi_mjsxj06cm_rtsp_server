/********************************************************************
* file: mi_rtsp_server.c               date: 六 2024-08-03 11:00:16 *
*                                                                   *
* Description: Fixed Audio (PCM16->G711A + Downsample)              *
********************************************************************/
#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <arpa/inet.h>
#include <signal.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h> // 引入 int16_t
#include "rtsp_demo.h"

#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)

#define MAX_BUFFER_SIZE 128
#define EPOLL_TIMEOUT 60000 // milliseconds

// ==========================================
// [新增] G.711 A-law 编码表与转换函数
// ==========================================
unsigned char linear2alaw(int16_t pcm_val)
{
	int mask;
	int seg;
	unsigned char aval;

	pcm_val = pcm_val >> 3;

	if (pcm_val >= 0) {
		mask = 0xD5;
	} else {
		mask = 0x55;
		pcm_val = -pcm_val - 1;
	}

	if (pcm_val < 32) {
		seg = 0;
	} else if (pcm_val < 64) {
		seg = 1;
	} else if (pcm_val < 128) {
		seg = 2;
	} else if (pcm_val < 256) {
		seg = 3;
	} else if (pcm_val < 512) {
		seg = 4;
	} else if (pcm_val < 1024) {
		seg = 5;
	} else if (pcm_val < 2048) {
		seg = 6;
	} else {
		seg = 7;
	}

	if (seg >= 8)
		return (unsigned char) (0x7F ^ mask);
	else {
		aval = (unsigned char) (seg << 4) | ((pcm_val >> (seg ? seg - 1 : 0)) & 0x0F);
		return (aval ^ mask);
	}
}
// ==========================================

typedef int(*set_frame)(rtsp_session_handle session, const uint8_t *frame, int len, uint64_t ts);

struct mi_stream_info {
	const char *unix_path;
	const char *stream_path;
	const set_frame callback;
	const int msg_offset;
	bool enabled;
	int fd;
	int ch;
	struct epoll_event ev;
	rtsp_session_handle session;
} mi_info[3] = {
	{ "/run/video_mainstream/control", "/main", rtsp_tx_video, 4, true },
	{ "/run/video_substream/control", "/sub", rtsp_tx_video, 4, true },
	{ "/var/run/audio_in/control", NULL, rtsp_tx_audio, 6, true}
};

static int flag_run = 1;
static void sig_proc(int signo)
{
	flag_run = 0;
}

int unix_sock_connect(const char *path)
{
	struct sockaddr_un unixAddr;

	int unixSock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (unixSock < 0) {
		perror("socket\n");
		return unixSock;
	}

	memset(&unixAddr, 0, sizeof(unixAddr));
	unixAddr.sun_family = AF_UNIX;
	strcpy(unixAddr.sun_path, path);

	int ret = connect(unixSock, (struct sockaddr*)&unixAddr, sizeof(unixAddr));
	if (ret < 0) {
		perror("connect\n");
		close(unixSock);
		return -1;
	}

	return unixSock;
}

int main(int argc, char *argv[]) {
	int map_len, count;
	int map_fd;
	uint8_t *map_addr;
	struct mi_stream_info* p;
	int epoll_fd;
	int ret, ch = 0;
	uint64_t ts = 0;
	int port = 0;
	unsigned int msg_len;
	rtsp_demo_handle demo;
	struct epoll_event evs[10];
	struct msghdr msg;
	struct cmsghdr cm;
	struct iovec iov[2];
	char iovPart1[4];
	char iovPart2[MAX_BUFFER_SIZE];
	
	// [新增] 音频编码缓冲区
	unsigned char audio_encode_buf[1024];

	iov[0].iov_base = iovPart1;
	iov[0].iov_len = sizeof(iovPart1);
	iov[1].iov_base = iovPart2;
	iov[1].iov_len = sizeof(iovPart2);

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = iov;
	msg.msg_iovlen = 2;

	if (argc > 1) {
		port = atoi(argv[1]);
	}

	demo = rtsp_new_demo(port);
	if (NULL == demo) {
		printf("rtsp_new_demo failed\n");
	}

	epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (epoll_fd < 0) {
		perror("epoll_create1");
	}

	for (ch = 0; ch < 3; ch++) {
		p = &mi_info[ch];
		if (!p->enabled) {
			continue;
		}
		p->ch = ch;
		p->fd = unix_sock_connect(p->unix_path);
		if (p->fd < 0) {
			printf("connect %s error\n", p->unix_path);
		}
		recvmsg(p->fd, &msg, MSG_DONTWAIT); /* need recv one package */
		msg.msg_control = &cm;
		msg.msg_controllen = CMSG_LEN(sizeof(int));
		p->ev.events = EPOLLIN;
		p->ev.data.ptr = p;
		ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, p->fd, &p->ev);
		if (ret < 0) {
			printf("epoll add %s error\n", p->unix_path);
		}
		if (NULL == p->stream_path) {
			continue;
		}
		p->session = rtsp_new_session(demo, p->stream_path);
		if (NULL == p->session) {
			printf("rtsp_new_session failed\n");
			continue;
		}
		rtsp_set_video(p->session, RTSP_CODEC_ID_VIDEO_H265, NULL, 0);
		rtsp_sync_video_ts(p->session, rtsp_get_reltime(), rtsp_get_ntptime());
		
		// 音频设置为 G711A (PCMA), 8000Hz
		rtsp_set_audio(p->session, RTSP_CODEC_ID_AUDIO_G711A, NULL, 0);
		rtsp_sync_audio_ts(p->session, rtsp_get_reltime(), rtsp_get_ntptime());
	}
	mi_info[2].session = mi_info[0].session;

	signal(SIGINT, sig_proc);
	signal(SIGPIPE, SIG_IGN);

	printf("start...\n");

	ts = rtsp_get_reltime();
	while (flag_run) {
		count = epoll_wait(epoll_fd, evs, 10, EPOLL_TIMEOUT);
		for (int i = 0; i < count; i++) {
			struct mi_stream_info* p = evs[i].data.ptr;
			ssize_t len = recvmsg(p->fd, &msg, MSG_DONTWAIT);
			if (len < 0) {
				printf("recv err:%d\n", len);
				continue;
			}

			map_len = *((int*)(&iovPart2[4]));
			map_fd  = *(int*)CMSG_DATA(&cm);
			if (unlikely(map_fd <= 0)) {
				printf("map_fd error:%d\n", map_fd);
				continue;
			}

			map_addr = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE, map_fd, 0);
			if (unlikely((void *)-1 == map_addr)) {
				close(map_fd);
				printf("map error\n");
				continue;
			}

			msg_len = ((unsigned int *)(map_addr + 64))[p->msg_offset];
			
			// =======================================================
			// [核心修改] 音频处理逻辑
			// =======================================================
			if (!p->stream_path) { /* audio only send main */
				// 1. 获取原始 PCM 16bit 数据指针
				int16_t *pcm_src = (int16_t *)(map_addr + 96);
				
				// 2. 计算采样点数 (通常 msg_len 为 640 字节 = 320 个采样点)
				int src_samples = msg_len / 2;
				
				// 3. 降采样 + 编码 (16k PCM -> 8k G.711A)
				// 每次循环步进为 2 (i+=2)，实现从 16k 降到 8k
				int encoded_len = 0;
				for (int j = 0; j < src_samples && encoded_len < sizeof(audio_encode_buf); j += 2) {
					audio_encode_buf[encoded_len++] = linear2alaw(pcm_src[j]);
				}
				
				// 4. 发送编码后的数据
				(p->callback)(p->session, audio_encode_buf, encoded_len, ts);
			}
			// =======================================================
			else {
				// 视频流保持不变
				(p->callback)(p->session, (const uint8_t *)(map_addr+96), msg_len, ts);
			}

			munmap(map_addr, map_len);
			close(map_fd);
		}

		do {
			ret = rtsp_do_event(demo);
			if (ret > 0)
				continue;
			if (ret < 0)
				break;
			usleep(20000);
		} while (rtsp_get_reltime() - ts < 1000000 / 25);
		if (ret < 0)
			break;
		ts += 1000000 / 25;
	}

	printf("\nexit.........\n");

	for (ch = 0; ch < 3; ch++) {
		p = &mi_info[ch];
		if (!p->enabled || p->fd < 0) {
			continue;
		}
		close(p->fd);
		if (p->stream_path) {
			rtsp_del_session(p->session);
		}
	}

	close(epoll_fd);
	rtsp_del_demo(demo);

	return 0;
}

#ifdef __cplusplus
};
#endif
