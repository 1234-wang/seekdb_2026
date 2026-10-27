// Isolated GPU graph construction. No VSAG, seekdb, networking or CPU base-graph Build.
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <cuvs/neighbors/hnsw.h>
#include <dlpack/dlpack.h>
#include <hip/hip_runtime_api.h>
#include <execinfo.h>
#include <fcntl.h>
#include <link.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
static double elapsed(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }
static std::string quoted(const std::string &text) {
  std::string result = "\"";
  for (unsigned char c : text.substr(0, 512)) {
    if (c == '\\' || c == '"') { result += '\\'; result += char(c); }
    else if (c < 32) result += ' ';
    else result += char(c);
  }
  return result + '"';
}
static void checked(cuvsError_t code, const char *operation) {
  if (code != CUVS_SUCCESS) {
    const char *error = cuvsGetLastErrorText();
    throw std::runtime_error(std::string(operation) + ": " + (error ? error : "unknown hipVS error"));
  }
}
static void hip_checked(hipError_t code, const char *operation) {
  if (code != hipSuccess) throw std::runtime_error(std::string(operation) + ": " + hipGetErrorString(code));
}
struct InvalidInput : std::runtime_error { using std::runtime_error::runtime_error; };
static uint64_t integer(const char *text, const char *name) {
  if (!text || !*text) throw InvalidInput(std::string("missing ") + name);
  uint64_t value = 0;
  for (const unsigned char c : std::string(text)) {
    if (c < '0' || c > '9' || value > (std::numeric_limits<uint64_t>::max() - (c - '0')) / 10)
      throw InvalidInput(std::string("invalid or overflowing ") + name);
    value = value * 10 + c - '0';
  }
  return value;
}
static uint64_t multiply(uint64_t a, uint64_t b) {
  if (b && a > std::numeric_limits<uint64_t>::max() / b) throw InvalidInput("shape/size overflow");
  return a * b;
}
static uint64_t host_available() {
  uint64_t available = std::numeric_limits<uint64_t>::max();
  std::ifstream input("/proc/meminfo"); std::string line;
  while (std::getline(input, line)) if (line.rfind("MemAvailable:", 0) == 0) {
    const auto begin = line.find_first_of("0123456789");
    if (begin != std::string::npos) available = std::stoull(line.substr(begin)) * 1024;
    break;
  }
  std::ifstream cap_file("/sys/fs/cgroup/memory.max"), used_file("/sys/fs/cgroup/memory.current");
  std::string cap; uint64_t used = 0;
  if ((cap_file >> cap) && cap != "max" && (used_file >> used)) {
    const uint64_t maximum = integer(cap.c_str(), "cgroup memory limit");
    const uint64_t remaining = maximum > used ? maximum - used : 0;
    available = std::min(available, remaining);
  }
  return available;
}
static bool same_file(const struct stat &a, const struct stat &b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size
    && a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
    && a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
struct Descriptor {
  int value = -1;
  explicit Descriptor(int fd = -1) : value(fd) {}
  ~Descriptor() { if (value >= 0) close(value); }
  Descriptor(const Descriptor &) = delete;
  Descriptor &operator=(const Descriptor &) = delete;
};
struct Handles {
  cuvsResources_t resources = 0;
  cuvsCagraIndexParams_t cp = nullptr;
  cuvsCagraIndex_t cagra = nullptr;
  cuvsHnswIndexParams_t hp = nullptr;
  cuvsHnswIndex_t hnsw = nullptr;
  ~Handles() {
    if (hnsw) cuvsHnswIndexDestroy(hnsw);
    if (hp) cuvsHnswIndexParamsDestroy(hp);
    if (cagra) cuvsCagraIndexDestroy(cagra);
    if (cp) cuvsCagraIndexParamsDestroy(cp);
    if (resources) cuvsResourcesDestroy(resources);
  }
};
struct StagingFile {
  std::string path;
  struct stat identity {};
  explicit StagingFile(const std::filesystem::path &parent) {
    std::string pattern = (parent / ".hipvs-stage-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end()); name.push_back(0);
    Descriptor created(mkstemp(name.data()));
    if (created.value < 0) throw std::runtime_error("cannot create own staging file");
    path = name.data();
    if (fstat(created.value, &identity)) { unlink(path.c_str()); throw std::runtime_error("cannot stat own staging file"); }
  }
  ~StagingFile() {
    struct stat current {};
    if (!path.empty() && !lstat(path.c_str(), &current) && current.st_dev == identity.st_dev && current.st_ino == identity.st_ino)
      unlink(path.c_str());
  }
};
static int reject_vsag(dl_phdr_info *object, size_t, void *found) {
  const std::string name = object->dlpi_name ? object->dlpi_name : "";
  if (name.find("libvsag.so") != std::string::npos || name.find("/vsag-sdk/") != std::string::npos)
    *static_cast<bool *>(found) = true;
  return 0;
}
static void terminate_evidence() noexcept {
  static const char text[] = "hipVS helper terminated by an uncaught vendor exception; bounded backtrace follows (cause not diagnosed).\n";
  (void)write(STDERR_FILENO, text, sizeof(text) - 1);
  try {
    if (auto exception = std::current_exception()) std::rethrow_exception(exception);
  } catch (const std::exception &error) {
    const char *message = error.what();
    if (message) (void)write(STDERR_FILENO, message, strnlen(message, 512));
    (void)write(STDERR_FILENO, "\n", 1);
  } catch (...) {}
  void *frames[24]; const int count = backtrace(frames, 24);
  backtrace_symbols_fd(frames, count, STDERR_FILENO);
  _Exit(134);
}

int main(int argc, char **argv) {
  std::set_terminate(terminate_evidence);
  const char *usage = "seekdb_hipvs_build --input RAW_F32 --output NEW_HNSW --rows N --dim D --m M --ef-construction EF --host-threads 1|4";
  if (argc == 2 && std::string(argv[1]) == "--help") { std::cout << usage << '\n'; return 0; }
  if (argc != 15) { std::cerr << usage << '\n'; return 2; }
  const auto total_start = Clock::now();
  const char *phase = "validate";
  auto event = [&](const char *value) { phase = value; std::cout << "{\"phase\":" << quoted(value) << "}" << std::endl; };
  try {
    const char *names[] = {"--input", "--output", "--rows", "--dim", "--m", "--ef-construction", "--host-threads"};
    for (int i = 0; i < 7; ++i) if (std::string(argv[1 + 2*i]) != names[i]) throw InvalidInput("use the documented named argument order; no unknown or duplicate options");
    const uint64_t n = integer(argv[6], "rows"), dim = integer(argv[8], "dim"), m = integer(argv[10], "m"),
      ef = integer(argv[12], "ef-construction"), threads = integer(argv[14], "host-threads");
    if (!n || n >= uint64_t(std::numeric_limits<uint32_t>::max()) || !dim || dim > 65536 || m < 2 || m > 512
        || ef < m || ef > 1000000 || (threads != 1 && threads != 4) || n <= 4*m)
      throw InvalidInput("unsupported shape/parameters: need N>4M, dim1..65536, M2..512, M<=EF<=1M, threads1|4");
    const uint64_t elements = multiply(n, dim), bytes = multiply(elements, sizeof(float));
    if (bytes > uint64_t(std::numeric_limits<size_t>::max()) || bytes > uint64_t(std::numeric_limits<ssize_t>::max()) || bytes > (8ULL << 30))
      throw InvalidInput("raw input exceeds addressable or 8GiB safety bound");
    const std::filesystem::path input(argv[2]), output(argv[4]);
    if (!input.is_absolute() || !output.is_absolute() || input.lexically_normal() != input || output.lexically_normal() != output)
      throw InvalidInput("input/output must be normalized absolute paths");
    const auto parent = std::filesystem::canonical(output.parent_path());
    Descriptor parent_fd(open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)); struct stat parent_info {};
    if (parent_fd.value < 0 || fstat(parent_fd.value, &parent_info) || parent_info.st_uid != geteuid() || (parent_info.st_mode & 0022))
      throw InvalidInput("output parent must be owned and not writable by other users");
    const auto filename = output.filename().string(); struct stat existing {};
    if (filename.empty() || !fstatat(parent_fd.value, filename.c_str(), &existing, AT_SYMLINK_NOFOLLOW) || errno != ENOENT)
      throw InvalidInput("output must be a new nonexistent file");
    Descriptor input_fd(open(input.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)); struct stat before {}, after {};
    if (input_fd.value < 0 || fstat(input_fd.value, &before) || !S_ISREG(before.st_mode) || before.st_size < 0 || uint64_t(before.st_size) != bytes)
      throw InvalidInput("raw input must be a regular non-symlink file with exactly N*D float32 bytes");
    const uint64_t host_estimate = multiply(bytes, 3) + multiply(n, 24*m + 256);
    const uint64_t available = host_available();
    if (host_estimate > available - available/4) throw InvalidInput("insufficient available host/cgroup memory for conservative staging estimate");
    bool foreign_library = false; dl_iterate_phdr(reject_vsag, &foreign_library);
    if (foreign_library) throw InvalidInput("VSAG/private SDK libraries are loaded; use a clean isolated helper process");
    event("read_and_validate_float32");
    std::vector<float> base(static_cast<size_t>(elements)); size_t offset = 0;
    while (offset < bytes) {
      const ssize_t got = read(input_fd.value, reinterpret_cast<char *>(base.data()) + offset, std::min<size_t>(bytes - offset, 4*1024*1024));
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0) throw InvalidInput("raw input read failed or was truncated");
      offset += size_t(got);
    }
    if (fstat(input_fd.value, &after) || !same_file(before, after)) throw InvalidInput("input identity changed during reading");
    for (float value : base) if (!std::isfinite(value)) throw InvalidInput("all input float32 values must be finite");
    int count = 0, device = 0; hipDeviceProp_t properties {}; size_t free_gpu = 0, total_gpu = 0;
    hip_checked(hipGetDeviceCount(&count), "hipGetDeviceCount");
    hip_checked(hipGetDevice(&device), "hipGetDevice");
    if (count <= 0 || device < 0 || device >= count) throw std::runtime_error("no current AMD GPU device");
    hip_checked(hipGetDeviceProperties(&properties, device), "hipGetDeviceProperties");
    hip_checked(hipMemGetInfo(&free_gpu, &total_gpu), "hipMemGetInfo");
    const uint64_t gpu_estimate = multiply(bytes, 2) + multiply(n, 80*m);
    if (gpu_estimate > uint64_t(free_gpu) - uint64_t(free_gpu)/4) throw InvalidInput("insufficient free GPU memory for conservative graph estimate");
    std::cout << "{\"phase\":\"hardware\",\"device\":" << device << ",\"device_count\":" << count
      << ",\"name\":" << quoted(properties.name) << ",\"architecture\":" << quoted(properties.gcnArchName)
      << ",\"gpu_free_bytes\":" << free_gpu << ",\"gpu_total_bytes\":" << total_gpu
      << ",\"rows\":" << n << ",\"dim\":" << dim << ",\"m\":" << m << ",\"ef_construction\":" << ef
      << ",\"host_threads\":" << threads << ",\"input_bytes\":" << bytes << ",\"metric\":\"L2Expanded\"}" << std::endl;
    Handles h;
    event("gpu_cagra_build_begin"); const auto build_start = Clock::now();
    checked(cuvsResourcesCreate(&h.resources), "ResourcesCreate");
    checked(cuvsCagraIndexParamsCreate(&h.cp), "CagraParamsCreate");
    checked(cuvsCagraIndexCreate(&h.cagra), "CagraIndexCreate");
    h.cp->metric = L2Expanded; h.cp->graph_degree = 2*m; h.cp->intermediate_graph_degree = 4*m; h.cp->build_algo = NN_DESCENT;
    int64_t shape[] = {int64_t(n), int64_t(dim)}; DLManagedTensor tensor {};
    tensor.dl_tensor.data = base.data(); tensor.dl_tensor.device = {kDLCPU, 0}; tensor.dl_tensor.ndim = 2;
    tensor.dl_tensor.dtype = {kDLFloat, 32, 1}; tensor.dl_tensor.shape = shape;
    checked(cuvsCagraBuild(h.resources, h.cp, &tensor, h.cagra), "CagraBuild");
    checked(cuvsStreamSync(h.resources), "CagraBuild sync");
    int64_t actual_n = 0, actual_dim = 0;
    checked(cuvsCagraIndexGetSize(h.cagra, &actual_n), "CagraGetSize");
    checked(cuvsCagraIndexGetDims(h.cagra, &actual_dim), "CagraGetDims");
    if (actual_n != int64_t(n) || actual_dim != int64_t(dim)) throw std::runtime_error("GPU index row/dimension mismatch");
    const double build_seconds = elapsed(build_start);
    std::cout << "{\"phase\":\"gpu_cagra_build_complete\",\"seconds\":" << build_seconds << "}" << std::endl;
    event("cpu_upper_layers_begin"); const auto conversion_start = Clock::now();
    checked(cuvsHnswIndexParamsCreate(&h.hp), "HnswParamsCreate");
    h.hp->hierarchy = CPU; h.hp->num_threads = int(threads); h.hp->ef_construction = int(ef);
    checked(cuvsHnswIndexCreate(&h.hnsw), "HnswIndexCreate");
    checked(cuvsHnswFromCagra(h.resources, h.hp, h.cagra, h.hnsw), "HnswFromCagra");
    checked(cuvsStreamSync(h.resources), "HNSW conversion sync");
    const double conversion_seconds = elapsed(conversion_start);
    std::cout << "{\"phase\":\"cpu_upper_layers_complete\",\"seconds\":" << conversion_seconds << "}" << std::endl;
    event("serialize_begin"); const auto serialization_start = Clock::now();
    StagingFile staging(parent);
    checked(cuvsHnswSerialize(h.resources, staging.path.c_str(), h.hnsw), "HnswSerialize");
    Descriptor serialized(open(staging.path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)); struct stat saved {};
    if (serialized.value < 0 || fstat(serialized.value, &saved) || !S_ISREG(saved.st_mode) || saved.st_size <= 0
        || saved.st_dev != staging.identity.st_dev || saved.st_ino != staging.identity.st_ino || fsync(serialized.value))
      throw std::runtime_error("serialized file identity/size/fsync failed");
    // Atomic, same-filesystem publication without overwriting any pre-existing output.
    if (linkat(AT_FDCWD, staging.path.c_str(), parent_fd.value, filename.c_str(), 0)) throw std::runtime_error("exclusive output publication failed");
    std::cout << "{\"phase\":\"complete\",\"status\":\"GPU_HNSW_FILE_READY\",\"output_bytes\":" << saved.st_size
      << ",\"gpu_build_seconds\":" << build_seconds << ",\"cpu_upper_layer_seconds\":" << conversion_seconds
      << ",\"serialize_publish_seconds\":" << elapsed(serialization_start) << ",\"total_seconds\":" << elapsed(total_start)
      << ",\"labels\":\"dense_row_offsets_parent_must_map_original_ids\",\"vsag_loaded\":false}" << std::endl;
    return 0;
  } catch (const InvalidInput &error) {
    std::cout << "{\"phase\":" << quoted(phase) << ",\"status\":\"REJECTED\",\"error\":" << quoted(error.what()) << "}" << std::endl;
    std::cerr << error.what() << '\n'; return 2;
  } catch (const std::exception &error) {
    std::cout << "{\"phase\":" << quoted(phase) << ",\"status\":\"FAILED\",\"error\":" << quoted(error.what()) << "}" << std::endl;
    std::cerr << error.what() << '\n'; return 1;
  }
}
