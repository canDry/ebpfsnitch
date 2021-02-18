#include <unistd.h>
#include <fstream>
#include <iostream>
#include <string>
#include <signal.h>
#include <unistd.h>
#include <netinet/in.h>
#include <linux/netfilter.h>
#include <thread>
#include <arpa/inet.h>
#include <unordered_map>
#include <mutex>
#include <assert.h>
#include <algorithm>
#include <condition_variable>
#include <poll.h>
#include <sys/un.h>
#include <nlohmann/json.hpp>

#include <bcc/bcc_version.h>
#include <bcc/BPF.h>

#include <libnetfilter_queue/libnetfilter_queue.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "ebpfsnitch_daemon.hpp"

static uint64_t
nanoseconds()
{
    struct timespec l_timespec;

    clock_gettime(CLOCK_MONOTONIC, &l_timespec);

    return l_timespec.tv_nsec + (l_timespec.tv_sec * 1000000000);
}

std::string
file_to_string(const std::string &p_path);

std::string
ipv4_to_string(const uint32_t p_address);

iptables_raii::iptables_raii(std::shared_ptr<spdlog::logger> p_log):
    m_log(p_log)
{
    m_log->trace("adding iptables rules");

    std::system("iptables -A OUTPUT -j NFQUEUE --queue-num 0");
}

iptables_raii::~iptables_raii()
{
    m_log->trace("removing iptables rules");

    std::system("iptables -D OUTPUT -j NFQUEUE --queue-num 0");
}

ebpfsnitch_daemon::ebpfsnitch_daemon(
    std::shared_ptr<spdlog::logger> p_log
):
m_log(p_log),
m_shutdown(false)
{
    m_log->trace("ebpfsnitch_daemon constructor");
    
    m_log->trace("compiling ebpf probes");
    const ebpf::StatusTuple l_init_res = m_bpf.init(file_to_string("probes.c"));
    if (l_init_res.code() != 0) {
        m_log->error("g_bpf.init() failed, {}", l_init_res.msg());

        throw std::runtime_error("failed to compile eBPF");
    }
    
    m_nfq_handle = nfq_open();
    if (m_nfq_handle == NULL) {
        m_log->error("nfq_open() failed");

        throw std::runtime_error("placeholder");
    }

    if (nfq_unbind_pf(m_nfq_handle, AF_INET) < 0) {
        m_log->error("nfq_unbind_pf() failed");

        throw std::runtime_error("placeholder");
    }

    if (nfq_bind_pf(m_nfq_handle, AF_INET) < 0) {
        m_log->error("nfq_bind_pf() failed");

        throw std::runtime_error("placeholder");
    }

    m_nfq_queue = nfq_create_queue(
        m_nfq_handle,
        0,
        &ebpfsnitch_daemon::nfq_handler_indirect,
        (void *)this
    );

    if (m_nfq_queue == NULL) {
        m_log->error("nfq_create_queue() failed");

        throw std::runtime_error("placeholder");
    }

    const uint32_t l_queue_flags = NFQA_CFG_F_UID_GID | NFQA_CFG_F_GSO;
    const int l_flag_status = nfq_set_queue_flags(
        m_nfq_queue,
        l_queue_flags,
        l_queue_flags
    );

    if (l_flag_status != 0) {
        m_log->error("nfq_set_queue_flags() failed");

        throw std::runtime_error("placeholder");
    }

    if (nfq_set_mode(m_nfq_queue, NFQNL_COPY_PACKET, 0xffff) < 0) {
        m_log->error("nfq_set_mode() failed");

        throw std::runtime_error("placeholder");
    }

    m_nfq_fd = nfq_fd(m_nfq_handle);

    if (m_nfq_fd <= 0) {
        m_log->error("nfq_fd() failed");

        throw std::runtime_error("placeholder");
    }

    m_iptables_raii = std::make_shared<iptables_raii>(p_log);

    m_log->trace("attaching kprobes");
    ebpf::StatusTuple l_attach_res = m_bpf.attach_kprobe(
        "tcp_v4_connect",
        "probe_tcp_v4_connect_entry"
    );

    if (l_attach_res.code() != 0) {
        m_log->error("g_bpf.attach_kprobe() failed, {}", l_attach_res.msg());

        throw std::runtime_error("placeholder");
    }

    l_attach_res = m_bpf.attach_kprobe(
        "tcp_v4_connect",
        "probe_tcp_v4_connect_return",
        0,
        BPF_PROBE_RETURN
    );

    if (l_attach_res.code() != 0) {
        m_log->error("g_bpf.attach_kprobe() failed, {}", l_attach_res.msg());

        throw std::runtime_error("placeholder");
    }

    l_attach_res = m_bpf.attach_tracepoint(
        "sock:inet_sock_set_state",
        "tracepoint__sock__inet_sock_set_state"
    );

    if (l_attach_res.code() != 0) {
        m_log->error("m_bpf.attach_tracepoint failed, {}", l_attach_res.msg());

        throw std::runtime_error("placeholder");
    }

    const ebpf::StatusTuple l_open_res = m_bpf.open_perf_buffer(
        "g_probe_ipv4_events",
        &ebpfsnitch_daemon::bpf_reader_indirect,
        NULL,
        (void *)this
    );

    if (l_open_res.code() != 0) {
        m_log->error("g_bpf.open_perf_buffer() failed, {}", l_open_res.msg());

        throw std::runtime_error("placeholder");
    }

    m_perf_buffer = m_bpf.get_perf_buffer("g_probe_ipv4_events");

    if (m_perf_buffer == NULL) {
        m_log->error("g_bpf.get_perf_buffer() failed, {}");

        throw std::runtime_error("placeholder");
    }

    m_filter_thread   = std::thread( &ebpfsnitch_daemon::filter_thread,   this );
    m_probe_thread   = std::thread( &ebpfsnitch_daemon::probe_thread,   this );
    m_control_thread = std::thread( &ebpfsnitch_daemon::control_thread, this );
}

ebpfsnitch_daemon::~ebpfsnitch_daemon()
{
    m_log->trace("ebpfsnitch_daemon destructor");;

    m_log->trace("joining threads");
    m_shutdown.store(true);
    m_control_thread.join();
    m_filter_thread.join();
    m_probe_thread.join();

    nfq_destroy_queue(m_nfq_queue);
    nfq_close(m_nfq_handle);

    m_log->trace("detaching ebpf kprobes");
    /*
    const ebpf::StatusTuple l_detach_res = m_bpf.detach_kprobe(clone_fnname);

    if (l_detach_res.code() != 0) {
        m_log->error("m_bpf.detach_kprobe() failed {}", l_detach_res.msg());
    }
    */
}

void
ebpfsnitch_daemon::filter_thread()
{
    m_log->trace("ebpfsnitch_daemon::filter_thread() entry");

    char l_buffer[1024 * 64] __attribute__ ((aligned));

    struct pollfd l_poll_fd;
    l_poll_fd.fd     = m_nfq_fd;
    l_poll_fd.events = POLLIN;

    while (true) {
        if (m_shutdown.load()) {
            break;
        }
        
        int l_ret = poll(&l_poll_fd, 1, 1000);

        if (l_ret < 0) {
            m_log->error("poll() error {}", l_ret);

            break;
        } else if (l_ret == 0) {
            continue;
        }

        l_ret = recv(m_nfq_fd, l_buffer, sizeof(l_buffer), 0);

        if (l_ret <= 0) {
            m_log->error("recv() error {}", l_ret);

            break;
        }

        nfq_handle_packet(m_nfq_handle, l_buffer, l_ret);
    }

    m_log->trace("ebpfsnitch_daemon::filter_thread() exit");
}

void
ebpfsnitch_daemon::probe_thread()
{
    m_log->trace("ebpfsnitch_daemon::probe_thread() entry");

    while (!m_shutdown.load()) {
        m_perf_buffer->poll(100);
    }

    m_log->trace("ebpfsnitch_daemon::probe_thread() exit");
}

void
ebpfsnitch_daemon::bpf_reader(
    void *const p_data,
    const int   p_data_size
){
    assert(p_data);

    struct probe_ipv4_event_t *const l_info =
        static_cast<probe_ipv4_event_t *>(p_data);

    if (l_info->m_remove) {
        m_log->info("got remove command {}", l_info->m_handle);

        return;
    }

    /*
    const uint64_t l_now = nanoseconds();

    m_log->info("now before: {}ns call: {}ns difference: {}ns",
        l_now,
        l_info->m_timestamp,
        l_now - l_info->m_timestamp
    );
    */

    const std::string l_destination_address =
        ipv4_to_string(l_info->m_destination_address);

    const std::string l_source_address =
        ipv4_to_string(l_info->m_destination_address);

    const std::string l_path = 
        "/proc/" +
        std::to_string(l_info->m_process_id) +
        "/exe";

    char l_readlink_buffer[1024 * 32 ];

    const ssize_t l_readlink_status = readlink(
        l_path.c_str(),
        l_readlink_buffer,
        sizeof(l_readlink_buffer) - 1
    );

    if (l_readlink_status == -1) {
        m_log->error("failed to read link {}", l_path);
    }

    l_readlink_buffer[l_readlink_status] = '\0';

    const std::string l_command_line = std::string(l_readlink_buffer);

    m_log->info(
        "got event handle {} uid {} pid {} sourcePort {} sourceAddress {} "
        "destinationPort {} destinationAddress {}",
        l_info->m_handle,
        l_info->m_user_id,
        l_info->m_process_id,
        l_info->m_source_port,
        l_source_address,
        l_info->m_destination_port,
        l_destination_address
    );

    const std::string l_key =
        std::to_string(l_info->m_source_port) +
        std::to_string(l_info->m_destination_port);

    struct connection_info_t l_info2;
    l_info2.m_user_id    = l_info->m_user_id;
    l_info2.m_process_id = l_info->m_process_id;
    l_info2.m_executable = l_command_line;

    {
        std::lock_guard<std::mutex> l_guard(m_lock);

        m_mapping[l_key] = l_info2;
    }

    process_unhandled();
}

void
ebpfsnitch_daemon::bpf_reader_indirect(
    void *const p_cb_cookie,
    void *const p_data,
    const int   p_data_size
){
    assert(p_cb_cookie);
    assert(p_data);

    class ebpfsnitch_daemon *const l_self =
        (class ebpfsnitch_daemon *const)p_cb_cookie;

    l_self->bpf_reader(p_data, p_data_size);
}

bool
ebpfsnitch_daemon::process_nfq_event(
    const struct nfq_event_t &l_nfq_event,
    const bool                p_queue_unassociated
) {
    m_log->info("process_nfq_event");

    const std::optional<struct connection_info_t> l_optional_info =
        lookup_connection_info(l_nfq_event);

    if (l_optional_info) {
        const struct connection_info_t l_info = l_optional_info.value();

        const std::optional<bool> l_verdict = get_verdict(l_info.m_executable);

        if (l_verdict) {
            if (l_verdict.value()) {
                m_log->info("verdict allow {}", l_info.m_executable);

                std::lock_guard<std::mutex> l_guard(m_response_lock);

                nfq_set_verdict(
                    m_nfq_queue,
                    l_nfq_event.m_nfq_id,
                    NF_ACCEPT,
                    0,
                    NULL
                );

                return true;
            } else {
                m_log->info("verdict deny {}", l_info.m_executable);

                std::lock_guard<std::mutex> l_guard(m_response_lock);

                nfq_set_verdict(
                    m_nfq_queue,
                    l_nfq_event.m_nfq_id,
                    NF_DROP,
                    0,
                    NULL
                );

                return true;
            }
        } else {
            m_log->info("no verdict yet");
        }
    } else {
        m_log->info("unassociated");
    }

    if (p_queue_unassociated) {
        std::lock_guard<std::mutex> l_guard_undecided(
            m_undecided_packets_lock
        );

        m_undecided_packets.push(l_nfq_event);
    }
    
    if (l_optional_info) {
        m_log->info("unassociated and has optional");

        const struct connection_info_t l_info = l_optional_info.value();

        struct event_t l_event;
        l_event.m_executable       = l_info.m_executable;
        l_event.m_user_id          = l_info.m_user_id;
        l_event.m_process_id       = l_info.m_process_id;
        l_event.m_source_port      = l_nfq_event.m_source_port;
        l_event.m_destination_port = l_nfq_event.m_destination_port;

        std::lock_guard<std::mutex> l_guard_1(m_events_lock);

        if (m_active_queries.find(l_info.m_executable)
            == m_active_queries.end()
        ) {
            m_events.push(l_event);
            m_active_queries.insert(l_info.m_executable);
        }
    }

    return false;
}

int
ebpfsnitch_daemon::nfq_handler(
    struct nfq_q_handle *const p_qh,
    struct nfgenmsg *const     p_nfmsg,
    struct nfq_data *const     p_nfa
){
    struct nfqnl_msg_packet_hdr *l_header = nfq_get_msg_packet_hdr(p_nfa);	

    struct nfq_event_t l_nfq_event;
    l_nfq_event.m_nfq_id = ntohl(l_header->packet_id);

    if (nfq_get_uid(p_nfa, &l_nfq_event.m_user_id) == 0) {
        m_log->error("unknown allowing no uid");
        std::lock_guard<std::mutex> l_guard(m_response_lock);
        return nfq_set_verdict(p_qh, l_nfq_event.m_nfq_id, NF_ACCEPT, 0, NULL);
    }

    if (nfq_get_gid(p_nfa,& l_nfq_event.m_group_id) == 0) {
        m_log->error("unknown allowing no gid");
        std::lock_guard<std::mutex> l_guard(m_response_lock);
        return nfq_set_verdict(p_qh, l_nfq_event.m_nfq_id, NF_ACCEPT, 0, NULL);
    }

    unsigned char *l_data = NULL;
    const int l_ret = nfq_get_payload(p_nfa, &l_data);

    if (l_ret < 24) {
        m_log->error("unknown allowing malformed");
        std::lock_guard<std::mutex> l_guard(m_response_lock);
        return nfq_set_verdict(p_qh, l_nfq_event.m_nfq_id, NF_ACCEPT, 0, NULL);
    }

    l_nfq_event.m_protocol = *((uint8_t*) (l_data + 9));

    if (l_nfq_event.m_protocol != 6) {
        m_log->info(
            "allowing unknown protocol "
            "userId {} groupId {} protocol {}",
            l_nfq_event.m_user_id,
            l_nfq_event.m_group_id,
            l_nfq_event.m_protocol
        );

        std::lock_guard<std::mutex> l_guard(m_response_lock);
        return nfq_set_verdict(p_qh, l_nfq_event.m_nfq_id, NF_ACCEPT, 0, NULL);
    }

    l_nfq_event.m_source_port      = ntohs(*((uint16_t*) (l_data + 20)));
    l_nfq_event.m_destination_port = ntohs(*((uint16_t*) (l_data + 22)));

    process_nfq_event(l_nfq_event, true);

    return 0;
}

int
ebpfsnitch_daemon::nfq_handler_indirect(
    struct nfq_q_handle *const p_qh,
    struct nfgenmsg *const     p_nfmsg,
    struct nfq_data *const     p_nfa,
    void *const                p_data
){
    assert(p_data);

    class ebpfsnitch_daemon *const l_self =
        (class ebpfsnitch_daemon *const)p_data;

    return l_self->nfq_handler(p_qh, p_nfmsg, p_nfa);
}

std::optional<struct connection_info_t>
ebpfsnitch_daemon::lookup_connection_info(const nfq_event_t &p_event)
{
    const std::string l_key = std::to_string(p_event.m_source_port) +
        std::to_string(p_event.m_destination_port);

    std::lock_guard<std::mutex> l_guard(m_lock);

    if (m_mapping.find(l_key) != m_mapping.end()) {
        return std::optional<struct connection_info_t>(m_mapping[l_key]);
    } else {
        return std::nullopt;
    }
}

std::optional<bool>
ebpfsnitch_daemon::get_verdict(const std::string &p_executable)
{
    std::lock_guard<std::mutex> l_guard(m_verdicts_lock);

    if (m_verdicts.find(p_executable) != m_verdicts.end()) {
        return std::optional<bool>(m_verdicts[p_executable]);
    } else {
        return std::nullopt;
    }
}

void
ebpfsnitch_daemon::control_thread()
{
    m_log->trace("ebpfsnitch_daemon::control_thread() entry");

    int l_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (l_fd <= 0) {
        throw std::runtime_error("socket()");
    }

    const char *const l_path = "/tmp/ebpfsnitch.sock";

    unlink(l_path);

    struct sockaddr_un l_addr;
    memset(&l_addr, 0, sizeof(l_addr));
    l_addr.sun_family = AF_UNIX;
    strcpy(l_addr.sun_path, l_path);

    if (bind(l_fd, (struct sockaddr*)&l_addr, sizeof(l_addr)) < 0) {
        throw std::runtime_error("bind()");
    }

    if (listen(l_fd, 5) < 0) {
        throw std::runtime_error("listen()");
    }

    if (chmod("/tmp/ebpfsnitch.sock", 666) != 0){
        throw std::runtime_error("chmod()");
    }

    struct pollfd l_poll_fd;
    l_poll_fd.fd     = l_fd;
    l_poll_fd.events = POLLIN;

    while (true) {
        if (m_shutdown.load()) {
            break;
        }
        
        int l_ret = poll(&l_poll_fd, 1, 1000);

        if (l_ret < 0) {
            m_log->error("poll() unix socket error {}", l_ret);

            break;
        } else if (l_ret == 0) {
            continue;
        }

        int l_client_fd = accept(
            l_fd,
            (struct sockaddr *)&l_addr,
            (socklen_t*)&l_addr
        );

        if (l_client_fd < 0) {
            m_log->error("accept() unix socket error {}", l_client_fd);
        }

        m_log->info("accept unix socket connection");

        handle_control(l_client_fd);
    }

    close(l_fd);

    m_log->trace("ebpfsnitch_daemon::control_thread() exit");
}

void
ebpfsnitch_daemon::handle_control(const int p_sock)
{
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        if (m_shutdown.load()) {
            break;
        }

        struct event_t l_event;
        
        {
            std::lock_guard<std::mutex> l_guard(m_events_lock);

            if (m_events.size() == 0) {
                continue;
            }

            l_event = m_events.front();
            m_events.pop();
        }

        const nlohmann::json l_json = {
            { "executable",      l_event.m_executable       },
            { "userId",          l_event.m_user_id          },
            { "processId",       l_event.m_process_id       },
            { "sourcePort",      l_event.m_source_port      },
            { "destinationPort", l_event.m_destination_port }
        };

        const std::string l_json_serialized = l_json.dump() + "\n";

        const ssize_t l_status = write(
            p_sock ,
            l_json_serialized.c_str(),
            l_json_serialized.size()
        );

        if (l_status == -1) {
            m_log->error("write() failed");

            break;
        }

        unsigned char l_line[1024];
        ssize_t l_rv = read(p_sock, &l_line, 1024);
        unsigned char *l_p = (unsigned char *)memchr(l_line, '\n', 1024);
        
        if (l_p == NULL) {
            m_log->error("no newline");

            break;
        }

        *l_p = '\0';

        m_log->info("got command |{}|", l_line);

        nlohmann::json l_verdict = nlohmann::json::parse(l_line);

        {
            std::lock_guard<std::mutex> l_guard(m_verdicts_lock);
            m_verdicts[l_verdict["executable"]] = l_verdict["allow"];
            
            std::lock_guard<std::mutex> l_guard2(m_events_lock);
            m_active_queries.erase(l_verdict["executable"]);
        }

        process_unhandled();
    }

    close(p_sock);
}

void
ebpfsnitch_daemon::process_unhandled()
{
    std::queue<struct nfq_event_t> l_remaining;

    m_log->info("process unhandled");

    std::lock_guard<std::mutex> l_guard(m_undecided_packets_lock);

    while (m_undecided_packets.size()) {
        struct nfq_event_t l_unhandled = m_undecided_packets.front(); 

        if (!process_nfq_event(l_unhandled, false)) {
            m_log->info("still undecided");

            l_remaining.push(l_unhandled);
        }

        m_undecided_packets.pop();
    }
    
    m_undecided_packets = l_remaining;
}

std::shared_ptr<spdlog::logger> g_log;

std::string
file_to_string(const std::string &p_path) {
    std::ifstream l_stream(p_path);

    if (l_stream.is_open() == false) {
        throw std::runtime_error("std::ifstream() failed");
    }

    return std::string(
        (std::istreambuf_iterator<char>(l_stream)),
        std::istreambuf_iterator<char>()
    );
}

std::string
ipv4_to_string(const uint32_t p_address)
{
    char l_buffer[INET_ADDRSTRLEN];

    const char *const l_status = inet_ntop(
        AF_INET,
        &p_address,
        l_buffer,
        INET_ADDRSTRLEN
    );

    if (l_status == NULL) {
        throw std::runtime_error("inet_ntop() failed");
    }

    return std::string(l_buffer);
}

static void
trace_ebpf()
{
    std::ifstream l_pipe("/sys/kernel/debug/tracing/trace_pipe");
    std::string l_line;

    while (true) {
        if (std::getline(l_pipe, l_line)) {
            g_log->trace("eBPF log: {}", l_line);
        } else {
            sleep(1);
        }
    }
}

std::shared_ptr<ebpfsnitch_daemon> g_daemon;
std::condition_variable g_shutdown;
std::mutex g_shutdown_mutex;

static void
signal_handler(const int p_sig)
{
    g_log->info("signal_handler");

    g_daemon.reset();

    g_shutdown.notify_all();
}

int
main()
{
    g_log = spdlog::stdout_color_mt("console");
    g_log->set_level(spdlog::level::trace);

    g_log->info("LIBBCC_VERSION: {}", LIBBCC_VERSION);

    signal(SIGINT, signal_handler); 

    g_daemon = std::make_shared<ebpfsnitch_daemon>(g_log);

    std::unique_lock<std::mutex> l_lock(g_shutdown_mutex);
    g_shutdown.wait(l_lock);

    g_log->info("post g_shutdown condition");

    return 0;
}