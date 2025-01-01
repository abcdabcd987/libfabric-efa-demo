// clang-format off
/*
Example run:

server$ ./build/6_write
domain:  rdmap79s0-rdm, nic:  rdmap79s0, fabric: efa, link: 100Gbps
Run client with the following command:
  ./build/6_write fe8000000000000008e7effffeeee81d000000003e88df080000000000000000 [page_size num_pages]
Registered 1 buffer on cuda:0
------
Received CONNECT message from client:
  addr: fe80000000000000083425fffe7d535100000000057558100000000000000000
  MR[0]: addr=0x7f2440800000 size=16777216 rkey=0x000000000070000a
  MR[1]: addr=0x7f2442400000 size=16777216 rkey=0x0000000000a00031
Received RandomFill request from client:
  remote_context: 0x00000123
  seed: 0xb584035fabe6ce9b
  page_size: 1048576
  num_pages: 8
Generating random data..
Finished RDMA WRITE to the remote GPU memory.
------
^C

client$ ./build/6_write fe8000000000000008e7effffeeee81d000000003e88df080000000000000000
domain:  rdmap79s0-rdm, nic:  rdmap79s0, fabric: efa, link: 100Gbps
Registered 2 buffers on cuda:0
Sent CONNECT message to server
Sent RandomFillRequest to server. page_size: 1048576, num_pages: 8
Received RDMA WRITE to local GPU memory.
Data is correct
*/
// clang-format on

#include <algorithm>
#include <cuda.h>
#include <cuda_runtime.h>
#include <functional>
#include <inttypes.h>
#include <memory>
#include <netdb.h>
#include <pthread.h>
#include <random>
#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_rma.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <string_view>
#include <time.h>
#include <unistd.h>
#include <vector>

#define CHECK(stmt)                                                            \
  do {                                                                         \
    if (!(stmt)) {                                                             \
      fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, #stmt);                \
      std::exit(1);                                                            \
    }                                                                          \
  } while (0)

#define FI_CHECK(stmt)                                                         \
  do {                                                                         \
    int rc = (stmt);                                                           \
    if (rc) {                                                                  \
      fprintf(stderr, "%s:%d %s failed with %d (%s)\n", __FILE__, __LINE__,    \
              #stmt, rc, fi_strerror(-rc));                                    \
      std::exit(1);                                                            \
    }                                                                          \
  } while (0)

#define CUDA_CHECK(stmt)                                                       \
  do {                                                                         \
    cudaError_t rc = (stmt);                                                   \
    if (rc != cudaSuccess) {                                                   \
      fprintf(stderr, "%s:%d %s failed with %d (%s)\n", __FILE__, __LINE__,    \
              #stmt, rc, cudaGetErrorString(rc));                              \
      std::exit(1);                                                            \
    }                                                                          \
  } while (0)

#define CU_CHECK(stmt)                                                         \
  do {                                                                         \
    CUresult rc = (stmt);                                                      \
    if (rc != CUDA_SUCCESS) {                                                  \
      const char *err_str;                                                     \
      cuGetErrorString(rc, &err_str);                                          \
      fprintf(stderr, "%s:%d %s failed with %d (%s)\n", __FILE__, __LINE__,    \
              #stmt, rc, err_str);                                             \
      std::exit(1);                                                            \
    }                                                                          \
  } while (0)

constexpr size_t kBufAlign = 128; // EFA alignment requirement
constexpr size_t kMessageBufferSize = 8192;
constexpr size_t kCompletionQueueReadCount = 16;
constexpr size_t kMemoryRegionSize = 16 << 20;
constexpr size_t kEfaImmDataSize = 4;

struct Buffer;
struct Network;

struct EfaAddress {
  uint8_t bytes[32];

  explicit EfaAddress(uint8_t bytes[32]) { memcpy(this->bytes, bytes, 32); }

  std::string ToString() const {
    char buf[65];
    for (size_t i = 0; i < 32; i++) {
      snprintf(buf + 2 * i, 3, "%02x", bytes[i]);
    }
    return std::string(buf, 64);
  }

  static EfaAddress Parse(const std::string &str) {
    if (str.size() != 64) {
      fprintf(stderr, "Unexpected address length %zu\n", str.size());
      std::exit(1);
    }
    uint8_t bytes[32];
    for (size_t i = 0; i < 32; i++) {
      sscanf(str.c_str() + 2 * i, "%02hhx", &bytes[i]);
    }
    return EfaAddress(bytes);
  }
};

enum class RdmaOpType : uint8_t {
  kRecv = 0,
  kSend = 1,
  kWrite = 2,
  kRemoteWrite = 3,
};

struct RdmaRecvOp {
  Buffer *buf;
  fi_addr_t src_addr; // Set after completion
  size_t recv_size;   // Set after completion
};
static_assert(std::is_pod_v<RdmaRecvOp>);

struct RdmaSendOp {
  Buffer *buf;
  size_t len;
  fi_addr_t dest_addr;
};
static_assert(std::is_pod_v<RdmaSendOp>);

struct RdmaWriteOp {
  Buffer *buf;
  size_t offset;
  size_t len;
  uint32_t imm_data;
  uint64_t dest_ptr;
  fi_addr_t dest_addr;
  uint64_t dest_key;
};
static_assert(std::is_pod_v<RdmaWriteOp>);

struct RdmaRemoteWriteOp {
  uint32_t op_id;
};
static_assert(std::is_pod_v<RdmaRemoteWriteOp>);
static_assert(sizeof(RdmaRemoteWriteOp) <= kEfaImmDataSize);

struct RdmaOp {
  RdmaOpType type;
  union {
    RdmaRecvOp recv;
    RdmaSendOp send;
    RdmaWriteOp write;
    RdmaRemoteWriteOp remote_write;
  };
  std::function<void(Network &, RdmaOp &)> callback;
};

struct Network {
  struct fi_info *fi;
  struct fid_fabric *fabric;
  struct fid_domain *domain;
  struct fid_cq *cq;
  struct fid_av *av;
  struct fid_ep *ep;
  EfaAddress addr;

  std::unordered_map<void *, struct fid_mr *> mr;
  std::unordered_map<uint32_t, RdmaOp *> remote_write_ops;

  static Network Open(struct fi_info *fi);

  fi_addr_t AddPeerAddress(const EfaAddress &peer_addr);
  void RegisterMemory(Buffer &buf);
  struct fid_mr *GetMR(const Buffer &buf);

  void PollCompletion();
  void PostRecv(Buffer &buf,
                std::function<void(Network &, RdmaOp &)> &&callback);
  void PostSend(fi_addr_t addr, Buffer &buf, size_t len,
                std::function<void(Network &, RdmaOp &)> &&callback);
  void PostWrite(RdmaWriteOp &&write,
                 std::function<void(Network &, RdmaOp &)> &&callback);
  void AddRemoteWrite(uint32_t id,
                      std::function<void(Network &, RdmaOp &)> &&callback);
};

void *align_up(void *ptr, size_t align) {
  uintptr_t addr = (uintptr_t)ptr;
  return (void *)((addr + align - 1) & ~(align - 1));
}

struct Buffer {
  void *data;
  size_t size;
  int cuda_device;
  int dmabuf_fd;

  static Buffer Alloc(size_t size, size_t align) {
    void *raw_data = malloc(size);
    CHECK(raw_data != nullptr);
    return Buffer(raw_data, size, align, -1, -1);
  }

  static Buffer AllocCuda(size_t size, size_t align) {
    void *raw_data;
    struct cudaPointerAttributes attrs = {};
    CUDA_CHECK(cudaMalloc(&raw_data, size));
    CUDA_CHECK(cudaPointerGetAttributes(&attrs, raw_data));
    CHECK(attrs.type == cudaMemoryTypeDevice);
    int cuda_device = attrs.device;
    int fd = -1;
    CU_CHECK(cuMemGetHandleForAddressRange(
        &fd, (CUdeviceptr)align_up(raw_data, align), size,
        CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, 0));
    return Buffer(raw_data, size, align, cuda_device, fd);
  }

  bool is_cuda() const { return cuda_device >= 0; }

  Buffer(Buffer &&other)
      : data(other.data), size(other.size), cuda_device(other.cuda_device),
        dmabuf_fd(other.dmabuf_fd), raw_data(other.raw_data) {
    other.data = nullptr;
    other.raw_data = nullptr;
    other.size = 0;
    other.cuda_device = -1;
    other.dmabuf_fd = -1;
  }

  ~Buffer() {
    if (is_cuda()) {
      CUDA_CHECK(cudaFree(raw_data));
    } else {
      free(raw_data);
    }
  }

private:
  void *raw_data;

  Buffer(void *raw_data, size_t raw_size, size_t align, int cuda_device,
         int dmabuf_fd) {
    this->raw_data = raw_data;
    this->data = align_up(raw_data, align);
    this->size = (size_t)((uintptr_t)raw_data + raw_size - (uintptr_t)data);
    this->cuda_device = cuda_device;
    this->dmabuf_fd = dmabuf_fd;
  }
  Buffer(const Buffer &) = delete;
};

struct fi_info *GetInfo() {
  struct fi_info *hints, *info;
  hints = fi_allocinfo();
  hints->caps = FI_MSG | FI_RMA | FI_HMEM | FI_LOCAL_COMM | FI_REMOTE_COMM;
  hints->ep_attr->type = FI_EP_RDM;
  hints->fabric_attr->prov_name = strdup("efa");
  hints->domain_attr->mr_mode = FI_MR_LOCAL | FI_MR_HMEM | FI_MR_VIRT_ADDR |
                                FI_MR_ALLOCATED | FI_MR_PROV_KEY;
  hints->domain_attr->threading = FI_THREAD_SAFE;
  FI_CHECK(fi_getinfo(FI_VERSION(2, 0), nullptr, nullptr, 0, hints, &info));
  fi_freeinfo(hints);
  return info;
}

Network Network::Open(struct fi_info *fi) {
  struct fid_fabric *fabric;
  FI_CHECK(fi_fabric(fi->fabric_attr, &fabric, nullptr));

  struct fid_domain *domain;
  FI_CHECK(fi_domain(fabric, fi, &domain, nullptr));

  struct fid_cq *cq;
  struct fi_cq_attr cq_attr = {};
  cq_attr.format = FI_CQ_FORMAT_DATA;
  FI_CHECK(fi_cq_open(domain, &cq_attr, &cq, nullptr));

  struct fid_av *av;
  struct fi_av_attr av_attr = {};
  FI_CHECK(fi_av_open(domain, &av_attr, &av, nullptr));

  struct fid_ep *ep;
  FI_CHECK(fi_endpoint(domain, fi, &ep, nullptr));
  FI_CHECK(fi_ep_bind(ep, &cq->fid, FI_SEND | FI_RECV));
  FI_CHECK(fi_ep_bind(ep, &av->fid, 0));

  FI_CHECK(fi_enable(ep));

  uint8_t addr[64];
  size_t addrlen = sizeof(addr);
  FI_CHECK(fi_getname(&ep->fid, addr, &addrlen));
  if (addrlen != 32) {
    fprintf(stderr, "Unexpected address length %zu\n", addrlen);
    std::exit(1);
  }

  return Network{fi, fabric, domain, cq, av, ep, EfaAddress(addr)};
}

fi_addr_t Network::AddPeerAddress(const EfaAddress &peer_addr) {
  fi_addr_t addr = FI_ADDR_UNSPEC;
  int ret = fi_av_insert(av, peer_addr.bytes, 1, &addr, 0, nullptr);
  if (ret != 1) {
    fprintf(stderr, "fi_av_insert failed: %d\n", ret);
    std::exit(1);
  }
  return addr;
}

void Network::RegisterMemory(Buffer &buf) {
  struct fid_mr *mr;
  struct fi_mr_attr mr_attr = {
      .iov_count = 1,
      .access = FI_SEND | FI_RECV | FI_REMOTE_WRITE | FI_REMOTE_READ |
                FI_WRITE | FI_READ,
  };
  struct iovec iov = {.iov_base = buf.data, .iov_len = buf.size};
  struct fi_mr_dmabuf dmabuf = {
      .fd = buf.dmabuf_fd, .offset = 0, .len = buf.size, .base_addr = buf.data};
  uint64_t flags = 0;
  if (buf.is_cuda()) {
    mr_attr.iface = FI_HMEM_CUDA;
    mr_attr.device.cuda = buf.cuda_device;
    if (buf.dmabuf_fd != -1) {
      mr_attr.dmabuf = &dmabuf;
      flags = FI_MR_DMABUF;
    } else {
      mr_attr.mr_iov = &iov;
    }
  } else {
    mr_attr.mr_iov = &iov;
  }
  FI_CHECK(fi_mr_regattr(domain, &mr_attr, flags, &mr));
  this->mr[buf.data] = mr;
}

struct fid_mr *Network::GetMR(const Buffer &buf) {
  auto it = mr.find(buf.data);
  CHECK(it != mr.end());
  return it->second;
}

void Network::PostRecv(Buffer &buf,
                       std::function<void(Network &, RdmaOp &)> &&callback) {
  auto *op = new RdmaOp{
      .type = RdmaOpType::kRecv,
      .recv =
          RdmaRecvOp{.buf = &buf, .src_addr = FI_ADDR_UNSPEC, .recv_size = 0},
      .callback = std::move(callback),
  };
  struct iovec iov = {
      .iov_base = buf.data,
      .iov_len = buf.size,
  };
  struct fi_msg msg = {
      .msg_iov = &iov,
      .desc = &GetMR(buf)->mem_desc,
      .iov_count = 1,
      .addr = FI_ADDR_UNSPEC,
      .context = op,
  };
  FI_CHECK(fi_recvmsg(ep, &msg, 0)); // TODO: handle EAGAIN
}

void Network::PostSend(fi_addr_t addr, Buffer &buf, size_t len,
                       std::function<void(Network &, RdmaOp &)> &&callback) {
  CHECK(len <= buf.size);
  auto *op = new RdmaOp{
      .type = RdmaOpType::kSend,
      .send = RdmaSendOp{.buf = &buf, .len = len, .dest_addr = addr},
      .callback = std::move(callback),
  };
  struct iovec iov = {
      .iov_base = buf.data,
      .iov_len = len,
  };
  struct fi_msg msg = {
      .msg_iov = &iov,
      .desc = &GetMR(buf)->mem_desc,
      .iov_count = 1,
      .addr = addr,
      .context = op,
  };
  FI_CHECK(fi_sendmsg(ep, &msg, 0)); // TODO: handle EAGAIN
}

void Network::PostWrite(RdmaWriteOp &&write,
                        std::function<void(Network &, RdmaOp &)> &&callback) {
  auto *op = new RdmaOp{
      .type = RdmaOpType::kWrite,
      .write = std::move(write),
      .callback = std::move(callback),
  };
  struct iovec iov = {
      .iov_base = (uint8_t *)write.buf->data + write.offset,
      .iov_len = write.len,
  };
  struct fi_rma_iov rma_iov = {
      .addr = write.dest_ptr,
      .len = write.len,
      .key = write.dest_key,
  };
  struct fi_msg_rma msg = {
      .msg_iov = &iov,
      .desc = &GetMR(*write.buf)->mem_desc,
      .iov_count = 1,
      .addr = write.dest_addr,
      .rma_iov = &rma_iov,
      .rma_iov_count = 1,
      .context = op,
      .data = write.imm_data,
  };
  uint64_t flags = 0;
  if (write.imm_data) {
    flags |= FI_REMOTE_CQ_DATA;
  }
  FI_CHECK(fi_writemsg(ep, &msg, flags)); // TODO: handle EAGAIN
}

void Network::AddRemoteWrite(
    uint32_t id, std::function<void(Network &, RdmaOp &)> &&callback) {
  CHECK(remote_write_ops.count(id) == 0);
  auto *op = new RdmaOp{
      .type = RdmaOpType::kRemoteWrite,
      .remote_write = RdmaRemoteWriteOp{.op_id = id},
      .callback = std::move(callback),
  };
  remote_write_ops[id] = op;
}

void HandleCompletion(Network &net, const struct fi_cq_data_entry &cqe) {
  RdmaOp *op = nullptr;
  if (cqe.flags & FI_REMOTE_WRITE) {
    // REMOTE WRITE does not have op_context
    // NOTE(lequn): EFA only supports 4 bytes of immediate data.
    uint32_t op_id = cqe.data;
    if (!op_id)
      return;
    auto it = net.remote_write_ops.find(op_id);
    if (it == net.remote_write_ops.end())
      return;
    op = it->second;
    net.remote_write_ops.erase(it);
  } else {
    // RECV / SEND / WRITE
    op = (RdmaOp *)cqe.op_context;
    if (!op)
      return;
    if (cqe.flags & FI_RECV) {
      op->recv.recv_size = cqe.len;
    } else if (cqe.flags & FI_SEND) {
      // Nothing special
    } else if (cqe.flags & FI_WRITE) {
      // Nothing special
    } else {
      fprintf(stderr, "Unhandled completion type. cqe.flags=%lx\n", cqe.flags);
      std::exit(1);
    }
  }
  if (op->callback)
    op->callback(net, *op);
  delete op;
}

void Network::PollCompletion() {
  struct fi_cq_data_entry cqe[kCompletionQueueReadCount];
  for (;;) {
    auto ret = fi_cq_read(cq, cqe, kCompletionQueueReadCount);
    if (ret > 0) {
      for (ssize_t i = 0; i < ret; i++) {
        HandleCompletion(*this, cqe[i]);
      }
    } else if (ret == -FI_EAVAIL) {
      struct fi_cq_err_entry err_entry;
      ret = fi_cq_readerr(cq, &err_entry, 0);
      if (ret < 0) {
        fprintf(stderr, "fi_cq_readerr error: %zd (%s)\n", ret,
                fi_strerror(-ret));
        std::exit(1);
      } else if (ret > 0) {
        fprintf(stderr, "Failed libfabric operation: %s\n",
                fi_cq_strerror(cq, err_entry.prov_errno, err_entry.err_data,
                               nullptr, 0));
      } else {
        fprintf(stderr, "fi_cq_readerr returned 0 unexpectedly.\n");
        std::exit(1);
      }
    } else if (ret == -FI_EAGAIN) {
      // No more completions
      break;
    } else {
      fprintf(stderr, "fi_cq_read error: %zd (%s)\n", ret, fi_strerror(-ret));
      std::exit(1);
    }
  }
}

enum class AppMessageType : uint8_t {
  kConnect = 0,
  kRandomFill = 1,
};

struct AppMessageBase {
  AppMessageType type;
};

struct AppConnectMessage {
  struct MemoryRegion {
    uint64_t addr;
    uint64_t size;
    uint64_t rkey;
  };

  AppMessageBase base;
  EfaAddress client_addr;
  size_t num_mr;

  MemoryRegion &mr(size_t index) {
    CHECK(index < num_mr);
    return ((MemoryRegion *)((uintptr_t)&base + sizeof(*this)))[index];
  }

  size_t MessageBytes() const {
    return sizeof(*this) + num_mr * sizeof(MemoryRegion);
  }
};

struct AppRandomFillMessage {
  AppMessageBase base;
  uint32_t remote_context;
  uint64_t seed;
  size_t page_size;
  size_t num_pages;

  uint32_t &page_idx(size_t index) {
    CHECK(index < num_pages);
    return ((uint32_t *)((uintptr_t)&base + sizeof(*this)))[index];
  }

  size_t MessageBytes() const {
    return sizeof(*this) + num_pages * sizeof(uint32_t);
  }
};

std::vector<uint8_t> RandomBytes(uint64_t seed, size_t size) {
  CHECK(size % sizeof(uint64_t) == 0);
  std::vector<uint8_t> buf(size);
  std::mt19937_64 gen(seed);
  std::uniform_int_distribution<uint64_t> dist;
  for (size_t i = 0; i < size; i += sizeof(uint64_t)) {
    *(uint64_t *)(buf.data() + i) = dist(gen);
  }
  return buf;
}

struct RandomFillRequestState {
  Buffer *cuda_buf;
  fi_addr_t client_addr = FI_ADDR_UNSPEC;
  bool done = false;
  AppConnectMessage *connect_msg = nullptr;

  explicit RandomFillRequestState(Buffer *cuda_buf) : cuda_buf(cuda_buf) {}

  void HandleConnect(Network &net, RdmaOp &op) {
    auto *base_msg = (AppMessageBase *)op.recv.buf->data;
    CHECK(base_msg->type == AppMessageType::kConnect);
    CHECK(op.recv.recv_size >= sizeof(AppConnectMessage));
    auto &msg = *(AppConnectMessage *)base_msg;
    CHECK(op.recv.recv_size == msg.MessageBytes());
    CHECK(msg.num_mr > 0);

    // Save the message. Note that we don't reuse the buffer.
    connect_msg = &msg;

    // Add the client to AV
    client_addr = net.AddPeerAddress(msg.client_addr);

    printf("Received CONNECT message from client:\n");
    printf("  addr: %s\n", msg.client_addr.ToString().c_str());
    for (size_t i = 0; i < msg.num_mr; i++) {
      printf("  MR[%zu]: addr=0x%012lx size=%lu rkey=0x%016lx\n", i,
             msg.mr(i).addr, msg.mr(i).size, msg.mr(i).rkey);
    }
  }

  void HandleRequest(Network &net, RdmaOp &op) {
    auto *base_msg = (const AppMessageBase *)op.recv.buf->data;
    CHECK(base_msg->type == AppMessageType::kRandomFill);
    CHECK(op.recv.recv_size >= sizeof(AppRandomFillMessage));
    auto &msg = *(AppRandomFillMessage *)base_msg;
    CHECK(op.recv.recv_size == msg.MessageBytes());

    printf("Received RandomFill request from client:\n");
    printf("  remote_context: 0x%08x\n", msg.remote_context);
    printf("  seed: 0x%016lx\n", msg.seed);
    printf("  page_size: %zu\n", msg.page_size);
    printf("  num_pages: %zu\n", msg.num_pages);

    // Generate random data and copy to local GPU memory
    printf("Generating random data");
    for (size_t i = 0; i < connect_msg->num_mr; ++i) {
      auto bytes = RandomBytes(msg.seed + i, msg.page_size * msg.num_pages);
      CUDA_CHECK(cudaMemcpy((uint8_t *)cuda_buf->data + i * bytes.size(),
                            bytes.data(), bytes.size(),
                            cudaMemcpyHostToDevice));
      printf(".");
      fflush(stdout);
    }
    printf("\n");

    // RDMA WRITE the data to remote GPU.
    //
    // NOTE(lequn): iov_limit==4, rma_iov_limit==1.
    // So need multiple WRITE instead of a vectorized WRITE.
    for (size_t i = 0; i < connect_msg->num_mr; ++i) {
      for (size_t j = 0; j < msg.num_pages; j++) {
        uint32_t imm_data = 0;
        std::function<void(Network &, RdmaOp &)> callback;
        if (i + 1 == connect_msg->num_mr && j + 1 == msg.num_pages) {
          // The last WRITE.
          // NOTE(lequn): EFA RDM guarantees send-after-send ordering.
          imm_data = msg.remote_context;
          callback = [this](Network &net, RdmaOp &op) {
            CHECK(op.type == RdmaOpType::kWrite);
            done = true;
            printf("Finished RDMA WRITE to the remote GPU memory.\n");
          };
        } else {
          // Don't send immediate data. Don't wake up the remote side.
          // Also skip local callback.
        }
        net.PostWrite(
            {.buf = cuda_buf,
             .offset = i * (msg.page_size * msg.num_pages) + j * msg.page_size,
             .len = msg.page_size,
             .imm_data = imm_data,
             .dest_ptr =
                 connect_msg->mr(i).addr + msg.page_idx(j) * msg.page_size,
             .dest_addr = client_addr,
             .dest_key = connect_msg->mr(i).rkey},
            std::move(callback));
      }
    }
  }

  void OnRecv(Network &net, RdmaOp &op) {
    if (client_addr == FI_ADDR_UNSPEC) {
      HandleConnect(net, op);
    } else {
      HandleRequest(net, op);
    }
  }
};

int ServerMain(int argc, char **argv) {
  // Open Netowrk
  struct fi_info *info = GetInfo();
  auto net = Network::Open(info);
  printf("domain: %14s", info->domain_attr->name);
  printf(", nic: %10s", info->nic->device_attr->name);
  printf(", fabric: %s", info->fabric_attr->prov_name);
  printf(", link: %.0fGbps", info->nic->link_attr->speed / 1e9);
  printf("\n");
  printf("Run client with the following command:\n");
  printf("  %s %s [page_size num_pages]\n", argv[0],
         net.addr.ToString().c_str());

  // Allocate and register message buffer
  auto buf1 = Buffer::Alloc(kMessageBufferSize, kBufAlign);
  net.RegisterMemory(buf1);
  auto buf2 = Buffer::Alloc(kMessageBufferSize, kBufAlign);
  net.RegisterMemory(buf2);

  // Allocate and register CUDA memory
  auto cuda_buf = Buffer::AllocCuda(kMemoryRegionSize * 2, kBufAlign);
  net.RegisterMemory(cuda_buf);
  printf("Registered 1 buffer on cuda:%d\n", cuda_buf.cuda_device);

  // Loop forever. Accept one client at a time.
  for (;;) {
    printf("------\n");
    // State machine
    RandomFillRequestState s(&cuda_buf);
    // RECV for CONNECT
    net.PostRecv(buf1, [&s](Network &net, RdmaOp &op) { s.OnRecv(net, op); });
    // RECV for RandomFillRequest
    net.PostRecv(buf2, [&s](Network &net, RdmaOp &op) { s.OnRecv(net, op); });
    // Wait for completion
    while (!s.done) {
      net.PollCompletion();
    }
  }

  return 0;
}

int ClientMain(int argc, char **argv) {
  CHECK(argc == 2 || argc == 4);
  auto server_addrname = EfaAddress::Parse(argv[1]);
  size_t page_size, num_pages;
  if (argc == 4) {
    page_size = std::stoull(argv[2]);
    num_pages = std::stoull(argv[3]);
  } else {
    page_size = 1 << 20;
    num_pages = 8;
  }
  size_t max_pages = kMemoryRegionSize / page_size;
  CHECK(page_size * num_pages <= kMemoryRegionSize);

  // Open Netowrk
  struct fi_info *info = GetInfo();
  auto net = Network::Open(info);
  printf("domain: %14s", info->domain_attr->name);
  printf(", nic: %10s", info->nic->device_attr->name);
  printf(", fabric: %s", info->fabric_attr->prov_name);
  printf(", link: %.0fGbps", info->nic->link_attr->speed / 1e9);
  printf("\n");
  auto server_addr = net.AddPeerAddress(server_addrname);

  // Allocate and register message buffer
  auto buf1 = Buffer::Alloc(kMessageBufferSize, kBufAlign);
  net.RegisterMemory(buf1);

  // Allocate and register CUDA memory
  auto cuda_buf1 = Buffer::AllocCuda(kMemoryRegionSize, kBufAlign);
  net.RegisterMemory(cuda_buf1);
  auto cuda_buf2 = Buffer::AllocCuda(kMemoryRegionSize, kBufAlign);
  net.RegisterMemory(cuda_buf2);
  printf("Registered 2 buffers on cuda:%d\n", cuda_buf1.cuda_device);

  // Prepare request
  std::mt19937_64 rng(0xabcdabcd987UL);
  uint64_t req_seed = rng();
  std::vector<uint32_t> page_idx;
  std::vector<uint32_t> tmp(max_pages);
  std::iota(tmp.begin(), tmp.end(), 0);
  std::sample(tmp.begin(), tmp.end(), std::back_inserter(page_idx), num_pages,
              rng);

  // Send address and MR to server
  auto &connect_msg = *(AppConnectMessage *)buf1.data;
  connect_msg = {
      .base = {.type = AppMessageType::kConnect},
      .client_addr = net.addr,
      .num_mr = 2,
  };
  connect_msg.mr(0) = {.addr = (uint64_t)cuda_buf1.data,
                       .size = kMemoryRegionSize,
                       .rkey = net.GetMR(cuda_buf1)->key};
  connect_msg.mr(1) = {.addr = (uint64_t)cuda_buf2.data,
                       .size = kMemoryRegionSize,
                       .rkey = net.GetMR(cuda_buf2)->key};
  bool connect_sent = false;
  net.PostSend(
      server_addr, buf1, connect_msg.MessageBytes(),
      [&connect_sent](Network &net, RdmaOp &op) { connect_sent = true; });
  while (!connect_sent) {
    net.PollCompletion();
  }
  printf("Sent CONNECT message to server\n");

  // Prepare to receive the last REMOTE WRITE from server
  bool last_remote_write_received = false;
  uint32_t remote_write_op_id = 0x123;
  net.AddRemoteWrite(remote_write_op_id,
                     [&last_remote_write_received](Network &net, RdmaOp &op) {
                       last_remote_write_received = true;
                     });

  // Send message to server
  auto &req_msg = *(AppRandomFillMessage *)buf1.data;
  req_msg = {
      .base = {.type = AppMessageType::kRandomFill},
      .remote_context = remote_write_op_id,
      .seed = req_seed,
      .page_size = page_size,
      .num_pages = num_pages,
  };
  for (size_t i = 0; i < num_pages; i++) {
    req_msg.page_idx(i) = page_idx[i];
  }
  bool req_sent = false;
  net.PostSend(server_addr, buf1, req_msg.MessageBytes(),
               [&req_sent](Network &net, RdmaOp &op) { req_sent = true; });
  while (!req_sent) {
    net.PollCompletion();
  }
  printf("Sent RandomFillRequest to server. page_size: %zu, num_pages: %zu\n",
         page_size, num_pages);

  // Wait for REMOTE WRITE from server
  while (!last_remote_write_received) {
    net.PollCompletion();
  }
  printf("Received RDMA WRITE to local GPU memory.\n");

  // Verify data
  auto expected1 = RandomBytes(req_seed, page_size * num_pages);
  auto expected2 = RandomBytes(req_seed + 1, page_size * num_pages);
  auto actual1 = std::vector<uint8_t>(page_size * num_pages);
  auto actual2 = std::vector<uint8_t>(page_size * num_pages);
  for (size_t i = 0; i < num_pages; i++) {
    CUDA_CHECK(cudaMemcpy(actual1.data() + i * page_size,
                          (uint8_t *)cuda_buf1.data + page_idx[i] * page_size,
                          page_size, cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(actual2.data() + i * page_size,
                          (uint8_t *)cuda_buf2.data + page_idx[i] * page_size,
                          page_size, cudaMemcpyDeviceToHost));
  }
  CHECK(expected1 == actual1);
  CHECK(expected2 == actual2);
  printf("Data is correct\n");

  return 0;
}

int main(int argc, char **argv) {
  if (argc == 1) {
    return ServerMain(argc, argv);
  } else {
    return ClientMain(argc, argv);
  }
}
