#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <sys/mman.h>
#include <assert.h>
#include <linux/videodev2.h>
#include <linux/fb.h>
#include <pthread.h>
#include <poll.h>
#include <semaphore.h>

#define TimeOut 5 

#define CapNum 10

#define CapWidth 640
#define CapHeight 480

#define HDMI1080P
#ifdef HDMI1080P
#define hdmi_width 720
#define hdmi_height 480
#endif

#define ReqButNum 3

#define IsRearCamera 0

#define  FPS 10

#define PIXELFMT V4L2_PIX_FMT_YUYV
#define FIMC0CAPFMT V4L2_PIX_FMT_BGR32
#define CapDelay 100*1000


#define CLEAR(x)    memset(&(x), 0, sizeof(x))

#define MIN(x,y) ((x) < (y) ? (x) : (y))
#define MAX(x,y) ((x) > (y) ? (x) : (y))
#define CLAMP(x,l,h) ((x) < (l) ? (l) : ((x) > (h) ? (h) : (x)))

#define ERRSTR strerror(errno)
#define debug
#define LOG(...) fprintf(stderr, __VA_ARGS__)

#define ERR(...) __info("Error", __FILE__, __LINE__, __VA_ARGS__)
#define ERR_ON(cond, ...) ((cond) ? ERR(__VA_ARGS__) : 0)

#define CRIT(...) \
	do { \
		__info("Critical", __FILE__, __LINE__, __VA_ARGS__); \
		exit(EXIT_FAILURE); \
	} while(0)
#define CRIT_ON(cond, ...) do { if (cond) CRIT(__VA_ARGS__); } while(0)
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

typedef struct
{
	void *start;
	int length;
	int bytesused;
}BUFTYPE;
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

	
char lcd_path[] = "/dev/fb0";
char fimc0_path[] = "/dev/video0";
char cam_path[] = "/dev/video13";
char hdmi_path[] = "/dev/video10";


static sem_t lcd_sem;
static sem_t hdmi_sem;
static sem_t fimc0_sem;
BUFTYPE *fimc0_out_buf;
BUFTYPE *buffers;
static int n_buffer = 0;
void *fimc_in = NULL;
void *fimc_out = NULL;

int fimc0_out_buf_length;
int fimc0_cap_buf_length;
void *fimc0_out[16];
void *fimc0_cap[16];

static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;
static int lcd_buf_size;
static char *fb_buf = NULL;
static int tsp_fd;
static int fimc0_fd;
static int hdmi_fd;
static int hdmi_index = 16;
static pthread_t capture_tid;
static pthread_t display_tid; 
	int lcd_fd;
	int cam_fd;
int display_x = 0;
int display_y = 0;
static int fimc0_cap_index = 0;
char *temp_buf=NULL;
int display_format(int pixelformat)
{
			debug("{pixelformat = %c%c%c%c}\n",
				pixelformat & 0xff,(pixelformat >> 8)&0xff,
				(pixelformat >> 16) & 0xff,(pixelformat >> 24)&0xff
				);
}
static inline int __info(const char *prefix, const char *file, int line,
	const char *fmt, ...)
{
	int errsv = errno;
	va_list va;

	va_start(va, fmt);
	fprintf(stderr, "%s(%s:%d): ", prefix, file, line);
	vfprintf(stderr, fmt, va);
	va_end(va);
	errno = errsv;

	return 1;
}


struct format {
	unsigned long fourcc;
	unsigned long width;
	unsigned long height;
};
void dump_format(char *str, struct v4l2_format *fmt)
{
	if (V4L2_TYPE_IS_MULTIPLANAR(fmt->type)) {
		struct v4l2_pix_format_mplane *pix = &fmt->fmt.pix_mp;
		LOG("%s: width=%u height=%u format=%.4s bpl=%u\n", str,
			pix->width, pix->height, (char*)&pix->pixelformat,
			pix->plane_fmt[0].bytesperline);
	} else {
		struct v4l2_pix_format *pix = &fmt->fmt.pix;
		LOG("%s: width=%u height=%u format=%.4s bpl=%u\n", str,
			pix->width, pix->height, (char*)&pix->pixelformat,
			pix->bytesperline);
	}
}
int open_camera_device()
{
	int fd;

	if((fd = open(cam_path,O_RDWR | O_NONBLOCK)) < 0)
	{
		perror("Fail to open");
		exit(EXIT_FAILURE);
	} 
	cam_fd = fd;
	if((fimc0_fd = open(fimc0_path,O_RDWR | O_NONBLOCK)) < 0)
	{
		perror("Fail to open");
		exit(EXIT_FAILURE);
	} 
	
	debug("open cam success %d\n",fd);
	return fd;
}
int open_hdmi_device()
{
	int fd;

	if((fd = open(hdmi_path,O_RDWR)) < 0)
	{
		perror("Fail to open");
		exit(EXIT_FAILURE);
	} 
	hdmi_fd = fd;

	debug("open hdmi success %d\n",fd);
	return fd;
}


int open_lcd_device()
{
	int fd;
	int err;
	int ret;
	if((fd = open(lcd_path, O_RDWR | O_NONBLOCK)) < 0)
	{
		perror("Fail to open");
		exit(EXIT_FAILURE);
	} 
	debug("open lcd success %d\n",fd);

	if(-1 == ioctl(fd, FBIOGET_FSCREENINFO,&finfo))
	{
		perror("Fail to ioctl:FBIOGET_FSCREENINFO\n");
		exit(EXIT_FAILURE);
	}
	if (-1==ioctl(fd, FBIOGET_VSCREENINFO, &vinfo)) 
	{
		perror("Fail to ioctl:FBIOGET_VSCREENINFO\n");
		exit(EXIT_FAILURE);
	}
    lcd_buf_size = vinfo.xres * vinfo.yres * vinfo.bits_per_pixel / 8;	
	debug("vinfo.xres:%d, vinfo.yres:%d, vinfo.bits_per_pixel:%d, lcd_buf_size:%d, finfo.line_length:%d\n",vinfo.xres, vinfo.yres, vinfo.bits_per_pixel, lcd_buf_size, finfo.line_length); 


	lcd_fd = fd;
	
	vinfo.activate = FB_ACTIVATE_FORCE;
	vinfo.yres_virtual = vinfo.yres;
	ret = ioctl(fd, FBIOPUT_VSCREENINFO, &vinfo );
	if( ret < 0 )
	{
		debug( "ioctl FBIOPUT_VSCREENINFO failed\n");
		return -1;
	}
	
    //mmap framebuffer    
    fb_buf = (char *)mmap(
	    NULL,
	    lcd_buf_size,
	    PROT_READ | PROT_WRITE,MAP_SHARED ,
	    lcd_fd, 
	    0);    
	if(NULL == fb_buf)
	{
		perror("Fail to mmap fb_buf");
		exit(EXIT_FAILURE);
	}
	ret = ioctl( lcd_fd, FBIOBLANK, FB_BLANK_UNBLANK );
	if( ret < 0 )
	{
			debug( "ioctl FBIOBLANK failed\n");
			return -1;
	}
	
	return fd;
}
int fb_wait_for_vsync(int lcd_fd)
{
	int ret;
	unsigned long temp;

	ret = ioctl(lcd_fd, FBIO_WAITFORVSYNC, &temp);
	if (ret < 0) {
		err("Wait for vsync failed");
		return -1;
	}
	return 0;
}
int cam_reqbufs()
{
	struct v4l2_requestbuffers req;
	int i;
	debug("%s: +\n", __func__);
	int n_buffers = 0;
	CLEAR(req);

	req.count  = ReqButNum;
	req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_USERPTR;

	if (-1 == ioctl(cam_fd, VIDIOC_REQBUFS, &req)) {
		if (EINVAL == errno) {
			fprintf(stderr, "%s does not support "
				 "user pointer i/o\n", "campture");
			exit(EXIT_FAILURE);
		} else {
			debug("VIDIOC_REQBUFS");
			exit(EXIT_FAILURE);
		}
	}

	buffers = calloc(ReqButNum, sizeof(*buffers));

	if (!buffers) {
		fprintf(stderr, "Out of memory\n");
		exit(EXIT_FAILURE);
	}

	for (n_buffers = 0; n_buffers < ReqButNum; ++n_buffers) {
		buffers[n_buffers].length = fimc0_out_buf_length;
		buffers[n_buffers].start = malloc(fimc0_out_buf_length);

		if (!buffers[n_buffers].start) {
			fprintf(stderr, "Out of memory\n");
			exit(EXIT_FAILURE);
		}
	}
	debug("%s: -\n", __func__);
}
int set_ctrl(int fd, int cmd, int value)
{
	struct v4l2_control ctrl;
	CLEAR(ctrl);
	
	ctrl.id = cmd;
	ctrl.value = value;
	
	if(-1 == ioctl(fd,VIDIOC_S_CTRL,&ctrl))
	{
		debug("Can't set the ctrl:%d\n",__LINE__);
		perror("Fail to ioctl\n");
		exit(EXIT_FAILURE);
	}
	return 0;
}
int fimc0_reqbufs()
{
	int i = 0;
	int err;
	int ret;
	struct v4l2_control ctrl;
	struct v4l2_requestbuffers reqbuf;
	struct v4l2_requestbuffers rb;
	CLEAR(rb);
	/* enqueue the dmabuf to vivi */
	struct v4l2_buffer b;
	CLEAR(b);


	debug("%s: +\n", __func__);
		/* request buffers for FIMC0 */
	rb.count = ReqButNum;
	rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	rb.memory = V4L2_MEMORY_MMAP;
	ret = ioctl(fimc0_fd, VIDIOC_REQBUFS, &rb);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_REQBUFS: %s\n", ERRSTR))
		return -errno;
	debug("fimc0 output_buf_num:%d\n",rb.count);
	int n;

	n_buffer = rb.count;

	fimc0_out_buf = calloc(rb.count,sizeof(*fimc0_out_buf));
	if(fimc0_out_buf == NULL){
		fprintf(stderr,"Out of memory\n");
		exit(EXIT_FAILURE);
	}

	
		/* mmap DMABUF */
	struct v4l2_plane plane[2];
#if 1
	CLEAR(plane);
	CLEAR(b);
	b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = 0;
	b.m.planes = plane;
	b.length = 1;

	for (n = 0; n < ReqButNum; ++n) {
		b.index = n;
		ret = ioctl(fimc0_fd, VIDIOC_QUERYBUF, &b);
		
		if (ERR_ON(ret < 0, "fimc0: VIDIOC_REQBUFS: %s\n", ERRSTR))
		exit(EXIT_FAILURE);
		
		//	debug("fimc0 querybuf:%d,%d\n", b.m.planes[0].length, b.m.planes[0].m.mem_offset);
			fimc0_out_buf[n].start = mmap(NULL,
						b.m.planes[0].length,
						PROT_READ | PROT_WRITE,
						MAP_SHARED, fimc0_fd,
						b.m.planes[0].m.mem_offset);


		
		//	fimc0_out_buf[n].start = fimc0_out[n];
			fimc0_out_buf[n].length = b.m.planes[0].length;
				if (fimc0_out[n] == MAP_FAILED) {
				debug("Failed mmap buffer %d for %d\n", n,
							fimc0_fd);
				return -1;
			}

		fimc0_out_buf_length = b.m.planes[0].length;
		debug("fimc0 querybuf:0x%08lx,%d,%d\n", fimc0_out_buf[n], fimc0_out_buf_length, b.m.planes[0].m.mem_offset);
		
	//	debug("fimc0 output:plane.length:%d\n",fimc0_out_buf_length);
	}
#endif
	CLEAR(plane);
	CLEAR(b);

	rb.count = ReqButNum;
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	rb.memory = V4L2_MEMORY_MMAP;
	ret = ioctl(fimc0_fd, VIDIOC_REQBUFS, &rb);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_REQBUFS: %s\n", ERRSTR))
		return -errno;

	for (n = 0; n < ReqButNum; ++n) {
	
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = n;
		b.m.planes = plane;
		b.length = 1;

		b.index = n;
		ret = ioctl(fimc0_fd, VIDIOC_QUERYBUF, &b);
		if (ERR_ON(ret < 0, "fimc0: VIDIOC_QUERYBUF: %s\n", ERRSTR))
			return -errno;

		fimc0_cap[n] = mmap(NULL,
						b.m.planes[0].length,
						PROT_READ | PROT_WRITE,
						MAP_SHARED, fimc0_fd,
						b.m.planes[0].m.mem_offset);
			if (fimc0_cap[n] == MAP_FAILED) {
				debug("Failed mmap buffer %d for %d\n", n,
							fimc0_fd);
				return -1;
			}

		fimc0_cap_buf_length = b.m.planes[0].length;
		debug("fimc0 capture:plane.length:%d\n",fimc0_cap_buf_length);	
	}

	debug("%s -\n", __func__);
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
	debug("%s +\n", __func__);	
	ret = ioctl(hdmi_fd, VIDIOC_REQBUFS, &rqbufs);	
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

	debug("%s -\n", __func__);
	return 0;

}
int cam_setfmt()
{
	int err;
	int ret;
	struct v4l2_fmtdesc fmt;
	struct v4l2_capability cap;
	struct v4l2_format stream_fmt;
	struct v4l2_input input;
	struct v4l2_control ctrl;
	struct v4l2_streamparm stream;
	
	memset(&fmt,0,sizeof(fmt));
	fmt.index = 0;
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	while((ret = ioctl(cam_fd,VIDIOC_ENUM_FMT,&fmt)) == 0)
	{
		fmt.index ++ ;
		debug("{pixelformat = %c%c%c%c},description = '%s'\n",
				fmt.pixelformat & 0xff,(fmt.pixelformat >> 8)&0xff,
				(fmt.pixelformat >> 16) & 0xff,(fmt.pixelformat >> 24)&0xff,
				fmt.description);
	}
	ret = ioctl(cam_fd,VIDIOC_QUERYCAP,&cap);
	if(ret < 0){
		perror("FAIL to ioctl VIDIOC_QUERYCAP");
		exit(EXIT_FAILURE);
	}

	if(!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
	{
		debug("The Current device is not a video capture device\n");
		exit(EXIT_FAILURE);
	
	}

	if(!(cap.capabilities & V4L2_CAP_STREAMING))
	{
		debug("The Current device does not support streaming i/o\n");
		exit(EXIT_FAILURE);
	}

	CLEAR(stream_fmt);
	stream_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	stream_fmt.fmt.pix.width = CapWidth;
	stream_fmt.fmt.pix.height = CapHeight;
	stream_fmt.fmt.pix.pixelformat = PIXELFMT;
	stream_fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;

	if(-1 == ioctl(cam_fd,VIDIOC_S_FMT,&stream_fmt))
	{
		debug("Can't set the fmt\n");
		perror("Fail to ioctl\n");
		exit(EXIT_FAILURE);
	}
	debug("VIDIOC_S_FMT successfully\n");
	
	debug("%s: -\n", __func__);
	return 0;
}
int hdmi_setfmt()
{
	int ret;

#ifdef HDMI1080P
	struct v4l2_dv_preset presetinfo;
	memset(&presetinfo, 0, sizeof(presetinfo)); 
	presetinfo.preset = 1;//1080P@60hz
	if((ret = ioctl(hdmi_fd, VIDIOC_S_DV_PRESET, &presetinfo))<0)
	{
		printf("VIDIOC_G_DV_PRESET error:%d\n",ret);
		return -1;
	}
	printf("preset:%d\n",presetinfo.preset);
#endif	
	/* configure desired image size */	
	struct v4l2_format fmt;	
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;	
	fmt.fmt.pix.width = hdmi_width;	
	fmt.fmt.pix.height = hdmi_height;	
	debug("%s: +\n", __func__);
	/* format is hardcoded: draw procedures work only in 32-bit mode */	
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_BGR32;	
	ret = ioctl(hdmi_fd, VIDIOC_S_FMT, &fmt);	
	BYE_ON(ret < 0, "VIDIOC_S_FMT failed: %s\n", ERRSTR);

	/* update format struct to values adjusted by a driver */	
	ret = ioctl(hdmi_fd, VIDIOC_G_FMT, &fmt);	
	BYE_ON(ret < 0, "VIDIOC_G_FMT failed: %s\n", ERRSTR);

	/* crop output area on display */
	struct v4l2_crop crop;
	crop.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	crop.c.left = 0;
	crop.c.top = 0;
	crop.c.width = hdmi_width;
	crop.c.height = hdmi_height;
	ret = ioctl(hdmi_fd, VIDIOC_S_CROP, &crop);
	BYE_ON(ret < 0, "VIDIOC_S_CROP failed: %s\n", ERRSTR);
	
	debug("%s: -\n", __func__);
	return 0;
}



int cam_setrate()
{
	int err;
	int ret;

	struct v4l2_streamparm stream;

	CLEAR(stream);
    stream.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    stream.parm.capture.capturemode = 0;
    stream.parm.capture.timeperframe.numerator = 1;
    stream.parm.capture.timeperframe.denominator = FPS;

    err = ioctl(cam_fd, VIDIOC_S_PARM, &stream);
	if(err < 0)
    debug("FimcV4l2 start: error %d, VIDIOC_S_PARM", err);

	return 0;

}
int fimc0_setfmt()
{
	int err;
	int ret;
	struct v4l2_fmtdesc fmt;
	struct v4l2_capability cap;
	struct v4l2_format stream_fmt;
	struct v4l2_input input;
	struct v4l2_control ctrl;
	struct v4l2_streamparm stream;

	debug("%s: +\n", __func__);
	CLEAR(stream_fmt);
	stream_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	stream_fmt.fmt.pix.width = CapWidth;
	stream_fmt.fmt.pix.height = CapHeight;
	stream_fmt.fmt.pix.pixelformat = PIXELFMT;
	stream_fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;
		/* get format from VIVI */
	ret = ioctl(cam_fd, VIDIOC_G_FMT, &stream_fmt);
	if (ERR_ON(ret < 0, "vivi: VIDIOC_G_FMT: %s\n", ERRSTR))
		return -errno;
	dump_format("cam_fd-capture", &stream_fmt);


		/* setup format for FIMC 0 */
	/* keep copy of format for to-mplane conversion */
	
	struct v4l2_pix_format pix = stream_fmt.fmt.pix;

	CLEAR(stream_fmt);
	stream_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	struct v4l2_pix_format_mplane *pix_mp = &stream_fmt.fmt.pix_mp;

	pix_mp->width = pix.width;
	pix_mp->height = pix.height;
	pix_mp->pixelformat = pix.pixelformat;
	pix_mp->num_planes = 1;
	pix_mp->plane_fmt[0].bytesperline = pix.bytesperline;

	dump_format("fimc0-output", &stream_fmt);
	ret = ioctl(fimc0_fd, VIDIOC_S_FMT, &stream_fmt);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_S_FMT: %s\n", ERRSTR))
		return -errno;
	dump_format("fimc0-output", &stream_fmt);
	
		/* set format on fimc0 capture */
	stream_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	/* try cmdline format, or use fimc0-output instead */

	struct v4l2_pix_format_mplane *pix_mp_f = &stream_fmt.fmt.pix_mp;
	CLEAR(*pix_mp_f);
	pix_mp_f->pixelformat = V4L2_PIX_FMT_RGB32;
	pix_mp_f->width = hdmi_width;
	pix_mp_f->height = hdmi_height;
	pix_mp_f->plane_fmt[0].bytesperline = 0;


	dump_format("pre-fimc0-capture", &stream_fmt);
	ret = ioctl(fimc0_fd, VIDIOC_S_FMT, &stream_fmt);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_S_FMT: %s\n", ERRSTR))
		return -errno;
/*
	 CLEAR(ctrl);
   // ctrl.id = V4L2_CID_ALPHA_COMPONENT;
	ctrl.id = V4L2_CID_BLUE_BALANCE;
	ctrl.value = 0;
	 ret = ioctl(fimc0_fd, VIDIOC_G_CTRL, &ctrl);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_G_CTRL: %s\n", ERRSTR))
		return -errno;
	printf("V4L2_CID_ALPHA_COMPONENT:%d\n",ctrl.value);

*/
	set_ctrl(fimc0_fd, V4L2_CID_HFLIP, 1);
	set_ctrl(fimc0_fd, V4L2_CID_VFLIP, 0);
	debug("%s -\n", __func__);

}
int init_device()
{
	cam_setfmt();
	fimc0_setfmt();
	hdmi_setfmt();
	fimc0_reqbufs();
	hdmi_reqbufs();
	cam_reqbufs();
	cam_setrate();

	debug("%s -\n", __func__);

}
int hdmi_streamon()
{
	enum v4l2_buf_type type;
	int ret;
	int i;
	for(i = 0; i < ReqButNum; i++)
	hdmi_qbuf(i);
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;	
	ret = ioctl(hdmi_fd, VIDIOC_STREAMON, &type);	
	BYE_ON(ret, "VIDIOC_STREAMON failed: %s\n", ERRSTR);
	return 0;

}
int hdmi_streamoff()
{
	enum v4l2_buf_type type;
	int ret;
	int i;
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;	
	ret = ioctl(hdmi_fd, VIDIOC_STREAMOFF, &type);	
	BYE_ON(ret, "VIDIOC_STREAMON failed: %s\n", ERRSTR);
	return 0;

}
int start_capturing()
{
	unsigned int i;
	enum v4l2_buf_type type;
		int ret;
	struct v4l2_buffer b;
	struct v4l2_plane plane;
	debug("%s +\n", __func__);
	
	for(i = 0;i < n_buffer;i ++)
	{
		struct v4l2_buffer buf;

		CLEAR(buf);	
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;	
		buf.memory = V4L2_MEMORY_USERPTR;	
		buf.index = i;	
		buf.m.userptr = (unsigned long)buffers[i].start;
		buf.length = buffers[i].length;
		debug("cam qbuf:%d,userptr:0x%08x,length:%d\n",i, buf.m.userptr, buf.length);
		if(-1 == ioctl(cam_fd,VIDIOC_QBUF,&buf))
		{
			perror("cam Fail to ioctl 'VIDIOC_QBUF'");
			exit(EXIT_FAILURE);
		}
	}

	CLEAR(plane);
	CLEAR(b);
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = 0;
	b.m.planes = &plane;
	b.length = 1;
	for(i = 0;i < n_buffer;i ++)
	{
		b.index = i;
		ret = ioctl(fimc0_fd, VIDIOC_QBUF, &b);
		if (ERR_ON(ret < 0, "fimc0: VIDIOC_QBUF: %s\n", ERRSTR))
			return -errno;	

	}

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if(-1 == ioctl(cam_fd,VIDIOC_STREAMON,&type))
	{
		debug("i = %d.\n",i);
		perror("cam_fd Fail to ioctl 'VIDIOC_STREAMON'");
		exit(EXIT_FAILURE);
	}
	
			/* start processing */
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ret = ioctl(fimc0_fd, VIDIOC_STREAMON, &type);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_STREAMON: %s\n", ERRSTR))
		return -errno;
/*
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	ret = ioctl(fimc0_fd, VIDIOC_STREAMON, &type);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_STREAMON: %s\n", ERRSTR))
		return -errno;
*/
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	ret = ioctl(fimc0_fd, VIDIOC_STREAMON, &type);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_STREAMON: %s\n", ERRSTR))
		return -errno;
	debug("%s -\n", __func__);
	hdmi_streamon();
	return 0;
}
int cam_cap_dbuf(int *index)
{
	
	unsigned int i;
	enum v4l2_buf_type type;
	int ret;
	struct v4l2_buffer buf;

	bzero(&buf,sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_USERPTR;
	if(-1 == ioctl(cam_fd,VIDIOC_DQBUF,&buf))
	{
		perror("Fail to ioctl 'VIDIOC_DQBUF'");
		exit(EXIT_FAILURE);
	}
	buffers[buf.index].bytesused = buf.bytesused;

	*index = buf.index;

//	debug("%s -\n", __func__);
	return 0;

}
int cam_cap_qbuf(int index)
{
	struct v4l2_buffer buf;

		bzero(&buf,sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_USERPTR;
		buf.index = index;

		buf.m.userptr = (unsigned long)buffers[index].start;
		buf.length = buffers[index].length;
	//	debug("cam qbuf:%d,userptr:0x%08x,length:%d\n",i, buf.m.userptr, buf.length);
		if(-1 == ioctl(cam_fd,VIDIOC_QBUF,&buf))
		{
			perror("cam Fail to ioctl 'VIDIOC_QBUF'");
			exit(EXIT_FAILURE);
		}
//		debug("%s -\n", __func__);

	return 0;
}
int process_image(void *addr,int length);
int fimc0_out_qbuf(int index)
{
	unsigned int i;
	enum v4l2_buf_type type;
	int ret;
	struct v4l2_buffer b, buf;
	struct v4l2_plane plane[3];
	
	/* enqueue buffer to fimc0 output */
	CLEAR(plane);
	CLEAR(b);
	b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = index;
	b.m.planes = plane;
	b.length = 1;
	if(b.memory == V4L2_MEMORY_USERPTR)
	{
		plane[0].m.userptr = (unsigned long)fimc0_out_buf[index].start;
		plane[0].length = (unsigned long)fimc0_out_buf[index].length;
		plane[0].bytesused = fimc0_out_buf[index].length;
	}
	else
	{
		memcpy(fimc0_out_buf[index].start, buffers[index].start, buffers[index].length);

	}

//	debug("fimc0_out_buf:0x%08lx,length:%d,byteused:%d\n",fimc0_out_buf[index].start, 	fimc0_out_buf[index].length, fimc0_out_buf[index].bytesused);
	//process_image(fimc0_out_buf[index].start,0);	
	ret = ioctl(fimc0_fd, VIDIOC_QBUF, &b);
	
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_QBUF: %s\n", ERRSTR))
		return -errno;	
//	debug("%s -\n", __func__);
}

int fimc0_out_dbuf(int *index)
{

	unsigned int i;
	enum v4l2_buf_type type;
	int ret;
	struct v4l2_buffer b, buf;
	struct v4l2_plane plane[3];
	
	/* enqueue buffer to fimc0 output */
	CLEAR(plane);
	CLEAR(b);
	b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
//	b.index = index;
	b.m.planes = plane;
	b.length = 1;
//	plane[0].m.userptr = (unsigned long)fimc0_out_buf[index].start;
//	plane[0].length = (unsigned long)fimc0_out_buf[index].length;
//	planes[0].bytesused = fimc0_out_buf[buf.index].length;
	ret = ioctl(fimc0_fd, VIDIOC_DQBUF, &b);
	*index = b.index;
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_DQBUF: %s\n", ERRSTR))
		return -errno;	
}

int fimc0_cap_dbuf(int *index)
{

	unsigned int i,j;

	enum v4l2_buf_type type;
	int ret;
	struct v4l2_buffer b, buf;
	struct v4l2_plane plane[3];
	static int count = 0;
	/* enqueue buffer to fimc0 output */
	CLEAR(plane);
	CLEAR(b);
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
//	b.index = index;
	b.m.planes = plane;
	b.length = 1;
	ret = ioctl(fimc0_fd, VIDIOC_DQBUF, &b);
	*index = b.index;
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_DQBUF: %s\n", ERRSTR))
		return -errno;	
	fimc0_cap_index = b.index;
	count ++;
//	memcpy((void *)temp_buf, (void *)fimc0_cap[b.index], 800*480*4);
	if(hdmi_index != 16)
	{
		unsigned int tmp;
		unsigned int *s;
		unsigned int *d;
	//	s =  (unsigned int *)fimc0_cap[b.index];
	//	d =  (unsigned int *)hdmi_buffer[hdmi_index].data;
		memcpy((void *)hdmi_buffer[hdmi_index].data, (void *)fimc0_cap[b.index], hdmi_width*hdmi_height*4);
	/*	j = hdmi_width*hdmi_height;
		while(j--)
			*d++ = 0xff000000 | *s++;
	*/
			
			sem_post(&lcd_sem);
	}

//	debug("%s,%d\n",__func__, count);
	return 0;
}

int fimc0_cap_qbuf(int index)
{
//	int *pdata = (int *)addr;
	int ret;
	struct v4l2_buffer b;
	struct v4l2_plane plane;
	static unsigned int count = 0;
	//sleep(0);
//	debug("%s +\n", __func__);
		CLEAR(plane);
	CLEAR(b);
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.m.planes = &plane;
	b.length = 1;
	b.index = index;
	
	ret = ioctl(fimc0_fd, VIDIOC_QBUF, &b);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_QBUF: %s\n", ERRSTR))
		return -errno;
//	debug("%s -\n", __func__);
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

void process_cam_to_fimc0()
{
	int index;
//	debug("%s +\n", __func__);
	cam_cap_dbuf(&index);
	fimc0_out_qbuf(index);
//	debug("%s -,index:%d\n",__func__, index);
}
void process_fimc0_to_cam()
{
	int index;
//	debug("%s +\n", __func__);
	fimc0_out_dbuf(&index);
	cam_cap_qbuf(index);
//	debug("%s -,index:%d\n",__func__, index);
}

int process_fimc0_capture()
{
//	int *pdata = (int *)addr;
	int ret;
	struct v4l2_buffer b;
	struct v4l2_plane plane;
	static unsigned int count = 0;
	//sleep(0);
//	debug("%s +\n", __func__);
		CLEAR(plane);
	CLEAR(b);
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.m.planes = &plane;
	b.length = 1;

	/* grab processed buffers */
	ret = ioctl(fimc0_fd, VIDIOC_DQBUF, &b);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_DQBUF: %s\n", ERRSTR))
		return -errno;
	count ++;
	debug("%s,%d\n",__func__, count);

	ret = ioctl(fimc0_fd, VIDIOC_QBUF, &b);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_QBUF: %s\n", ERRSTR))
		return -errno;
//	debug("%s -\n", __func__);
	//memcpy(fb_buf, fimc_out, 640)	

}
void process_fimc0_to_hdmi()
{
	int index;
	//static int count = 0;
	
	debug("%s +\n", __func__);
	fimc0_cap_dbuf(&index);
	hdmi_qbuf(index);
	//usleep(1000);
	//if(count == 1)
		//hdmi_streamon();
	//count ++;
	
	debug("%s -,index:%d\n",__func__, index);
}
void process_hdmi_to_fimc0()
{
	int index;
	debug("%s +\n", __func__);
	
	hdmi_dbuf(&index);
	fimc0_cap_qbuf(index);
	debug("%s -,index:%d\n",__func__, index);
}
int mainloop(int cam_fd)
{ 
	int count = 1;//CapNum;
	clock_t startTime, finishTime;
	double selectTime, frameTime;
	struct pollfd fds[3];
	int nfds = 0;

	while(count++  > 0)
	{
		{
			struct timeval tv;
			int r;
			struct timeval start;
			struct timeval end;
			int time_use=0;
			gettimeofday(&start,NULL);
			
			fds[0].events |= POLLIN | POLLPRI;
			fds[0].fd = cam_fd;

			fds[1].events |= POLLIN | POLLPRI | POLLOUT;
			fds[1].fd = fimc0_fd;
			//++nfds;

			fds[2].events |= POLLIN | POLLPRI | POLLOUT;
			fds[2].fd = hdmi_fd;
			
			r = poll(fds, 2, -1);
			if(-1 == r)
			{
				if(EINTR == errno)
					continue;
				
				perror("Fail to select");
				exit(EXIT_FAILURE);
			}
			if(0 == r)
			{
				fprintf(stderr,"select Timeout\n");
				exit(EXIT_FAILURE);
			}

			if (fds[0].revents & POLLIN)
			{
				process_cam_to_fimc0();
				gettimeofday(&end,NULL);
				time_use=(end.tv_sec-start.tv_sec)*1000000+(end.tv_usec-start.tv_usec);
			//	debug("time_use is %dms\n",time_use/1000);
			}
			if (fds[1].revents & POLLIN)
			{
		//		debug("fimc0 has data to read\n");
				int index;
				fimc0_cap_dbuf(&index);
				sem_wait(&fimc0_sem);
				fimc0_cap_qbuf(index);
			}
			if (fds[1].revents & POLLOUT)
			{
				process_fimc0_to_cam();
			}
			if (fds[2].revents & (POLLIN | POLLPRI | POLLOUT))
			{
				debug("hdmi has data to read\n");
			//	fimc0_cap_qbuf(fimc0_cap_index);
			}
			if (fds[2].revents & POLLOUT)
			{
				//process_hdmi_to_fimc0();
				debug("hdmi has data to write\n");
			}
			//usleep(1000);
		}
	}
	return 0;
}

void cam_streamoff()
{
	enum v4l2_buf_type type;
	
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if(-1 == ioctl(cam_fd,VIDIOC_STREAMOFF,&type))
	{
		perror("Fail to ioctl 'VIDIOC_STREAMOFF'");
		exit(EXIT_FAILURE);
	}
	return;
}
int fimc0_streamoff()
{
	enum v4l2_buf_type type;
	int ret;
	/* start processing */
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ret = ioctl(fimc0_fd, VIDIOC_STREAMOFF, &type);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_STREAMON: %s\n", ERRSTR))
		return -errno;

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	ret = ioctl(fimc0_fd, VIDIOC_STREAMOFF, &type);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_STREAMON: %s\n", ERRSTR))
		return -errno;
	return;
}
void unmap_camer_device()
{
	unsigned int i;

	for(i = 0;i < n_buffer;i ++)
	{
		if(-1 == munmap(buffers[i].start, buffers[i].length))
		{
			exit(EXIT_FAILURE);
		}
	}
	if (-1 == munmap(fb_buf, lcd_buf_size)) 
	{          
		perror(" Error: framebuffer device munmap() failed.\n");          
		exit (EXIT_FAILURE) ;       
	}   
	free(buffers);
	free(fb_buf);

	return;
}
void unmap_fimc0_device()
{
	unsigned int i;

	for(i = 0;i < n_buffer;i ++)
	{
		if(-1 == munmap(fimc0_out_buf[i].start, fimc0_out_buf[i].length))
		{
			exit(EXIT_FAILURE);
		}
	}
	free(fimc0_out_buf);
	return;
}
void unmap_hdmi_device()
{
	unsigned int i;

	for(i = 0;i < ReqButNum;i ++)
	{
		if(-1 == munmap(hdmi_buffer[i].data, hdmi_buffer[i].size))
		{
			exit(EXIT_FAILURE);
		}
	}
//	free(hdmi_buffer);
	return;
}
void close_camer_device()
{
	if(-1 == close(lcd_fd))
	{
		perror("Fail to close lcd_fd");
		exit(EXIT_FAILURE);
	}
	if(-1 == close(cam_fd))
	{
		perror("Fail to close cam_fd");
		exit(EXIT_FAILURE);
	}
	if(-1 == close(hdmi_fd))
	{
		perror("Fail to close hdmi_fd");
		exit(EXIT_FAILURE);
	}
	return;
}
static void *cam_thread(void *pVoid)
{
	mainloop(cam_fd);
}
static void *display_thread(void *pVoid)
{
	static unsigned int count = 0;
	debug("display_thread start\n");

	int num = 800*480*4;
	while(1)
	{
		hdmi_dbuf(&hdmi_index);
	//	debug("display_thread:%d\n", hdmi_index);
		sem_wait(&lcd_sem);
		hdmi_qbuf(hdmi_index);
		hdmi_index = 16;
		sem_post(&fimc0_sem);
		count ++;
	}
}
int main()
{
	sem_init(&lcd_sem, 0, 0);
	sem_init(&hdmi_sem, 0, 0);
	sem_init(&fimc0_sem, 0, 0);
	temp_buf =(char *)malloc(800*480*4);
	open_lcd_device();
	open_camera_device();
	open_hdmi_device();
	init_device();
//	return 0;
	start_capturing();
	pthread_create(&capture_tid,NULL,cam_thread,(void *)NULL);  
	pthread_create(&display_tid,NULL,display_thread,(void *)NULL); 
	//while('q' != getchar())
	while(1)
	{
		sleep(1);
	}
	pthread_cancel(display_tid);
	pthread_cancel(capture_tid);
	hdmi_streamoff();
	fimc0_streamoff();
	cam_streamoff();
	unmap_camer_device();
	unmap_fimc0_device();
	unmap_hdmi_device();
	close_camer_device();
	return 0;
}
