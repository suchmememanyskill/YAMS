#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT   8080
#define WIDTH  1280
#define HEIGHT 720
#define FPS    30

struct buffer {
  void  *start;
  size_t length;
};

static volatile int keep_running = 1;

void error_exit(const char *s) {
  perror(s);
  exit(EXIT_FAILURE);
}

static void sig_handler(int signo) {
  if (signo == SIGINT || signo == SIGTERM) {
    keep_running = 0;
    printf("\nCaught signal, shutting down...\n");
  }
}

int main(int argc, char **argv) {
  const char *dev_name = "/dev/video0";
  if (argc > 1)
    dev_name = argv[1];

  // 1. Open device
  int fd = open(dev_name, O_RDWR | O_NONBLOCK, 0);
  if (fd < 0)
    error_exit("open video device");

  struct v4l2_capability cap;
  if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0)
    error_exit("VIDIOC_QUERYCAP");
  if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
    fprintf(stderr, "%s is not a video capture device\n", dev_name);
    exit(EXIT_FAILURE);
  }
  if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
    fprintf(stderr, "%s does not support streaming i/o\n", dev_name);
    exit(EXIT_FAILURE);
  }

  // 2. Set format
  struct v4l2_format fmt;
  memset(&fmt, 0, sizeof(fmt));
  fmt.type             = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width    = WIDTH;
  fmt.fmt.pix.height   = HEIGHT;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
  fmt.fmt.pix.field    = V4L2_FIELD_ANY;
  if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
    error_exit("VIDIOC_S_FMT");

  // Print actual set format (camera might adjust it to nearest supported)
  printf("Set format: %d x %d\n", fmt.fmt.pix.width, fmt.fmt.pix.height);

  // Optional: Set framerate
  struct v4l2_streamparm parm;
  memset(&parm, 0, sizeof(parm));
  parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  parm.parm.capture.timeperframe.numerator   = 1;
  parm.parm.capture.timeperframe.denominator = FPS;
  ioctl(fd, VIDIOC_S_PARM, &parm);

  // 3. Request buffers for Memory Mapping
  struct v4l2_requestbuffers req;
  memset(&req, 0, sizeof(req));
  req.count  = 4;
  req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0)
    error_exit("VIDIOC_REQBUFS");

  struct buffer *buffers = calloc(req.count, sizeof(*buffers));
  for (unsigned int i = 0; i < req.count; ++i) {
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = i;
    if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0)
      error_exit("VIDIOC_QUERYBUF");
    buffers[i].length = buf.length;
    buffers[i].start  = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                              MAP_SHARED, fd, buf.m.offset);
    if (buffers[i].start == MAP_FAILED)
      error_exit("mmap");
  }

  // 4. Queue buffers
  for (unsigned int i = 0; i < req.count; ++i) {
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = i;
    if (ioctl(fd, VIDIOC_QBUF, &buf) < 0)
      error_exit("VIDIOC_QBUF");
  }

  // 5. Start streaming from the camera
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(fd, VIDIOC_STREAMON, &type) < 0)
    error_exit("VIDIOC_STREAMON");

  // 6. Setup TCP Socket Server
  int server_fd, client_socket = -1;
  struct sockaddr_in address;
  int opt     = 1;
  socklen_t addrlen = sizeof(address);

  if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0)
    error_exit("socket failed");
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
    error_exit("setsockopt");

  // Make server socket non-blocking
  int flags = fcntl(server_fd, F_GETFL, 0);
  if (flags < 0 || fcntl(server_fd, F_SETFL, flags | O_NONBLOCK) < 0)
    error_exit("fcntl server_fd");

  address.sin_family      = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port        = htons(PORT);

  if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    error_exit("bind");
  if (listen(server_fd, 3) < 0)
    error_exit("listen");

  printf("Camera capture started.\nListening on port %d... Waiting for "
         "connection.\n",
         PORT);

  signal(SIGINT, sig_handler);
  signal(SIGTERM, sig_handler);
  signal(SIGPIPE, SIG_IGN); // Prevent crashing when client disconnects

  int    fresh_buf_idx = -1;
  int    send_buf_idx  = -1;
  int    send_state    = 0;
  size_t send_offset   = 0;
  char   boundary_buf[256];
  size_t boundary_len  = 0;
  size_t *bytesused    = calloc(req.count, sizeof(*bytesused)); // Track lengths per buffer
  if (!bytesused)
    error_exit("calloc bytesused");

  const char *http_header =
      "HTTP/1.0 200 OK\r\n"
      "Cache-Control: no-cache\r\n"
      "Pragma: no-cache\r\n"
      "Connection: close\r\n"
      "Content-Type: multipart/x-mixed-replace; boundary=mjpegstream\r\n\r\n";
  size_t http_header_len = strlen(http_header);

  while (keep_running) {
    fd_set rd_fds, wr_fds;
    FD_ZERO(&rd_fds);
    FD_ZERO(&wr_fds);
    FD_SET(fd, &rd_fds); // Always wait for camera
    int max_fd = fd;

    if (client_socket < 0) {
      FD_SET(server_fd, &rd_fds); // Wait for new client
      if (server_fd > max_fd)
        max_fd = server_fd;
    } else {
      FD_SET(client_socket, &rd_fds); // Detect disconnects/drain requests
      int want_to_send = 0;
      if (send_state > 0)
        want_to_send = 1;
      else if (fresh_buf_idx >= 0)
        want_to_send = 1; // ready to start a new frame
      if (want_to_send) {
        FD_SET(client_socket, &wr_fds);
      }
      if (client_socket > max_fd)
        max_fd = client_socket;
    }

    struct timeval tv = {2, 0}; // 2 second timeout
    int r = select(max_fd + 1, &rd_fds, &wr_fds, NULL, &tv);
    if (r == -1) {
      if (errno == EINTR)
        continue;
      perror("select");
      break;
    }
    if (r == 0)
      continue; // Timeout

    // 1. Handle new client connection
    if (client_socket < 0 && FD_ISSET(server_fd, &rd_fds)) {
      if ((client_socket = accept(server_fd, (struct sockaddr *)&address,
                                  &addrlen)) >= 0) {
        printf("Client connected!\n");
        int cflags = fcntl(client_socket, F_GETFL, 0);
        if (cflags < 0 || fcntl(client_socket, F_SETFL, cflags | O_NONBLOCK) < 0)
          error_exit("fcntl client_socket");
        send_state  = 1;
        send_offset = 0;
        // Discard any existing fresh frame so client starts with absolutely
        // newest data
        if (fresh_buf_idx >= 0) {
          struct v4l2_buffer ret_buf;
          memset(&ret_buf, 0, sizeof(ret_buf));
          ret_buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
          ret_buf.memory = V4L2_MEMORY_MMAP;
          ret_buf.index  = fresh_buf_idx;
          ioctl(fd, VIDIOC_QBUF, &ret_buf);
          fresh_buf_idx = -1;
        }
      }
    }

    // 2. Handle camera frame
    if (FD_ISSET(fd, &rd_fds)) {
      struct v4l2_buffer buf;
      memset(&buf, 0, sizeof(buf));
      buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      if (ioctl(fd, VIDIOC_DQBUF, &buf) == 0) {
        if (fresh_buf_idx >= 0) {
          // Client was too slow to even start sending this frame.
          // Return the old fresh one to V4L2 immediately, keep newest.
          struct v4l2_buffer old_buf;
          memset(&old_buf, 0, sizeof(old_buf));
          old_buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
          old_buf.memory = V4L2_MEMORY_MMAP;
          old_buf.index  = fresh_buf_idx;
          ioctl(fd, VIDIOC_QBUF, &old_buf);
        }
        fresh_buf_idx           = buf.index;
        bytesused[buf.index]    = buf.bytesused;
      } else if (errno != EAGAIN) {
        perror("VIDIOC_DQBUF");
        break;
      }
    }

    // 3. Client reading (drain HTTP requests, detect disconnects)
    if (client_socket >= 0 && FD_ISSET(client_socket, &rd_fds)) {
      char junk[1024];
      int  n = recv(client_socket, junk, sizeof(junk), 0);
      if (n <= 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        printf("Client disconnected.\n");
        close(client_socket);
        client_socket = -1;
        send_state    = 0;
        if (send_buf_idx >= 0) {
          struct v4l2_buffer ret_buf;
          memset(&ret_buf, 0, sizeof(ret_buf));
          ret_buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
          ret_buf.memory = V4L2_MEMORY_MMAP;
          ret_buf.index  = send_buf_idx;
          ioctl(fd, VIDIOC_QBUF, &ret_buf);
          send_buf_idx = -1;
        }
      }
    }

    // 4. Client writable
    if (client_socket >= 0 && FD_ISSET(client_socket, &wr_fds)) {
      if (send_state == 0 && fresh_buf_idx >= 0) {
        // We are idle and have a frame! Start sending.
        send_buf_idx  = fresh_buf_idx;
        fresh_buf_idx = -1; // Claim it
        send_state    = 2;  // skip HTTP header if already sent
        send_offset   = 0;
        boundary_len  = snprintf(boundary_buf, sizeof(boundary_buf),
                                 "--mjpegstream\r\n"
                                 "Content-Type: image/jpeg\r\n"
                                 "Content-Length: %zu\r\n\r\n",
                                 bytesused[send_buf_idx]);
      }

      int disconnect = 0;
      if (send_state == 1) { // HTTP HEADER
        ssize_t to_send = (ssize_t)(http_header_len - send_offset);
        ssize_t sent    = send(client_socket, http_header + send_offset,
                               (size_t)to_send, MSG_NOSIGNAL);
        if (sent > 0) {
          send_offset += (size_t)sent;
          if (send_offset == http_header_len) {
            send_state = 0; // Move to IDLE, waiting for frames
          }
        } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
          disconnect = 1;
      } else if (send_state == 2) { // BOUNDARY
        ssize_t to_send = (ssize_t)(boundary_len - send_offset);
        ssize_t sent    = send(client_socket, boundary_buf + send_offset,
                               (size_t)to_send, MSG_NOSIGNAL);
        if (sent > 0) {
          send_offset += (size_t)sent;
          if (send_offset == boundary_len) {
            send_state  = 3;
            send_offset = 0;
          }
        } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
          disconnect = 1;
      } else if (send_state == 3) { // PAYLOAD
        ssize_t to_send = (ssize_t)(bytesused[send_buf_idx] - send_offset);
        ssize_t sent    = send(client_socket,
                               (char *)buffers[send_buf_idx].start + send_offset,
                               (size_t)to_send, MSG_NOSIGNAL);
        if (sent > 0) {
          send_offset += (size_t)sent;
          if (send_offset == bytesused[send_buf_idx]) {
            send_state  = 4;
            send_offset = 0;
          }
        } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
          disconnect = 1;
      } else if (send_state == 4) { // FOOTER
        const char *footer  = "\r\n";
        ssize_t     to_send = (ssize_t)(2 - send_offset);
        ssize_t     sent    = send(client_socket, footer + send_offset,
                                   (size_t)to_send, MSG_NOSIGNAL);
        if (sent > 0) {
          send_offset += (size_t)sent;
          if (send_offset == 2) {
            send_state = 0; // IDLE
            // Return buffer to V4L2 so it can capture into it again
            struct v4l2_buffer ret_buf;
            memset(&ret_buf, 0, sizeof(ret_buf));
            ret_buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ret_buf.memory = V4L2_MEMORY_MMAP;
            ret_buf.index  = send_buf_idx;
            ioctl(fd, VIDIOC_QBUF, &ret_buf);
            send_buf_idx = -1;
          }
        } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
          disconnect = 1;
      }

      if (disconnect) {
        printf("Client disconnected during write.\n");
        close(client_socket);
        client_socket = -1;
        send_state    = 0;
        if (send_buf_idx >= 0) {
          struct v4l2_buffer ret_buf;
          memset(&ret_buf, 0, sizeof(ret_buf));
          ret_buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
          ret_buf.memory = V4L2_MEMORY_MMAP;
          ret_buf.index  = send_buf_idx;
          ioctl(fd, VIDIOC_QBUF, &ret_buf);
          send_buf_idx = -1;
        }
      }
    }
  }

  // Cleanup resources
  printf("Cleaning up resources...\n");
  if (client_socket >= 0)
    close(client_socket);
  close(server_fd);

  // Stop streaming
  enum v4l2_buf_type s_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ioctl(fd, VIDIOC_STREAMOFF, &s_type);

  // Unmap memory
  for (unsigned int i = 0; i < req.count; ++i) {
    munmap(buffers[i].start, buffers[i].length);
  }
  free(buffers);
  free(bytesused);
  close(fd);

  printf("Exiting safely.\n");
  return 0;
}
