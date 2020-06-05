/*
 * Copyright (C) 2020 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#ifndef SRC__RMF_FLEET_ADAPTER__SERVICES__DETAIL__IMPL_NEGOTIATE_HPP
#define SRC__RMF_FLEET_ADAPTER__SERVICES__DETAIL__IMPL_NEGOTIATE_HPP

#include "../Negotiate.hpp"

namespace rmf_fleet_adapter {
namespace services {

//==============================================================================
template<typename Subscriber>
void Negotiate::operator()(const Subscriber& s)
{
  s.add([n = shared_from_this()]()
  {
    // This service will be discarded if it is unsubscribed from
    n->discard();
  });

  auto validators =
      rmf_traffic::agv::NegotiatingRouteValidator::Generator(_viewer).all();

  std::vector<JobPtr> search_jobs;
  search_jobs.reserve(validators.size() * _goals.size());

  for (const auto& goal : _goals)
  {
    for (const auto& validator : validators)
    {
      auto job = std::make_shared<jobs::Planning>(
            _planner, _starts, goal,
            rmf_traffic::agv::Plan::Options(validator)
            .interrupt_flag(_interrupted));

      job->progress().options().saturation_limit(200000);
      _evaluator.initialize(job->progress());

      search_jobs.emplace_back(std::move(job));
    }
  }

  const double initial_max_cost =
      _evaluator.best_estimate.cost * _evaluator.estimate_leeway;

  const std::size_t N_jobs = search_jobs.size();

  for (const auto& job : search_jobs)
    job->progress().options().maximum_cost_estimate(initial_max_cost);

  auto check_if_finished = [this, s, N_jobs]() -> bool
  {
    if (_finished)
      return true;

    if (_evaluator.finished_count >= N_jobs || *_interrupted)
    {
      if (_evaluator.best_result.progress
          && _evaluator.best_result.progress->success())
      {
        _finished = true;
        // This means we found a successful plan to submit to the negotiation.
        s.on_next(
              [r = *_evaluator.best_result.progress,
               approval = std::move(_approval),
               responder = std::move(_responder)]()
        {
          responder->submit(
                r->get_itinerary(),
                [r = std::move(r), approval = std::move(approval)]()
                -> UpdateVersion
          {
            if (approval)
              return approval(*r);

            return rmf_utils::nullopt;
          });
        });

        s.on_completed();
        this->interrupt();
        return true;
      }
      else if (_alternatives && !_alternatives->empty())
      {
        _finished = true;
        // This means we could not find a successful plan, but we have some
        // alternatives to offer the parent in the negotiation.
        s.on_next(
              [n = shared_from_this()]()
        {
          n->_responder->reject(*n->_alternatives);
        });

        s.on_completed();
        this->interrupt();
        return true;
      }
      else if (!_attempting_rollout)
      {
        _finished = true;
        // This means we could not find any plan or any alternatives to offer
        // the parent, so all we can do is forfeit.
        s.on_next(
              [n = shared_from_this()]()
        {
          std::vector<rmf_traffic::schedule::ParticipantId> blockers(
                n->_blockers.begin(), n->_blockers.end());

          n->_responder->forfeit(std::move(blockers));
        });

        s.on_completed();
        this->interrupt();
        return true;
      }

      // If we land here, that means a rollout is still being calculated, and
      // we will consider the service finished when that rollout is ready
    }

    return false;
  };

  _search_sub = rmf_rxcpp::make_job_from_action_list(search_jobs)
      .observe_on(rxcpp::observe_on_event_loop())
      .subscribe(
        [n = shared_from_this(), s, N_jobs,
         check_if_finished = std::move(check_if_finished)](
        const jobs::Planning::Result& result)
  {
    if (n->_discarded)
    {
      s.on_completed();
      return;
    }

    bool resume = false;
    if (n->_evaluator.evaluate(result.job.progress()))
    {
      resume = true;
    }
    else if (!n->_attempting_rollout && !n->_alternatives
             && n->_viewer->parent_id())
    {
      const auto parent_id = *n->_viewer->parent_id();
      for (const auto p : result.job.progress().blockers())
      {
        if (p == parent_id)
        {
          n->_attempting_rollout = true;
          auto rollout_source = result.job.progress();
          static_cast<rmf_traffic::agv::NegotiatingRouteValidator*>(
                rollout_source.options().validator().get())->mask(parent_id);

          n->_rollout_job = std::make_shared<jobs::Rollout>(
                std::move(rollout_source), parent_id,
                std::chrono::seconds(15), 200);

          n->_rollout_sub =
              rmf_rxcpp::make_job<jobs::Rollout::Result>(n->_rollout_job)
              .observe_on(rxcpp::observe_on_event_loop())
              .subscribe(
                [n, check_if_finished](const jobs::Rollout::Result& result)
          {
            n->_alternatives = result.alternatives;
            n->_attempting_rollout = false;
            check_if_finished();
          });
        }
      }
    }

    if (!check_if_finished())
    {
      const auto job = result.job.shared_from_this();
      if (resume)
      {
        if (n->_current_jobs.find(job) != n->_current_jobs.end())
        {
          job->resume();
        }
        else
        {
          n->_resume_jobs.push(job);
        }
      }
      else
      {
        const auto& blockers = job->progress().blockers();
        for (const auto p : blockers)
          n->_blockers.insert(p);

        if (!job->progress().success())
          job->discard();

        n->_current_jobs.erase(job);
      }

      while (n->_current_jobs.size() < max_concurrent_jobs
             && !n->_resume_jobs.empty())
      {
        n->_resume_next();
      }
    }
  });
}

} // namespace services
} // namespace rmf_fleet_adapter

#endif // SRC__RMF_FLEET_ADAPTER__SERVICES__DETAIL__IMPL_NEGOTIATE_HPP
