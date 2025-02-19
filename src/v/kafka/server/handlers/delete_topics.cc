// Copyright 2020 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#include "kafka/server/handlers/delete_topics.h"

#include "cluster/metadata_cache.h"
#include "cluster/topics_frontend.h"
#include "kafka/server/errors.h"
#include "kafka/server/quota_manager.h"
#include "kafka/server/request_context.h"
#include "kafka/server/response.h"
#include "model/metadata.h"
#include "model/namespace.h"
#include "model/timeout_clock.h"

#include <seastar/core/coroutine.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/future.hh>
#include <seastar/core/smp.hh>
#include <seastar/util/log.hh>

#include <fmt/ostream.h>

#include <chrono>
#include <string_view>

namespace kafka {

std::vector<model::topic_namespace>
create_topic_namespaces(std::vector<model::topic> topic_names) {
    std::vector<model::topic_namespace> ret;
    ret.reserve(topic_names.size());
    std::transform(
      std::begin(topic_names),
      std::end(topic_names),
      std::back_inserter(ret),
      [](model::topic& tp) {
          return model::topic_namespace(model::kafka_namespace, std::move(tp));
      });
    return ret;
}

delete_topics_response create_response(std::vector<cluster::topic_result> res) {
    delete_topics_response resp;
    resp.data.responses.reserve(res.size());
    std::transform(
      res.begin(),
      res.end(),
      std::back_inserter(resp.data.responses),
      [](cluster::topic_result tr) {
          return deletable_topic_result{
            .name = std::move(tr.tp_ns.tp),
            .error_code = map_topic_error_code(tr.ec)};
      });
    return resp;
}

template<>
ss::future<response_ptr>
delete_topics_handler::handle(request_context ctx, ss::smp_service_group) {
    delete_topics_request request;
    request.decode(ctx.reader(), ctx.header().version);
    log_request(ctx.header(), request);

    auto unauthorized_it = std::partition(
      request.data.topic_names.begin(),
      request.data.topic_names.end(),
      [&ctx](const model::topic& topic) {
          return ctx.authorized(security::acl_operation::remove, topic);
      });

    std::vector<model::topic> unauthorized(
      std::make_move_iterator(unauthorized_it),
      std::make_move_iterator(request.data.topic_names.end()));

    request.data.topic_names.erase(
      unauthorized_it, request.data.topic_names.end());

    std::vector<cluster::topic_result> res;

    // Measure the partition mutation rate
    auto resp_delay = 0ms;
    auto quota_exceeded_it = co_await ssx::partition(
      request.data.topic_names.begin(),
      request.data.topic_names.end(),
      [&ctx, &resp_delay](const model::topic& t) {
          const auto cfg = ctx.metadata_cache().get_topic_cfg(
            model::topic_namespace_view(model::kafka_namespace, t));
          const auto mutations = cfg ? cfg->partition_count : 0;
          /// Capture before next scheduling point below
          auto& resp_delay_ref = resp_delay;
          return ctx.quota_mgr()
            .record_partition_mutations(ctx.header().client_id, mutations)
            .then([&resp_delay_ref](std::chrono::milliseconds delay) {
                resp_delay_ref = std::max(delay, resp_delay_ref);
                return delay == 0ms;
            });
      });

    std::vector<model::topic> quota_exceeded(
      std::make_move_iterator(quota_exceeded_it),
      std::make_move_iterator(request.data.topic_names.end()));

    request.data.topic_names.erase(
      quota_exceeded_it, request.data.topic_names.end());

    if (!request.data.topic_names.empty()) {
        auto tout = request.data.timeout_ms + model::timeout_clock::now();
        res = co_await ctx.topics_frontend().delete_topics(
          create_topic_namespaces(std::move(request.data.topic_names)), tout);
    }

    auto resp = create_response(std::move(res));
    resp.data.throttle_time_ms = resp_delay;
    for (auto& topic : unauthorized) {
        resp.data.responses.push_back(deletable_topic_result{
          .name = std::move(topic),
          .error_code = error_code::topic_authorization_failed,
        });
    }

    for (auto& topic : quota_exceeded) {
        /// The throttling_quota_exceeded code is only respected by newer
        /// clients, older clients will observe a different failure and be
        /// throttled at the connection layer
        const auto ec = (ctx.header().version >= api_version(5))
                          ? error_code::throttling_quota_exceeded
                          : error_code::unknown_server_error;
        resp.data.responses.push_back(deletable_topic_result{
          .name = std::move(topic),
          .error_code = ec,
          .error_message = "Too many partition mutations requested"});
    }

    co_return co_await ctx.respond(std::move(resp));
}

} // namespace kafka
