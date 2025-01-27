#pragma once

#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <optional>
#include <memory>
#include <condition_variable>

#include <spdlog/spdlog.h>

#include "misc.hpp"
#include "rule_engine.hpp"
#include "bpf_wrapper.hpp"
#include "nfq_wrapper.hpp"
#include "process_manager.hpp"

extern std::condition_variable g_shutdown;

std::string nfq_event_to_string(const nfq_event_t &p_event);

class iptables_raii {
public:
    iptables_raii(std::shared_ptr<spdlog::logger> p_log);

    ~iptables_raii();

    static void remove_rules();

private:
    std::shared_ptr<spdlog::logger> m_log;
};

class ebpfsnitch_daemon {
public:
    ebpfsnitch_daemon(
        std::shared_ptr<spdlog::logger> p_log,
        std::optional<std::string>      p_group,
        std::optional<std::string>      p_rules_path
    );

    ~ebpfsnitch_daemon();

private:
    rule_engine_t m_rule_engine;

    void filter_thread(std::shared_ptr<nfq_wrapper> p_nfq);
    void probe_thread();
    void control_thread();

    void handle_control(const int p_sock);

    std::mutex m_response_lock;
    void process_unhandled();

    void
    bpf_reader(
        void *const p_data,
        const int   p_data_size
    );

    int
    nfq_handler(const struct nlmsghdr *const p_header);

    int
    nfq_handler_incoming(const struct nlmsghdr *const p_header);

    bool
    process_nfq_event(
        const struct nfq_event_t &l_nfq_event,
        const bool                p_queue_unassociated
    );
    // packets with an application without a user verdict
    std::queue<struct nfq_event_t> m_undecided_packets;
    std::mutex m_undecided_packets_lock;

    // packets not yet associated with an application
    std::queue<struct nfq_event_t> m_unassociated_packets;
    std::mutex m_unassociated_packets_lock;
    void process_unassociated();

    std::shared_ptr<bpf_wrapper_ring> m_ring_buffer;
    std::shared_ptr<spdlog::logger>   m_log;
    const std::optional<std::string>  m_group;
    std::shared_ptr<nfq_wrapper>      m_nfq;
    std::shared_ptr<nfq_wrapper>      m_nfq_incoming;
    process_manager                   m_process_manager;

    bool
    process_associated_event(
        const struct nfq_event_t       &l_nfq_event,
        const struct connection_info_t &l_info
    );

    std::mutex m_lock;
    std::unordered_map<std::string, struct connection_info_t> m_mapping;
    std::optional<struct connection_info_t>
    lookup_connection_info(const nfq_event_t &p_event);

    std::atomic<bool> m_shutdown;
    bpf_wrapper_object m_bpf_wrapper;

    std::unique_ptr<iptables_raii> m_iptables_raii;

    void set_verdict(const uint32_t p_id, const uint32_t p_verdict);

    std::vector<std::thread> m_thread_group;

    void
    process_dns(
        const char *const p_start,
        const char *const p_end
    );

    std::mutex m_reverse_dns_lock;
    std::unordered_map<uint32_t, std::string> m_reverse_dns;
    std::optional<std::string> lookup_domain(const uint32_t p_address);
};