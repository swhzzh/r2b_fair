// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/*
 * Copyright (C) 2021 Renmin Univeristy of China
 *
 * Author: Chaoyang Liu <lcy96@ruc.edu.cn>
 *
 * Modified from dmclock (https://github.com/ceph/dmclock)
 *
 * This is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version
 * 2.1, as published by the Free Software Foundation.  See file
 * COPYING.
 */

/*
* Copyright (C) 2017 Red Hat Inc.
*
* Author: J. Eric Ivancich <ivancich@redhat.com>
*
* This is free software; you can redistribute it and/or modify it
* under the terms of the GNU Lesser General Public License version
* 2.1, as published by the Free Software Foundation.  See file
* COPYING.
*/


#pragma once

/* COMPILATION OPTIONS
 *
 * By default we include an optimization over the originally published
 * dmclock algorithm using not the values of rho and delta that were
 * sent in with a request but instead the most recent rho and delta
 * values from the requests's client. To restore the algorithm's
 * original behavior, define DO_NOT_DELAY_TAG_CALC (i.e., compiler
 * argument -DDO_NOT_DELAY_TAG_CALC).
 *
 */

#include <assert.h>

#include <cmath>
#include <memory>
#include <map>
#include <deque>
#include <queue>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <iostream>
#include <sstream>
#include <fstream>
#include <limits>
#include <unistd.h>

#include <cstring>

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
// #include <unistd.h>
#include <arpa/inet.h>

#include <atomic>

#include <boost/variant.hpp>

#include "indirect_intrusive_heap.h"
#include "run_every.h"
#include "dmclock_util.h"
#include "dmclock_recs.h"

#ifdef PROFILE
#include "profile.h"
#endif


namespace crimson {

    namespace dmclock {

        namespace c = crimson;

        constexpr double max_tag = std::numeric_limits<double>::is_iec559 ?
                                   std::numeric_limits<double>::infinity() :
                                   std::numeric_limits<double>::max();
        constexpr double min_tag = std::numeric_limits<double>::is_iec559 ?
                                   -std::numeric_limits<double>::infinity() :
                                   std::numeric_limits<double>::lowest();
        constexpr uint tag_modulo = 1000000;

        enum ClientType {
            R, // reservation client
            B, // burst client
            A,  // area client
            O // other client, ?????????resource allocation
        };

        struct ClientInfo {
            double reservation;  // minimum
            double weight;       // proportional
            double limit;        // maximum
            double resource;

            // multiplicative inverses of above, which we use in calculations
            // and don't want to recalculate repeatedly
            double reservation_inv;
            double weight_inv;
            double limit_inv;

            ClientType client_type;

            // order parameters -- min, "normal", max
            ClientInfo(double _reservation, double _weight, double _limit) :
                    reservation(_reservation),
                    weight(_weight),
                    limit(_limit),
                    reservation_inv(0.0 == reservation ? 0.0 : 1.0 / reservation),
                    weight_inv(0.0 == weight ? 0.0 : 1.0 / weight),
                    limit_inv(0.0 == limit ? 0.0 : 1.0 / limit),
                    client_type(ClientType::O) {
                // empty
            }

            ClientInfo(double _reservation, double _weight, double _limit, ClientType _client_type) :
                    reservation(_reservation),
                    weight(_weight),
                    limit(_limit),
                    reservation_inv(0.0 == reservation ? 0.0 : 1.0 / reservation),
                    weight_inv(0.0 == weight ? 0.0 : 1.0 / weight),
                    limit_inv(0.0 == limit ? 0.0 : 1.0 / limit),
                    client_type(_client_type) {
                // empty
            }

            friend std::ostream &operator<<(std::ostream &out,
                                            const ClientInfo &client) {
                out <<
                    "{ ClientInfo:: r:" << client.reservation <<
                    " w:" << std::fixed << client.weight <<
                    " l:" << std::fixed << client.limit <<
                    " 1/r:" << std::fixed << client.reservation_inv <<
                    " 1/w:" << std::fixed << client.weight_inv <<
                    " 1/l:" << std::fixed << client.limit_inv <<
                    " }";
                return out;
            }

            void update_resource(double new_res) {
                resource = new_res;
            }
        }; // class ClientInfo


        struct RequestTag {
            double reservation;
            double proportion;
            double limit;
            bool ready; // true when within limit
            Time arrival;

            RequestTag(const RequestTag &prev_tag,
                       const ClientInfo &client,
                       const uint32_t delta,
                       const uint32_t rho,
                       const Time time,
                       const double cost = 0.0,
                       const double anticipation_timeout = 0.0) :
                    ready(false),
                    arrival(time) {
                Time max_time = time;
                if (time - anticipation_timeout < prev_tag.arrival)
                    max_time -= anticipation_timeout;

                // reservation = cost + tag_calc(max_time,
                reservation = tag_calc(max_time,
                                       prev_tag.reservation,
                                       client.reservation_inv,
                                       rho,
                                       true);
                proportion = tag_calc(max_time,
                                      prev_tag.proportion,
                                      client.weight_inv,
                                      delta,
                                      true);
                limit = tag_calc(max_time,
                                 prev_tag.limit,
                                 client.limit_inv,
                                 delta,
                                 false);

                assert(reservation < max_tag || proportion < max_tag);
            }

            RequestTag(const RequestTag &prev_tag,
                       const ClientInfo &client,
                       const ReqParams req_params,
                       const Time time,
                       const double cost = 0.0,
                       const double anticipation_timeout = 0.0) :
                    RequestTag(prev_tag, client, req_params.delta, req_params.rho, time,
                               cost, anticipation_timeout) { /* empty */ }

            RequestTag(double _res, double _prop, double _lim, const Time _arrival) :
                    reservation(_res),
                    proportion(_prop),
                    limit(_lim),
                    ready(false),
                    arrival(_arrival) {
                assert(reservation < max_tag || proportion < max_tag);
            }

            RequestTag(const RequestTag &other) :
                    reservation(other.reservation),
                    proportion(other.proportion),
                    limit(other.limit),
                    ready(other.ready),
                    arrival(other.arrival) {
                // empty
            }

            static std::string format_tag_change(double before, double after) {
                if (before == after) {
                    return std::string("same");
                } else {
                    std::stringstream ss;
                    ss << format_tag(before) << "=>" << format_tag(after);
                    return ss.str();
                }
            }

            static std::string format_tag(double value) {
                if (max_tag == value) {
                    return std::string("max");
                } else if (min_tag == value) {
                    return std::string("min");
                } else {
                    return format_time(value, tag_modulo);
                }
            }

        private:

            static double tag_calc(const Time time,
                                   double prev,
                                   double increment,
                                   uint32_t dist_req_val,
                                   bool extreme_is_high) {
                if (0.0 == increment) {
                    return extreme_is_high ? max_tag : min_tag;
                } else {
                    if (0 != dist_req_val) {
                        increment *= dist_req_val;
                    }
                    return std::max(time, prev + increment);
                }
            }

            friend std::ostream &operator<<(std::ostream &out,
                                            const RequestTag &tag) {
                out <<
                    "{ RequestTag:: ready:" << (tag.ready ? "true" : "false") <<
                    " r:" << format_tag(tag.reservation) <<
                    " p:" << format_tag(tag.proportion) <<
                    " l:" << format_tag(tag.limit) <<
                    #if 0 // try to resolve this to make sure Time is operator<<'able.
                    #ifndef DO_NOT_DELAY_TAG_CALC
	  " arrival:" << tag.arrival <<
#endif
                    #endif
                    " }";
                return out;
            }
        }; // class RequestTag


        // C is client identifier type, R is request type,
        // U1 determines whether to use client information function dynamically,
        // B is heap branching factor
        template<typename C, typename R, bool U1, uint B>
        class PriorityQueueBase {
            // we don't want to include gtest.h just for FRIEND_TEST
            friend class dmclock_server_client_idle_erase_Test;

            friend class dmclock_server_client_resource_update_Test;

            friend class dmclock_server_reserv_client_info_Test;

            friend class dmclock_server_pull_ready_and_under_limit_Test;

            friend class dmclock_server_pull_burst_duration_Test;

            friend class dmclock_server_pull_schedule_order_Test;

            friend class dmclock_server_burst_client_info_Test;

        public:

            using RequestRef = std::unique_ptr<R>;

        protected:

            using TimePoint = decltype(std::chrono::steady_clock::now());
            using Duration = std::chrono::milliseconds;
            using MarkPoint = std::pair<TimePoint, Counter>;

            enum class ReadyOption {
                ignore, lowers, raises
            };

            // forward decl for friend decls
            template<double RequestTag::*, ReadyOption, bool>
            struct ClientCompare;

            class ClientReq {
                friend PriorityQueueBase;

                RequestTag tag;
                C client_id;
                RequestRef request;
                ClientType client_type;

            public:

                ClientReq(const RequestTag &_tag,
                          const C &_client_id,
                          RequestRef &&_request) :
                        tag(_tag),
                        client_id(_client_id),
                        request(std::move(_request)) {
                    // empty
                }

                friend std::ostream &operator<<(std::ostream &out, const ClientReq &c) {
                    out << "{ ClientReq:: tag:" << c.tag << " client:" <<
                        c.client_id << " }";
                    return out;
                }
            }; // class ClientReq

        public:

            // NOTE: ClientRec is in the "public" section for compatibility
            // with g++ 4.8.4, which complains if it's not. By g++ 6.3.1
            // ClientRec could be "protected" with no issue. [See comments
            // associated with function submit_top_request.]
            class ClientRec {
                friend PriorityQueueBase<C, R, U1, B>;

                C client;
                RequestTag prev_tag;
                std::deque<ClientReq> requests;

                // amount added from the proportion tag as a result of
                // an idle client becoming unidle
                double prop_delta = 0.0;

                c::IndIntruHeapData reserv_heap_data{};
                c::IndIntruHeapData deltar_heap_data{};
                c::IndIntruHeapData r_limit_heap_data{};
                c::IndIntruHeapData lim_heap_data{};
                c::IndIntruHeapData ready_heap_data{};
                c::IndIntruHeapData burst_heap_data{};
                c::IndIntruHeapData best_heap_data{};
                c::IndIntruHeapData best_limit_heap_data{};
//#if USE_PROP_HEAP
//                c::IndIntruHeapData prop_heap_data{};
//#endif

            public:

                const ClientInfo *info;
                bool idle;
                Counter last_tick;
                uint32_t cur_rho;
                uint32_t cur_delta;

                double resource;
                // deltar counter
                std::atomic_uint deltar_counter;
                std::atomic_uint deltar_break_limit_counter;
                double deltar;
                double dlimit;
                // burst request counter
                std::atomic_uint b_counter;
                std::atomic_uint b_break_limit_counter;
                // burst slice: t = resource * win_size / limit
//                Time burst_slice = 1.0;

                // counter for test
                std::atomic_uint r0_counter;
                std::atomic_uint r0_break_limit_counter;
                std::atomic_uint be_counter;
                std::atomic_uint be_break_limit_counter;

                std::atomic_uint r_compensation;

                ClientRec(C _client,
                          const ClientInfo *_info,
                          Counter current_tick) :
                        client(_client),
                        prev_tag(0.0, 0.0, 0.0, TimeZero),
                        info(_info),
                        idle(true),
                        last_tick(current_tick),
                        cur_rho(1),
                        cur_delta(1) {
                    deltar_counter.store(0);
                    deltar_break_limit_counter.store(0);
                    b_counter.store(0);
                    b_break_limit_counter.store(0);
                    r0_counter.store(0);
                    r0_break_limit_counter.store(0);
                    be_counter.store(0);
                    be_break_limit_counter.store(0);
                    // empty
                    r_compensation.store(0);
                }

                inline const RequestTag &get_req_tag() const {
                    return prev_tag;
                }

                static inline void assign_unpinned_tag(double &lhs, const double rhs) {
                    if (rhs != max_tag && rhs != min_tag) {
                        lhs = rhs;
                    }
                }

                inline void update_req_tag(const RequestTag &_prev,
                                           const Counter &_tick) {
                    assign_unpinned_tag(prev_tag.reservation, _prev.reservation);
                    assign_unpinned_tag(prev_tag.limit, _prev.limit);
                    assign_unpinned_tag(prev_tag.proportion, _prev.proportion);
                    prev_tag.arrival = _prev.arrival;
                    last_tick = _tick;
                }

                inline void add_request(const RequestTag &tag,
                                        const C &client_id,
                                        RequestRef &&request) {
                    requests.emplace_back(ClientReq(tag, client_id, std::move(request)));
                }

                inline const ClientReq &next_request() const {
                    return requests.front();
                }

                inline ClientReq &next_request() {
                    return requests.front();
                }

                inline void pop_request() {
                    requests.pop_front();
                }

                inline bool has_request() const {
                    return !requests.empty();
                }

                inline size_t request_count() const {
                    return requests.size();
                }

                // NB: because a deque is the underlying structure, this
                // operation might be expensive
                bool remove_by_req_filter_fw(std::function<bool(RequestRef &&)> filter_accum) {
                    bool any_removed = false;
                    for (auto i = requests.begin();
                         i != requests.end();
                        /* no inc */) {
                        if (filter_accum(std::move(i->request))) {
                            any_removed = true;
                            i = requests.erase(i);
                        } else {
                            ++i;
                        }
                    }
                    return any_removed;
                }

                // NB: because a deque is the underlying structure, this
                // operation might be expensive
                bool remove_by_req_filter_bw(std::function<bool(RequestRef &&)> filter_accum) {
                    bool any_removed = false;
                    for (auto i = requests.rbegin();
                         i != requests.rend();
                        /* no inc */) {
                        if (filter_accum(std::move(i->request))) {
                            any_removed = true;
                            i = decltype(i){requests.erase(std::next(i).base())};
                        } else {
                            ++i;
                        }
                    }
                    return any_removed;
                }

                inline bool
                remove_by_req_filter(std::function<bool(RequestRef &&)> filter_accum,
                                     bool visit_backwards) {
                    if (visit_backwards) {
                        return remove_by_req_filter_bw(filter_accum);
                    } else {
                        return remove_by_req_filter_fw(filter_accum);
                    }
                }

                friend std::ostream &
                operator<<(std::ostream &out,
                           const typename PriorityQueueBase<C, R, U1, B>::ClientRec &e) {
                    out << "{ ClientRec::" <<
                        " client:" << e.client <<
                        " prev_tag:" << e.prev_tag <<
                        " req_count:" << e.requests.size() <<
                        " top_req:";
                    if (e.has_request()) {
                        out << e.next_request();
                    } else {
                        out << "none";
                    }
                    out << " }";

                    return out;
                }

//                void update_burst_slice(double win_size) {
//                    if (info->client_type == ClientType::B) {
//                        burst_slice = resource * win_size * info->limit_inv;
//                    }
//                }
            }; // class ClientRec

            using ClientRecRef = std::shared_ptr<ClientRec>;

            // when we try to get the next request, we'll be in one of three
            // situations -- we'll have one to return, have one that can
            // fire in the future, or not have any
            enum class NextReqType {
                returning, future, none
            };

            // specifies which queue next request will get popped from
            enum class HeapId {
                reservation, deltar, ready, burst, prop, best_effort
            };

            // this is returned from next_req to tell the caller the situation
            struct NextReq {
                NextReqType type;
                union {
                    HeapId heap_id;
                    Time when_ready;
                };

                inline explicit NextReq() :
                        type(NextReqType::none) {}

                inline NextReq(HeapId _heap_id) :
                        type(NextReqType::returning),
                        heap_id(_heap_id) {}

                inline NextReq(Time _when_ready) :
                        type(NextReqType::future),
                        when_ready(_when_ready) {}

                // calls to this are clearer than calls to the default
                // constructor
                static inline NextReq none() {
                    return NextReq();
                }
            };


            // a function that can be called to look up client information
            using ClientInfoFunc = std::function<const ClientInfo *(const C &)>;


            bool empty() const {
                // TODO: to be modified
                DataGuard g(data_mtx);
//	            return (prop_heap.empty() || ! prop_heap.top().has_request());
                return (resv_heap.empty() || !resv_heap.top().has_request()) &&
                       (burst_heap.empty() || !burst_heap.top().has_request()) &&
                       (best_heap.empty() || !best_heap.top().has_request());
            }


            size_t client_count() const {
                DataGuard g(data_mtx);
                return client_map.size();
            }


            size_t request_count() const {
                // TODO: to be modified
                DataGuard g(data_mtx);
                size_t total = 0;
                for (auto i = resv_heap.cbegin(); i != resv_heap.cend(); ++i) {
                    total += i->request_count();
                }
                for (auto i = burst_heap.cbegin(); i != burst_heap.cend(); ++i) {
                    total += i->request_count();
                }
                for (auto i = best_heap.cbegin(); i != best_heap.cend(); ++i) {
                    total += i->request_count();
                }
                return total;
            }


            bool remove_by_req_filter(std::function<bool(RequestRef &&)> filter_accum,
                                      bool visit_backwards = false) {
                bool any_removed = false;
                DataGuard g(data_mtx);
                for (auto i : client_map) {
                    bool modified =
                            i.second->remove_by_req_filter(filter_accum, visit_backwards);
                    if (modified) {
                        // TODO: by different client type
                        if (i.second->info->client_type == ClientType::R) {
                            resv_heap.adjust(*i.second);
                            deltar_heap.adjust(*i.second);
                            r_limit_heap.adjust(*i.second);
                        }

                        if (i.second->info->client_type == ClientType::B) {
                            burst_heap.adjust(*i.second);
                            limit_heap.adjust(*i.second);
                        }

                        if (i.second->info->client_type == ClientType::A ||
                            i.second->info->client_type == ClientType::O) {
                            best_heap.adjust(*i.second);
                            best_limit_heap.adjust(*i.second);
                        }

//                        prop_heap.adjust(*i.second);

                        any_removed = true;
                    }
                }
                return any_removed;
            }


            // use as a default value when no accumulator is provide
            static void request_sink(RequestRef &&req) {
                // do nothing
            }


            void remove_by_client(const C &client,
                                  bool reverse = false,
                                  std::function<void(RequestRef &&)> accum = request_sink) {
                DataGuard g(data_mtx);

                auto i = client_map.find(client);

                if (i == client_map.end()) return;

                if (reverse) {
                    for (auto j = i->second->requests.rbegin();
                         j != i->second->requests.rend();
                         ++j) {
                        accum(std::move(j->request));
                    }
                } else {
                    for (auto j = i->second->requests.begin();
                         j != i->second->requests.end();
                         ++j) {
                        accum(std::move(j->request));
                    }
                }

                i->second->requests.clear();
// TODO: by different client type
                if (i->second->info->client_type == ClientType::R) {
                    resv_heap.adjust(*i->second);
                    deltar_heap.adjust(*i->second);
                    r_limit_heap.adjust(*i->second);
                    //reduce_total_reserv(i->second->info->reservation);
                }

                if (i->second->info->client_type == ClientType::B) {
                    burst_heap.adjust(*i->second);
                    limit_heap.adjust(*i->second);
                }
                if (i->second->info->client_type == ClientType::A || i->second->info->client_type == ClientType::O) {
                    best_heap.adjust(*i->second);
                    best_limit_heap.adjust(*i->second);
                }

//                prop_heap.adjust(*i->second);

                // reduce_total_wgt(i->second->info->weight);
                // update_client_res();
                if (ClientType::O != i->second->info->client_type) {
                    add_total_wgt_and_update_client_res(0 - i->second->info->weight);
                }
            }


            uint get_heap_branching_factor() const {
                return B;
            }


            void update_client_info(const C &client_id) {
                DataGuard g(data_mtx);
                auto client_it = client_map.find(client_id);
                if (client_map.end() != client_it) {
                    ClientRec &client = (*client_it->second);
                    double old_wgt = client.info->weight;
                    //reduce_total_wgt(client.info->weight);
                    // reduce_total_reserv(client.info->reservation);
                    client.info = client_info_f(client_id);
                    // add_total_wgt(client.info->weight);
                    //add_total_reserv(client.info->reservation);
                    if (ClientType::O != client.info->client_type) {
                        add_total_wgt_and_update_client_res(client.info->weight - old_wgt);
                    }
                }
            }


            void update_client_infos() {
                DataGuard g(data_mtx);
                for (auto i : client_map) {
                    i.second->info = client_info_f(i.second->client);
                }
            }


            friend std::ostream &operator<<(std::ostream &out,
                                            const PriorityQueueBase &q) {
                std::lock_guard<decltype(q.data_mtx)> guard(q.data_mtx);

                out << "{ PriorityQueue::";
                for (const auto &c : q.client_map) {
                    out << "  { client:" << c.first << ", record:" << *c.second <<
                        " }";
                }
                if (!q.resv_heap.empty()) {
                    const auto &resv = q.resv_heap.top();
                    out << " { reservation_top:" << resv << " }";
                    const auto &ready = q.burst_heap.top();
                    out << " { ready_top:" << ready << " }";
                    const auto &limit = q.limit_heap.top();
                    out << " { limit_top:" << limit << " }";
                } else {
                    out << " HEAPS-EMPTY";
                }
                out << " }";

                return out;
            }

            // for debugging
            void display_queues(std::ostream &out,
                                bool show_res = true,
                                bool show_lim = true,
                                bool show_ready = true,
                                bool show_prop = true) const {
                auto filter = [](const ClientRec &e) -> bool { return true; };
                DataGuard g(data_mtx);
                if (show_res) {
                    resv_heap.display_sorted(out << "RESER:", filter);
                    deltar_heap.display_sorted(out << "DELTA:", filter);
                }
                if (show_lim) {
                    limit_heap.display_sorted(out << "LIMIT:", filter);
                }
                if (show_ready) {
                    burst_heap.display_sorted(out << "READY:", filter);
                }
//#if USE_PROP_HEAP
//                if (show_prop) {
//                    prop_heap.display_sorted(out << "PROPO:", filter);
//                }
//#endif
            } // display_queues


        protected:

            // The ClientCompare functor is essentially doing a precedes?
            // operator, returning true if and only if the first parameter
            // must precede the second parameter. If the second must precede
            // the first, or if they are equivalent, false should be
            // returned. The reason for this behavior is that it will be
            // called to test if two items are out of order and if true is
            // returned it will reverse the items. Therefore false is the
            // default return when it doesn't matter to prevent unnecessary
            // re-ordering.
            //
            // The template is supporting variations in sorting based on the
            // heap in question and allowing these variations to be handled
            // at compile-time.
            //
            // tag_field determines which tag is being used for comparison
            //
            // ready_opt determines how the ready flag influences the sort
            //
            // use_prop_delta determines whether the proportional delta is
            // added in for comparison
            template<double RequestTag::*tag_field,
                    ReadyOption ready_opt,
                    bool use_prop_delta>
            struct ClientCompare {
                bool operator()(const ClientRec &n1, const ClientRec &n2) const {
                    if (n1.has_request()) {
                        if (n2.has_request()) {
                            const auto &t1 = n1.next_request().tag;
                            const auto &t2 = n2.next_request().tag;
                            if (ReadyOption::ignore == ready_opt || t1.ready == t2.ready) {
                                // if we don't care about ready or the ready values are the same
                                if (use_prop_delta) {
                                    return (t1.*tag_field + n1.prop_delta) <
                                           (t2.*tag_field + n2.prop_delta);
                                } else {
                                    return t1.*tag_field < t2.*tag_field;
                                }
                            } else if (ReadyOption::raises == ready_opt) {
                                // use_ready == true && the ready fields are different
                                return t1.ready;
                            } else {
                                return t2.ready;
                            }
                        } else {
                            // n1 has request but n2 does not
                            return true;
                        }
                    } else if (n2.has_request()) {
                        // n2 has request but n1 does not
                        return false;
                    } else {
                        // both have none; keep stable w false
                        return false;
                    }
                }
            };

            ClientInfoFunc client_info_f;
            static constexpr bool is_dynamic_cli_info_f = U1;

            mutable std::mutex data_mtx;
            using DataGuard = std::lock_guard<decltype(data_mtx)>;

            // stable mapping between client ids and client queues
            std::map<C, ClientRecRef> client_map;

            //
            std::map<C, int> client_no;
            std::atomic_uint next_client_no;

            std::map<C, const ClientInfo*> compensated_client_map; 

            c::IndIntruHeap<ClientRecRef,
                    ClientRec,
                    &ClientRec::reserv_heap_data,
                    ClientCompare<&RequestTag::reservation,
                            ReadyOption::ignore,
                            false>,
                    B> resv_heap;
            c::IndIntruHeap<ClientRecRef,
                    ClientRec,
                    &ClientRec::deltar_heap_data,
                    ClientCompare<&RequestTag::proportion,
                            ReadyOption::raises,
                            true>,
                    B> deltar_heap;
            c::IndIntruHeap<ClientRecRef,
                    ClientRec,
                    &ClientRec::r_limit_heap_data,
                    ClientCompare<&RequestTag::limit,
                            ReadyOption::lowers,
                            false>,
                    B> r_limit_heap;
//#if USE_PROP_HEAP
//            c::IndIntruHeap<ClientRecRef,
//                    ClientRec,
//                    &ClientRec::prop_heap_data,
//                    ClientCompare<&RequestTag::proportion,
//                            ReadyOption::raises,
//                            true>,
//                    B> prop_heap;
//#endif
            c::IndIntruHeap<ClientRecRef,
                    ClientRec,
                    &ClientRec::lim_heap_data,
                    ClientCompare<&RequestTag::limit,
                            ReadyOption::lowers,
                            false>,
                    B> limit_heap;
            c::IndIntruHeap<ClientRecRef,
                    ClientRec,
                    &ClientRec::burst_heap_data,
                    ClientCompare<&RequestTag::proportion,
                            ReadyOption::raises,
                            true>,
                    B> burst_heap;
            c::IndIntruHeap<ClientRecRef,
                    ClientRec,
                    &ClientRec::best_heap_data,
                    ClientCompare<&RequestTag::proportion,
                            ReadyOption::raises,
                            true>,
                    B> best_heap;
            c::IndIntruHeap<ClientRecRef,
                    ClientRec,
                    &ClientRec::best_limit_heap_data,
                    ClientCompare<&RequestTag::limit,
                            ReadyOption::lowers,
                            false>,
                    B> best_limit_heap;
            // if all reservations are met and all other requestes are under
            // limit, this will allow the request next in terms of
            // proportion to still get issued
            bool allow_limit_break;
            double anticipation_timeout;

            std::atomic_bool finishing;

            // every request creates a tick
            Counter tick = 0;

            // performance data collection
            size_t reserv_sched_count = 0;
            size_t prop_sched_count = 0;
            size_t limit_break_sched_count = 0;

            Duration idle_age;
            Duration erase_age;
            Duration check_time;
            std::deque<MarkPoint> clean_mark_points;

            // system capacity
            double system_capacity;
            // start time of window
            Time win_start = 0.0;
            // size of time window
            Time win_size = 20.0;
            double total_wgt = 0.0;
            double total_res = 0.0;

            std::ofstream ofs;
            std::ofstream ofs_pwd;
            std::string s_path;
            int client_socket;

            // mutex for the end of a window
            std::mutex m_win;
            std::mutex m_update_wgt_res;

            // NB: All threads declared at end, so they're destructed first!

            std::unique_ptr<RunEvery> cleaning_job;


            void init_client_socket() {
                client_socket = socket(AF_INET, SOCK_STREAM, 0);
                if (client_socket == -1) {
                    // std::cout << "Error: socket" << std::endl;
                    return;
                }
                // connect
                struct sockaddr_in serverAddr;
                serverAddr.sin_family = AF_INET;
                serverAddr.sin_port = htons(18000);
                serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
                if (connect(client_socket, (struct sockaddr *) &serverAddr, sizeof(serverAddr)) < 0) {
                    // std::cout << "Error: connect" << std::endl;
                    client_socket = -1;
                    return;
                }
            }

            // COMMON constructor that others feed into; we can accept three
            // different variations of durations
            template<typename Rep, typename Per>
            PriorityQueueBase(ClientInfoFunc _client_info_f,
                              std::chrono::duration<Rep, Per> _idle_age,
                              std::chrono::duration<Rep, Per> _erase_age,
                              std::chrono::duration<Rep, Per> _check_time,
                              bool _allow_limit_break,
                              double _anticipation_timeout) :
                    client_info_f(_client_info_f),
                    allow_limit_break(_allow_limit_break),
                    anticipation_timeout(_anticipation_timeout),
                    finishing(false),
                    idle_age(std::chrono::duration_cast<Duration>(_idle_age)),
                    erase_age(std::chrono::duration_cast<Duration>(_erase_age)),
                    check_time(std::chrono::duration_cast<Duration>(_check_time)),
                    system_capacity(8000),
                    win_size(30) {
                assert(_erase_age >= _idle_age);
                assert(_check_time < _idle_age);
                cleaning_job =
                        std::unique_ptr<RunEvery>(
                                new RunEvery(check_time,
                                             std::bind(&PriorityQueueBase::do_clean, this)));
                //ofs.open("/root/swh/result/scheduling.txt", std::ios_base::out | std::ios_base::app);
                char path[255];
                getcwd(path, 255);
                s_path = path;
                s_path += "/scheduling.txt";
//              init_client_socket();
                next_client_no.store(0);
                // just update client res due to the update of the sys cap and win size
                add_total_wgt_and_update_client_res(0);
            }

            template<typename Rep, typename Per>
            PriorityQueueBase(ClientInfoFunc _client_info_f,
                              std::chrono::duration<Rep, Per> _idle_age,
                              std::chrono::duration<Rep, Per> _erase_age,
                              std::chrono::duration<Rep, Per> _check_time,
                              bool _allow_limit_break,
                              double _anticipation_timeout,
                              double _system_capacity,
                              double _mclock_win_size) :
                    client_info_f(_client_info_f),
                    allow_limit_break(_allow_limit_break),
                    anticipation_timeout(_anticipation_timeout),
                    finishing(false),
                    idle_age(std::chrono::duration_cast<Duration>(_idle_age)),
                    erase_age(std::chrono::duration_cast<Duration>(_erase_age)),
                    check_time(std::chrono::duration_cast<Duration>(_check_time)),
                    system_capacity(_system_capacity),
                    win_size(_mclock_win_size) {
                assert(_erase_age >= _idle_age);
                assert(_check_time < _idle_age);
                cleaning_job =
                        std::unique_ptr<RunEvery>(
                                new RunEvery(check_time,
                                             std::bind(&PriorityQueueBase::do_clean, this)));
                //ofs.open("/root/swh/result/scheduling.txt", std::ios_base::out | std::ios_base::app);
                char path[255];
                getcwd(path, 255);
                s_path = path;
                s_path += "/scheduling.txt";
//              init_client_socket();
                next_client_no.store(0);
                // just update client res due to the update of the sys cap and win size
                add_total_wgt_and_update_client_res(0);
            }

            ~PriorityQueueBase() {
                finishing = true;
//              close(client_socket);
                //ofs.close();
            }


            inline const ClientInfo *get_cli_info(ClientRec &client) const {
                if (is_dynamic_cli_info_f) {
                    client.info = client_info_f(client.client);
                }
                return client.info;
            }

            // ????????????????????????is_dynamic_cli_info_f, ??????????????????????????????????????????weight
            // ???????????????????????????, ????????????????????????
            // weight tag is used to limit the total resource of certain client, but deltar is just the incremental part of reservation client
            const ClientInfo *client_info_wrapper(ClientRec &client) {
//                 if (is_dynamic_cli_info_f) {
//                     // for weight update
//                     const ClientInfo* temp_client_info = client_info_f(client.client);
//                     // ????????????, ?????????????????????????????????????????????, ??????, mClockPoolQueue??????????????????????????????, ???????????????????????????
//                     // ????????????????????????struct???new???delete, ????????????mClockPoolQueue???????????????, ??????????????????????????????????????????, ??????????????????dmclock?????????dynamic????????? ???????????????
//                     // ?????????????????????????????????
//                     if (temp_client_info != client.info)
//                     {
//                         // ????????????, ??????window?????????????????????
//                         new_client_map[client.client] = temp_client_info;
//                     }
                    
//                     // if (temp_client_info->weight != client.info->weight){
//                     //     add_total_wgt_and_update_client_res(temp_client_info->weight - client.info->weight);
//                     // }
//                     // client.info = client_info_f(client.client);
// //                if (client.info->client_type == ClientType::R) {
// //                  const std::shared_ptr<ClientInfo> info(
// //                          new ClientInfo(client.info->reservation, client.deltar, client.info->limit, ClientType::R));
// //                  return info.get();
// //                }
//                 }
                if (client.info->client_type == ClientType::R) {
                    // const std::shared_ptr<ClientInfo> info(
                    //         new ClientInfo(client.info->reservation + client.r_compensation, client.info->weight,
                    //                        client.info->limit, ClientType::R));
                    // return info.get();
                    return compensated_client_map[client.client];
                }
//                if (client.info->client_type == ClientType::B) {
//                    const std::shared_ptr<ClientInfo> info(
//                            new ClientInfo(0.0, client.info->weight, client.dlimit, ClientType::B));
//                    return info.get();
//                }
                return client.info;
            }

            // ?????????window?????????????????????, ??????????????????R???????????????????????????, 
            // ????????????R???reservation??????????????????, ??????????????????????????????????????????????????????, ???????????????r????????????, ???????????????????????????????????????????????????
            // ???????????? ??????????????????????????????????????????, ?????????check??????window?????????????????????client_type????????????????? ??????????????????
            void handle_client_type_change(std::shared_ptr<ClientRec> client_rec, const ClientInfo* new_client_info){

                // if (client_rec->info->client_type == new_client_info->client_type)
                // {
                //     return;
                // }
                move_to_another_heap(client_rec, new_client_info);
                
                
                

            }

            void move_to_another_heap(std::shared_ptr<ClientRec> client, const ClientInfo* new_client_info){
                // delete from original heap
                delete_from_heaps(client);
                if (client->has_request())
                {
                    if (ClientType::R == new_client_info->client_type)
                    {
                        if (!resv_heap.empty())
                        {
                            auto& top = resv_heap.top();
                            if (top.has_request())
                            {
                                client->next_request().tag = RequestTag(top.next_request().tag);
                            }
                            client->prev_tag = RequestTag(top.prev_tag);
                        }
                    }
                    else if (ClientType::B == new_client_info->client_type)
                    {
                        if (!burst_heap.empty())
                        {
                            auto& top = burst_heap.top();
                            if (top.has_request())
                            {
                            client->next_request().tag = RequestTag(top.next_request().tag);
                            }
                            client->prev_tag = RequestTag(top.prev_tag);
                        }
                        
                    }
                    else{
                        if (!best_heap.empty())
                        {
                            auto& top = best_heap.top();
                            if (top.has_request())
                            {
                            client->next_request().tag = RequestTag(top.next_request().tag);
                            }
                            client->prev_tag = RequestTag(top.prev_tag);
                        }
                    }
                }
                else{
                    if (ClientType::R == new_client_info->client_type)
                    {
                        if (!resv_heap.empty())
                        {
                            auto& top = resv_heap.top();
                            client->prev_tag = RequestTag(top.prev_tag);
                        }
                    }
                    else if (ClientType::B == new_client_info->client_type)
                    {
                        if (!burst_heap.empty())
                        {
                            auto& top = burst_heap.top();
                            client->prev_tag = RequestTag(top.prev_tag);
                        }
                        
                    }
                    else{
                        if (!best_heap.empty())
                        {
                            auto& top = best_heap.top();
                            client->prev_tag = RequestTag(top.prev_tag);
                        }
                    }
                }
                // add to new heap
                if (new_client_info->client_type == ClientType::R) {
                    resv_heap.push(client);
                    r_limit_heap.push(client);
                    deltar_heap.push(client);
                    resv_heap.adjust(*client);
                    r_limit_heap.adjust(*client);
                    deltar_heap.adjust(*client.get());
                }

                else if (new_client_info->client_type == ClientType::B) {
                    limit_heap.push(client);
                    burst_heap.push(client);
                    limit_heap.adjust(*client);
                    burst_heap.adjust(*client);
                }

                else if (new_client_info->client_type == ClientType::A || new_client_info->client_type == ClientType::O) {
                    best_heap.push(client);
                    best_limit_heap.push(client);
                    best_heap.adjust(*client);
                    best_limit_heap.adjust(*client);
                }
                
                
            }


            void printScheduling(std::shared_ptr<ClientRec> client) {
                std::string client_name;
                if (ClientType::R == client->info->client_type) {
                    client_name = "R_";
                } else if (ClientType::B == client->info->client_type) {
                    client_name = "B_";
                } else if (ClientType::A == client->info->client_type) {
                    client_name = "A_";
                } else {
                    client_name = "O_";
                }

//                if (typeid(client->client) != typeid(int)){
                client_name += std::to_string(client_no[client->client]);
//                }
                std::stringstream s_builder;
                s_builder << std::fixed << get_time() << "," << client_name << "(" << client->resource << ", "
                          << client->info->reservation << "+" << client->r_compensation << ","
                          << client->info->weight << ", " << client->info->limit << "):\t"
                          << client->r0_counter << ", " << client->r0_break_limit_counter << ", "
                          << client->deltar_counter << ", " << client->deltar_break_limit_counter << ", "
                          << client->b_counter << ", " << client->b_break_limit_counter << ", "
                          << client->be_counter << ", " << client->be_break_limit_counter << "\n";
                const std::string info = s_builder.str();
//              ofs << info;
                ofs_pwd << info;
//              if (client_socket != -1) {
//                send(client_socket, info.c_str(), info.length(), 0);
//              } else {
//                ofs << "socket connect failed" << std::endl;
//                ofs_pwd << "socket connect failed" << std::endl;
//              }
            }

            // data_mtx must be held by caller
            void do_add_request(RequestRef &&request,
                                const C &client_id,
                                const ReqParams &req_params,
                                const Time time,
                                const double cost = 0.0) {
                ++tick;

                // this pointer will help us create a reference to a shared
                // pointer, no matter which of two codepaths we take
                ClientRec *temp_client;

                auto client_it = client_map.find(client_id);
                if (client_map.end() != client_it) {
                    temp_client = &(*client_it->second); // address of obj of shared_ptr
                } else {
                    const ClientInfo *info = client_info_f(client_id);
                    ClientRecRef client_rec =
                            std::make_shared<ClientRec>(client_id, info, tick);
                    if (info->client_type == ClientType::R) {
                        resv_heap.push(client_rec);
                        r_limit_heap.push(client_rec);
                        deltar_heap.push(client_rec);
                    }

                    if (info->client_type == ClientType::B) {
                        limit_heap.push(client_rec);
                        burst_heap.push(client_rec);
                    }

                    if (info->client_type == ClientType::A || info->client_type == ClientType::O) {
                        best_heap.push(client_rec);
                        best_limit_heap.push(client_rec);
                    }

//                    prop_heap.push(client_rec);

                    client_map[client_id] = client_rec;
                    compensated_client_map[client_id] = new ClientInfo(info->reservation, info->weight, info->limit, info->client_type);
//                    client_no[client_id] = atomic_fetch_add(&next_client_no, 1);
                    client_no[client_id] = next_client_no.fetch_add(1);

                    // add_total_wgt(info->weight);
                    //add_total_reserv(info->reservation);
                    // update clients' resource
                    // update_client_res();
                    if (ClientType::O != info->client_type) {
                        add_total_wgt_and_update_client_res(info->weight);
                    }
                    temp_client = &(*client_rec); // address of obj of shared_ptr
                }
                // for convenience, we'll create a reference to the shared pointer
                ClientRec &client = *temp_client;

                if (client.idle) {
                    // We need to do an adjustment so that idle clients compete
                    // fairly on proportional tags since those tags may have
                    // drifted from real-time. Either use the lowest existing
                    // proportion tag -- O(1) -- or the client with the lowest
                    // previous proportion tag -- O(n) where n = # clients.
                    //
                    // So we don't have to maintain a propotional queue that
                    // keeps the minimum on proportional tag alone (we're
                    // instead using a ready queue), we'll have to check each
                    // client.
                    //
                    // The alternative would be to maintain a proportional queue
                    // (define USE_PROP_TAG) and do an O(1) operation here.

                    // Was unable to confirm whether equality testing on
                    // std::numeric_limits<double>::max() is guaranteed, so
                    // we'll use a compile-time calculated trigger that is one
                    // third the max, which should be much larger than any
                    // expected organic value.
                    constexpr double lowest_prop_tag_trigger =
                            std::numeric_limits<double>::max() / 3.0;

                    double lowest_prop_tag = std::numeric_limits<double>::max();
                    for (auto const &c : client_map) {
                        // don't use ourselves (or anything else that might be
                        // listed as idle) since we're now in the map
                        if (!c.second->idle) {
                            double p;
                            // use either lowest proportion tag or previous proportion tag
                            if (c.second->has_request()) {
                                p = c.second->next_request().tag.proportion +
                                    c.second->prop_delta;
                            } else {
                                p = c.second->get_req_tag().proportion + c.second->prop_delta;
                            }

                            if (p < lowest_prop_tag) {
                                lowest_prop_tag = p;
                            }
                        }
                    }

                    // if this conditional does not fire, it
                    if (lowest_prop_tag < lowest_prop_tag_trigger) {
                        client.prop_delta = lowest_prop_tag - time;
                    }
                    client.idle = false;
                } // if this client was idle

#ifndef DO_NOT_DELAY_TAG_CALC
                RequestTag tag(0, 0, 0, time);

                if (!client.has_request()) {
//                    const ClientInfo* client_info = get_cli_info(client);
                    const ClientInfo *client_info = client_info_wrapper(client);
                    assert(client_info);
                    tag = RequestTag(client.get_req_tag(),
                                     *client_info,
                                     req_params,
                                     time,
                                     cost,
                                     anticipation_timeout);

                    // copy tag to previous tag for client
                    client.update_req_tag(tag, tick);
                }
#else
                const ClientInfo* client_info = client_info_wrapper(client);
                assert(client_info);
                RequestTag tag(client.get_req_tag(),
                           *client_info,
                           req_params,
                           time,
                           cost,
                           anticipation_timeout);

                // copy tag to previous tag for client
                client.update_req_tag(tag, tick);
#endif

                client.add_request(tag, client.client, std::move(request));
                if (1 == client.requests.size()) {
                    // NB: can the following 4 calls to adjust be changed
                    // promote? Can adding a request ever demote a client in the
                    // heaps?
                    if (client.info->client_type == ClientType::R) {
                        resv_heap.adjust(client);
                        r_limit_heap.adjust(client);
                        deltar_heap.adjust(client);
                    }

                    if (client.info->client_type == ClientType::B) {
                        limit_heap.adjust(client);
                        burst_heap.adjust(client);
                    }

                    if (client.info->client_type == ClientType::A || client.info->client_type == ClientType::O) {
                        best_heap.adjust(client);
                        best_limit_heap.adjust(client);
                    }

//                    prop_heap.adjust(client);
                }

                client.cur_rho = req_params.rho;
                client.cur_delta = req_params.delta;

                if (client.info->client_type == ClientType::R) {
                    resv_heap.adjust(client);
                    r_limit_heap.adjust(client);
                    deltar_heap.adjust(client);
                }

                if (client.info->client_type == ClientType::B) {
                    limit_heap.adjust(client);
                    burst_heap.adjust(client);
                }

                if (client.info->client_type == ClientType::A || client.info->client_type == ClientType::O) {
                    best_heap.adjust(client);
                    best_limit_heap.adjust(client);
                }

//                prop_heap.adjust(client);
            } // add_request


            // data_mtx should be held when called; top of heap should have
            // a ready request
            template<typename C1, IndIntruHeapData ClientRec::*C2, typename C3>
            void pop_process_request(IndIntruHeap<C1, ClientRec, C2, C3, B> &heap,
                                     std::function<void(const C &client,
                                                        RequestRef &request)> process, Time now,
                                     bool is_delta = false) {
                // gain access to data
                ClientRec &top = heap.top();

                RequestRef request = std::move(top.next_request().request);
#ifndef DO_NOT_DELAY_TAG_CALC
                RequestTag tag = top.next_request().tag;
#endif

                // pop request and adjust heaps
                top.pop_request();

#ifndef DO_NOT_DELAY_TAG_CALC
                if (top.has_request()) {
                    ClientReq &next_first = top.next_request();
//	  const ClientInfo* client_info = get_cli_info(top);
                    const ClientInfo *client_info = client_info_wrapper(top);
                    assert(client_info);
                    // next_first.tag.arrival????????????request????????????now, ???????????????
                    next_first.tag = RequestTag(tag, *client_info,
                                                top.cur_delta, top.cur_rho,
                                                next_first.tag.arrival,
                                                0.0, anticipation_timeout);

                    // copy tag to previous tag for client
                    top.update_req_tag(next_first.tag, tick);
                }
#endif
//    const ClientInfo* client_info = get_cli_info(top);
                const ClientInfo *client_info = client_info_wrapper(top);
                if (client_info->client_type == ClientType::R) {

                    if (is_delta /*&& (now - win_start) < win_size*/) {
                        // top.deltar_counter++;
                        reduce_reservation_tags(top);
                    }
                    // else{
                    //   top.r0_counter++;
                    // }
                    resv_heap.demote(top);
                    deltar_heap.demote(top);
                    r_limit_heap.adjust(top);
                }

                if (client_info->client_type == ClientType::B) {
                    // if (top.info->client_type == ClientType::B) {
                    //     top.b_counter++;
                    // }
                    burst_heap.demote(top);
                    limit_heap.adjust(top);
                }
                if (client_info->client_type == ClientType::A || client_info->client_type == ClientType::O) {
                    //   top.be_counter++;
                    best_heap.demote(top);
                    best_limit_heap.adjust(top);
                }

//                prop_heap.demote(top);


                // TODO: update counter in do_next_request


                // process
                process(top.client, request);
            } // pop_process_request


            // data_mtx should be held when called
            void reduce_reservation_tags(ClientRec &client) {
                const ClientInfo* client_info = client.info;
                if (ClientType::R == client.info->client_type)
                {
                    client_info = compensated_client_map[client.client];
                }
                
                for (auto &r : client.requests) {
                    // r.tag.reservation -= client.info->reservation_inv;
                    r.tag.reservation -= client_info->reservation_inv;

#ifndef DO_NOT_DELAY_TAG_CALC
                    // reduce only for front tag. because next tags' value are invalid
                    break;
#endif
                }
                // don't forget to update previous tag
                // client.prev_tag.reservation -= client.info->reservation_inv;
                client.prev_tag.reservation -= client_info->reservation_inv;
                resv_heap.promote(client);
            }

            // ??????????????????????????????, ???????????????????????????
            // data_mtx should be held when called
            void reduce_reservation_tags(const C &client_id) {
                auto client_it = client_map.find(client_id);

                // means the client was cleaned from map; should never happen
                // as long as cleaning times are long enough
                assert(client_map.end() != client_it);
                reduce_reservation_tags(*client_it->second);
            }

            std::string get_client_type(const ClientInfo* info){
                if (ClientType::R == info->client_type) {
                    return "R";
                } else if (ClientType::B == info->client_type) {
                    return "B";
                } else if (ClientType::A == info->client_type) {
                    return "A";
                } else {
                    return "O";
                }
            }
            // data_mtx should be held when called
            NextReq do_next_request(Time now) {
                // if proportional queue is empty, all are empty (i.e., no
                // active clients)
                if (resv_heap.empty() && burst_heap.empty() && best_heap.empty()) {
                    return NextReq::none();
                }

                if (now - win_start >= win_size) {
                    // ???????????????????????????????????????
                    std::unique_lock<std::mutex> lock(m_win, std::try_to_lock);
                    if (lock.owns_lock()) {
                        // ???????????????, ???????????????????????????????????????
                        win_start = std::max(win_start + win_size, now);

//                ofs.open("/root/swh/result/scheduling.txt", std::ios_base::app);

                        ofs_pwd.open(s_path.c_str(), std::ios_base::app);
                        for (auto c : client_map) {
                            printScheduling(c.second);

                            // ????????????clientinfo??????, ??????clientRec?????????????????????, ????????????????????????????????????
                            // ???????????????????????????????????????????????????????????????, ?????????????????? 
                            const ClientInfo* temp_client_info = client_info_f(c.second->client);
                            if (temp_client_info != c.second->info)
                            {
                                std::string new_client_type = get_client_type(temp_client_info);
                                std::string old_client_type = get_client_type(c.second->info);

                                ofs_pwd << "update: " << "(" << old_client_type << "," << c.second->info->reservation << "," << c.second->info->weight << "," << c.second->info->limit << ") -> " 
                                        <<"(" << new_client_type << "," << temp_client_info->reservation << "," << temp_client_info->weight << "," << temp_client_info->limit << ")\n";
                                // client type update
                                // ???????????????????????????pool noexist, ??????????????????clean???
                                if (temp_client_info->client_type != c.second->info->client_type)
                                {
                                    move_to_another_heap(c.second, temp_client_info);
                                }
                                const ClientInfo* for_delete = c.second->info;
                                c.second->info = temp_client_info;
                                // client weight update
                                if (temp_client_info->weight != for_delete->weight)
                                {
                                    add_total_wgt_and_update_client_res(temp_client_info->weight - for_delete->weight);
                                }
                                // delete old client info; must not delete pool_noexist
                                if (for_delete->weight != 0 || for_delete->reservation != 0 || for_delete->limit != 0)
                                {
                                    delete for_delete;
                                }
                            }

                            if (ClientType::R == c.second->info->client_type) {
                                // if (c.second->idle)
                                // {
                                //     c.second->r_compensation = 0;
                                // }
                                // ????????????, ?????????????????????, ??????????????????, ??????????????????????????????reservation???????????????????????????80%??????
                                // ?????????????????????80%, ?????????client???????????????????????????
                                if (c.second->r0_counter >= c.second->info->reservation * win_size * 0.8)
                                {
                                    int compensate =
                                        (c.second->info->reservation * win_size - c.second->r0_counter) / win_size;
                                    c.second->r_compensation += compensate;
                                    if (c.second->r_compensation < 0) {
                                        c.second->r_compensation = 0;
                                    }
                                    else if (c.second->r_compensation > c.second->info->reservation * 0.1) {
                                        c.second->r_compensation = c.second->info->reservation * 0.1;
                                    }

                                    const ClientInfo* temp_info = compensated_client_map[c.second->client];
                                    compensated_client_map[c.second->client] = new ClientInfo(c.second->info->reservation + c.second->r_compensation, c.second->info->weight,
                                               c.second->info->limit, ClientType::R);
                                    delete temp_info;

                                }
                            }

                            c.second->b_counter = 0;
                            c.second->b_break_limit_counter = 0;
                            c.second->deltar_counter = 0;
                            c.second->deltar_break_limit_counter = 0;
                            c.second->r0_counter = 0;
                            c.second->r0_break_limit_counter = 0;
                            c.second->be_counter = 0;
                            c.second->be_break_limit_counter = 0;
                        }
                        // for (auto c : client_map) {
                        //   printScheduling(c.second);
                        // }
//                ofs.close();
                        ofs_pwd.close();

                        // handle clientinfo update
                        // for (auto c: new_client_map)
                        // {

                        // }
                        // new_client_map.clear();
                    }
                }


                // try constraint (reservation) based scheduling
                if (!resv_heap.empty()) {
                    auto &reserv = resv_heap.top();
//                    reserv.r_counter = 0;
                    if (reserv.has_request() &&
                        reserv.next_request().tag.reservation <= now) {
                        reserv.r0_counter++;
                        return NextReq(HeapId::reservation);
                    }
                }

                // no existing reservations before now, so try weight-based
                // scheduling

                // all items that are within limit are eligible based on
                // priority
                if (!limit_heap.empty()) {
                    auto limits = &limit_heap.top();
                    while (limits->has_request() &&
                           !limits->next_request().tag.ready &&
                           limits->next_request().tag.limit <= now) {
                        limits->next_request().tag.ready = true;
//                        if (limits->info->client_type == ClientType::R) {
//                            deltar_heap.promote(*limits);
//                        }
//                        if (limits->info->client_type == ClientType::B) {
                        burst_heap.promote(*limits);
//                        }
//                        prop_heap.promote(*limits);
                        limit_heap.demote(*limits);

                        limits = &limit_heap.top();
                    }
                }

                // try burst based scheduling
                if (!burst_heap.empty()) {
                    auto &bursts = burst_heap.top();
                    if (bursts.b_counter < std::max(bursts.resource, 0.0) &&
                        bursts.has_request() &&
                        bursts.next_request().tag.ready &&
                        bursts.next_request().tag.proportion < max_tag) {
                        bursts.b_counter++;
                        return NextReq(HeapId::burst);
                    }
                }

                if (!r_limit_heap.empty()) {
                    auto limits = &r_limit_heap.top();
                    while (limits->has_request() &&
                           !limits->next_request().tag.ready &&
                           limits->next_request().tag.limit <= now) {
                        limits->next_request().tag.ready = true;
//                        if (limits->info->client_type == ClientType::R) {
//                            deltar_heap.promote(*limits);
//                        }
//                        if (limits->info->client_type == ClientType::B) {
                        deltar_heap.promote(*limits);
//                        }
//                        prop_heap.promote(*limits);
                        r_limit_heap.demote(*limits);

                        limits = &r_limit_heap.top();
                    }
                }


                if (!deltar_heap.empty()) {
                    auto &deltar = deltar_heap.top();
                    if (deltar.deltar_counter < std::max(deltar.resource - deltar.info->reservation * win_size, 0.0) &&
                        deltar.has_request() &&
                        deltar.next_request().tag.ready &&
                        deltar.next_request().tag.proportion < max_tag) {
                        deltar.deltar_counter++;
                        return NextReq(HeapId::deltar);
                    }
                }

                // ???????????????, ??????????????????be???be????????????client???, ready tag??????????????????, ???????????????ready????????????????????????.
             if (!best_limit_heap.empty()) {
               auto limits = &best_limit_heap.top();
               while (limits->has_request() &&
                      !limits->next_request().tag.ready &&
                      limits->next_request().tag.limit <= now) {
                 limits->next_request().tag.ready = true;

                 best_heap.promote(*limits);
                 best_limit_heap.demote(*limits);

                 limits = &best_limit_heap.top();
               }
             }

                if (!best_heap.empty()) {
                    auto &bests = best_heap.top();
                    if (bests.has_request() &&
                        bests.next_request().tag.ready &&
                        bests.next_request().tag.proportion < max_tag) {
                        bests.be_counter++;
                        return NextReq(HeapId::best_effort);
                    }
                }

                // if nothing is scheduled by reservation or
                // proportion/weight, and if we allow limit break, try to
                // schedule something with the lowest proportion tag or
                // alternatively lowest reservation tag.
                if (allow_limit_break) {

                    // ??????burst?????????????????????, ?????????????????????burst, ????????????burst????????????
                    if (!burst_heap.empty()) {
                        auto &bursts = burst_heap.top();
                        if (bursts.has_request() &&
                            bursts.next_request().tag.proportion < max_tag) {
                            bursts.b_break_limit_counter++;
                            return NextReq(HeapId::burst);
                        }
                    }

                    if (!best_heap.empty()) {
                        auto &bests = best_heap.top();
                        if (bests.has_request() &&
                            bests.next_request().tag.proportion < max_tag) {
                            bests.be_break_limit_counter++;
                            return NextReq(HeapId::best_effort);
                        }
                    }


                    if (!deltar_heap.empty()) {
                        auto &deltar = deltar_heap.top();
                        if (deltar.has_request() &&
                            deltar.next_request().tag.proportion < max_tag) {
                            deltar.deltar_break_limit_counter++;
                            return NextReq(HeapId::deltar);
                        }

                    }


                    // check reserve heap again to ensure the qos of reserve client
                    if (!resv_heap.empty()) {
                        auto &reserv = resv_heap.top();
                        if (reserv.has_request() &&
                            reserv.next_request().tag.reservation < max_tag) {
                            reserv.r0_break_limit_counter++;
                            return NextReq(HeapId::reservation);
                        }
                    }
                }



                // nothing scheduled; make sure we re-run when next
                // reservation item or next limited item comes up
                Time next_call = TimeMax;
                if (!resv_heap.empty()) {
                    if (resv_heap.top().has_request()) {
                        next_call =
                                min_not_0_time(next_call,
                                               resv_heap.top().next_request().tag.reservation);
                    }
                }
                if (!r_limit_heap.empty()) {
                    if (r_limit_heap.top().has_request()) {
                        const auto &next = r_limit_heap.top().next_request();
                        assert(!next.tag.ready || max_tag == next.tag.proportion);
                        next_call = min_not_0_time(next_call, next.tag.limit);
                    }
                }
                if (!limit_heap.empty()) {
                    if (limit_heap.top().has_request()) {
                        const auto &next = limit_heap.top().next_request();
                        assert(!next.tag.ready || max_tag == next.tag.proportion);
                        next_call = min_not_0_time(next_call, next.tag.limit);
                    }
                }
                if (next_call < TimeMax) {
                    return NextReq(next_call);
                } else {
                    return NextReq::none();
                }
            } // do_next_request


            // if possible is not zero and less than current then return it;
            // otherwise return current; the idea is we're trying to find
            // the minimal time but ignoring zero
            static inline const Time &min_not_0_time(const Time &current,
                                                     const Time &possible) {
                return TimeZero == possible ? current : std::min(current, possible);
            }


            /*
             * This is being called regularly by RunEvery. Every time it's
             * called it notes the time and delta counter (mark point) in a
             * deque. It also looks at the deque to find the most recent
             * mark point that is older than clean_age. It then walks the
             * map and delete all server entries that were last used before
             * that mark point.
             */
            void do_clean() {
                TimePoint now = std::chrono::steady_clock::now();
                DataGuard g(data_mtx);
                clean_mark_points.emplace_back(MarkPoint(now, tick));

                // first erase the super-old client records

                Counter erase_point = 0;
                auto point = clean_mark_points.front();
                while (point.first <= now - erase_age) {
                    erase_point = point.second;
                    clean_mark_points.pop_front();
                    point = clean_mark_points.front();
                }

                Counter idle_point = 0;
                for (auto i : clean_mark_points) {
                    if (i.first <= now - idle_age) {
                        idle_point = i.second;
                    } else {
                        break;
                    }
                }

                if (erase_point > 0 || idle_point > 0) {
                    for (auto i = client_map.begin(); i != client_map.end(); /* empty */) {
                        auto i2 = i++;
                        if (erase_point && i2->second->last_tick <= erase_point) {
                            delete_from_heaps(i2->second);
                            client_map.erase(i2);
                            client_no.erase(i2->first);
                            //reduce_total_wgt(i2->second->info->weight);
                            // reduce_total_reserv(i2->second->info->reservation);
                            // update_client_res();
                            if (0 == i2->second->info->weight) {
                                continue;
                            }
                            // ?????????check_removed_client?????????????????????????????????, ????????????m_update_wgt?????????, ????????????, ????????????????????????????????????????????????,
                            // ??????????????????wgt
                            // ?????????????????????????????????, ??????????????????????????????, cleanjob????????????????????????, ???????????????
                            if (ClientType::O != i2->second->info->client_type) {
                                add_total_wgt_and_update_client_res(0 - i2->second->info->weight);
                            }
                        } else if (idle_point && i2->second->last_tick <= idle_point) {
                            i2->second->idle = true;
                        }
                    } // for
                } // if

                // ??????pool???????????????????????????????????????
            } // do_clean


            // data_mtx must be held by caller
            template<IndIntruHeapData ClientRec::*C1, typename C2>
            void delete_from_heap(ClientRecRef &client,
                                  c::IndIntruHeap<ClientRecRef, ClientRec, C1, C2, B> &heap) {
                auto i = heap.rfind(client);
                heap.remove(i);
            }


            // data_mtx must be held by caller
            void delete_from_heaps(ClientRecRef &client) {
                if (client->info->client_type == ClientType::R) {
                    delete_from_heap(client, resv_heap);
                    delete_from_heap(client, deltar_heap);
                    delete_from_heap(client, r_limit_heap);
                }
                if (client->info->client_type == ClientType::A || client->info->client_type == ClientType::O) {
                    delete_from_heap(client, best_heap);
                    delete_from_heap(client, best_limit_heap);
                }
                if (client->info->client_type == ClientType::B) {
                    delete_from_heap(client, limit_heap);
                    delete_from_heap(client, burst_heap);
                }
//                delete_from_heap(client, prop_heap);
            }

            void set_win_size(Time _win_size) {
                win_size = _win_size;
            }

            void set_sys_cap(double _system_capacity) {
                system_capacity = _system_capacity;
            }

            size_t get_client_num() {
                return client_map.size();
            }

            void update_client_res() {
                for (auto c: client_map) {
                    c.second->resource = system_capacity * c.second->info->weight * win_size / total_wgt;
//                if (c.second->info->client_type == ClientType::R) {
//                  c.second->dlimit = system_capacity * c.second->info->weight / total_wgt;
//                  c.second->deltar = c.second->dlimit > c.second->info->reservation ? c.second->dlimit -
//                                                                                      c.second->info->reservation
//                                                                                    : 0;
////                        c.second->deltar = c.second->info->weight;
//                  c.second->dlimit = 0;
//                }
                }
            }

            void add_total_wgt_and_update_client_res(double wgt) {
                std::lock_guard<std::mutex> lock(m_update_wgt_res);
                //check_removed_client();
                total_wgt += wgt;
                for (auto c: client_map) {
                    c.second->resource = system_capacity * c.second->info->weight * win_size / total_wgt;
//                if (c.second->info->client_type == ClientType::R) {
//                  c.second->dlimit = system_capacity * c.second->info->weight / total_wgt;
//                  c.second->deltar = c.second->dlimit > c.second->info->reservation ? c.second->dlimit -
//                                                                                      c.second->info->reservation
//                                                                                    : 0;
////                        c.second->deltar = c.second->info->weight;
//                  c.second->dlimit = 0;
//                }
                }
            }

            void check_removed_client() {
                for (auto c: client_map) {
                    const ClientInfo *temp = client_info_f(c.second->client);
                    // ??????pool_noexist
                    if (0 == temp->weight) {
                        total_wgt -= c.second->info->weight;
                        // ????????????weight??????0, ????????????cleanjob?????????client_map????????????client
                        c.second->info = temp;
                    }
                }
            }

            void add_total_wgt(double wgt) {
                total_wgt += wgt;
            }

            void add_total_reserv(double reserv) {
                total_res += reserv;
            }

            void reduce_total_wgt(double wgt) {
                if (total_wgt > wgt) total_wgt -= wgt;
            }

            void reduce_total_reserv(double reserv) {
                if (total_res >= reserv) total_res -= reserv;
                else total_res = 0;
            }

            ClientInfo *get_real_client_info(const ClientRec &client) {
                ClientInfo new_client = ClientInfo(client.info->reservation, client.deltar, client.info->limit);
                return &new_client;
            }
        }; // class PriorityQueueBase


        template<typename C, typename R, bool U1 = false, uint B = 2>
        class PullPriorityQueue : public PriorityQueueBase<C, R, U1, B> {
            using super = PriorityQueueBase<C, R, U1, B>;

        public:

            // When a request is pulled, this is the return type.
            struct PullReq {
                struct Retn {
                    C client;
                    typename super::RequestRef request;
                    PhaseType phase;
                };

                typename super::NextReqType type;
                boost::variant<Retn, Time> data;

                bool is_none() const { return type == super::NextReqType::none; }

                bool is_retn() const { return type == super::NextReqType::returning; }

                Retn &get_retn() {
                    return boost::get<Retn>(data);
                }

                bool is_future() const { return type == super::NextReqType::future; }

                Time getTime() const { return boost::get<Time>(data); }
            };


#ifdef PROFILE
            ProfileTimer<std::chrono::nanoseconds> pull_request_timer;
            ProfileTimer<std::chrono::nanoseconds> add_request_timer;
#endif

            template<typename Rep, typename Per>
            PullPriorityQueue(typename super::ClientInfoFunc _client_info_f,
                              std::chrono::duration<Rep, Per> _idle_age,
                              std::chrono::duration<Rep, Per> _erase_age,
                              std::chrono::duration<Rep, Per> _check_time,
                              bool _allow_limit_break = false,
                              double _anticipation_timeout = 0.0) :
                    super(_client_info_f,
                          _idle_age, _erase_age, _check_time,
                          _allow_limit_break, _anticipation_timeout) {
                // empty
            }

            template<typename Rep, typename Per>
            PullPriorityQueue(typename super::ClientInfoFunc _client_info_f,
                              std::chrono::duration<Rep, Per> _idle_age,
                              std::chrono::duration<Rep, Per> _erase_age,
                              std::chrono::duration<Rep, Per> _check_time,
                              double _system_capacity,
                              double _mclock_win_size,
                              bool _allow_limit_break = false,
                              double _anticipation_timeout = 0.0) :
                    super(_client_info_f,
                          _idle_age, _erase_age, _check_time,
                          _allow_limit_break, _anticipation_timeout, _system_capacity,
                          _mclock_win_size) {
                // empty
            }

            // pull convenience constructor
            PullPriorityQueue(typename super::ClientInfoFunc _client_info_f,
                              bool _allow_limit_break = false,
                              double _anticipation_timeout = 0.0) :
                    PullPriorityQueue(_client_info_f,
                                      std::chrono::minutes(10),
                                      std::chrono::minutes(15),
                                      std::chrono::minutes(6),
                                      _allow_limit_break,
                                      _anticipation_timeout) {
                // empty
            }

            // pull convenience constructor
            PullPriorityQueue(typename super::ClientInfoFunc _client_info_f,
                              double _system_capacity, double _mclock_win_size,
                              bool _allow_limit_break = false,
                              double _anticipation_timeout = 0.0) :
                    PullPriorityQueue(_client_info_f,
                                      std::chrono::minutes(10),
                                      std::chrono::minutes(15),
                                      std::chrono::minutes(6),
                                      _system_capacity,
                                      _mclock_win_size,
                                      _allow_limit_break,
                                      _anticipation_timeout) {
                // empty
            }

            inline void add_request(R &&request,
                                    const C &client_id,
                                    const ReqParams &req_params,
                                    double addl_cost = 0.0) {
                add_request(typename super::RequestRef(new R(std::move(request))),
                            client_id,
                            req_params,
                            get_time(),
                            addl_cost);
            }


            inline void add_request(R &&request,
                                    const C &client_id,
                                    double addl_cost = 0.0) {
                static const ReqParams null_req_params;
                add_request(typename super::RequestRef(new R(std::move(request))),
                            client_id,
                            null_req_params,
                            get_time(),
                            addl_cost);
            }


            inline void add_request_time(R &&request,
                                         const C &client_id,
                                         const ReqParams &req_params,
                                         const Time time,
                                         double addl_cost = 0.0) {
                add_request(typename super::RequestRef(new R(std::move(request))),
                            client_id,
                            req_params,
                            time,
                            addl_cost);
            }


            inline void add_request(typename super::RequestRef &&request,
                                    const C &client_id,
                                    const ReqParams &req_params,
                                    double addl_cost = 0.0) {
                add_request(request, req_params, client_id, get_time(), addl_cost);
            }


            inline void add_request(typename super::RequestRef &&request,
                                    const C &client_id,
                                    double addl_cost = 0.0) {
                static const ReqParams null_req_params;
                add_request(request, null_req_params, client_id, get_time(), addl_cost);
            }


            // this does the work; the versions above provide alternate interfaces
            void add_request(typename super::RequestRef &&request,
                             const C &client_id,
                             const ReqParams &req_params,
                             const Time time,
                             double addl_cost = 0.0) {
                typename super::DataGuard g(this->data_mtx);
#ifdef PROFILE
                add_request_timer.start();
#endif
                super::do_add_request(std::move(request),
                                      client_id,
                                      req_params,
                                      time,
                                      addl_cost);
                // no call to schedule_request for pull version
#ifdef PROFILE
                add_request_timer.stop();
#endif
            }


            inline PullReq pull_request() {
                return pull_request(get_time());
            }

            PullReq pull_request(Time now) {
                PullReq result;
                typename super::DataGuard g(this->data_mtx);
#ifdef PROFILE
                pull_request_timer.start();
#endif

                typename super::NextReq next = super::do_next_request(now);
                result.type = next.type;
                switch (next.type) {
                    case super::NextReqType::none:
                        return result;
                    case super::NextReqType::future:
                        result.data = next.when_ready;
                        return result;
                    case super::NextReqType::returning:
                        // to avoid nesting, break out and let code below handle this case
                        break;
                    default:
                        assert(false);
                }

                // we'll only get here if we're returning an entry

                auto process_f =
                        [&](PullReq &pull_result, PhaseType phase) ->
                                std::function<void(const C &,
                                                   typename super::RequestRef &)> {
                            return [&pull_result, phase](const C &client,
                                                         typename super::RequestRef &request) {
                                pull_result.data =
                                        typename PullReq::Retn{client, std::move(request), phase};
                            };
                        };

                switch (next.heap_id) {
                    case super::HeapId::reservation:
                        super::pop_process_request(this->resv_heap,
                                                   process_f(result, PhaseType::reservation), now);
                        ++this->reserv_sched_count;
                        break;
                    case super::HeapId::deltar:
                        super::pop_process_request(this->deltar_heap,
                                                   process_f(result, PhaseType::priority), now, true);
//                        { // need to use retn temporarily
//                            auto &retn = boost::get<typename PullReq::Retn>(result.data);
//                            super::reduce_reservation_tags(retn.client);
//                        }
                        ++this->prop_sched_count;
                        break;
                    case super::HeapId::burst:
                        super::pop_process_request(this->burst_heap,
                                                   process_f(result, PhaseType::priority), now);
                        ++this->prop_sched_count;
                        break;
                    case super::HeapId::best_effort:
                        super::pop_process_request(this->best_heap,
                                                   process_f(result, PhaseType::priority), now);
                        ++this->prop_sched_count;
                        break;
//                    case super::HeapId::prop:
//                        super::pop_process_request(this->prop_heap,
//                                                   process_f(result, PhaseType::priority), now, true);
//                        ++this->prop_sched_count;
//                        break;
                    default:
                        assert(false);
                }

#ifdef PROFILE
                pull_request_timer.stop();
#endif
                return result;
            } // pull_request


        protected:


            // data_mtx should be held when called; unfortunately this
            // function has to be repeated in both push & pull
            // specializations
            typename super::NextReq next_request() {
                return next_request(get_time());
            }
        }; // class PullPriorityQueue


        // PUSH version
        template<typename C, typename R, bool U1 = false, uint B = 2>
        class PushPriorityQueue : public PriorityQueueBase<C, R, U1, B> {

        protected:

            using super = PriorityQueueBase<C, R, U1, B>;

        public:

            // a function to see whether the server can handle another request
            using CanHandleRequestFunc = std::function<bool(void)>;

            // a function to submit a request to the server; the second
            // parameter is a callback when it's completed
            using HandleRequestFunc =
            std::function<void(const C &, typename super::RequestRef, PhaseType)>;

        protected:

            CanHandleRequestFunc can_handle_f;
            HandleRequestFunc handle_f;
            // for handling timed scheduling
            std::mutex sched_ahead_mtx;
            std::condition_variable sched_ahead_cv;
            Time sched_ahead_when = TimeZero;

#ifdef PROFILE
            public:
              ProfileTimer<std::chrono::nanoseconds> add_request_timer;
              ProfileTimer<std::chrono::nanoseconds> request_complete_timer;
            protected:
#endif

            // NB: threads declared last, so constructed last and destructed first

            std::thread sched_ahead_thd;

        public:

            // push full constructor
            template<typename Rep, typename Per>
            PushPriorityQueue(typename super::ClientInfoFunc _client_info_f,
                              CanHandleRequestFunc _can_handle_f,
                              HandleRequestFunc _handle_f,
                              std::chrono::duration<Rep, Per> _idle_age,
                              std::chrono::duration<Rep, Per> _erase_age,
                              std::chrono::duration<Rep, Per> _check_time,
                              bool _allow_limit_break = false,
                              double anticipation_timeout = 0.0) :
                    super(_client_info_f,
                          _idle_age, _erase_age, _check_time,
                          _allow_limit_break, anticipation_timeout) {
                can_handle_f = _can_handle_f;
                handle_f = _handle_f;
                sched_ahead_thd = std::thread(&PushPriorityQueue::run_sched_ahead, this);
            }

            template<typename Rep, typename Per>
            PushPriorityQueue(typename super::ClientInfoFunc _client_info_f,
                              CanHandleRequestFunc _can_handle_f,
                              HandleRequestFunc _handle_f,
                              std::chrono::duration<Rep, Per> _idle_age,
                              std::chrono::duration<Rep, Per> _erase_age,
                              std::chrono::duration<Rep, Per> _check_time,
                              double _system_capacity,
                              double _mclock_win_size,
                              bool _allow_limit_break = false,
                              double anticipation_timeout = 0.0) :
                    super(_client_info_f,
                          _idle_age, _erase_age, _check_time, _allow_limit_break, anticipation_timeout,
                          _system_capacity, _mclock_win_size) {
                can_handle_f = _can_handle_f;
                handle_f = _handle_f;
                sched_ahead_thd = std::thread(&PushPriorityQueue::run_sched_ahead, this);
            }


            // push convenience constructor
            PushPriorityQueue(typename super::ClientInfoFunc _client_info_f,
                              CanHandleRequestFunc _can_handle_f,
                              HandleRequestFunc _handle_f,
                              bool _allow_limit_break = false,
                              double _anticipation_timeout = 0.0) :
                    PushPriorityQueue(_client_info_f,
                                      _can_handle_f,
                                      _handle_f,
                                      std::chrono::minutes(10),
                                      std::chrono::minutes(15),
                                      std::chrono::minutes(6),
                                      _allow_limit_break,
                                      _anticipation_timeout) {
                // empty
            }

            PushPriorityQueue(typename super::ClientInfoFunc _client_info_f,
                              CanHandleRequestFunc _can_handle_f,
                              HandleRequestFunc _handle_f,
                              double _system_capacity,
                              double _mclock_win_size,
                              bool _allow_limit_break = false,
                              double _anticipation_timeout = 0.0) :
                    PushPriorityQueue(_client_info_f,
                                      _can_handle_f,
                                      _handle_f,
                                      std::chrono::minutes(10),
                                      std::chrono::minutes(15),
                                      std::chrono::minutes(6),
                                      _system_capacity,
                                      _mclock_win_size,
                                      _allow_limit_break,
                                      _anticipation_timeout) {
                // empty
            }

            ~PushPriorityQueue() {
                this->finishing = true;
                sched_ahead_cv.notify_one();
                sched_ahead_thd.join();
            }

        public:

            inline void add_request(R &&request,
                                    const C &client_id,
                                    const ReqParams &req_params,
                                    double addl_cost = 0.0) {
                add_request(typename super::RequestRef(new R(std::move(request))),
                            client_id,
                            req_params,
                            get_time(),
                            addl_cost);
            }


            inline void add_request(typename super::RequestRef &&request,
                                    const C &client_id,
                                    const ReqParams &req_params,
                                    double addl_cost = 0.0) {
                add_request(request, req_params, client_id, get_time(), addl_cost);
            }


            inline void add_request_time(const R &request,
                                         const C &client_id,
                                         const ReqParams &req_params,
                                         const Time time,
                                         double addl_cost = 0.0) {
                add_request(typename super::RequestRef(new R(request)),
                            client_id,
                            req_params,
                            time,
                            addl_cost);
            }


            void add_request(typename super::RequestRef &&request,
                             const C &client_id,
                             const ReqParams &req_params,
                             const Time time,
                             double addl_cost = 0.0) {
                typename super::DataGuard g(this->data_mtx);
#ifdef PROFILE
                add_request_timer.start();
#endif
                super::do_add_request(std::move(request),
                                      client_id,
                                      req_params,
                                      time,
                                      addl_cost);
                schedule_request();
#ifdef PROFILE
                add_request_timer.stop();
#endif
            }


            void request_completed() {
                typename super::DataGuard g(this->data_mtx);
#ifdef PROFILE
                request_complete_timer.start();
#endif
                schedule_request();
#ifdef PROFILE
                request_complete_timer.stop();
#endif
            }

        protected:

            // data_mtx should be held when called; furthermore, the heap
            // should not be empty and the top element of the heap should
            // not be already handled
            //
            // NOTE: the use of "super::ClientRec" in either the template
            // construct or as a parameter to submit_top_request generated
            // a compiler error in g++ 4.8.4, when ClientRec was
            // "protected" rather than "public". By g++ 6.3.1 this was not
            // an issue. But for backwards compatibility
            // PriorityQueueBase::ClientRec is public.
            template<typename C1,
                    IndIntruHeapData super::ClientRec::*C2,
                    typename C3,
                    uint B4>
            C submit_top_request(IndIntruHeap<C1, typename super::ClientRec, C2, C3, B4> &heap,
                                 PhaseType phase) {
                C client_result;
                super::pop_process_request(heap,
                                           [this, phase, &client_result]
                                                   (const C &client,
                                                    typename super::RequestRef &request) {
                                               client_result = client;
                                               handle_f(client, std::move(request), phase);
                                           }, get_time());
                return client_result;
            }

            template<typename C1,
                    IndIntruHeapData super::ClientRec::*C2,
                    typename C3,
                    uint B4>
            C submit_top_request(IndIntruHeap<C1, typename super::ClientRec, C2, C3, B4> &heap,
                                 PhaseType phase, bool is_delta) {
                C client_result;
                super::pop_process_request(heap,
                                           [this, phase, &client_result]
                                                   (const C &client,
                                                    typename super::RequestRef &request) {
                                               client_result = client;
                                               handle_f(client, std::move(request), phase);
                                           }, get_time(), is_delta);
                return client_result;
            }


            // data_mtx should be held when called
            void submit_request(typename super::HeapId heap_id) {
//                C client;
                switch (heap_id) {
                    case super::HeapId::reservation:
                        // don't need to note client
                        (void) submit_top_request(this->resv_heap, PhaseType::reservation);
                        // unlike the other two cases, we do not reduce reservation
                        // tags here
                        ++this->reserv_sched_count;
                        break;
                    case super::HeapId::deltar:
                        // don't need to note client
                        // unlike the other two cases, we do not reduce reservation
                        (void) submit_top_request(this->deltar_heap, PhaseType::priority, true);
//                        super::reduce_reservation_tags(client);
                        // tags here
                        ++this->prop_sched_count;
                        break;
                    case super::HeapId::burst:
                        (void) submit_top_request(this->burst_heap, PhaseType::priority);
                        ++this->prop_sched_count;
                        break;
                    case super::HeapId::best_effort:
                        (void) submit_top_request(this->best_heap, PhaseType::priority);
                        ++this->prop_sched_count;
                        break;
//                    case super::HeapId::prop:
//                        (void) submit_top_request(this->prop_heap, PhaseType::priority, true);
//                        ++this->prop_sched_count;
//                        break;
                    default:
                        assert(false);
                }
            } // submit_request


            // data_mtx should be held when called; unfortunately this
            // function has to be repeated in both push & pull
            // specializations
            typename super::NextReq next_request() {
                return next_request(get_time());
            }


            // data_mtx should be held when called; overrides member
            // function in base class to add check for whether a request can
            // be pushed to the server
            typename super::NextReq next_request(Time now) {
                if (!can_handle_f()) {
                    typename super::NextReq result;
                    result.type = super::NextReqType::none;
                    return result;
                } else {
                    return super::do_next_request(now);
                }
            } // next_request


            // data_mtx should be held when called
            void schedule_request() {
                typename super::NextReq next_req = next_request();
                switch (next_req.type) {
                    case super::NextReqType::none:
                        return;
                    case super::NextReqType::future:
                        sched_at(next_req.when_ready);
                        break;
                    case super::NextReqType::returning:
                        submit_request(next_req.heap_id);
                        break;
                    default:
                        assert(false);
                }
            }


            // this is the thread that handles running schedule_request at
            // future times when nothing can be scheduled immediately
            void run_sched_ahead() {
                std::unique_lock<std::mutex> l(sched_ahead_mtx);

                while (!this->finishing) {
                    if (TimeZero == sched_ahead_when) {
                        sched_ahead_cv.wait(l);
                    } else {
                        Time now;
                        while (!this->finishing && (now = get_time()) < sched_ahead_when) {
                            long microseconds_l = long(1 + 1000000 * (sched_ahead_when - now));
                            auto microseconds = std::chrono::microseconds(microseconds_l);
                            sched_ahead_cv.wait_for(l, microseconds);
                        }
                        sched_ahead_when = TimeZero;
                        if (this->finishing) return;

                        l.unlock();
                        if (!this->finishing) {
                            typename super::DataGuard g(this->data_mtx);
                            schedule_request();
                        }
                        l.lock();
                    }
                }
            }


            void sched_at(Time when) {
                std::lock_guard<std::mutex> l(sched_ahead_mtx);
                if (this->finishing) return;
                if (TimeZero == sched_ahead_when || when < sched_ahead_when) {
                    sched_ahead_when = when;
                    sched_ahead_cv.notify_one();
                }
            }
        }; // class PushPriorityQueue

    } // namespace dmclock
} // namespace crimson
