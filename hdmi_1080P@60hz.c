#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#define ERRSTR strerror(errno)
#define debug
#define BYE_ON(cond, ...) \
do { \
	if (cond) { \
		int errsv = errno; \
		fprintf(stderr, "ERROR(%s:%d) : ", \
			__FILE__, __LINE__); \
		errno = errsv; \
		fprintf(stderr,  __VA_ARGS__); \
		abort(); \
	} \
} while(0)

#define ReqButNum 3

//#define HDMI480P
//#define HDMI720P
#define HDMI1080P

#ifdef HDMI480P
#define hdmi_width 720
#define hdmi_height 480
#endif
#ifdef HDMI720P
#define hdmi_width 1280
#define hdmi_height 720
#endif 
#ifdef HDMI1080P
#define hdmi_width 1920
#define hdmi_height 1080
#endif 


#define CLEAR(x)    memset(&(x), 0, sizeof(x))

#define BYE_ON(cond, ...) \
do { \
	if (cond) { \
		int errsv = errno; \
		fprintf(stderr, "ERROR(%s:%d) : ", \
			__FILE__, __LINE__); \
		errno = errsv; \
		fprintf(stderr,  __VA_ARGS__); \
		abort(); \
	} \
} while(0)

struct hdmibuffer {
	int index;
	void *data;
	size_t size;
	size_t width;
	size_t height;

	/* buffer state */
	double t;
};
struct hdmibuffer hdmi_buffer[ReqButNum];

struct buffer {
	int index;
	void *data;
	size_t size;
	size_t width;
	size_t height;

	/* buffer state */
	double t;
};

struct context {
	int fd;
	struct hdmibuffer *buffer;
	size_t buffer_cnt;
};

char hdmi_path[] = "/dev/video10";

static int hdmi_fd;

int open_hdmi_device()
{
	int fd;

	if((fd = open(hdmi_path,O_RDWR)) < 0)
	{
		perror("Fail to open");
		exit(EXIT_FAILURE);
	} 
	hdmi_fd = fd;
	
	printf("open hdmi success %d\n",fd);
	return fd;
}

int hdmi_setfmt()
{
	int ret;
	/* configure desired image size */	
	struct v4l2_format fmt;	
	struct v4l2_fmtdesc fmtdesc;
		memset(&fmtdesc,0,sizeof(fmtdesc));	
	fmtdesc.index = 0;
	fmtdesc.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	while((ret = ioctl(hdmi_fd,VIDIOC_ENUM_FMT,&fmtdesc)) == 0)
	{
		fmtdesc.index ++ ;
		printf("{pixelformat = %c%c%c%c},description = '%s'\n",
				fmtdesc.pixelformat & 0xff,(fmtdesc.pixelformat >> 8)&0xff,
				(fmtdesc.pixelformat >> 16) & 0xff,(fmtdesc.pixelformat >> 24)&0xff,
				fmtdesc.description);
	}
		memset(&fmt,0,sizeof(fmt));	
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;	
	fmt.fmt.pix.width = hdmi_width;	
	fmt.fmt.pix.height = hdmi_height;	
	fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;
	//V4L2_FIELD_NONE;
	//V4L2_FIELD_INTERLACED;
	printf("%s: +\n", __func__);
	/* format is hardcoded: draw procedures work only in 32-bit mode */	
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_BGR32;	
	ret = ioctl(hdmi_fd, VIDIOC_S_FMT, &fmt);	
	BYE_ON(ret < 0, "VIDIOC_S_FMT failed: %s\n", ERRSTR);

	/* update format struct to values adjusted by a driver */	
	ret = ioctl(hdmi_fd, VIDIOC_G_FMT, &fmt);	
	BYE_ON(ret < 0, "VIDIOC_G_FMT failed: %s\n", ERRSTR);
	printf("getfmt:%d,%d\n", fmt.fmt.pix.width, fmt.fmt.pix.height);
	BYE_ON((fmt.fmt.pix.width < hdmi_width) | (fmt.fmt.pix.height < hdmi_height), "VIDIOC_G_FMT failed: %s\n", ERRSTR);
	/* crop output area on display */
	struct v4l2_crop crop;
	crop.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	crop.c.left = 0;
	crop.c.top = 0;
	crop.c.width = hdmi_width;
	crop.c.height = hdmi_height;
	ret = ioctl(hdmi_fd, VIDIOC_S_CROP, &crop);
	BYE_ON(ret < 0, "VIDIOC_S_CROP failed: %s\n", ERRSTR);
	
	printf("%s: -\n", __func__);
	return 0;
}
int hdmi_reqbufs()
{
	int ret;
	int i,j;
	struct v4l2_requestbuffers rqbufs;	
	struct v4l2_plane plane;
	struct v4l2_buffer buf;
	struct v4l2_format fmt;
	rqbufs.count = ReqButNum;	
	rqbufs.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;	
	rqbufs.memory = V4L2_MEMORY_MMAP;	
	printf("%s +\n", __func__);	
	ret = ioctl(hdmi_fd, VIDIOC_REQBUFS, &rqbufs);
	printf("reqbuf:%d\n", rqbufs.count);
	BYE_ON(ret < 0, "VIDIOC_REQBUFS failed: %s\n", ERRSTR);	
	BYE_ON(rqbufs.count < ReqButNum, "failed to get %d buffers\n",		ReqButNum);

//	ret = ioctl(hdmi_fd, VIDIOC_G_FMT, &fmt);	
//	BYE_ON(ret < 0, "VIDIOC_G_FMT failed: %s\n", ERRSTR);

	/* buffers initalization */
	for (i = 0; i < ReqButNum; ++i) {

		buf.index = i;
		buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.m.planes = &plane;
		buf.length = 1;
		/* get buffer properties from a driver */
		ret = ioctl(hdmi_fd, VIDIOC_QUERYBUF, &buf);
		BYE_ON(ret < 0, "VIDIOC_QUERYBUF for buffer %d failed: %s\n",
			buf.index, ERRSTR);

		hdmi_buffer[i].index = i;
		/* mmap buffer to user space */
		hdmi_buffer[i].data = mmap(NULL, plane.length, PROT_READ | PROT_WRITE,
			MAP_SHARED, hdmi_fd, plane.m.mem_offset);
		BYE_ON(hdmi_buffer[i].data == MAP_FAILED, "mmap failed: %s\n",
			ERRSTR);
		hdmi_buffer[i].size = plane.length;
		hdmi_buffer[i].width = hdmi_width;
		hdmi_buffer[i].height = hdmi_height;
		/* fill buffer with black */
		for (j = 0; 4 * j < hdmi_buffer[i].size; ++j)
			((unsigned int *)hdmi_buffer[i].data)[j] = 0xff000000;
	}

	printf("%s -\n", __func__);
	return 0;

}

int init_device()
{
	hdmi_setfmt();
	hdmi_reqbufs();

printf("%s -\n", __func__);

}
int hdmi_streamon()
{
	enum v4l2_buf_type type;
	int ret;

//	hdmi_qbuf(2);
	struct timeval tv;	
	gettimeofday(&tv, NULL);
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;	
	ret = ioctl(hdmi_fd, VIDIOC_STREAMON, &type);	
	BYE_ON(ret, "VIDIOC_STREAMON failed: %s\n", ERRSTR);
	return 0;

}
int hdmi_qbuf(int index)
{
	struct v4l2_buffer buf;
	struct v4l2_plane plane;

	debug("queue +\n");
	memset(&buf, 0, sizeof(buf));
	memset(&plane, 0, sizeof(plane));
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	buf.index = index;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.m.planes = &plane;
	buf.length = 1;
	int ret;
	ret = ioctl(hdmi_fd, VIDIOC_QBUF, &buf);
	BYE_ON(ret, "VIDIOC_QBUF(index = %d) failed: %s\n",
		index, ERRSTR);
	//sem_post(&lcd_sem);
	return 0;
}

int hdmi_dbuf(int *index)
{
	int ret;
	struct v4l2_buffer buf;
	struct v4l2_plane plane;
	
	debug("dequeue +\n");
	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.m.planes = &plane;
	buf.length = 1;
	ret = ioctl(hdmi_fd, VIDIOC_DQBUF, &buf);
	BYE_ON(ret, "VIDIOC_DQBUF failed: %s\n", ERRSTR);
	*index = buf.index;
	
	return 0;
}

unsigned int *display_bmp_image()
{
	unsigned int *img_buf;
    FILE *fp;
    int count = 0, i = 0, j=0, r, g, b;
    char ch;
    char *pch;
    int buf_size = hdmi_width * hdmi_height;
    img_buf = (unsigned int *)malloc(buf_size * sizeof(int));
    memset(img_buf, 0, sizeof(img_buf));
    if(NULL == (fp = fopen("./test.bmp", "r")))
    {
        printf("open the image file %s failed!\n", "test.bmp");
        return;
    }
    for(i = 0 ; i < 54 ; i++)
        ch=fgetc(fp);

	for(i = 0; i < hdmi_height; i++)
	{
			for(j = 0; j < hdmi_width; j++)
		{
			  r = fgetc(fp);
			  g = fgetc(fp);
			  b = fgetc(fp);
			  img_buf[(hdmi_height - i - 1) * hdmi_width + j] = (unsigned int)(0xff000000 | (b << 16) | (g << 8) | r);
		}
	}
    fclose(fp);
	
	return (void *)img_buf;
}

int setup_preset(int fd,  int preset)
{
	int ret;
	int count;
	struct v4l2_dv_preset presetinfo;
	struct v4l2_dv_enum_preset enuminfo;
	memset(&presetinfo, 0, sizeof(presetinfo)); 
	memset(&enuminfo, 0, sizeof(enuminfo)); 
	while((ret = ioctl(fd, VIDIOC_ENUM_DV_PRESETS, &enuminfo)) >= 0)
	{
		printf("preset:%d,name:%s\n",enuminfo.preset, enuminfo.name);
		enuminfo.index++; 
	}
	memset(&presetinfo, 0, sizeof(presetinfo)); 
	presetinfo.preset = preset;
	if((ret = ioctl(fd, VIDIOC_S_DV_PRESET, &presetinfo))<0)
	{
		printf("VIDIOC_G_DV_PRESET error:%d\n",ret);
		return -1;
	}
	memset(&presetinfo, 0, sizeof(presetinfo)); 
	if((ret = ioctl(fd, VIDIOC_G_DV_PRESET, &presetinfo))<0)
	{
		printf("VIDIOC_G_DV_PRESET error:%d\n",ret);
		return -1;
	}
	printf("preset:%d\n",presetinfo.preset);
	return 0;
}
int main()
{
	int static i = 0;
	int	j = 0;
	int k = 0;
	int r, g, b;
	unsigned int *bmp_buf;
	bmp_buf = display_bmp_image();
	open_hdmi_device();

	setup_preset(hdmi_fd, 18);

	//return 0;
	hdmi_setfmt();
	hdmi_reqbufs();
	hdmi_qbuf(0);
	hdmi_qbuf(1);
	hdmi_qbuf(2);
	hdmi_streamon();
	
	while(1)
	{
		hdmi_dbuf(&i);
		k = (k + 8)%24;
		printf("i=%d, %lx\n",i, 0xff000000 | (0xff << k));
		memcpy((void *)hdmi_buffer[i].data, (void *)bmp_buf, hdmi_width * hdmi_height * 4);

		hdmi_qbuf(i);	
		getchar();
		for(j = 0; j < hdmi_width * hdmi_height; j ++)
			((unsigned int *)hdmi_buffer[i].data)[j] = 0xff000000 | (0xff << k);
	
	}
}